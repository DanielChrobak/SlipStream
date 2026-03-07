
import { enableControl } from './input.js';
import { MSG, C } from './constants.js';
import { S, $, safe, safeAsync, updateClockOffset, resetClockSync,
    startMetricsLogger, stopMetricsLogger, resetSessionStats, recordPacket,
    clientTimeUs, logVideoDrop, logAudioDrop, logNetworkDrop, log, bus } from './state.js';
import { handleAudioPacket, closeAudio, initDecoder, decodeFrame, stopKeyframeRetry } from './media.js';
import { updateMonOpts, updateCodecOpts, updateCodecDropdown, setHostCodecs,
    updateLoadingStage, showLoading, hideLoading, getStoredCodec, getStoredSoftwareEncode, initCodecDetection,
    updateSoftwareEncodeState,
    getStoredFps, updateFpsDropdown, closeTabbedMode } from './ui.js';
import { resetRenderer, setCursorStyle } from './renderer.js';
import { stopMic } from './mic.js';
import { showAuth, hideAuth, clearSession, validateSession } from './auth.js';
import { sendPing, requestKeyframe, requestRecoveryKeyframe, clearPendingKeyReq,
    applyCodec, applyFps, applySoftwareEncode, getMaxInFlightFrames, expectedChunkSize,
    tryRecoverFrameGroup, resetProtocolState } from './protocol.js';

const BASE_URL = location.origin;
let hasConnection = false;
let waitingFirstFrame = false;
let connectionAttempts = 0;
let pingInterval = null;
let channelsReady = 0;
let connectSeqCounter = 0;
let activeConnectSeq = 0;
let firstFrameWatchdog = null;
let activeConnectAbort = null;
let lastFrameCompletedAt = 0;
const MAX_SERVER_BUSY_RETRIES = 5;
const TEXT_DECODER = new TextDecoder();
let lastChunkCleanupAt = 0;
let lastPendingFecCleanupAt = 0;
const AUDIO_PKT_DATA = 0;
const AUDIO_PKT_FEC = 1;
const audioFecGroups = new Map();
const seenAudioPacketIds = new Set();
let lastAudioFecCleanupAt = 0;
const pendingVideoFec = new Map();

const cleanupPendingVideoFec = now => {
    if (now - lastPendingFecCleanupAt < 100 && pendingVideoFec.size < 96) return;
    lastPendingFecCleanupAt = now;
    for (const [frameId, entry] of pendingVideoFec) {
        if (now - entry.arrivalMs > C.FRAME_TIMEOUT_MS) pendingVideoFec.delete(frameId);
    }
};

const stashPendingVideoFec = (frameId, frameInfo, groupIndex, chunkData, arrivalMs) => {
    const pending = pendingVideoFec.get(frameId) || {
        arrivalMs,
        lastPacketMs: arrivalMs,
        total: frameInfo.total,
        frameSize: frameInfo.frameSize,
        dataChunkSize: frameInfo.dataChunkSize,
        capTs: frameInfo.capTs,
        sourceTs: frameInfo.sourceTs,
        encodeEndTs: frameInfo.encodeEndTs,
        enqueueTs: frameInfo.enqueueTs,
        isKey: frameInfo.isKey,
        fecGroupSize: frameInfo.fecGroupSize,
        groups: new Map()
    };

    if (pending.total !== frameInfo.total || pending.frameSize !== frameInfo.frameSize ||
        pending.dataChunkSize !== frameInfo.dataChunkSize || pending.capTs !== frameInfo.capTs ||
        pending.sourceTs !== frameInfo.sourceTs || pending.encodeEndTs !== frameInfo.encodeEndTs ||
        pending.enqueueTs !== frameInfo.enqueueTs) {
        pendingVideoFec.delete(frameId);
        return;
    }

    pending.arrivalMs = Math.min(pending.arrivalMs, arrivalMs);
    pending.lastPacketMs = Math.max(pending.lastPacketMs, arrivalMs);
    if (!pending.groups.has(groupIndex)) pending.groups.set(groupIndex, chunkData.slice());
    pendingVideoFec.set(frameId, pending);
};

const attachPendingVideoFec = (frameId, frame) => {
    const pending = pendingVideoFec.get(frameId);
    if (!pending) return;
    if (pending.total !== frame.total || pending.frameSize !== frame.frameSize ||
        pending.dataChunkSize !== frame.dataChunkSize || pending.capTs !== frame.capTs ||
        pending.sourceTs !== frame.sourceTs || pending.encodeEndTs !== frame.encodeEndTs ||
        pending.enqueueTs !== frame.enqueueTs) {
        pendingVideoFec.delete(frameId);
        return;
    }

    frame.arrivalMs = Math.min(frame.arrivalMs, pending.arrivalMs);
    frame.lastPacketMs = Math.max(frame.lastPacketMs, pending.lastPacketMs);
    frame.fecGroupSize = Math.max(frame.fecGroupSize, pending.fecGroupSize || 1);
    for (const [groupIndex, payload] of pending.groups) {
        if (!frame.fecParts.has(groupIndex)) frame.fecParts.set(groupIndex, payload);
    }
    pendingVideoFec.delete(frameId);
};

