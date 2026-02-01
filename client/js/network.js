import { enableControl } from './input.js';
import { MSG, C, S, $, mkBuf, Stage, updateClockOffset, resetClockSync, startMetricsLogger, stopMetricsLogger, resetSessionStats, recordPacket, clientTimeUs, allChannelsOpen, logVideoDrop } from './state.js';
import { handleAudioPkt, closeAudio, initDecoder, decodeFrame, setReqKeyFn, stopKeyframeRetryTimer } from './media.js';
import { updateMonOpts, updateCodecOpts, setNetCbs, updateLoadingStage, showLoading, hideLoading, getStoredCodec, initCodecDetection, getStoredFps, updateFpsDropdown } from './ui.js';
import { stopPresentLoop } from './renderer.js';

const baseUrl = window.location.origin;
let hasConnected = false, waitFirstFrame = false, connAttempts = 0, pingInterval = null, channelsReady = 0;
const AUTH_KEY = 'slipstream_session';

const sendControl = buf => { if (S.dcControl?.readyState !== 'open') return false; try { S.dcControl.send(buf); return true; } catch { return false; } };

const authEl = { overlay: $('authOverlay'), user: $('usernameInput'), pass: $('passwordInput'), err: $('authError'), btn: $('authSubmit') };
const validUser = u => u?.length >= 3 && u.length <= 32 && /^[a-zA-Z0-9_-]+$/.test(u);
const validPass = p => p?.length >= 8;

const getStoredSession = () => { try { const d = JSON.parse(localStorage.getItem(AUTH_KEY)); if (d?.token && d?.expiresAt > Date.now()) return d; localStorage.removeItem(AUTH_KEY); } catch {} return null; };
const storeSession = (token, username, expiresIn) => { try { localStorage.setItem(AUTH_KEY, JSON.stringify({ token, username, expiresAt: Date.now() + expiresIn * 1000 })); } catch {} };
const clearSession = () => { try { localStorage.removeItem(AUTH_KEY); } catch {} S.sessionToken = S.username = null; S.authenticated = false; };
const clearPing = () => { clearInterval(pingInterval); pingInterval = null; };
const setAuthErr = (err, el) => { authEl.err.textContent = err; [authEl.user, authEl.pass].forEach(e => e.classList.toggle('error', e === el)); el?.focus(); };

const showAuthModal = (err = '') => {
    authEl.user.value = authEl.pass.value = '';
    setAuthErr(err, err ? authEl.user : null);
    authEl.overlay.classList.add('visible'); authEl.btn.disabled = false;
    hideLoading(); setTimeout(() => authEl.user.focus(), 100);
};

const hideAuthModal = () => { authEl.overlay.classList.remove('visible'); setAuthErr('', null); };

const authenticateHTTP = async (username, password) => {
    const res = await fetch(`${baseUrl}/api/auth`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ username, password }) });
    const data = await res.json();
    if (!res.ok) throw new Error(res.status === 429 ? `Too many attempts. Try again in ${Math.ceil(data.lockoutSeconds / 60)} minutes.` : data.error || 'Authentication failed');
    S.sessionToken = data.token; S.username = username; S.authenticated = true;
    storeSession(data.token, username, data.expiresIn);
    return data.token;
};

const validateSession = async () => {
    if (!S.sessionToken) return false;
    try {
        const res = await fetch(`${baseUrl}/api/session`, { headers: { 'Authorization': `Bearer ${S.sessionToken}` } });
        if (res.ok) { const data = await res.json(); if (data.valid) { S.authenticated = true; S.username = data.username; return true; } }
    } catch {}
    clearSession();
    return false;
};

authEl.user.addEventListener('input', e => { e.target.value = e.target.value.replace(/[^a-zA-Z0-9_-]/g, '').slice(0, 32); setAuthErr('', null); });
authEl.user.addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); authEl.pass.focus(); } });
authEl.pass.addEventListener('keydown', e => { if (e.key === 'Enter' && validUser(authEl.user.value) && validPass(authEl.pass.value)) authEl.btn.click(); });

