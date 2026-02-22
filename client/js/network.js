
import { enableControl, setClipboardRequestFn, setClipboardPushFn, setCursorCaptureFn } from './input.js';
import { MSG, C, S, $, mkBuf, safe, safeAsync, updateClockOffset, resetClockSync,
    startMetricsLogger, stopMetricsLogger, resetSessionStats, recordPacket,
    clientTimeUs, logVideoDrop, logAudioDrop, logNetworkDrop, log } from './state.js';
import { handleAudioPacket, closeAudio, initDecoder, decodeFrame, setReqKeyFn,
    setSendAudioEnableFn, stopKeyframeRetryTimer } from './media.js';
import { updateMonOpts, updateCodecOpts, updateCodecDropdown, setHostCodecs, setNetCbs,
    updateLoadingStage, showLoading, hideLoading, getStoredCodec, initCodecDetection,
    getStoredFps, updateFpsDropdown, closeTabbedMode } from './ui.js';
import { resetRenderer, setCursorStyle } from './renderer.js';
import { setDcMic, setMicEnableCallback, closeMic } from './mic.js';

const BASE_URL = location.origin;
let hasConnection = false;
let waitingFirstFrame = false;
let connectionAttempts = 0;
let pingInterval = null;
let channelsReady = 0;
let connectSeqCounter = 0;
let activeConnectSeq = 0;
let firstFrameWatchdog = null;
let lastKeyReqAt = 0;
let pendingKeyReqTimer = null;
const sendControl = buf => {
    if (S.dcControl?.readyState !== 'open') {
        logNetworkDrop('Control channel not open');
        return false;
    }
    return safe(() => { S.dcControl.send(buf); return true; }, false, 'NET');
};
const mkCtrlMsg = (type, size, fn) => sendControl(mkBuf(size, v => { v.setUint32(0, type, true); fn?.(v); }));
const mkByte5Msg = (type, val) => mkCtrlMsg(type, 5, v => v.setUint8(4, val));
const sendAudioEnable = en => mkByte5Msg(MSG.AUDIO_ENABLE, en ? 1 : 0);
const sendMicEnable = en => mkByte5Msg(MSG.MIC_ENABLE, en ? 1 : 0);
const sendMonitor = idx => mkByte5Msg(MSG.MONITOR_SET, idx);
const sendCodec = id => mkByte5Msg(MSG.CODEC_SET, id);
const sendCursorCapture = en => mkByte5Msg(MSG.CURSOR_CAPTURE, en ? 1 : 0);
const sendFps = (fps, mode) => mkCtrlMsg(MSG.FPS_SET, 7, v => { v.setUint16(4, fps, true); v.setUint8(6, mode); });
const requestKeyframe = (reason = 'unspecified') => {
    if (S.dcControl?.readyState !== 'open') return false;

    const now = performance.now();
    const minInterval = C.KEY_REQ_MIN_INTERVAL_MS || 180;
    const waitMs = Math.max(0, minInterval - (now - lastKeyReqAt));

    if (waitMs > 0) {
        if (!pendingKeyReqTimer) {
            pendingKeyReqTimer = setTimeout(() => {
                pendingKeyReqTimer = null;
                requestKeyframe('coalesced');
            }, waitMs);
        }
        return false;
    }

    if (pendingKeyReqTimer) {
        clearTimeout(pendingKeyReqTimer);
        pendingKeyReqTimer = null;
    }

    const sent = mkCtrlMsg(MSG.REQUEST_KEY, 4);
    if (sent) {
        lastKeyReqAt = performance.now();
        log.debug('NET', 'Requested keyframe', { reason });
    }
    return sent;
};
const requestClipboard = () => S.dcControl?.readyState === 'open' && mkCtrlMsg(MSG.CLIPBOARD_GET, 4);
const clipboardEncoder = new TextEncoder();
const pushClipboardToHost = async () => {
    if (S.dcControl?.readyState !== 'open') {
        logNetworkDrop('Clipboard push skipped: control channel not open');
        return false;
    }
    if (!navigator.clipboard?.readText) {
        logNetworkDrop('Clipboard read unavailable in browser');
        return false;
    }

    const text = await navigator.clipboard.readText();
    if (!text) {
        log.debug('NET', 'Clipboard push skipped: empty clipboard');
        return false;
    }

    const encoded = clipboardEncoder.encode(text);
    if (encoded.byteLength > 1048576) {
        logNetworkDrop('Clipboard push skipped: payload too large', { len: encoded.byteLength });
        return false;
    }

    const sent = mkCtrlMsg(MSG.CLIPBOARD_DATA, 8 + encoded.byteLength, v => {
        v.setUint32(4, encoded.byteLength, true);
        new Uint8Array(v.buffer).set(encoded, 8);
    });

    if (sent) {
        log.debug('NET', 'Clipboard pushed to host', { len: encoded.byteLength });
    } else {
        logNetworkDrop('Clipboard push send failed');
    }

    return sent;
};