const trimSeenAudioPacketIds = () => {
    if (seenAudioPacketIds.size <= 4096) return;
    while (seenAudioPacketIds.size > 2048) {
        const first = seenAudioPacketIds.values().next().value;
        if (first === undefined) break;
        seenAudioPacketIds.delete(first);
    }
};

const getAudioGroupStart = (packetId, groupSize) => packetId - (packetId % groupSize);

const cleanupAudioFecGroups = now => {
    if (now - lastAudioFecCleanupAt < 250 && audioFecGroups.size < 96) return;
    lastAudioFecCleanupAt = now;
    for (const [groupStart, group] of audioFecGroups) {
        if (now - group.updatedAt > 1500) audioFecGroups.delete(groupStart);
    }
};

const deliverAudioPacket = packet => {
    const buffer = packet instanceof ArrayBuffer ? packet : packet.buffer.slice(packet.byteOffset, packet.byteOffset + packet.byteLength);
    recordPacket(buffer.byteLength, 'audio');
    handleAudioPacket(buffer);
};

const tryRecoverAudioGroup = groupStart => {
    const group = audioFecGroups.get(groupStart);
    if (!group?.hasFec || !group.fecPayload?.length) return false;

    const missing = [];
    for (let i = 0; i < group.groupSize; i++) {
        const packetId = groupStart + i;
        if (!group.dataPackets.has(packetId)) missing.push(packetId);
    }

    if (missing.length !== 1 || group.dataPackets.size < group.groupSize - 1) return false;

    const recovered = group.fecPayload.slice();
    for (let i = 0; i < group.groupSize; i++) {
        const packetId = groupStart + i;
        const packet = group.dataPackets.get(packetId);
        if (!packet) continue;
        const limit = Math.min(recovered.length, packet.length);
        for (let j = 0; j < limit; j++) recovered[j] ^= packet[j];
    }

    if (recovered.byteLength < C.AUDIO_HEADER) return false;
    const recoveredView = new DataView(recovered.buffer, recovered.byteOffset, recovered.byteLength);
    const msgType = recoveredView.getUint32(0, true);
    const packetId = recoveredView.getUint32(12, true);
    const dataLength = recoveredView.getUint16(18, true);
    const packetType = recoveredView.getUint8(20);
    const expectedLen = C.AUDIO_HEADER + dataLength;

    if (msgType !== MSG.AUDIO_DATA || packetType !== AUDIO_PKT_DATA) return false;
    if (packetId !== missing[0] || expectedLen > recovered.byteLength || dataLength === 0) return false;
    if (seenAudioPacketIds.has(packetId)) return false;

    const recoveredPacket = recovered.slice(0, expectedLen);
    seenAudioPacketIds.add(packetId);
    trimSeenAudioPacketIds();
    deliverAudioPacket(recoveredPacket);
    log.info('NET', 'Audio FEC recovered packet', { packetId, groupStart });
    return true;
};

const abortPendingConnect = reason => {
    if (!activeConnectAbort) return;
    try { activeConnectAbort.abort(reason || 'connection-reset'); } catch (e) { log.debug('NET', 'Abort already triggered', { error: e?.message }); }
    activeConnectAbort = null;
};

const clearPing = () => { if (pingInterval) { clearInterval(pingInterval); pingInterval = null; } };

const clearFirstFrameWatchdog = () => { if (firstFrameWatchdog) { clearTimeout(firstFrameWatchdog); firstFrameWatchdog = null; } };

const armFirstFrameWatchdog = () => {
    clearFirstFrameWatchdog();
    const seq = activeConnectSeq;
    firstFrameWatchdog = setTimeout(() => {
        if (!waitingFirstFrame || seq !== activeConnectSeq) return;
        log.warn('NET', 'Still waiting for first frame', {
            seq, pcState: S.pc?.connectionState, iceState: S.pc?.iceConnectionState,
            control: S.dcControl?.readyState, video: S.dcVideo?.readyState,
            audio: S.dcAudio?.readyState, input: S.dcInput?.readyState,
            mic: S.dcMic?.readyState, channelsReady
        });
    }, 3000);
};

// --- Monitor list parsing ---
const parseMonitorList = data => {
    const view = new DataView(data);
    let offset = 5;
    S.currentMon = view.getUint8(offset++);
    const count = view.getUint8(4);
    S.monitors = [];
    for (let i = 0; i < count; i++) {
        const index = view.getUint8(offset++);
        const width = view.getUint16(offset, true); offset += 2;
        const height = view.getUint16(offset, true); offset += 2;
        const refreshRate = view.getUint16(offset, true); offset += 2;
        const isPrimary = view.getUint8(offset++) === 1;
        const nameLen = view.getUint8(offset++);
        const name = TEXT_DECODER.decode(new Uint8Array(data, offset, nameLen));
        offset += nameLen;
        S.monitors.push({ index, width, height, refreshRate, isPrimary, name });
    }
    log.info('NET', 'Monitor list received', { count, current: S.currentMon });
    updateMonOpts();
};

