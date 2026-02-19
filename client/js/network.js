
import { enableControl, setClipboardRequestFn, setCursorCaptureFn } from './input.js';
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
const requestKeyframe = () => mkCtrlMsg(MSG.REQUEST_KEY, 4);
const requestClipboard = () => S.dcControl?.readyState === 'open' && mkCtrlMsg(MSG.CLIPBOARD_GET, 4);

const sendPing = () => {
    if (S.dcControl?.readyState !== 'open') return;
    S.dcControl.send(mkBuf(16, v => {
        v.setUint32(0, MSG.PING, true);
        v.setBigUint64(8, BigInt(clientTimeUs()), true);
    }));
};
setClipboardRequestFn(requestClipboard);
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
        hideLoading();
        hasConnection = true;
        log.info('NET', 'First frame received');
    }

    S.chunks.delete(frameId);
};
const handleControl = e => {
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
        if (!hasConnection) { updateLoadingStage('Connected'); waitingFirstFrame = true; }
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
            navigator.clipboard.writeText(text).then(
                () => log.debug('NET', 'Clipboard set', { len: textLen }),
                err => logNetworkDrop('Clipboard write failed', { error: err.message })
            );
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
    const chunkIndex = view.getUint16(16, true);
    const totalChunks = view.getUint16(18, true);
    const frameType = view.getUint8(20);
    const chunkData = new Uint8Array(e.data, C.HEADER);

    if (totalChunks === 0 || chunkIndex >= totalChunks || captureTs <= 0) {
        logVideoDrop('Invalid packet data');
        return;
    }

    recordPacket(length, 'video');
    S.stats.bytes += length;
    if (S.lastFrameId > 0 && frameId < S.lastFrameId) {
        logVideoDrop('Stale frame', { frameId, lastId: S.lastFrameId });
        return;
    }
    for (const [id, frame] of S.chunks) {
        if (arrivalMs - frame.arrivalMs > C.FRAME_TIMEOUT_MS && frame.received < frame.total) {
            logVideoDrop('Frame timeout', { frameId: id });
            S.chunks.delete(id);
            S.stats.framesTimeout++;
            if (frame.isKey) {
                S.needKey = 1;
                requestKeyframe();
            }
        }
    }
    if (!S.chunks.has(frameId)) {
        S.chunks.set(frameId, {
            parts: Array(totalChunks).fill(null),
            total: totalChunks,
            received: 0,
            capTs: captureTs,
            encMs: view.getUint32(8, true) / 1000,
            arrivalMs,
            isKey: frameType === 1
        });
        if (S.chunks.size > C.MAX_FRAMES) {
            const oldest = [...S.chunks.keys()].sort((a, b) => a - b)[0];
            if (oldest !== frameId) {
                logVideoDrop('Max frames exceeded');
                S.chunks.delete(oldest);
            }
        }
    }

    const frame = S.chunks.get(frameId);
    if (!frame) return;
    if (frame.parts[chunkIndex]) {
        logNetworkDrop('Duplicate chunk', { frameId, chunkIndex });
        return;
    }

    frame.parts[chunkIndex] = chunkData;
    frame.received++;

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
const onAllChannelsOpen = async () => {
    log.info('NET', 'All channels open');

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
const onChannelClose = () => {
    log.info('NET', 'Channel closed');

    S.fpsSent = S.codecSent = 0;
    clearPing();
    stopMetricsLogger();
    resetRenderer();
    stopKeyframeRetryTimer?.();
    channelsReady = 0;
    closeMic();
};
const setupDataChannel = (dc, onMessage) => {
    dc.binaryType = 'arraybuffer';

    dc.onopen = async () => {
        channelsReady++;
        log.debug('NET', 'Channel open', { label: dc.label, ready: channelsReady });
        if (channelsReady === 5) await onAllChannelsOpen();
    };

    dc.onclose = onChannelClose;

    dc.onerror = err => {
        logNetworkDrop('Channel error', { label: dc.label, error: err?.error?.message || 'Unknown' });
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
        log.info('NET', 'Connection state', { state: pc.connectionState });

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
    DC_CONFIG.forEach(([key, name, config, handler]) => {
        S[key] = pc.createDataChannel(name, config);
        setupDataChannel(S[key], handler);
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
    log.debug('NET', 'Sending offer');

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
        const data = await res.json().catch(() => ({}));
        if (res.status === 401) {
            clearSession();
            showAuth('Session expired.');
            return;
        }
        throw new Error(data.error || 'Server rejected offer');
    }

    const answer = await res.json();
    await pc.setRemoteDescription(new RTCSessionDescription(answer));
    log.info('NET', 'Connection established');
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