authEl.btn.addEventListener('click', async () => {
    const { value: username } = authEl.user, { value: password } = authEl.pass;
    if (!validUser(username)) { setAuthErr('Username must be 3-32 characters', authEl.user); return; }
    if (!validPass(password)) { setAuthErr('Password must be at least 8 characters', authEl.pass); return; }
    authEl.btn.disabled = true; authEl.err.textContent = 'Authenticating...';
    try {
        await authenticateHTTP(username, password);
        hideAuthModal(); showLoading(false);
        updateLoadingStage(Stage.CONNECT, 'Connecting...');
        await connect();
    } catch (e) { setAuthErr(e.message, authEl.user); authEl.btn.disabled = false; }
});

$('disconnectBtn')?.addEventListener('click', () => { cleanup(); clearSession(); hasConnected = false; showAuthModal(); });

const sendMonSel = i => sendControl(mkBuf(5, v => { v.setUint32(0, MSG.MONITOR_SET, true); v.setUint8(4, i); }));
const sendFps = (fps, mode) => sendControl(mkBuf(7, v => { v.setUint32(0, MSG.FPS_SET, true); v.setUint16(4, fps, true); v.setUint8(6, mode); }));
const sendCodec = codecId => sendControl(mkBuf(5, v => { v.setUint32(0, MSG.CODEC_SET, true); v.setUint8(4, codecId); }));
const reqKey = () => sendControl(mkBuf(4, v => { v.setUint32(0, MSG.REQUEST_KEY, true); }));

setReqKeyFn(reqKey);

const sendPing = () => {
    if (S.dcControl?.readyState !== 'open') return;
    S.dcControl.send(mkBuf(16, v => { v.setUint32(0, MSG.PING, true); v.setBigUint64(8, BigInt(clientTimeUs()), true); }));
};

const parseMonList = data => {
    const view = new DataView(data);
    let off = 5;
    S.currentMon = view.getUint8(off++);
    S.monitors = Array.from({ length: view.getUint8(4) }, () => {
        const index = view.getUint8(off++);
        const [width, height, refreshRate] = [view.getUint16(off, true), view.getUint16(off + 2, true), view.getUint16(off + 4, true)];
        off += 6;
        const isPrimary = view.getUint8(off++) === 1, nameLen = view.getUint8(off++);
        const name = new TextDecoder().decode(new Uint8Array(data, off, nameLen));
        off += nameLen;
        return { index, width, height, refreshRate, isPrimary, name };
    });
    updateMonOpts();
};

const applyCodec = async codecId => { if (sendCodec(codecId)) { S.currentCodec = codecId; S.codecSent = true; } };
const applyFps = val => {
    const fps = +val, mode = fps === S.hostFps ? 1 : 0;
    if (sendFps(fps, mode)) { S.currentFps = fps; S.currentFpsMode = mode; S.fpsSent = true; updateFpsDropdown(fps); }
};

const processFrame = (frameId, frame) => {
    if (!frame.parts.every(p => p)) { S.stats.framesDropped++; logVideoDrop('Incomplete frame'); S.chunks.delete(frameId); return; }
    const buf = frame.total === 1 ? frame.parts[0] : (() => {
        const b = new Uint8Array(frame.parts.reduce((s, p) => s + p.byteLength, 0));
        let off = 0; frame.parts.forEach(p => { b.set(p, off); off += p.byteLength; }); return b;
    })();

    S.stats.recv++; S.stats.framesComplete++;
    S.stats[frame.isKey ? 'keyframesReceived' : 'deltaFramesReceived']++;
    if (frameId > S.lastFrameId) S.lastFrameId = frameId;

    decodeFrame({ buf, capTs: frame.capTs, encMs: frame.encMs, isKey: frame.isKey, arrivalMs: frame.arrivalMs });
    if (waitFirstFrame) { waitFirstFrame = false; hideLoading(); hasConnected = true; }
    S.chunks.delete(frameId);
};