// --- Frame processing ---
const processFrame = (frameId, frame) => {
    if (!frame.parts.every(p => p)) {
        const missingChunks = frame.parts.map((p, i) => p ? null : i).filter(i => i !== null);
        logVideoDrop('Incomplete frame', {
            frameId, received: frame.received, total: frame.total,
            missingChunks: missingChunks.slice(0, 10).join(','),
            missingCount: missingChunks.length,
            isKey: frame.isKey ? 1 : 0,
            fecRecovered: frame.fecRecovered || 0,
            fecPartsReceived: frame.fecParts.size
        });
        S.chunks.delete(frameId);
        return;
    }
    const buffer = frame.total === 1
        ? frame.parts[0]
        : (() => {
            const totalSize = frame.parts.reduce((s, p) => s + p.byteLength, 0);
            const combined = new Uint8Array(totalSize);
            let offset = 0;
            frame.parts.forEach(p => { combined.set(p, offset); offset += p.byteLength; });
            return combined;
        })();

    // Stream corruption check: verify reassembled size matches expected
    if (buffer.byteLength !== frame.frameSize) {
        logVideoDrop('STREAM CORRUPTION: reassembled size mismatch', {
            frameId, expected: frame.frameSize, actual: buffer.byteLength,
            isKey: frame.isKey ? 1 : 0, chunks: frame.total,
            fecRecovered: frame.fecRecovered || 0
        });
        S.chunks.delete(frameId);
        return;
    }

    // Stream corruption check: verify bitstream header
    if (buffer.byteLength >= 4) {
        const d = buffer instanceof Uint8Array ? buffer : new Uint8Array(buffer);
        let validStart = false;
        const codecId = S.currentCodec;
        if (codecId === 2) { // H.264
            validStart = (d[0] === 0x00 && d[1] === 0x00 && d[2] === 0x00 && d[3] === 0x01) ||
                         (d[0] === 0x00 && d[1] === 0x00 && d[2] === 0x01);
        } else if (codecId === 1) { // H.265
            validStart = (d[0] === 0x00 && d[1] === 0x00 && d[2] === 0x00 && d[3] === 0x01) ||
                         (d[0] === 0x00 && d[1] === 0x00 && d[2] === 0x01);
        } else if (codecId === 0) { // AV1
            const obuType = (d[0] >> 3) & 0x0F;
            validStart = (obuType >= 1 && obuType <= 8);
        }
        if (!validStart) {
            logVideoDrop('STREAM CORRUPTION: invalid bitstream header', {
                frameId, isKey: frame.isKey ? 1 : 0,
                header: `${d[0].toString(16).padStart(2,'0')} ${d[1].toString(16).padStart(2,'0')} ${d[2].toString(16).padStart(2,'0')} ${d[3].toString(16).padStart(2,'0')}`,
                size: buffer.byteLength, codec: codecId,
                fecRecovered: frame.fecRecovered || 0
            });
        }
    }

    // Frame gap detection
    if (S.lastFrameId > 0 && frameId > S.lastFrameId + 1) {
        const gapSize = frameId - S.lastFrameId - 1;
        if (gapSize > 0) {
            log.warn('VIDEO', `Frame gap detected: ${gapSize} frames missing`, {
                prevId: S.lastFrameId, currentId: frameId, isKey: frame.isKey ? 1 : 0
            });

            if (!frame.isKey) {
                if (gapSize >= 2) {
                    S.needKey = 1;
                    requestRecoveryKeyframe('frame-gap-large');
                } else {
                    requestKeyframe('frame-gap-small');
                }
            }
        }
    }

    S.stats.framesComplete++;
    if (frame.isKey) S.stats.keyframesReceived++;
    if (frameId > S.lastFrameId) S.lastFrameId = frameId;
    lastFrameCompletedAt = performance.now();

    const reassemblyCompleteMs = performance.now();
    const assemblyMs = reassemblyCompleteMs - frame.arrivalMs;

    log.debug('VIDEO', 'Frame complete', {
        id: frameId, key: frame.isKey ? 1 : 0, size: buffer.byteLength,
        chunks: frame.total, fecRecovered: frame.fecRecovered || 0,
        assemblyMs: assemblyMs.toFixed(1)
    });

    const decodeAccepted = decodeFrame({
        buf: buffer,
        capTs: frame.capTs,
        sourceTs: frame.sourceTs,
        encodeEndTs: frame.encodeEndTs,
        enqueueTs: frame.enqueueTs,
        encMs: frame.encMs,
        isKey: frame.isKey,
        arrivalMs: frame.arrivalMs,
        firstPacketMs: frame.arrivalMs,
        lastPacketMs: frame.lastPacketMs,
        reassemblyCompleteMs
    });

    if (waitingFirstFrame && decodeAccepted) {
        waitingFirstFrame = false;
        clearFirstFrameWatchdog();
        hideLoading();
        hasConnection = true;
        log.info('NET', 'First frame received', { frameId, isKey: frame.isKey ? 1 : 0, size: buffer.byteLength });
    }
    S.chunks.delete(frameId);
};

