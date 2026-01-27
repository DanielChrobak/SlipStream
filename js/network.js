import './renderer.js';
import { enableControl } from './input.js';
import { MSG, C, S, $, mkBuf, Stage, updateClockOffset, resetClockSync, startMetricsLogger, stopMetricsLogger, resetSessionStats, recordPacket, clientTimeUs } from './state.js';
import { handleAudioPkt, closeAudio, initDecoder, decodeFrame, setReqKeyFn, stopKeyframeRetryTimer } from './media.js';
import { updateMonOpts, updateFpsOpts, updateCodecOpts, setNetCbs, updateLoadingStage, showLoading, hideLoading, getStoredCodec, initCodecDetection } from './ui.js';
import { stopPresentLoop } from './renderer.js';

const baseUrl = window.location.origin;
let lastPingSendUs = 0, hasConnected = false, waitFirstFrame = false, connAttempts = 0, pingInterval = null;
const AUTH_KEY = 'slipstream_session';

const sendMsg = buf => { if (S.dc?.readyState !== 'open') return false; try { S.dc.send(buf); return true; } catch { return false; } };

const authEl = { overlay: $('authOverlay'), user: $('usernameInput'), pass: $('passwordInput'), err: $('authError'), btn: $('authSubmit') };
const validUser = u => u?.length >= 3 && u.length <= 32 && /^[a-zA-Z0-9_-]+$/.test(u);
const validPass = p => p?.length >= 8;

const getStoredSession = () => { try { const d = JSON.parse(localStorage.getItem(AUTH_KEY)); if (d?.token && d?.expiresAt > Date.now()) return d; localStorage.removeItem(AUTH_KEY); } catch {} return null; };
const storeSession = (token, username, expiresIn) => { try { localStorage.setItem(AUTH_KEY, JSON.stringify({ token, username, expiresAt: Date.now() + expiresIn * 1000 })); } catch {} };
const clearSession = () => { try { localStorage.removeItem(AUTH_KEY); } catch {} S.sessionToken = S.username = null; S.authenticated = false; };
const clearPing = () => { clearInterval(pingInterval); pingInterval = null; };

const setAuthErr = (err, el) => {
    authEl.err.textContent = err;
    [authEl.user, authEl.pass].forEach(e => e.classList.toggle('error', e === el));
    el?.focus();
};

const showAuthModal = (err = '') => {
    authEl.user.value = authEl.pass.value = '';
    setAuthErr(err, err ? authEl.user : null);
    authEl.overlay.classList.add('visible');
    authEl.btn.disabled = false;
    hideLoading();
    setTimeout(() => authEl.user.focus(), 100);
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
    authEl.btn.disabled = true;
    authEl.err.textContent = 'Authenticating...';
    try {
        await authenticateHTTP(username, password);
        hideAuthModal();
        showLoading(false);
        updateLoadingStage(Stage.CONNECT, 'Connecting...');
        await connect();
    } catch (e) { setAuthErr(e.message, authEl.user); authEl.btn.disabled = false; }
});

$('disconnectBtn')?.addEventListener('click', () => { cleanup(); clearSession(); hasConnected = false; showAuthModal(); });

export const sendMonSel = i => sendMsg(mkBuf(5, v => { v.setUint32(0, MSG.MONITOR_SET, true); v.setUint8(4, i); }));
export const sendFps = (fps, mode) => sendMsg(mkBuf(7, v => { v.setUint32(0, MSG.FPS_SET, true); v.setUint16(4, fps, true); v.setUint8(6, mode); }));
export const sendCodec = codecId => sendMsg(mkBuf(5, v => { v.setUint32(0, MSG.CODEC_SET, true); v.setUint8(4, codecId); }));
export const reqKey = () => sendMsg(mkBuf(4, v => { v.setUint32(0, MSG.REQUEST_KEY, true); }));

setReqKeyFn(reqKey);

const sendPing = () => {
    if (S.dc?.readyState !== 'open') return;
    lastPingSendUs = clientTimeUs();
    S.dc.send(mkBuf(16, v => { v.setUint32(0, MSG.PING, true); v.setBigUint64(8, BigInt(lastPingSendUs), true); }));
};