const sendPing = () => {
    if (S.dcControl?.readyState !== 'open') return;
    S.dcControl.send(mkBuf(16, v => {
        v.setUint32(0, MSG.PING, true);
        v.setBigUint64(8, BigInt(clientTimeUs()), true);
    }));
};
setClipboardRequestFn(requestClipboard);
setClipboardPushFn(pushClipboardToHost);
setReqKeyFn(requestKeyframe);
const authElements = {
    overlay: $('authOverlay'),
    username: $('usernameInput'),
    password: $('passwordInput'),
    error: $('authError'),
    submit: $('authSubmit')
};
const validateUsername = u => u?.length >= 3 && u.length <= 32 && /^[a-zA-Z0-9_-]+$/.test(u);
const validatePassword = p => p?.length >= 8;
const clearSession = () => {
    S.username = null;
    S.authenticated = 0;
    log.info('AUTH', 'Session cleared');
};

const clearPing = () => {
    if (pingInterval) {
        clearInterval(pingInterval);
        pingInterval = null;
    }
};

const clearFirstFrameWatchdog = () => {
    if (firstFrameWatchdog) {
        clearTimeout(firstFrameWatchdog);
        firstFrameWatchdog = null;
    }
};

const armFirstFrameWatchdog = () => {
    clearFirstFrameWatchdog();
    const seq = activeConnectSeq;
    firstFrameWatchdog = setTimeout(() => {
        if (!waitingFirstFrame || seq !== activeConnectSeq) return;
        log.warn('NET', 'Still waiting for first frame', {
            seq,
            pcState: S.pc?.connectionState,
            iceState: S.pc?.iceConnectionState,
            control: S.dcControl?.readyState,
            video: S.dcVideo?.readyState,
            audio: S.dcAudio?.readyState,
            input: S.dcInput?.readyState,
            mic: S.dcMic?.readyState,
            channelsReady
        });
    }, 3000);
};

const setAuthError = (error, focusEl) => {
    authElements.error.textContent = error;
    [authElements.username, authElements.password].forEach(e =>
        e.classList.toggle('error', e === focusEl)
    );
    if (focusEl) focusEl.focus();
};

const showAuth = (error = '') => {
    authElements.username.value = authElements.password.value = '';
    setAuthError(error, error ? authElements.username : null);
    authElements.overlay.classList.add('visible');
    authElements.submit.disabled = false;
    hideLoading();
    setTimeout(() => authElements.username.focus(), 100);
    log.info('AUTH', 'Auth dialog shown', { hasError: !!error });
};

const hideAuth = () => {
    authElements.overlay.classList.remove('visible');
    authElements.password.value = '';
    setAuthError('', null);
    log.debug('AUTH', 'Auth dialog hidden');
};
const authHTTP = async (username, password) => {
    log.info('AUTH', 'Authenticating', { username });

    const res = await fetch(`${BASE_URL}/api/auth`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        credentials: 'include',
        body: JSON.stringify({ username, password })
    });

    const data = await res.json();

    if (!res.ok) {
        const error = res.status === 429
            ? `Too many attempts. Try in ${Math.ceil(data.lockoutSeconds / 60)} min.`
            : data.error || 'Authentication failed';
        log.warn('AUTH', 'Auth failed', { status: res.status, error });
        throw new Error(error);
    }

    S.username = username;
    S.authenticated = 1;
    log.info('AUTH', 'Authenticated', { username });
    return true;
};
const validateSession = async () => {
    const ok = await safeAsync(async () => {
        const res = await fetch(`${BASE_URL}/api/session`, { credentials: 'include' });
        if (res.ok) {
            const data = await res.json();
            if (data.valid) {
                S.authenticated = 1;
                S.username = data.username;
                log.info('AUTH', 'Session valid', { username: data.username });
                return true;
            }
        }
        return false;
    }, false, 'AUTH');

    if (!ok) clearSession();
    return ok;
};
authElements.username.addEventListener('input', e => {
    e.target.value = e.target.value.replace(/[^a-zA-Z0-9_-]/g, '').slice(0, 32);
    setAuthError('', null);
});