// --- Control message handler ---
const handleControl = async e => {
    const arrivalUs = clientTimeUs();
    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < 4) { logNetworkDrop('Invalid control message'); return; }

    const view = new DataView(e.data);
    const msgType = view.getUint32(0, true);
    const length = e.data.byteLength;

    if (msgType === MSG.PING && length === 24) {
        updateClockOffset(Number(view.getBigUint64(8, true)), Number(view.getBigUint64(16, true)), arrivalUs);
        return recordPacket(length, 'control');
    }
    if (msgType === MSG.HOST_INFO && length >= 6) {
        const hostFlags = length >= 7 ? view.getUint8(6) : 0;
        const softwareEncodeEnabled = (hostFlags & 0x01) !== 0;
        const softwareEncodeForced = (hostFlags & 0x02) !== 0;
        S.hostFps = view.getUint16(4, true);
        updateSoftwareEncodeState({ enabled: softwareEncodeEnabled, forced: softwareEncodeForced });
        if (!S.fpsSent) setTimeout(() => applyFps(getStoredFps() ?? 60), 50);
        const softwareEncodePref = getStoredSoftwareEncode();
        if (!S.softwareEncodeSent && !softwareEncodeForced && softwareEncodeEnabled !== softwareEncodePref) {
            setTimeout(() => applySoftwareEncode(softwareEncodePref), 75);
        }
        if (!hasConnection) { updateLoadingStage('Connected'); waitingFirstFrame = true; armFirstFrameWatchdog(); log.info('NET', 'Waiting for first frame', { seq: activeConnectSeq }); }
        log.info('NET', 'Host info', { fps: S.hostFps, softwareEncodeEnabled, softwareEncodeForced });
        return recordPacket(length, 'control');
    }
    if (msgType === MSG.ENCODER_INFO && length >= 7) {
        const codecId = view.getUint8(4);
        const encoderFlags = view.getUint8(5);
        const nameLen = view.getUint8(6);
        if (length >= 7 + nameLen && nameLen <= 64) {
            S.hostEncoderName = nameLen > 0 ? TEXT_DECODER.decode(new Uint8Array(e.data, 7, nameLen)) : null;
            log.info('NET', 'Host encoder info', {
                codec: codecId,
                software: (encoderFlags & 0x01) !== 0,
                forced: (encoderFlags & 0x02) !== 0,
                encoder: S.hostEncoderName || 'unknown'
            });
        }
        return recordPacket(length, 'control');
    }
    if (msgType === MSG.CODEC_CAPS && length === 5) {
        setHostCodecs(view.getUint8(4));
        if (!S.codecSent) setTimeout(() => applyCodec(getStoredCodec()), 100);
        log.info('NET', 'Codec caps', { caps: view.getUint8(4).toString(2) });
        return recordPacket(length, 'control');
    }
    if (msgType === MSG.CODEC_ACK && length === 5) {
        S.currentCodec = view.getUint8(4);
        updateCodecDropdown(S.currentCodec);
        initDecoder(true);
        log.info('NET', 'Codec ack', { codec: S.currentCodec });
        return recordPacket(length, 'control');
    }
    if (msgType === MSG.FPS_ACK && length === 7) {
        S.currentFps = view.getUint16(4, true);
        S.currentFpsMode = view.getUint8(6);
        updateFpsDropdown(S.currentFps);
        log.info('NET', 'FPS ack', { fps: S.currentFps, mode: S.currentFpsMode });
        return recordPacket(length, 'control');
    }
    if (msgType === MSG.MONITOR_LIST && length >= 6 && length < 1000) {
        const count = view.getUint8(4);
        if (count >= 1 && count <= 16) { recordPacket(length, 'control'); return parseMonitorList(e.data); }
    }
    if (msgType === MSG.CLIPBOARD_DATA && length >= 8) {
        const textLen = view.getUint32(4, true);
        if (textLen > 0 && length >= 8 + textLen && textLen <= 1048576) {
            const text = TEXT_DECODER.decode(new Uint8Array(e.data, 8, textLen));
            try { await navigator.clipboard.writeText(text); log.debug('NET', 'Clipboard set', { len: textLen }); }
            catch (err) { logNetworkDrop('Clipboard write failed', { error: err?.message }); }
        }
        return recordPacket(length, 'control');
    }
    if (msgType === MSG.CURSOR_SHAPE && length === 5) { setCursorStyle(view.getUint8(4)); return recordPacket(length, 'control'); }
    if (msgType === MSG.KICKED && length === 4) {
        logNetworkDrop('Kicked by server');
        cleanup();
        hasConnection = false;
        closeTabbedMode();
        showAuth('Disconnected: Another client connected');
        return;
    }
    if (msgType === MSG.VERSION && length >= 5) {
        const versionLen = view.getUint8(4);
        if (length >= 5 + versionLen && versionLen > 0 && versionLen <= 32) {
            S.hostVersion = TEXT_DECODER.decode(new Uint8Array(e.data, 5, versionLen));
            log.info('NET', 'Host version received', { version: S.hostVersion });
        }
        return recordPacket(length, 'control');
    }
    // Unhandled control message
    log.warn('NET', 'Unhandled control message', { type: '0x' + msgType.toString(16), length });
};

// --- Video packet handler ---
const VIDEO_PKT_DATA = 0, VIDEO_PKT_FEC = 1;

