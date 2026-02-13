import { enableControl, setClipboardRequestFn, setCursorCaptureFn } from './input.js';
import { MSG, C, S, $, mkBuf, safe, safeAsync, updateClockOffset, resetClockSync, startMetricsLogger, stopMetricsLogger, resetSessionStats, recordPacket, clientTimeUs, logVideoDrop, logAudioDrop, logNetworkDrop } from './state.js';
import { handleAudioPkt, closeAudio, initDecoder, decodeFrame, setReqKeyFn, setSendAudioEnableFn, stopKeyframeRetryTimer } from './media.js';
import { updateMonOpts, updateCodecOpts, setNetCbs, updateLoadingStage, showLoading, hideLoading, getStoredCodec, initCodecDetection, getStoredFps, updateFpsDropdown } from './ui.js';
import { resetRenderer, setCursorStyle } from './renderer.js';
import { setDcMic, setMicEnableCallback, closeMic } from './mic.js';

const baseUrl = location.origin;
let hasConn = 0, waitFirst = 0, connAtt = 0, pingInt = null, chReady = 0;

const sendCtrl = buf => {
    if (S.dcControl?.readyState !== 'open') { logNetworkDrop('Ctrl !open'); return 0; }
    return safe(() => { S.dcControl.send(buf); return 1; }, 0);
};

// Message senders - consolidated factory pattern
const mkMsg = (type, sz, fn) => sendCtrl(mkBuf(sz, v => { v.setUint32(0, type, 1); fn?.(v); }));
const sendAudioEn = en => mkMsg(MSG.AUDIO_ENABLE, 5, v => v.setUint8(4, en ? 1 : 0));
const sendMicEn = en => mkMsg(MSG.MIC_ENABLE, 5, v => v.setUint8(4, en ? 1 : 0));
const sendMon = i => mkMsg(MSG.MONITOR_SET, 5, v => v.setUint8(4, i));
const sendCodec = id => mkMsg(MSG.CODEC_SET, 5, v => v.setUint8(4, id));
const sendFps = (fps, m) => mkMsg(MSG.FPS_SET, 7, v => { v.setUint16(4, fps, 1); v.setUint8(6, m); });
const sendCurCap = en => mkMsg(MSG.CURSOR_CAPTURE, 5, v => v.setUint8(4, en ? 1 : 0));
const reqKey = () => mkMsg(MSG.REQUEST_KEY, 4);
const reqClip = () => S.dcControl?.readyState === 'open' && mkMsg(MSG.CLIPBOARD_GET, 4);
const sendPing = () => S.dcControl?.readyState === 'open' && S.dcControl.send(mkBuf(16, v => { v.setUint32(0, MSG.PING, 1); v.setBigUint64(8, BigInt(clientTimeUs()), 1); }));

setClipboardRequestFn(reqClip);
setReqKeyFn(reqKey);

const aEl = { ov: $('authOverlay'), u: $('usernameInput'), p: $('passwordInput'), e: $('authError'), b: $('authSubmit') };
const valU = u => u?.length >= 3 && u.length <= 32 && /^[a-zA-Z0-9_-]+$/.test(u);
const valP = p => p?.length >= 8;
const clrSes = () => { S.username = null; S.authenticated = 0; };
const clrPing = () => { clearInterval(pingInt); pingInt = null; };
const setErr = (err, el) => { aEl.e.textContent = err; [aEl.u, aEl.p].forEach(e => e.classList.toggle('error', e === el)); el?.focus(); };

const showAuth = (err = '') => { aEl.u.value = aEl.p.value = ''; setErr(err, err ? aEl.u : null); aEl.ov.classList.add('visible'); aEl.b.disabled = 0; hideLoading(); setTimeout(() => aEl.u.focus(), 100); };
const hideAuth = () => { aEl.ov.classList.remove('visible'); aEl.p.value = ''; setErr('', null); };

const authHTTP = async (u, p) => {
    const res = await fetch(`${baseUrl}/api/auth`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, credentials: 'include', body: JSON.stringify({ username: u, password: p }) });
    const d = await res.json();
    if (!res.ok) throw new Error(res.status === 429 ? `Too many attempts. Try in ${Math.ceil(d.lockoutSeconds / 60)} min.` : d.error || 'Auth failed');
    S.username = u; S.authenticated = 1; return 1;
};

const valSession = async () => {
    const ok = await safeAsync(async () => { const res = await fetch(`${baseUrl}/api/session`, { credentials: 'include' }); if (res.ok) { const d = await res.json(); if (d.valid) { S.authenticated = 1; S.username = d.username; return 1; } } return 0; }, 0);
    if (!ok) clrSes();
    return ok;
};