authElements.username.addEventListener('keydown', e => {
    if (e.key === 'Enter') { e.preventDefault(); authElements.password.focus(); }
});

authElements.password.addEventListener('keydown', e => {
    if (e.key === 'Enter' && validateUsername(authElements.username.value) && authElements.password.value.length >= 8) {
        authElements.submit.click();
    }
});

authElements.submit.addEventListener('click', async () => {
    const username = authElements.username.value;
    let password = authElements.password.value;
    authElements.password.value = '';

    if (!validateUsername(username)) {
        setAuthError('Username must be 3-32 characters', authElements.username);
        return;
    }
    if (!validatePassword(password)) {
        setAuthError('Password must be at least 8 characters', authElements.password);
        return;
    }

    authElements.submit.disabled = true;
    authElements.error.textContent = 'Authenticating...';

    try {
        await authHTTP(username, password);
        hideAuth();
        showLoading(false);
        updateLoadingStage('Connecting...');
        await connect();
    } catch (e) {
        setAuthError(e.message, authElements.username);
        authElements.submit.disabled = false;
    } finally {
        password = '';
    }
});
$('disconnectBtn')?.addEventListener('click', async () => {
    log.info('NET', 'User disconnect');
    cleanup();
    safe(() => fetch(`${BASE_URL}/api/logout`, { method: 'POST', credentials: 'include' }), undefined, 'AUTH');
    clearSession();
    hasConnection = false;
    showAuth();
});
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
        const name = new TextDecoder().decode(new Uint8Array(data, offset, nameLen));
        offset += nameLen;

        S.monitors.push({ index, width, height, refreshRate, isPrimary, name });
    }

    log.info('NET', 'Monitor list received', { count, current: S.currentMon });
    updateMonOpts();
};
const applyCodec = id => {
    if (sendCodec(id)) {
        S.currentCodec = id;
        S.codecSent = 1;
        log.info('NET', 'Codec set', { id });
    }
};

const applyFps = val => {
    const fps = +val;
    const mode = fps === S.hostFps ? 1 : 0;
    if (sendFps(fps, mode)) {
        S.currentFps = fps;
        S.currentFpsMode = mode;
        S.fpsSent = 1;
        updateFpsDropdown(fps);
        log.info('NET', 'FPS set', { fps, mode });
    }
};
const VIDEO_PKT_DATA = 0;
const VIDEO_PKT_FEC = 1;

const expectedChunkSize = (frame, chunkIndex) => {
    if (chunkIndex < 0 || chunkIndex >= frame.total) return 0;
    if (chunkIndex < frame.total - 1) return frame.dataChunkSize;
    const used = frame.dataChunkSize * Math.max(0, frame.total - 1);
    const remaining = frame.frameSize - used;
    return remaining > 0 ? remaining : frame.dataChunkSize;
};

const tryRecoverFrameGroup = (frameId, frame, groupIndex) => {
    const fec = frame.fecParts.get(groupIndex);
    if (!fec) return false;

    const groupSize = Math.max(1, frame.fecGroupSize || C.FEC_GROUP_SIZE || 4);
    const start = groupIndex * groupSize;
    if (start >= frame.total) return false;
    const end = Math.min(start + groupSize, frame.total);

    const missing = [];
    for (let i = start; i < end; i++) {
        if (!frame.parts[i]) missing.push(i);
    }
    if (missing.length !== 1) return false;

    const missingIndex = missing[0];
    const recoveredSize = expectedChunkSize(frame, missingIndex);
    if (recoveredSize <= 0 || recoveredSize > fec.byteLength) return false;

    const recovered = new Uint8Array(recoveredSize);
    recovered.set(fec.subarray(0, recoveredSize));

    for (let i = start; i < end; i++) {
        if (i === missingIndex) continue;
        const part = frame.parts[i];
        if (!part) return false;
        const partLen = Math.min(part.byteLength, recoveredSize);
        for (let j = 0; j < partLen; j++) recovered[j] ^= part[j];
    }

    frame.parts[missingIndex] = recovered;
    frame.partSizes[missingIndex] = recoveredSize;
    frame.received++;
    frame.fecRecovered = (frame.fecRecovered || 0) + 1;

    log.debug('VIDEO', 'Recovered chunk from FEC', { frameId, groupIndex, chunkIndex: missingIndex });
    return true;
};