const handleControlMsg = e => {
    const arrivalUs = clientTimeUs();
    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < 4) return;

    const view = new DataView(e.data), msgType = view.getUint32(0, true), len = e.data.byteLength;

    if (msgType === MSG.PING && len === 24) {
        updateClockOffset(Number(view.getBigUint64(8, true)), Number(view.getBigUint64(16, true)), arrivalUs);
        return recordPacket(len, 'control');
    }
    if (msgType === MSG.HOST_INFO && len === 6) {
        S.hostFps = view.getUint16(4, true);
        if (!S.fpsSent) { const storedFps = getStoredFps(); setTimeout(() => applyFps(storedFps !== null ? storedFps : S.hostFps), 50); }
        if (!S.codecSent) setTimeout(() => applyCodec(getStoredCodec()), 100);
        if (!hasConnected) { updateLoadingStage(Stage.OK); waitFirstFrame = true; }
        return recordPacket(len, 'control');
    }
    if (msgType === MSG.CODEC_ACK && len === 5) { S.currentCodec = view.getUint8(4); initDecoder(true); return recordPacket(len, 'control'); }
    if (msgType === MSG.FPS_ACK && len === 7) { S.currentFps = view.getUint16(4, true); S.currentFpsMode = view.getUint8(6); updateFpsDropdown(S.currentFps); return recordPacket(len, 'control'); }
    if (msgType === MSG.MONITOR_LIST && len >= 6 && len < 1000 && view.getUint8(4) >= 1 && view.getUint8(4) <= 16) { recordPacket(len, 'control'); return parseMonList(e.data); }
};

const handleVideoMsg = e => {
    const arrivalMs = performance.now();
    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < C.HEADER) return;
    if (!S.tabVisible) { S.stats.bytes += e.data.byteLength; return; }

    const view = new DataView(e.data), len = e.data.byteLength;
    const capTs = Number(view.getBigUint64(0, true)), frameId = view.getUint32(12, true);
    const chunkIdx = view.getUint16(16, true), totalChunks = view.getUint16(18, true);
    const frameType = view.getUint8(20), chunk = new Uint8Array(e.data, C.HEADER);

    if (totalChunks === 0 || chunkIdx >= totalChunks || capTs <= 0) return;

    recordPacket(len, 'video'); S.stats.chunksReceived++; S.stats.bytes += len;
    if (S.lastFrameId > 0 && frameId < S.lastFrameId) { logVideoDrop('Stale frame'); return; }

    for (const [id, fr] of S.chunks) {
        if (arrivalMs - fr.arrivalMs > C.FRAME_TIMEOUT_MS && fr.received < fr.total) {
            S.chunks.delete(id); S.stats.framesTimeout++;
            logVideoDrop('Frame timeout');
            if (fr.isKey) { S.needKey = true; reqKey(); }
        }
    }

    if (!S.chunks.has(frameId)) {
        S.chunks.set(frameId, { parts: Array(totalChunks).fill(null), total: totalChunks, received: 0, capTs, encMs: view.getUint32(8, true) / 1000, arrivalMs, isKey: frameType === 1 });
        if (S.chunks.size > C.MAX_FRAMES) {
            const oldest = [...S.chunks.keys()].sort((a, b) => a - b)[0];
            if (oldest !== frameId) { logVideoDrop('Max frames exceeded'); S.chunks.delete(oldest); }
        }
    }

    const fr = S.chunks.get(frameId);
    if (!fr || fr.parts[chunkIdx]) return;
    fr.parts[chunkIdx] = chunk; fr.received++;
    if (fr.received === fr.total) processFrame(frameId, fr);
};

const handleAudioMsg = e => {
    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < C.AUDIO_HEADER) return;
    const view = new DataView(e.data), len = e.data.byteLength, msgType = view.getUint32(0, true);
    if (msgType === MSG.AUDIO_DATA && len >= C.AUDIO_HEADER) {
        const dataLen = view.getUint16(14, true);
        if (len === C.AUDIO_HEADER + dataLen && dataLen > 0) { recordPacket(len, 'audio'); return handleAudioPkt(e.data); }
    }
};

const onAllChannelsOpen = async () => {
    S.fpsSent = false; S.codecSent = false; S.authenticated = true;
    resetClockSync(); resetSessionStats();
    updateLoadingStage(Stage.OK, 'Connected');
    await initDecoder();
    clearPing(); startMetricsLogger();
    pingInterval = setInterval(sendPing, C.PING_MS);
};

const onChannelClose = () => {
    S.fpsSent = false; S.codecSent = false;
    clearPing(); stopMetricsLogger(); stopPresentLoop(); stopKeyframeRetryTimer?.();
    channelsReady = 0;
};

const setupDataChannel = (dc, onMessage) => {
    dc.binaryType = 'arraybuffer';
    dc.onopen = async () => { channelsReady++; if (channelsReady === 4) await onAllChannelsOpen(); };
    dc.onclose = onChannelClose; dc.onerror = () => {}; dc.onmessage = onMessage;
};

