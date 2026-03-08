
import { MSG, C } from './constants.js';
import { S, mkBuf, safe, log, logNetworkDrop, clientTimeUs, bus } from './state.js';

// --- Message helpers ---
const sendControl = (buf, options = {}) => {
    if (S.dcControl?.readyState !== 'open') {
        if (!options.suppressIfClosed) logNetworkDrop('Control channel not open');
        return false;
    }
    return safe(() => { S.dcControl.send(buf); return true; }, false, 'NET');
};
const mkCtrlMsg = (type, size, fn, options) => sendControl(mkBuf(size, v => { v.setUint32(0, type, true); fn?.(v); }), options);
const sendByteControl = (type, value, options) => mkCtrlMsg(type, 5, v => v.setUint8(4, value), options);
const sendBoolControl = (type, enabled, options) => sendByteControl(type, enabled ? 1 : 0, options);

export const sendAudioEnable = (en, options) => sendBoolControl(MSG.AUDIO_ENABLE, en, options);
export const sendMicEnable = (en, options) => sendBoolControl(MSG.MIC_ENABLE, en, options);
export const sendMonitor = idx => sendByteControl(MSG.MONITOR_SET, idx);
export const sendCursorCapture = (en, options) => sendBoolControl(MSG.CURSOR_CAPTURE, en, options);
const sendCodec = id => sendByteControl(MSG.CODEC_SET, id);
const sendFps = (fps, mode) => mkCtrlMsg(MSG.FPS_SET, 7, v => { v.setUint16(4, fps, true); v.setUint8(6, mode); });
const sendStreamTarget = (width, height, options) => mkCtrlMsg(MSG.STREAM_TARGET, 8, v => {
    v.setUint16(4, width, true);
    v.setUint16(6, height, true);
}, options);

export const sendPing = () => {
    if (S.dcControl?.readyState !== 'open') return;
    S.dcControl.send(mkBuf(16, v => {
        v.setUint32(0, MSG.PING, true);
        v.setBigUint64(8, BigInt(clientTimeUs()), true);
    }));
};

// --- Keyframe request with throttling ---
let lastKeyReqAt = 0, pendingKeyReqTimer = null, lastRecoveryKeyReqAt = 0;

export const requestKeyframe = (reason = 'unspecified') => {
    if (S.dcControl?.readyState !== 'open') return false;
    const now = performance.now();
    const minInterval = C.KEY_REQ_MIN_INTERVAL_MS;
    const waitMs = Math.max(0, minInterval - (now - lastKeyReqAt));
    if (waitMs > 0) {
        if (!pendingKeyReqTimer)
            pendingKeyReqTimer = setTimeout(() => { pendingKeyReqTimer = null; requestKeyframe('coalesced'); }, waitMs);
        return false;
    }
    if (pendingKeyReqTimer) { clearTimeout(pendingKeyReqTimer); pendingKeyReqTimer = null; }
    const sent = mkCtrlMsg(MSG.REQUEST_KEY, 4);
    if (sent) { lastKeyReqAt = performance.now(); log.debug('NET', 'Requested keyframe', { reason }); }
    return sent;
};

export const requestRecoveryKeyframe = reason => {
    const now = performance.now();
    if (now - lastRecoveryKeyReqAt < 1500) return false;
    lastRecoveryKeyReqAt = now;
    S.needKey = 1;
    return requestKeyframe(reason);
};

export const clearPendingKeyReq = () => {
    if (pendingKeyReqTimer) { clearTimeout(pendingKeyReqTimer); pendingKeyReqTimer = null; }
};

// --- Clipboard ---
const clipboardEncoder = new TextEncoder();
export const requestClipboard = () => S.dcControl?.readyState === 'open' && mkCtrlMsg(MSG.CLIPBOARD_GET, 4);

export const pushClipboardToHost = async () => {
    if (S.dcControl?.readyState !== 'open') { logNetworkDrop('Clipboard push: channel not open'); return false; }
    if (!navigator.clipboard?.readText) { logNetworkDrop('Clipboard read unavailable'); return false; }
    const text = await navigator.clipboard.readText();
    if (!text) { log.debug('NET', 'Clipboard push: empty'); return false; }
    const encoded = clipboardEncoder.encode(text);
    if (encoded.byteLength > 1048576) { logNetworkDrop('Clipboard push: too large', { len: encoded.byteLength }); return false; }
    const sent = mkCtrlMsg(MSG.CLIPBOARD_DATA, 8 + encoded.byteLength, v => {
        v.setUint32(4, encoded.byteLength, true);
        new Uint8Array(v.buffer).set(encoded, 8);
    });
    if (sent) log.debug('NET', 'Clipboard pushed', { len: encoded.byteLength });
    return sent;
};

// --- Apply settings ---
export const applyCodec = id => {
    if (sendCodec(id)) { S.currentCodec = id; S.codecSent = 1; log.info('NET', 'Codec set', { id }); }
};

export const applyFps = val => {
    const fps = +val;
    const mode = fps === S.hostFps ? 1 : 0;
    if (sendFps(fps, mode)) {
        S.currentFps = fps; S.currentFpsMode = mode; S.fpsSent = 1;
        bus.emit('fps:applied', fps);
        log.info('NET', 'FPS set', { fps, mode });
    }
};