const processFrame = (frameId, frame) => {
    if (!frame.parts.every(p => p)) {
        logVideoDrop('Incomplete frame', { frameId, received: frame.received, total: frame.total });
        S.stats.framesDropped++;
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

    S.stats.framesComplete++;
    if (frame.isKey) S.stats.keyframesReceived++;
    if (frameId > S.lastFrameId) S.lastFrameId = frameId;

    decodeFrame({
        buf: buffer,
        capTs: frame.capTs,
        encMs: frame.encMs,
        isKey: frame.isKey,
        arrivalMs: frame.arrivalMs
    });

    if (waitingFirstFrame) {
        waitingFirstFrame = false;
        clearFirstFrameWatchdog();
        hideLoading();
        hasConnection = true;
        log.info('NET', 'First frame received');
    }

    S.chunks.delete(frameId);
};
const handleControl = async e => {
    const arrivalUs = clientTimeUs();

    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < 4) {
        logNetworkDrop('Invalid control message');
        return;
    }

    const view = new DataView(e.data);
    const msgType = view.getUint32(0, true);
    const length = e.data.byteLength;
    if (msgType === MSG.PING && length === 24) {
        updateClockOffset(
            Number(view.getBigUint64(8, true)),
            Number(view.getBigUint64(16, true)),
            arrivalUs
        );
        return recordPacket(length, 'control');
    }
    if (msgType === MSG.HOST_INFO && length === 6) {
        S.hostFps = view.getUint16(4, true);
        if (!S.fpsSent) setTimeout(() => applyFps(getStoredFps() ?? 60), 50);
        if (!hasConnection) {
            updateLoadingStage('Connected');
            waitingFirstFrame = true;
            armFirstFrameWatchdog();
            log.info('NET', 'Waiting for first frame', { seq: activeConnectSeq });
        }
        log.info('NET', 'Host info', { fps: S.hostFps });
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
        if (count >= 1 && count <= 16) {
            recordPacket(length, 'control');
            return parseMonitorList(e.data);
        }
    }
    if (msgType === MSG.CLIPBOARD_DATA && length >= 8) {
        const textLen = view.getUint32(4, true);
        if (textLen > 0 && length >= 8 + textLen && textLen <= 1048576) {
            const text = new TextDecoder().decode(new Uint8Array(e.data, 8, textLen));
            try {
                await navigator.clipboard.writeText(text);
                log.debug('NET', 'Clipboard set', { len: textLen });
            } catch (err) {
                logNetworkDrop('Clipboard write failed', { error: err?.message });
            }
        }
        return recordPacket(length, 'control');
    }
    if (msgType === MSG.CURSOR_SHAPE && length === 5) {
        setCursorStyle(view.getUint8(4));
        return recordPacket(length, 'control');
    }
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
            S.hostVersion = new TextDecoder().decode(new Uint8Array(e.data, 5, versionLen));
            log.info('NET', 'Host version received', { version: S.hostVersion });
        }
        return recordPacket(length, 'control');
    }
};
const handleVideo = e => {
    const arrivalMs = performance.now();

    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < C.HEADER) {
        logVideoDrop('Invalid video packet');
        return;
    }

    const view = new DataView(e.data);
    const length = e.data.byteLength;

    const captureTs = Number(view.getBigUint64(0, true));
    const frameId = view.getUint32(12, true);
    const frameSize = view.getUint32(16, true);
    const chunkIndex = view.getUint16(20, true);
    const totalChunks = view.getUint16(22, true);
    const chunkBytes = view.getUint16(24, true);
    const dataChunkSize = view.getUint16(26, true);
    const frameType = view.getUint8(28);
    const packetType = view.getUint8(29);
    const fecGroupSize = view.getUint8(30) || C.FEC_GROUP_SIZE;

    if (totalChunks === 0 || captureTs <= 0 || frameSize === 0 || dataChunkSize === 0) {
        logVideoDrop('Invalid packet data');
        return;
    }
    if (packetType === VIDEO_PKT_DATA && chunkIndex >= totalChunks) {
        logVideoDrop('Invalid data chunk index', { frameId, chunkIndex, totalChunks });
        return;
    }
    if (packetType !== VIDEO_PKT_DATA && packetType !== VIDEO_PKT_FEC) {
        logVideoDrop('Unknown video packet type', { packetType, frameId });
        return;
    }
    if (length < C.HEADER || chunkBytes !== length - C.HEADER) {
        logVideoDrop('Video size mismatch', { frameId, chunkBytes, actual: length - C.HEADER });
        return;
    }
    const chunkData = new Uint8Array(e.data, C.HEADER, chunkBytes);

    recordPacket(length, 'video');
    S.stats.bytes += length;
    if (S.lastFrameId > 0 && frameId <= S.lastFrameId) {
        return;
    }
    for (const [id, frame] of S.chunks) {
        if (arrivalMs - frame.arrivalMs > C.FRAME_TIMEOUT_MS && frame.received < frame.total) {
            if (S.lastFrameId > 0 && id < S.lastFrameId) {
                S.chunks.delete(id);
                continue;
            }

            if (frame.fecParts.size > 0) {
                for (const groupIndex of frame.fecParts.keys()) {
                    tryRecoverFrameGroup(id, frame, groupIndex);
                }
                if (frame.received === frame.total) {
                    processFrame(id, frame);
                    continue;
                }
            }

            logVideoDrop('Frame timeout', { frameId: id });
            S.chunks.delete(id);
            S.stats.framesTimeout++;
            if (frame.isKey && frame.received > 0 && !S.needKey) {
                S.needKey = 1;
                requestKeyframe('timed-out-keyframe-unrecoverable');
            }
        }
    }
    if (!S.chunks.has(frameId)) {
        S.chunks.set(frameId, {
            parts: Array(totalChunks).fill(null),
            partSizes: Array(totalChunks).fill(0),
            total: totalChunks,
            received: 0,
            capTs: captureTs,
            encMs: view.getUint32(8, true) / 1000,
            arrivalMs,
            isKey: frameType === 1,
            frameSize,
            dataChunkSize,
            fecGroupSize: Math.max(1, fecGroupSize),
            fecParts: new Map(),
            fecRecovered: 0
        });
        if (S.chunks.size > C.MAX_FRAMES) {
            const oldest = [...S.chunks.keys()].sort((a, b) => a - b)[0];
            if (oldest !== frameId) {
                logVideoDrop('Max frames exceeded');
                const dropped = S.chunks.get(oldest);
                S.chunks.delete(oldest);
                if (dropped?.isKey && !S.needKey) {
                    S.needKey = 1;
                    requestKeyframe('evicted-keyframe-unrecoverable');
                }
            }
        }
    }

    const frame = S.chunks.get(frameId);
    if (!frame) return;
    if (frame.total !== totalChunks || frame.frameSize !== frameSize || frame.dataChunkSize !== dataChunkSize) {
        logVideoDrop('Frame metadata mismatch', { frameId });
        return;
    }

    if (packetType === VIDEO_PKT_DATA) {
        if (frame.parts[chunkIndex]) {
            logNetworkDrop('Duplicate data chunk', { frameId, chunkIndex });
            return;
        }

        frame.parts[chunkIndex] = chunkData;
        frame.partSizes[chunkIndex] = chunkBytes;
        frame.received++;

        const groupIndex = Math.floor(chunkIndex / frame.fecGroupSize);
        if (frame.fecParts.has(groupIndex)) {
            tryRecoverFrameGroup(frameId, frame, groupIndex);
        }
    } else {
        if (frame.fecParts.has(chunkIndex)) {
            logNetworkDrop('Duplicate FEC chunk', { frameId, groupIndex: chunkIndex });
            return;
        }
        frame.fecParts.set(chunkIndex, chunkData);
        tryRecoverFrameGroup(frameId, frame, chunkIndex);
    }

    if (frame.received === frame.total) {
        processFrame(frameId, frame);
    }
};
const handleAudio = e => {
    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < C.AUDIO_HEADER) {
        logAudioDrop('Invalid audio packet');
        return;
    }

    const view = new DataView(e.data);
    const length = e.data.byteLength;
    const msgType = view.getUint32(0, true);

    if (msgType === MSG.AUDIO_DATA && length >= C.AUDIO_HEADER) {
        const dataLen = view.getUint16(14, true);
        if (length === C.AUDIO_HEADER + dataLen && dataLen > 0) {
            recordPacket(length, 'audio');
            return handleAudioPacket(e.data);
        }
        logAudioDrop('Size mismatch', { expected: C.AUDIO_HEADER + dataLen, got: length });
    } else {
        logAudioDrop('Unknown audio message', { type: msgType });
    }
};
const onAllChannelsOpen = async connectSeq => {
    log.info('NET', 'All channels open', {
        seq: connectSeq,
        control: S.dcControl?.readyState,
        video: S.dcVideo?.readyState,
        audio: S.dcAudio?.readyState,
        input: S.dcInput?.readyState,
        mic: S.dcMic?.readyState
    });

    S.fpsSent = S.codecSent = 0;
    S.authenticated = 1;

    resetClockSync();
    resetSessionStats();
    updateLoadingStage('Connected');

    await initDecoder();
    clearPing();
    startMetricsLogger();

    pingInterval = setInterval(sendPing, C.PING_MS);

    setDcMic(S.dcMic);
    setMicEnableCallback(sendMicEnable);
};
const onChannelClose = (connectSeq, label) => {
    log.info('NET', 'Channel closed', {
        seq: connectSeq,
        activeSeq: activeConnectSeq,
        label,
        control: S.dcControl?.readyState,
        video: S.dcVideo?.readyState,
        audio: S.dcAudio?.readyState,
        input: S.dcInput?.readyState,
        mic: S.dcMic?.readyState
    });
    if (connectSeq !== activeConnectSeq) {
        log.warn('NET', 'Stale channel close callback', { seq: connectSeq, activeSeq: activeConnectSeq, label });
    }

    S.fpsSent = S.codecSent = 0;
    clearPing();
    clearFirstFrameWatchdog();
    stopMetricsLogger();
    resetRenderer();
    stopKeyframeRetryTimer?.();
    channelsReady = 0;
    closeMic();
};
const setupDataChannel = (dc, onMessage, connectSeq) => {
    dc.binaryType = 'arraybuffer';

    dc.onopen = async () => {
        channelsReady++;
        log.info('NET', 'Channel open', {
            seq: connectSeq,
            activeSeq: activeConnectSeq,
            label: dc.label,
            ready: channelsReady,
            state: dc.readyState
        });
        if (channelsReady === 5) await onAllChannelsOpen(connectSeq);
    };

    dc.onclose = () => onChannelClose(connectSeq, dc.label);

    dc.onerror = err => {
        logNetworkDrop('Channel error', {
            seq: connectSeq,
            activeSeq: activeConnectSeq,
            label: dc.label,
            error: err?.error?.message || 'Unknown'
        });
    };

    dc.onmessage = onMessage;
};
const DC_KEYS = ['dcControl', 'dcVideo', 'dcAudio', 'dcInput', 'dcMic'];