aEl.u.addEventListener('input', e => { e.target.value = e.target.value.replace(/[^a-zA-Z0-9_-]/g, '').slice(0, 32); setErr('', null); });
aEl.u.addEventListener('keydown', e => { if (e.key === 'Enter') { e.preventDefault(); aEl.p.focus(); } });
aEl.p.addEventListener('keydown', e => { if (e.key === 'Enter' && valU(aEl.u.value) && aEl.p.value.length >= 8) aEl.b.click(); });

aEl.b.addEventListener('click', async () => {
    const u = aEl.u.value; let p = aEl.p.value; aEl.p.value = '';
    if (!valU(u)) { setErr('Username 3-32 chars', aEl.u); return; }
    if (!valP(p)) { setErr('Password min 8 chars', aEl.p); return; }
    aEl.b.disabled = 1; aEl.e.textContent = 'Authenticating...';
    try { await authHTTP(u, p); hideAuth(); showLoading(0); updateLoadingStage('Connecting...'); await connect(); }
    catch (e) { setErr(e.message, aEl.u); aEl.b.disabled = 0; }
    finally { p = ''; }
});

$('disconnectBtn')?.addEventListener('click', async () => { cleanup(); safe(() => fetch(`${baseUrl}/api/logout`, { method: 'POST', credentials: 'include' })); clrSes(); hasConn = 0; showAuth(); });

const parseMonList = d => {
    const v = new DataView(d); let o = 5;
    S.currentMon = v.getUint8(o++);
    S.monitors = Array.from({ length: v.getUint8(4) }, () => {
        const idx = v.getUint8(o++), w = v.getUint16(o, 1), h = v.getUint16(o + 2, 1), rr = v.getUint16(o + 4, 1); o += 6;
        const pri = v.getUint8(o++) === 1, nl = v.getUint8(o++), nm = new TextDecoder().decode(new Uint8Array(d, o, nl)); o += nl;
        return { index: idx, width: w, height: h, refreshRate: rr, isPrimary: pri, name: nm };
    });
    updateMonOpts();
};

const applyCodec = id => { if (sendCodec(id)) { S.currentCodec = id; S.codecSent = 1; } };
const applyFps = val => { const fps = +val, m = fps === S.hostFps ? 1 : 0; if (sendFps(fps, m)) { S.currentFps = fps; S.currentFpsMode = m; S.fpsSent = 1; updateFpsDropdown(fps); } };

const procFrame = (fid, fr) => {
    if (!fr.parts.every(p => p)) { logVideoDrop('Incomplete', { fid, rcv: fr.received, tot: fr.total }); S.stats.framesDropped++; S.chunks.delete(fid); return; }
    const buf = fr.total === 1 ? fr.parts[0] : (() => { const b = new Uint8Array(fr.parts.reduce((s, p) => s + p.byteLength, 0)); let o = 0; fr.parts.forEach(p => { b.set(p, o); o += p.byteLength; }); return b; })();
    S.stats.framesComplete++; if (fr.isKey) S.stats.keyframesReceived++;
    if (fid > S.lastFrameId) S.lastFrameId = fid;
    decodeFrame({ buf, capTs: fr.capTs, encMs: fr.encMs, isKey: fr.isKey, arrivalMs: fr.arrivalMs });
    if (waitFirst) { waitFirst = 0; hideLoading(); hasConn = 1; }
    S.chunks.delete(fid);
};