const resetState = () => {
    clearPing(); stopMetricsLogger();
    S.dcControl?.close(); S.dcVideo?.close(); S.dcAudio?.close(); S.dcInput?.close(); S.pc?.close();
    stopPresentLoop(); stopKeyframeRetryTimer?.(); resetClockSync();
    try { if (S.decoder?.state !== 'closed') S.decoder?.close(); } catch {}
    S.dcControl = S.dcVideo = S.dcAudio = S.dcInput = S.pc = S.decoder = null;
    S.ready = S.fpsSent = S.codecSent = waitFirstFrame = false;
    S.chunks.clear(); S.lastFrameId = 0; S.frameMeta.clear(); S.presentQueue = [];
    channelsReady = 0;
};

const startConnection = async () => {
    if (S.sessionToken) {
        updateLoadingStage(Stage.AUTH, 'Validating session...');
        if (await validateSession()) { updateLoadingStage(Stage.CONNECT, 'Connecting...'); await connect(); return; }
    }
    showAuthModal();
};

const connect = async () => {
    if (!S.sessionToken) { showAuthModal(); return; }

    updateLoadingStage(Stage.CONNECT);
    resetState();

    const pc = S.pc = new RTCPeerConnection({ iceServers: [{ urls: 'stun:stun.l.google.com:19302' }], iceCandidatePoolSize: 4, bundlePolicy: 'max-bundle', rtcpMuxPolicy: 'require' });

    pc.onconnectionstatechange = () => {
        if (pc.connectionState === 'connected') connAttempts = 0;
        if (pc.connectionState === 'failed' || pc.connectionState === 'disconnected') {
            if (authEl.overlay.classList.contains('visible')) return;
            if (++connAttempts >= 3) { clearSession(); showAuthModal('Connection failed. Please login again.'); }
            else { showLoading(true); updateLoadingStage(Stage.ERR, 'Reconnecting...'); setTimeout(() => startConnection(), Math.min(1000 * connAttempts, 5000)); }
        }
    };

    S.dcControl = pc.createDataChannel('control', C.DC_CONTROL);
    S.dcVideo = pc.createDataChannel('video', C.DC_VIDEO);
    S.dcAudio = pc.createDataChannel('audio', C.DC_AUDIO);
    S.dcInput = pc.createDataChannel('input', C.DC_INPUT);

    setupDataChannel(S.dcControl, handleControlMsg);
    setupDataChannel(S.dcVideo, handleVideoMsg);
    setupDataChannel(S.dcAudio, handleAudioMsg);
    setupDataChannel(S.dcInput, () => {});

    const offer = await pc.createOffer();
    await pc.setLocalDescription(offer);

    await new Promise(resolve => {
        const timeout = setTimeout(resolve, 300);
        pc.addEventListener('icegatheringstatechange', () => { if (pc.iceGatheringState === 'complete') { clearTimeout(timeout); resolve(); } });
    });

    updateLoadingStage(Stage.CONNECT, 'Sending offer...');

    const res = await fetch(`${baseUrl}/api/offer`, {
        method: 'POST', headers: { 'Content-Type': 'application/json', 'Authorization': `Bearer ${S.sessionToken}` },
        body: JSON.stringify({ sdp: pc.localDescription.sdp, type: pc.localDescription.type })
    });

    if (!res.ok) {
        const data = await res.json().catch(() => ({}));
        if (res.status === 401) { clearSession(); showAuthModal('Session expired. Please login again.'); return; }
        throw new Error(data.error || 'Server rejected offer');
    }

    await pc.setRemoteDescription(new RTCSessionDescription(await res.json()));
};

const cleanup = () => {
    clearPing(); stopMetricsLogger(); stopPresentLoop(); stopKeyframeRetryTimer?.(); resetClockSync();
    S.dcControl?.close(); S.dcVideo?.close(); S.dcAudio?.close(); S.dcInput?.close(); S.pc?.close();
    channelsReady = 0;
};

(async () => {
    setNetCbs(applyFps, sendMonSel, applyCodec);
    const codecResult = await initCodecDetection();
    S.currentCodec = codecResult.codecId;
    await updateCodecOpts(); enableControl();

    const stored = getStoredSession();
    if (stored) { S.sessionToken = stored.token; S.username = stored.username; }
    showLoading(false);
    try { await startConnection(); } catch (e) { showAuthModal('Connection failed: ' + e.message); }
})();

window.onbeforeunload = () => { cleanup(); closeAudio(); };