const DC_CONFIG = [
    ['dcControl', 'control', C.DC_CONTROL, handleControl],
    ['dcVideo', 'video', C.DC_VIDEO, handleVideo],
    ['dcAudio', 'audio', C.DC_AUDIO, handleAudio],
    ['dcInput', 'input', C.DC_INPUT, () => {}],
    ['dcMic', 'mic', C.DC_MIC, () => {}]
];
const closeDataChannels = () => {
    [...DC_KEYS, 'pc'].forEach(key => {
        safe(() => S[key]?.close(), undefined, 'NET');
    });
};
const cleanup = () => {
    log.info('NET', 'Cleanup');

    clearPing();
    clearFirstFrameWatchdog();
    if (pendingKeyReqTimer) {
        clearTimeout(pendingKeyReqTimer);
        pendingKeyReqTimer = null;
    }
    stopMetricsLogger();
    resetRenderer();
    stopKeyframeRetryTimer?.();
    resetClockSync();
    closeDataChannels();
    channelsReady = 0;
    closeMic();
};
const resetState = () => {
    cleanup();

    if (S.decoder && S.decoder.state !== 'closed') {
        safe(() => S.decoder.close(), undefined, 'MEDIA');
    }

    DC_KEYS.forEach(k => S[k] = null);
    S.pc = S.decoder = null;
    S.ready = S.fpsSent = S.codecSent = waitingFirstFrame = 0;
    hasConnection = false;
    S.chunks.clear();
    S.lastFrameId = 0;
    S.frameMeta.clear();

    log.debug('NET', 'State reset');
};
const startConnection = async () => {
    updateLoadingStage('Authenticating...', 'Validating...');

    if (await validateSession()) {
        updateLoadingStage('Connecting...');
        await connect();
        return;
    }

    showAuth();
};
const connect = async () => {
    if (!S.authenticated) {
        showAuth();
        return;
    }

    log.info('NET', 'Connecting');
    updateLoadingStage('Connecting...', 'Establishing');
    resetState();

    const connectSeq = ++connectSeqCounter;
    activeConnectSeq = connectSeq;
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
        log.info('NET', 'Connection state', {
            seq: connectSeq,
            activeSeq: activeConnectSeq,
            state: pc.connectionState,
            ice: pc.iceConnectionState,
            signaling: pc.signalingState,
            channelsReady
        });

        if (connectSeq !== activeConnectSeq) {
            log.warn('NET', 'Stale connection state callback', { seq: connectSeq, activeSeq: activeConnectSeq, state: pc.connectionState });
            return;
        }

        if (pc.connectionState === 'connected') {
            connectionAttempts = 0;
        }

        if (pc.connectionState === 'failed' || pc.connectionState === 'disconnected') {
            logNetworkDrop('Connection failed', { state: pc.connectionState });

            if (authElements.overlay.classList.contains('visible')) return;

            closeTabbedMode();

            if (++connectionAttempts >= 3) {
                clearSession();
                showAuth('Connection failed. Please login again.');
            } else {
                showLoading(true);
                updateLoadingStage('Reconnecting...');
                const delay = Math.min(1000 * connectionAttempts, 5000);
                setTimeout(startConnection, delay);
            }
        }
    };
    pc.oniceconnectionstatechange = () => {
        log.info('NET', 'ICE connection state', {
            seq: connectSeq,
            activeSeq: activeConnectSeq,
            state: pc.iceConnectionState
        });
    };
    pc.onicegatheringstatechange = () => {
        log.debug('NET', 'ICE gathering state', {
            seq: connectSeq,
            activeSeq: activeConnectSeq,
            state: pc.iceGatheringState
        });
    };
    pc.onsignalingstatechange = () => {
        log.info('NET', 'Signaling state', {
            seq: connectSeq,
            activeSeq: activeConnectSeq,
            state: pc.signalingState
        });
    };
    DC_CONFIG.forEach(([key, name, config, handler]) => {
        S[key] = pc.createDataChannel(name, config);
        setupDataChannel(S[key], handler, connectSeq);
    });
    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);
    await new Promise(resolve => {
        const timeout = setTimeout(resolve, 300);
        pc.addEventListener('icegatheringstatechange', () => {
            if (pc.iceGatheringState === 'complete') {
                clearTimeout(timeout);
                resolve();
            }
        });
    });

    updateLoadingStage('Connecting...', 'Sending offer...');
    log.debug('NET', 'Sending offer', { seq: connectSeq });

    const res = await fetch(`${BASE_URL}/api/offer`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        credentials: 'include',
        body: JSON.stringify({
            sdp: pc.localDescription.sdp,
            type: pc.localDescription.type
        })
    });

    if (!res.ok) {
        let data = {};
        try {
            data = await res.json();
        } catch {
            data = {};
        }
        if (res.status === 401) {
            clearSession();
            showAuth('Session expired.');
            return;
        }
        throw new Error(data.error || 'Server rejected offer');
    }

    const answer = await res.json();
    await pc.setRemoteDescription(new RTCSessionDescription(answer));
    log.info('NET', 'Connection established', { seq: connectSeq });
};
(async () => {
    log.info('NET', 'Initializing');

    setNetCbs(applyFps, sendMonitor, applyCodec);
    setCursorCaptureFn(sendCursorCapture);
    setSendAudioEnableFn(sendAudioEnable);

    const codecResult = await initCodecDetection();
    S.currentCodec = codecResult.codecId;
    log.info('NET', 'Codec detection', { best: codecResult.codecName });

    await updateCodecOpts();
    enableControl();
    showLoading(false);

    try {
        await startConnection();
    } catch (e) {
        log.error('NET', 'Connection failed', { error: e.message });
        showAuth('Connection failed: ' + e.message);
    }
})();
window.onbeforeunload = () => {
    log.info('NET', 'Page unload, cleanup');
    cleanup();
    closeAudio();
};