const handleVideo = e => {
    const arrivalMs = performance.now();
    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < C.HEADER) { logVideoDrop('Invalid video packet'); return; }

    const view = new DataView(e.data);
    const length = e.data.byteLength;
    const captureTs = Number(view.getBigUint64(0, true));
    const sourceTs = Number(view.getBigUint64(8, true));
    const encodeEndTs = Number(view.getBigUint64(16, true));
    const enqueueTs = Number(view.getBigUint64(24, true));
    const frameId = view.getUint32(36, true);
    const frameSize = view.getUint32(40, true);
    const chunkIndex = view.getUint16(44, true);
    const totalChunks = view.getUint16(46, true);
    const chunkBytes = view.getUint16(48, true);
    const dataChunkSize = view.getUint16(50, true);
    const frameType = view.getUint8(52);
    const packetType = view.getUint8(53);
    const fecGroupSize = view.getUint8(54) || C.FEC_GROUP_SIZE;

    if (totalChunks === 0 || captureTs <= 0 || sourceTs <= 0 || frameSize === 0 || dataChunkSize === 0) { logVideoDrop('Invalid packet data'); return; }
    if (packetType === VIDEO_PKT_DATA && chunkIndex >= totalChunks) { logVideoDrop('Invalid data chunk index', { frameId, chunkIndex, totalChunks }); return; }
    if (packetType !== VIDEO_PKT_DATA && packetType !== VIDEO_PKT_FEC) { logVideoDrop('Unknown video packet type', { packetType, frameId }); return; }
    if (length < C.HEADER || chunkBytes !== length - C.HEADER) { logVideoDrop('Video size mismatch', { frameId, chunkBytes, actual: length - C.HEADER }); return; }

    const chunkData = new Uint8Array(e.data, C.HEADER, chunkBytes);
    recordPacket(length, 'video');
    S.stats.bytes += length;
    cleanupPendingVideoFec(arrivalMs);

    if (arrivalMs - lastChunkCleanupAt >= 50) {
        lastChunkCleanupAt = arrivalMs;
        for (const [id, frame] of S.chunks) {
            if (arrivalMs - frame.arrivalMs > C.FRAME_TIMEOUT_MS && frame.received < frame.total) {
                if (frame.fecParts.size > 0) {
                    for (const groupIndex of frame.fecParts.keys()) tryRecoverFrameGroup(id, frame, groupIndex);
                    if (frame.received === frame.total) { processFrame(id, frame); continue; }
                }
                logVideoDrop('Frame timeout', {
                    frameId: id, received: frame.received, total: frame.total,
                    ageMs: (arrivalMs - frame.arrivalMs).toFixed(0),
                    isKey: frame.isKey ? 1 : 0,
                    fecParts: frame.fecParts.size,
                    fecRecovered: frame.fecRecovered || 0
                }, { countDropped: false });
                S.chunks.delete(id);
                S.stats.framesTimeout++;
                if (frame.isKey && frame.received > 0) {
                    log.error('VIDEO', 'Timed-out keyframe is unrecoverable - requesting new keyframe', {
                        frameId: id, received: frame.received, total: frame.total
                    });
                    requestRecoveryKeyframe('timed-out-keyframe-unrecoverable');
                }
            }
        }
    }

    // Create frame entry if needed
    if (!S.chunks.has(frameId)) {
        if (packetType === VIDEO_PKT_FEC) {
            stashPendingVideoFec(frameId, {
                total: totalChunks,
                frameSize,
                dataChunkSize,
                capTs: captureTs,
                sourceTs,
                encodeEndTs,
                enqueueTs,
                isKey: frameType === 1,
                fecGroupSize: Math.max(1, fecGroupSize)
            }, chunkIndex, chunkData, arrivalMs);
            return;
        }

        S.chunks.set(frameId, {
            parts: Array(totalChunks).fill(null), partSizes: Array(totalChunks).fill(0),
            total: totalChunks, received: 0, capTs: captureTs,
            sourceTs,
            encodeEndTs,
            enqueueTs,
            encMs: view.getUint32(32, true) / 1000, arrivalMs, lastPacketMs: arrivalMs, isKey: frameType === 1,
            frameSize, dataChunkSize, fecGroupSize: Math.max(1, fecGroupSize),
            fecParts: new Map(), fecRecovered: 0
        });
        attachPendingVideoFec(frameId, S.chunks.get(frameId));

        const maxInFlight = getMaxInFlightFrames();
        if (S.chunks.size > maxInFlight) {
            const sorted = [...S.chunks.entries()].sort((a, b) => a[0] - b[0]);
            const candidate = sorted.find(([id, f]) => id !== frameId && f.received < f.total && !f.isKey)
                || sorted.find(([id, f]) => id !== frameId && f.received < f.total)
                || sorted.find(([id]) => id !== frameId);

            if (candidate) {
                const [victimId, victim] = candidate;
                S.chunks.delete(victimId);
                logVideoDrop('Max frames exceeded', {
                    inFlight: sorted.length, limit: maxInFlight, droppedFrameId: victimId,
                    droppedReceived: victim.received, droppedTotal: victim.total, droppedIsKey: victim.isKey ? 1 : 0
                });
                const now = performance.now();
                const stalled = now - lastFrameCompletedAt > Math.max(700, C.FRAME_TIMEOUT_MS / 2);
                if (victim.isKey || stalled) requestRecoveryKeyframe(victim.isKey ? 'evicted-keyframe-unrecoverable' : 'reassembly-stalled');
            }
        }
    }

    const frame = S.chunks.get(frameId);
    if (!frame) return;
    frame.lastPacketMs = Math.max(frame.lastPacketMs, arrivalMs);
    if (frame.total !== totalChunks || frame.frameSize !== frameSize || frame.dataChunkSize !== dataChunkSize) { logVideoDrop('Frame metadata mismatch', { frameId }); return; }

    if (packetType === VIDEO_PKT_DATA) {
        if (frame.parts[chunkIndex]) { logNetworkDrop('Duplicate data chunk', { frameId, chunkIndex }); return; }
        frame.parts[chunkIndex] = chunkData;
        frame.partSizes[chunkIndex] = chunkBytes;
        frame.received++;
        const groupIndex = Math.floor(chunkIndex / frame.fecGroupSize);
        if (frame.fecParts.has(groupIndex)) tryRecoverFrameGroup(frameId, frame, groupIndex);
    } else {
        if (frame.fecParts.has(chunkIndex)) { logNetworkDrop('Duplicate FEC chunk', { frameId, groupIndex: chunkIndex }); return; }
        frame.fecParts.set(chunkIndex, chunkData);
        tryRecoverFrameGroup(frameId, frame, chunkIndex);
    }

    if (frame.received === frame.total) processFrame(frameId, frame);
};