const parseMonList = data => {
    const view = new DataView(data);
    let off = 5;
    S.currentMon = view.getUint8(off++);
    S.monitors = Array.from({ length: view.getUint8(4) }, () => {
        const index = view.getUint8(off++);
        const [width, height, refreshRate] = [view.getUint16(off, true), view.getUint16(off + 2, true), view.getUint16(off + 4, true)];
        off += 6;
        const isPrimary = view.getUint8(off++) === 1;
        const nameLen = view.getUint8(off++);
        const name = new TextDecoder().decode(new Uint8Array(data, off, nameLen));
        off += nameLen;
        return { index, width, height, refreshRate, isPrimary, name };
    });
    updateMonOpts();
};

export const applyCodec = async codecId => {
    if (sendCodec(codecId)) { S.currentCodec = codecId; S.codecSent = true; console.log(`[CODEC] Requesting codec switch to ${codecId === 1 ? 'AV1' : 'H.264'}`); }
};

export const selDefFps = () => {
    const sel = $('fpsSel');
    if (!sel?.options.length) return S.clientFps;
    const opts = [...sel.options].map(o => +o.value);
    const best = opts.reduce((b, o) => Math.abs(o - S.clientFps) < Math.abs(b - S.clientFps) ? o : b, opts[0]);
    sel.value = opts.includes(S.clientFps) ? S.clientFps : best;
    return +sel.value;
};

export const applyFps = val => {
    const fps = +val, mode = fps === S.hostFps ? 1 : fps === S.clientFps ? 2 : 0;
    if (sendFps(fps, mode)) { S.currentFps = fps; S.currentFpsMode = mode; S.fpsSent = true; }
};

const processFrame = (frameId, frame) => {
    if (!frame.parts.every(p => p)) { S.stats.framesDropped++; S.chunks.delete(frameId); return; }
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

const handleMsg = e => {
    const arrivalMs = performance.now(), arrivalUs = clientTimeUs();
    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < 4) return;

    const view = new DataView(e.data), msgType = view.getUint32(0, true), len = e.data.byteLength;

    if (msgType === MSG.PING && len === 24) {
        updateClockOffset(Number(view.getBigUint64(8, true)), Number(view.getBigUint64(16, true)), arrivalUs);
        S.rtt = (arrivalUs - Number(view.getBigUint64(8, true))) / 1000;
        return recordPacket(len, 'control');
    }

    if (msgType === MSG.HOST_INFO && len === 6) {
        S.hostFps = view.getUint16(4, true);
        updateFpsOpts();
        if (!S.fpsSent) setTimeout(() => applyFps(selDefFps()), 50);
        if (!S.codecSent) setTimeout(() => applyCodec(getStoredCodec()), 100);
        if (!hasConnected) { updateLoadingStage(Stage.OK); waitFirstFrame = true; }
        return recordPacket(len, 'control');
    }

    if (msgType === MSG.CODEC_ACK && len === 5) {
        S.currentCodec = view.getUint8(4);
        console.log(`[CODEC] Server confirmed codec: ${S.currentCodec === 1 ? 'AV1' : 'H.264'}`);
        initDecoder(true);
        return recordPacket(len, 'control');
    }

    if (msgType === MSG.FPS_ACK && len === 7) {
        S.currentFps = view.getUint16(4, true); S.currentFpsMode = view.getUint8(6);
        return recordPacket(len, 'control');
    }

    if (msgType === MSG.MONITOR_LIST && len >= 6 && len < 1000 && view.getUint8(4) >= 1 && view.getUint8(4) <= 16) {
        recordPacket(len, 'control'); return parseMonList(e.data);
    }

    if (msgType === MSG.AUDIO_DATA && len >= C.AUDIO_HEADER) {
        const dataLen = view.getUint16(14, true);
        if (len === C.AUDIO_HEADER + dataLen && dataLen > 0) { recordPacket(len, 'audio'); return handleAudioPkt(e.data); }
    }

    if (len < C.HEADER) return;

    const capTs = Number(view.getBigUint64(0, true)), frameId = view.getUint32(12, true);
    const chunkIdx = view.getUint16(16, true), totalChunks = view.getUint16(18, true);
    const frameType = view.getUint8(20), chunk = new Uint8Array(e.data, C.HEADER);

    if (totalChunks === 0 || chunkIdx >= totalChunks || capTs <= 0) return;

    recordPacket(len, 'video');
    S.stats.chunksReceived++; S.stats.bytes += len;

    if (S.lastFrameId > 0 && frameId < S.lastFrameId) return;

    for (const [id, fr] of S.chunks) {
        if (arrivalMs - fr.arrivalMs > C.FRAME_TIMEOUT_MS && fr.received < fr.total) {
            S.chunks.delete(id); S.stats.framesTimeout++;
            if (fr.isKey) { S.needKey = true; reqKey(); }
        }
    }

    if (!S.chunks.has(frameId)) {
        S.chunks.set(frameId, { parts: Array(totalChunks).fill(null), total: totalChunks, received: 0, capTs, encMs: view.getUint32(8, true) / 1000, arrivalMs, isKey: frameType === 1 });
        if (S.chunks.size > C.MAX_FRAMES) {
            const oldest = [...S.chunks.keys()].sort((a, b) => a - b)[0];
            if (oldest !== frameId) S.chunks.delete(oldest);
        }
    }

    const fr = S.chunks.get(frameId);
    if (!fr || fr.parts[chunkIdx]) return;
    fr.parts[chunkIdx] = chunk; fr.received++;
    if (fr.received === fr.total) processFrame(frameId, fr);
};