let pendingStreamTargetTimer = null;
let pendingStreamTarget = { width: 0, height: 0 };
let sentStreamTarget = { width: 0, height: 0 };

const clearPendingStreamTarget = () => {
    if (pendingStreamTargetTimer) {
        clearTimeout(pendingStreamTargetTimer);
        pendingStreamTargetTimer = null;
    }
};

const flushStreamTarget = () => {
    clearPendingStreamTarget();
    const width = pendingStreamTarget.width | 0;
    const height = pendingStreamTarget.height | 0;
    if (width <= 0 || height <= 0) return false;
    if (width === sentStreamTarget.width && height === sentStreamTarget.height) return false;
    const sent = sendStreamTarget(width, height, { suppressIfClosed: true });
    if (sent) {
        sentStreamTarget = { width, height };
        log.debug('NET', 'Stream target sent', { width, height });
    }
    return sent;
};

export const updateStreamTarget = (width, height, immediate = false) => {
    const nextWidth = Math.max(0, width | 0);
    const nextHeight = Math.max(0, height | 0);
    if (nextWidth <= 0 || nextHeight <= 0) return;
    pendingStreamTarget = { width: nextWidth, height: nextHeight };
    if (immediate) {
        flushStreamTarget();
        return;
    }
    clearPendingStreamTarget();
    pendingStreamTargetTimer = setTimeout(flushStreamTarget, 120);
};

export const resendStreamTarget = () => {
    sentStreamTarget = { width: 0, height: 0 };
    flushStreamTarget();
};

// --- Adaptive quality ---
let lastAdaptiveQualityAt = 0, codecFallbackApplied = false;

const getFallbackCodec = () => {
    const caps = S.hostCodecs || 0;
    if (S.currentCodec === 0) { if (caps & 2) return 1; if (caps & 4) return 2; }
    if (S.currentCodec === 1 && (caps & 4)) return 2;
    return null;
};

export const handleDecodePressure = ({ queueSize } = {}) => {
    if (S.dcControl?.readyState !== 'open') return;
    const now = performance.now();
    if (now - lastAdaptiveQualityAt < 6000) return;
    if (S.currentFps > 30) {
        const target = S.currentFps > 45 ? 30 : 24;
        log.warn('NET', 'Adaptive: lowering FPS', { from: S.currentFps, to: target, queueSize });
        applyFps(target);
        lastAdaptiveQualityAt = now;
        return;
    }
    if (!codecFallbackApplied) {
        const fb = getFallbackCodec();
        if (fb !== null && fb !== S.currentCodec) {
            log.warn('NET', 'Adaptive: codec fallback', { from: S.currentCodec, to: fb, queueSize });
            applyCodec(fb);
            codecFallbackApplied = true;
            lastAdaptiveQualityAt = now;
        }
    }
};

// --- Video helpers ---
export const getMaxInFlightFrames = () => {
    const fps = Math.max(24, S.currentFps || S.hostFps || 60);
    return Math.max(C.MAX_FRAMES, Math.ceil((fps * C.FRAME_TIMEOUT_MS) / 1000) + 12);
};

export const expectedChunkSize = (frame, idx) => {
    if (idx < 0 || idx >= frame.total) return 0;
    if (idx < frame.total - 1) return frame.dataChunkSize;
    const remaining = frame.frameSize - frame.dataChunkSize * Math.max(0, frame.total - 1);
    return remaining > 0 ? remaining : frame.dataChunkSize;
};

export const tryRecoverFrameGroup = (frameId, frame, groupIndex) => {
    const fec = frame.fecParts.get(groupIndex);
    if (!fec) return false;
    const gs = Math.max(1, frame.fecGroupSize || C.FEC_GROUP_SIZE);
    const start = groupIndex * gs;
    if (start >= frame.total) return false;
    const end = Math.min(start + gs, frame.total);
    const missing = [];
    for (let i = start; i < end; i++) if (!frame.parts[i]) missing.push(i);
    if (missing.length !== 1) return false;
    const mi = missing[0], sz = expectedChunkSize(frame, mi);
    if (sz <= 0 || sz > fec.byteLength) return false;
    const rec = new Uint8Array(sz);
    rec.set(fec.subarray(0, sz));
    for (let i = start; i < end; i++) {
        if (i === mi) continue;
        if (!frame.parts[i]) return false;
        const len = Math.min(frame.parts[i].byteLength, sz);
        for (let j = 0; j < len; j++) rec[j] ^= frame.parts[i][j];
    }
    frame.parts[mi] = rec;
    frame.partSizes[mi] = sz;
    frame.received++;
    frame.fecRecovered = (frame.fecRecovered || 0) + 1;
    log.debug('VIDEO', 'FEC recovered', { frameId, groupIndex, chunk: mi });
    return true;
};

// --- Reset protocol state ---
export const resetProtocolState = () => {
    lastKeyReqAt = lastRecoveryKeyReqAt = lastAdaptiveQualityAt = 0;
    codecFallbackApplied = false;
    clearPendingKeyReq();
    clearPendingStreamTarget();
    sentStreamTarget = { width: 0, height: 0 };
};