// --- Audio packet handler ---
const handleAudio = e => {
    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < C.AUDIO_HEADER) { logAudioDrop('Invalid audio packet'); return; }
    const view = new DataView(e.data);
    const length = e.data.byteLength;
    const msgType = view.getUint32(0, true);
    if (msgType !== MSG.AUDIO_DATA || length < C.AUDIO_HEADER) {
        logAudioDrop('Unknown audio message', { type: msgType });
        return;
    }

    const packetId = view.getUint32(12, true);
    const dataLen = view.getUint16(18, true);
    const packetType = view.getUint8(20);
    const groupSize = Math.max(1, view.getUint8(21) || C.AUDIO_FEC_GROUP_SIZE || 4);
    const expectedLen = C.AUDIO_HEADER + dataLen;

    if (dataLen === 0 || expectedLen !== length) {
        logAudioDrop('Size mismatch', { packetId, expected: expectedLen, got: length, packetType });
        return;
    }
    if (packetType !== AUDIO_PKT_DATA && packetType !== AUDIO_PKT_FEC) {
        logAudioDrop('Unknown audio packet type', { packetType, packetId });
        return;
    }

    const now = performance.now();
    cleanupAudioFecGroups(now);

    if (packetType === AUDIO_PKT_DATA) {
        if (seenAudioPacketIds.has(packetId)) {
            log.debug('NET', 'Duplicate audio packet', { packetId });
            return;
        }

        seenAudioPacketIds.add(packetId);
        trimSeenAudioPacketIds();
        deliverAudioPacket(e.data);

        const groupStart = getAudioGroupStart(packetId, groupSize);
        const group = audioFecGroups.get(groupStart) || {
            groupSize,
            dataPackets: new Map(),
            hasFec: false,
            fecPayload: null,
            updatedAt: now
        };
        group.groupSize = groupSize;
        group.updatedAt = now;
        group.dataPackets.set(packetId, new Uint8Array(e.data));
        audioFecGroups.set(groupStart, group);

        if (tryRecoverAudioGroup(groupStart)) audioFecGroups.delete(groupStart);
        else if (group.dataPackets.size >= group.groupSize && group.hasFec) audioFecGroups.delete(groupStart);
        return;
    }

    const groupStart = packetId;
    const group = audioFecGroups.get(groupStart) || {
        groupSize,
        dataPackets: new Map(),
        hasFec: false,
        fecPayload: null,
        updatedAt: now
    };
    group.groupSize = groupSize;
    group.hasFec = true;
    group.updatedAt = now;
    group.fecPayload = new Uint8Array(e.data, C.AUDIO_HEADER, dataLen);
    audioFecGroups.set(groupStart, group);

    if (tryRecoverAudioGroup(groupStart) || (group.dataPackets.size >= group.groupSize && group.hasFec)) {
        audioFecGroups.delete(groupStart);
    }
};

// --- Channel lifecycle ---
const DC_KEYS = ['dcControl', 'dcVideo', 'dcAudio', 'dcInput', 'dcMic'];
const DC_CONFIG = [
    ['dcControl', 'control', C.DC_CONTROL, handleControl],
    ['dcVideo', 'video', C.DC_VIDEO, handleVideo],
    ['dcAudio', 'audio', C.DC_AUDIO, handleAudio],
    ['dcInput', 'input', C.DC_INPUT, () => {}],
    ['dcMic', 'mic', C.DC_MIC, () => {}]
];