const hCtrl = e => {
    const arrUs = clientTimeUs();
    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < 4) { logNetworkDrop('Bad ctrl'); return; }
    const v = new DataView(e.data), mt = v.getUint32(0, 1), len = e.data.byteLength;
    if (mt === MSG.PING && len === 24) { updateClockOffset(Number(v.getBigUint64(8, 1)), Number(v.getBigUint64(16, 1)), arrUs); return recordPacket(len, 'control'); }
    if (mt === MSG.HOST_INFO && len === 6) { S.hostFps = v.getUint16(4, 1); if (!S.fpsSent) setTimeout(() => applyFps(getStoredFps() ?? 60), 50); if (!S.codecSent) setTimeout(() => applyCodec(getStoredCodec()), 100); if (!hasConn) { updateLoadingStage('Connected'); waitFirst = 1; } return recordPacket(len, 'control'); }
    if (mt === MSG.CODEC_ACK && len === 5) { S.currentCodec = v.getUint8(4); initDecoder(1); return recordPacket(len, 'control'); }
    if (mt === MSG.FPS_ACK && len === 7) { S.currentFps = v.getUint16(4, 1); S.currentFpsMode = v.getUint8(6); updateFpsDropdown(S.currentFps); return recordPacket(len, 'control'); }
    if (mt === MSG.MONITOR_LIST && len >= 6 && len < 1000 && v.getUint8(4) >= 1 && v.getUint8(4) <= 16) { recordPacket(len, 'control'); return parseMonList(e.data); }
    if (mt === MSG.CLIPBOARD_DATA && len >= 8) { const tl = v.getUint32(4, 1); if (tl > 0 && len >= 8 + tl && tl <= 1048576) navigator.clipboard.writeText(new TextDecoder().decode(new Uint8Array(e.data, 8, tl))).catch(er => logNetworkDrop('Clip fail')); return recordPacket(len, 'control'); }
    if (mt === MSG.CURSOR_SHAPE && len === 5) { setCursorStyle(v.getUint8(4)); return recordPacket(len, 'control'); }
    if (mt === MSG.KICKED && len === 4) { logNetworkDrop('Kicked'); cleanup(); hasConn = 0; showAuth('Disconnected: Another client'); }
};

const hVideo = e => {
    const arrMs = performance.now();
    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < C.HEADER) { logVideoDrop('Bad video'); return; }
    const v = new DataView(e.data), len = e.data.byteLength, capTs = Number(v.getBigUint64(0, 1)), fid = v.getUint32(12, 1), ci = v.getUint16(16, 1), tc = v.getUint16(18, 1), ft = v.getUint8(20), ch = new Uint8Array(e.data, C.HEADER);
    if (tc === 0 || ci >= tc || capTs <= 0) { logVideoDrop('Bad pkt'); return; }
    recordPacket(len, 'video'); S.stats.bytes += len;
    if (S.lastFrameId > 0 && fid < S.lastFrameId) { logVideoDrop('Stale'); return; }
    for (const [id, fr] of S.chunks) {
        if (arrMs - fr.arrivalMs > C.FRAME_TIMEOUT_MS && fr.received < fr.total) {
            logVideoDrop('Timeout', { fid: id }); S.chunks.delete(id); S.stats.framesTimeout++;
            if (fr.isKey) { S.needKey = 1; reqKey(); }
        }
    }
    if (!S.chunks.has(fid)) {
        S.chunks.set(fid, { parts: Array(tc).fill(null), total: tc, received: 0, capTs, encMs: v.getUint32(8, 1) / 1000, arrivalMs: arrMs, isKey: ft === 1 });
        if (S.chunks.size > C.MAX_FRAMES) { const old = [...S.chunks.keys()].sort((a, b) => a - b)[0]; if (old !== fid) { logVideoDrop('Max fr'); S.chunks.delete(old); } }
    }
    const fr = S.chunks.get(fid); if (!fr) return;
    if (fr.parts[ci]) { logNetworkDrop('Dup'); return; }
    fr.parts[ci] = ch; fr.received++;
    if (fr.received === fr.total) procFrame(fid, fr);
};

const hAudio = e => {
    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < C.AUDIO_HEADER) { logAudioDrop('Bad audio'); return; }
    const v = new DataView(e.data), len = e.data.byteLength, mt = v.getUint32(0, 1);
    if (mt === MSG.AUDIO_DATA && len >= C.AUDIO_HEADER) { const dl = v.getUint16(14, 1); if (len === C.AUDIO_HEADER + dl && dl > 0) { recordPacket(len, 'audio'); return handleAudioPkt(e.data); } logAudioDrop('Size mismatch'); }
    else logAudioDrop('Unknown');
};

const onAllOpen = async () => {
    S.fpsSent = S.codecSent = 0; S.authenticated = 1;
    resetClockSync(); resetSessionStats(); updateLoadingStage('Connected');
    await initDecoder(); clrPing(); startMetricsLogger();
    pingInt = setInterval(sendPing, C.PING_MS);
    setDcMic(S.dcMic); setMicEnableCallback(sendMicEn);
};

const onClose = () => { S.fpsSent = S.codecSent = 0; clrPing(); stopMetricsLogger(); resetRenderer(); stopKeyframeRetryTimer?.(); chReady = 0; closeMic(); };