const setupDC = () => {
    S.dc.binaryType = 'arraybuffer';
    S.dc.onopen = async () => {
        S.fpsSent = false; S.codecSent = false; S.authenticated = true;
        resetClockSync(); resetSessionStats();
        updateLoadingStage(Stage.OK, 'Connected');
        await initDecoder();
        clearPing(); startMetricsLogger();
        pingInterval = setInterval(sendPing, C.PING_MS);
    };
    S.dc.onclose = () => { S.fpsSent = false; S.codecSent = false; clearPing(); stopMetricsLogger(); stopPresentLoop(); stopKeyframeRetryTimer?.(); };
    S.dc.onerror = () => {};
    S.dc.onmessage = handleMsg;
};

const resetState = () => {
    clearPing(); stopMetricsLogger();
    S.dc?.close(); S.pc?.close();
    stopPresentLoop(); stopKeyframeRetryTimer?.(); resetClockSync();
    try { if (S.decoder?.state !== 'closed') S.decoder?.close(); } catch {}
    S.dc = S.pc = S.decoder = null;
    S.ready = S.fpsSent = S.codecSent = waitFirstFrame = false;
    S.chunks.clear(); S.lastFrameId = lastPingSendUs = 0;
    S.frameMeta.clear(); S.presentQueue = [];
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

    try {
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

        S.dc = pc.createDataChannel('screen', C.DC);
        setupDC();

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
    } catch (e) { throw e; }
};

export const detectFps = async () => {
    if (window.screen?.refreshRate) return Math.round(window.screen.refreshRate);
    return new Promise(resolve => {
        let frameCount = 0, lastTime = performance.now();
        const samples = [];
        const measure = now => {
            if (++frameCount > 1) samples.push(now - lastTime);
            lastTime = now;
            if (samples.length < 30) requestAnimationFrame(measure);
            else { samples.sort((a, b) => a - b); const median = samples[samples.length >> 1]; resolve([30, 60, 120, 144].find(r => Math.abs(r - 1000 / median) <= 5) || Math.round(1000 / median)); }
        };
        requestAnimationFrame(measure);
    });
};

export const cleanup = () => { clearPing(); stopMetricsLogger(); stopPresentLoop(); stopKeyframeRetryTimer?.(); resetClockSync(); S.dc?.close(); S.pc?.close(); };

(async () => {
    setNetCbs(applyFps, sendMonSel, applyCodec);
    console.log('[INIT] Detecting codec support...');
    const codecResult = await initCodecDetection();
    S.currentCodec = codecResult.codecId;
    console.log(`[INIT] Will use ${codecResult.codecName} codec (${codecResult.hardwareAccel ? 'hardware' : 'software'})`);

    S.clientFps = await detectFps();
    updateFpsOpts();
    await updateCodecOpts();
    enableControl();

    const stored = getStoredSession();
    if (stored) { S.sessionToken = stored.token; S.username = stored.username; }
    showLoading(false);
    try { await startConnection(); } catch (e) { showAuthModal('Connection failed: ' + e.message); }
})();

window.onbeforeunload = () => { cleanup(); closeAudio(); };