const closeDataChannels = () => { [...DC_KEYS, 'pc'].forEach(key => safe(() => S[key]?.close(), undefined, 'NET')); };

const onAllChannelsOpen = async connectSeq => {
    log.info('NET', 'All channels open', {
        seq: connectSeq, control: S.dcControl?.readyState, video: S.dcVideo?.readyState,
        audio: S.dcAudio?.readyState, input: S.dcInput?.readyState, mic: S.dcMic?.readyState
    });
    S.fpsSent = S.codecSent = S.softwareEncodeSent = 0;
    S.authenticated = 1;
    resetClockSync();
    resetSessionStats();
    updateLoadingStage('Connected');
    await initDecoder();
    clearPing();
    startMetricsLogger();
    pingInterval = setInterval(sendPing, C.PING_MS);
};

const onChannelClose = (connectSeq, label) => {
    log.info('NET', 'Channel closed', {
        seq: connectSeq, activeSeq: activeConnectSeq, label,
        control: S.dcControl?.readyState, video: S.dcVideo?.readyState,
        audio: S.dcAudio?.readyState, input: S.dcInput?.readyState, mic: S.dcMic?.readyState
    });
    if (connectSeq !== activeConnectSeq) log.warn('NET', 'Stale channel close callback', { seq: connectSeq, activeSeq: activeConnectSeq, label });
    S.fpsSent = S.codecSent = S.softwareEncodeSent = 0;
    clearPing();
    clearFirstFrameWatchdog();
    stopMetricsLogger();
    resetRenderer();
    stopKeyframeRetry();
    channelsReady = 0;
    stopMic();
};

const setupDataChannel = (dc, onMessage, connectSeq) => {
    dc.binaryType = 'arraybuffer';
    dc.onopen = async () => {
        channelsReady++;
        log.info('NET', 'Channel open', { seq: connectSeq, activeSeq: activeConnectSeq, label: dc.label, ready: channelsReady, state: dc.readyState });
        if (channelsReady === 5) await onAllChannelsOpen(connectSeq);
    };
    dc.onclose = () => onChannelClose(connectSeq, dc.label);
    dc.onerror = err => logNetworkDrop('Channel error', { seq: connectSeq, activeSeq: activeConnectSeq, label: dc.label, error: err?.error?.message || 'Unknown' });
    dc.onmessage = onMessage;
};

// --- Cleanup & state reset ---
const cleanup = () => {
    log.info('NET', 'Cleanup');
    abortPendingConnect('cleanup');
    clearPing();
    clearFirstFrameWatchdog();
    clearPendingKeyReq();
    stopMetricsLogger();
    resetRenderer();
    stopKeyframeRetry();
    resetClockSync();
    closeDataChannels();
    audioFecGroups.clear();
    seenAudioPacketIds.clear();
    pendingVideoFec.clear();
    channelsReady = 0;
    stopMic();
};

const resetState = () => {
    cleanup();
    if (S.decoder && S.decoder.state !== 'closed') safe(() => S.decoder.close(), undefined, 'MEDIA');
    DC_KEYS.forEach(k => S[k] = null);
    S.pc = S.decoder = null;
    S.ready = S.fpsSent = S.codecSent = S.softwareEncodeSent = waitingFirstFrame = 0;
    S.hostEncoderName = null;
    hasConnection = false;
    lastFrameCompletedAt = 0;
    resetProtocolState();
    S.chunks.clear();
    S.lastFrameId = 0;
    S.frameMeta.clear();
    lastChunkCleanupAt = 0;
    lastAudioFecCleanupAt = 0;
    log.debug('NET', 'State reset');
};