const setupDC = (dc, onMsg) => {
    dc.binaryType = 'arraybuffer';
    dc.onopen = async () => { chReady++; if (chReady === 5) await onAllOpen(); };
    dc.onclose = onClose;
    dc.onerror = e => logNetworkDrop('DC err', { ch: dc.label });
    dc.onmessage = onMsg;
};

const resetSt = () => {
    clrPing(); stopMetricsLogger();
    ['dcControl', 'dcVideo', 'dcAudio', 'dcInput', 'dcMic', 'pc'].forEach(k => safe(() => S[k]?.close()));
    resetRenderer(); stopKeyframeRetryTimer?.(); resetClockSync();
    safe(() => S.decoder?.state !== 'closed' && S.decoder.close());
    S.dcControl = S.dcVideo = S.dcAudio = S.dcInput = S.dcMic = S.pc = S.decoder = null;
    S.ready = S.fpsSent = S.codecSent = waitFirst = 0;
    S.chunks.clear(); S.lastFrameId = 0; S.frameMeta.clear(); chReady = 0; closeMic();
};

const startConn = async () => {
    updateLoadingStage('Authenticating...', 'Validating...');
    if (await valSession()) { updateLoadingStage('Connecting...'); await connect(); return; }
    showAuth();
};

const connect = async () => {
    if (!S.authenticated) { showAuth(); return; }
    updateLoadingStage('Connecting...', 'Establishing'); resetSt();
    const pc = S.pc = new RTCPeerConnection({ iceServers: [{ urls: 'stun:stun.l.google.com:19302' }], iceCandidatePoolSize: 4, bundlePolicy: 'max-bundle', rtcpMuxPolicy: 'require' });
    pc.onconnectionstatechange = () => {
        if (pc.connectionState === 'connected') connAtt = 0;
        if (pc.connectionState === 'failed' || pc.connectionState === 'disconnected') {
            logNetworkDrop('Conn state', { st: pc.connectionState });
            if (aEl.ov.classList.contains('visible')) return;
            if (++connAtt >= 3) { clrSes(); showAuth('Connection failed. Login again.'); }
            else { showLoading(1); updateLoadingStage('Reconnecting...'); setTimeout(startConn, Math.min(1000 * connAtt, 5000)); }
        }
    };
    S.dcControl = pc.createDataChannel('control', C.DC_CONTROL);
    S.dcVideo = pc.createDataChannel('video', C.DC_VIDEO);
    S.dcAudio = pc.createDataChannel('audio', C.DC_AUDIO);
    S.dcInput = pc.createDataChannel('input', C.DC_INPUT);
    S.dcMic = pc.createDataChannel('mic', C.DC_MIC);
    setupDC(S.dcControl, hCtrl); setupDC(S.dcVideo, hVideo); setupDC(S.dcAudio, hAudio); setupDC(S.dcInput, () => {}); setupDC(S.dcMic, () => {});
    const offer = await pc.createOffer(); await pc.setLocalDescription(offer);
    await new Promise(r => { const t = setTimeout(r, 300); pc.addEventListener('icegatheringstatechange', () => { if (pc.iceGatheringState === 'complete') { clearTimeout(t); r(); } }); });
    updateLoadingStage('Connecting...', 'Sending offer...');
    const res = await fetch(`${baseUrl}/api/offer`, { method: 'POST', headers: { 'Content-Type': 'application/json' }, credentials: 'include', body: JSON.stringify({ sdp: pc.localDescription.sdp, type: pc.localDescription.type }) });
    if (!res.ok) { const d = await res.json().catch(() => ({})); if (res.status === 401) { clrSes(); showAuth('Session expired.'); return; } throw new Error(d.error || 'Server rejected'); }
    await pc.setRemoteDescription(new RTCSessionDescription(await res.json()));
};

const cleanup = () => { clrPing(); stopMetricsLogger(); resetRenderer(); stopKeyframeRetryTimer?.(); resetClockSync(); ['dcControl', 'dcVideo', 'dcAudio', 'dcInput', 'dcMic', 'pc'].forEach(k => safe(() => S[k]?.close())); chReady = 0; closeMic(); };

(async () => {
    setNetCbs(applyFps, sendMon, applyCodec); setCursorCaptureFn(sendCurCap); setSendAudioEnableFn(sendAudioEn);
    const cr = await initCodecDetection(); S.currentCodec = cr.codecId;
    await updateCodecOpts(); enableControl(); showLoading(0);
    try { await startConn(); } catch (e) { showAuth('Connection failed: ' + e.message); }
})();

window.onbeforeunload = () => { cleanup(); closeAudio(); closeMic(); };