// --- Connection ---
const connect = async (serverBusyRetry = 0) => {
    if (!S.authenticated) { showAuth(); return; }
    log.info('NET', 'Connecting');
    updateLoadingStage('Connecting...', 'Establishing');
    resetState();

    const connectSeq = ++connectSeqCounter;
    activeConnectSeq = connectSeq;
    abortPendingConnect('superseded');
    const attemptAbort = new AbortController();
    activeConnectAbort = attemptAbort;

    const ensureActive = () => {
        if (connectSeq !== activeConnectSeq || attemptAbort.signal.aborted) {
            const err = new Error('Connection attempt superseded');
            err.name = 'AbortError';
            throw err;
        }
    };

    log.info('NET', 'Connection attempt started', { seq: connectSeq, retry: connectionAttempts });

    const pc = S.pc = new RTCPeerConnection({
        iceServers: [
            { urls: 'stun:stun.l.google.com:19302' },
            { urls: 'stun:stun1.l.google.com:19302' },
            { urls: 'stun:stun2.l.google.com:19302' },
            { urls: 'stun:stun3.l.google.com:19302' },
            { urls: 'stun:stun4.l.google.com:19302' },
            { urls: 'stun:stun.cloudflare.com:3478' },
            { urls: 'stun:stun.services.mozilla.com:3478' }
        ],
        iceCandidatePoolSize: 4,
        bundlePolicy: 'max-bundle',
        rtcpMuxPolicy: 'require'
    });

    pc.onconnectionstatechange = () => {
        log.info('NET', 'Connection state', { seq: connectSeq, activeSeq: activeConnectSeq, state: pc.connectionState, ice: pc.iceConnectionState, signaling: pc.signalingState, channelsReady });
        if (connectSeq !== activeConnectSeq) { log.warn('NET', 'Stale connection state callback', { seq: connectSeq, activeSeq: activeConnectSeq, state: pc.connectionState }); return; }
        if (pc.connectionState === 'connected') connectionAttempts = 0;
        if (pc.connectionState === 'failed' || pc.connectionState === 'disconnected') {
            logNetworkDrop('Connection failed', { state: pc.connectionState });
            if ($('authOverlay').classList.contains('visible')) return;
            closeTabbedMode();
            if (++connectionAttempts >= 3) {
                clearSession();
                showAuth('Connection failed. Please login again.');
            } else {
                showLoading(true);
                updateLoadingStage('Reconnecting...');
                const delay = Math.min(1000 * connectionAttempts, 5000);
                setTimeout(() => startConnection().catch(e => log.error('NET', 'Reconnect failed', { error: e.message })), delay);
            }
        }
    };

    pc.oniceconnectionstatechange = () => log.info('NET', 'ICE connection state', { seq: connectSeq, activeSeq: activeConnectSeq, state: pc.iceConnectionState });
    pc.onicegatheringstatechange = () => log.debug('NET', 'ICE gathering state', { seq: connectSeq, activeSeq: activeConnectSeq, state: pc.iceGatheringState });
    pc.onsignalingstatechange = () => log.info('NET', 'Signaling state', { seq: connectSeq, activeSeq: activeConnectSeq, state: pc.signalingState });

    DC_CONFIG.forEach(([key, name, config, handler]) => {
        S[key] = pc.createDataChannel(name, config);
        setupDataChannel(S[key], handler, connectSeq);
    });

    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    await new Promise(resolve => {
        const done = () => { clearTimeout(timeout); attemptAbort.signal.removeEventListener('abort', done); resolve(); };
        const timeout = setTimeout(done, 300);
        pc.addEventListener('icegatheringstatechange', () => { if (pc.iceGatheringState === 'complete') done(); });
        attemptAbort.signal.addEventListener('abort', done, { once: true });
    });
    ensureActive();

    updateLoadingStage('Connecting...', 'Sending offer...');
    log.debug('NET', 'Sending offer', { seq: connectSeq });

    const offerTimeout = setTimeout(() => { try { attemptAbort.abort('offer-timeout'); } catch (e) { log.debug('NET', 'Offer timeout abort failed', { error: e?.message }); } }, 12000);
    let res;
    try {
        res = await fetch(`${BASE_URL}/api/offer`, {
            method: 'POST', headers: { 'Content-Type': 'application/json' },
            credentials: 'include', signal: attemptAbort.signal,
            body: JSON.stringify({ sdp: pc.localDescription.sdp, type: pc.localDescription.type })
        });
    } finally { clearTimeout(offerTimeout); }
    ensureActive();

    if (!res.ok) {
        let data = {};
        try { data = await res.json(); } catch (e) { log.debug('NET', 'Failed to parse error response body', { status: res.status, error: e?.message }); }
        if (res.status === 401) { clearSession(); showAuth('Session expired.'); return; }
        if (res.status === 503) {
            if (serverBusyRetry >= MAX_SERVER_BUSY_RETRIES) {
                throw new Error('Server is busy processing another offer. Please wait a moment and retry.');
            }
            const delayMs = Math.min(1000 * (serverBusyRetry + 1), 5000);
            log.warn('NET', 'Server busy (503), retrying', { seq: connectSeq, attempt: connectionAttempts, busyRetry: serverBusyRetry + 1, delayMs });
            ensureActive();
            await new Promise(r => setTimeout(r, delayMs));
            ensureActive();
            return connect(serverBusyRetry + 1);
        }
        throw new Error(data.error || 'Server rejected offer');
    }

    const answer = await res.json();
    ensureActive();
    await pc.setRemoteDescription(new RTCSessionDescription(answer));
    if (activeConnectAbort === attemptAbort) activeConnectAbort = null;
    log.info('NET', 'Connection established', { seq: connectSeq });
};

// --- Bus event handlers ---
bus.on('auth:connect', () => connect());
bus.on('user:disconnect', () => { cleanup(); hasConnection = false; });

// --- Bootstrap ---
const startConnection = async () => {
    updateLoadingStage('Authenticating...', 'Validating...');
    if (await validateSession()) { updateLoadingStage('Connecting...'); await connect(); return; }
    showAuth();
};

(async () => {
    log.info('NET', 'Initializing');
    const codecResult = await initCodecDetection();
    S.currentCodec = codecResult.codecId;
    log.info('NET', 'Codec detection', { best: codecResult.codecName });
    await updateCodecOpts();
    enableControl();
    showLoading(false);
    try { await startConnection(); }
    catch (e) { log.error('NET', 'Connection failed', { error: e.message }); showAuth('Connection failed: ' + e.message); }
})();

window.onbeforeunload = () => { log.info('NET', 'Page unload, cleanup'); cleanup(); closeAudio(); };
