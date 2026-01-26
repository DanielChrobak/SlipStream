import './renderer.js';
import { setKeyboardLockFns, enableControl } from './input.js';
import {
    MSG, C, S, $, mkBuf, Stage,
    updateClockOffset, resetClockSync,
    startMetricsLogger, stopMetricsLogger,
    resetSessionStats, recordPacket, clientTimeUs
} from './state.js';
import {
    handleAudioPkt, closeAudio, initDecoder,
    decodeFrame, setReqKeyFn, stopKeyframeRetryTimer
} from './media.js';
import {
    updateMonOpts, updateFpsOpts, setNetCbs,
    updateLoadingStage, showLoading, hideLoading,
    isLoadingVisible, isKeyboardLocked, exitFullscreen
} from './ui.js';
import { stopPresentLoop } from './renderer.js';

// Set up keyboard lock functions
setKeyboardLockFns(isKeyboardLocked, exitFullscreen);

// Connection state
const baseUrl = window.location.origin;
let lastPingSendUs = 0;
let hasConnected = false;
let waitFirstFrame = false;
let connAttempts = 0;
let pingInterval = null;

const AUTH_KEY = 'slipstream_session';

// Send message over data channel
const sendMsg = buf => {
    if (S.dc?.readyState !== 'open') {
        return false;
    }
    try {
        S.dc.send(buf);
        return true;
    } catch {
        return false;
    }
};

// ============================================
// Authentication UI
// ============================================

const authEl = {
    overlay: $('authOverlay'),
    user: $('usernameInput'),
    pass: $('passwordInput'),
    err: $('authError'),
    btn: $('authSubmit')
};

// Validation helpers
const validUser = u => u?.length >= 3 && u.length <= 32 && /^[a-zA-Z0-9_-]+$/.test(u);
const validPass = p => p?.length >= 8;

// Session storage helpers
const getStoredSession = () => {
    try {
        const data = JSON.parse(localStorage.getItem(AUTH_KEY));
        if (data?.token && data?.expiresAt > Date.now()) {
            return data;
        }
        localStorage.removeItem(AUTH_KEY);
    } catch {}
    return null;
};

const storeSession = (token, username, expiresIn) => {
    try {
        localStorage.setItem(AUTH_KEY, JSON.stringify({
            token,
            username,
            expiresAt: Date.now() + expiresIn * 1000
        }));
    } catch {}
};

const clearSession = () => {
    try {
        localStorage.removeItem(AUTH_KEY);
    } catch {}
    S.sessionToken = null;
    S.username = null;
    S.authenticated = false;
};

// Clear ping interval
const clearPing = () => {
    if (pingInterval) {
        clearInterval(pingInterval);
        pingInterval = null;
    }
};

// Auth UI helpers
const setAuthErr = (err, el) => {
    authEl.err.textContent = err;
    [authEl.user, authEl.pass].forEach(e => e.classList.toggle('error', e === el));
    el?.focus();
};

const showAuthModal = (err = '') => {
    authEl.user.value = '';
    authEl.pass.value = '';
    setAuthErr(err, err ? authEl.user : null);
    authEl.overlay.classList.add('visible');
    authEl.btn.disabled = false;
    hideLoading();
    setTimeout(() => authEl.user.focus(), 100);
};

const hideAuthModal = () => {
    authEl.overlay.classList.remove('visible');
    setAuthErr('', null);
};

// HTTP authentication
const authenticateHTTP = async (username, password) => {
    const res = await fetch(`${baseUrl}/api/auth`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ username, password })
    });

    const data = await res.json();

    if (!res.ok) {
        if (res.status === 429) {
            throw new Error(`Too many attempts. Try again in ${Math.ceil(data.lockoutSeconds / 60)} minutes.`);
        }
        throw new Error(data.error || 'Authentication failed');
    }

    S.sessionToken = data.token;
    S.username = username;
    S.authenticated = true;

    storeSession(data.token, username, data.expiresIn);

    return data.token;
};

// Validate existing session
const validateSession = async () => {
    if (!S.sessionToken) {
        return false;
    }

    try {
        const res = await fetch(`${baseUrl}/api/session`, {
            headers: { 'Authorization': `Bearer ${S.sessionToken}` }
        });

        if (res.ok) {
            const data = await res.json();
            if (data.valid) {
                S.authenticated = true;
                S.username = data.username;
                return true;
            }
        }
    } catch {}

    clearSession();
    return false;
};

// Auth input event handlers
authEl.user.addEventListener('input', e => {
    e.target.value = e.target.value.replace(/[^a-zA-Z0-9_-]/g, '').slice(0, 32);
    setAuthErr('', null);
});

authEl.user.addEventListener('keydown', e => {
    if (e.key === 'Enter') {
        e.preventDefault();
        authEl.pass.focus();
    }
});

authEl.pass.addEventListener('keydown', e => {
    if (e.key === 'Enter' && validUser(authEl.user.value) && validPass(authEl.pass.value)) {
        authEl.btn.click();
    }
});

// Auth submit handler
authEl.btn.addEventListener('click', async () => {
    const username = authEl.user.value;
    const password = authEl.pass.value;

    if (!validUser(username)) {
        setAuthErr('Username must be 3-32 characters', authEl.user);
        return;
    }

    if (!validPass(password)) {
        setAuthErr('Password must be at least 8 characters', authEl.pass);
        return;
    }

    authEl.btn.disabled = true;
    authEl.err.textContent = 'Authenticating...';

    try {
        await authenticateHTTP(username, password);
        hideAuthModal();
        showLoading(false);
        updateLoadingStage(Stage.CONNECT, 'Connecting...');
        await connect();
    } catch (e) {
        setAuthErr(e.message, authEl.user);
        authEl.btn.disabled = false;
    }
});

// Disconnect button handler
$('disconnectBtn')?.addEventListener('click', () => {
    cleanup();
    clearSession();
    hasConnected = false;
    showAuthModal();
});

// ============================================
// Network Message Senders
// ============================================

export const sendMonSel = index => {
    return sendMsg(mkBuf(5, v => {
        v.setUint32(0, MSG.MONITOR_SET, true);
        v.setUint8(4, index);
    }));
};

export const sendFps = (fps, mode) => {
    return sendMsg(mkBuf(7, v => {
        v.setUint32(0, MSG.FPS_SET, true);
        v.setUint16(4, fps, true);
        v.setUint8(6, mode);
    }));
};

export const reqKey = () => {
    return sendMsg(mkBuf(4, v => {
        v.setUint32(0, MSG.REQUEST_KEY, true);
    }));
};

// Set keyframe request function reference
setReqKeyFn(reqKey);

// Send ping message
const sendPing = () => {
    if (S.dc?.readyState !== 'open') {
        return;
    }

    lastPingSendUs = clientTimeUs();

    S.dc.send(mkBuf(16, v => {
        v.setUint32(0, MSG.PING, true);
        v.setBigUint64(8, BigInt(lastPingSendUs), true);
    }));
};

// ============================================
// Message Parsers
// ============================================

// Parse monitor list message
const parseMonList = data => {
    const view = new DataView(data);
    let offset = 5;

    const count = view.getUint8(4);
    S.currentMon = view.getUint8(offset++);

    S.monitors = Array.from({ length: count }, () => {
        const index = view.getUint8(offset++);
        const width = view.getUint16(offset, true);
        const height = view.getUint16(offset + 2, true);
        const refreshRate = view.getUint16(offset + 4, true);
        offset += 6;

        const isPrimary = view.getUint8(offset++) === 1;
        const nameLen = view.getUint8(offset++);
        const name = new TextDecoder().decode(new Uint8Array(data, offset, nameLen));
        offset += nameLen;

        return { index, width, height, refreshRate, isPrimary, name };
    });

    updateMonOpts();
};

// Select default FPS
export const selDefFps = () => {
    const sel = $('fpsSel');
    if (!sel?.options.length) {
        return S.clientFps;
    }

    const opts = [...sel.options].map(o => +o.value);
    const best = opts.reduce((b, o) =>
        Math.abs(o - S.clientFps) < Math.abs(b - S.clientFps) ? o : b, opts[0]
    );

    sel.value = opts.includes(S.clientFps) ? S.clientFps : best;
    return +sel.value;
};

// Apply FPS setting
export const applyFps = val => {
    const fps = +val;
    const mode = fps === S.hostFps ? 1 : fps === S.clientFps ? 2 : 0;

    if (sendFps(fps, mode)) {
        S.currentFps = fps;
        S.currentFpsMode = mode;
        S.fpsSent = true;
    }
};

// ============================================
// Frame Processing
// ============================================

// Process complete frame
const processFrame = (frameId, frame) => {
    // Verify all parts received
    if (!frame.parts.every(p => p)) {
        S.stats.framesDropped++;
        S.chunks.delete(frameId);
        return;
    }

    // Assemble buffer from parts
    let buf;
    if (frame.total === 1) {
        buf = frame.parts[0];
    } else {
        const totalSize = frame.parts.reduce((s, p) => s + p.byteLength, 0);
        buf = new Uint8Array(totalSize);
        let offset = 0;
        for (const part of frame.parts) {
            buf.set(part, offset);
            offset += part.byteLength;
        }
    }

    // Update stats
    S.stats.recv++;
    S.stats.framesComplete++;

    if (frame.isKey) {
        S.stats.keyframesReceived++;
    } else {
        S.stats.deltaFramesReceived++;
    }

    if (frameId > S.lastFrameId) {
        S.lastFrameId = frameId;
    }

    // Decode the frame
    decodeFrame({
        buf,
        capTs: frame.capTs,
        encMs: frame.encMs,
        isKey: frame.isKey,
        arrivalMs: frame.arrivalMs
    });

    // Hide loading on first frame
    if (waitFirstFrame) {
        waitFirstFrame = false;
        hideLoading();
        hasConnected = true;
    }

    S.chunks.delete(frameId);
};

// ============================================
// Message Handler
// ============================================

const handleMsg = e => {
    const arrivalMs = performance.now();
    const arrivalUs = clientTimeUs();

    if (!(e.data instanceof ArrayBuffer) || e.data.byteLength < 4) {
        return;
    }

    const view = new DataView(e.data);
    const msgType = view.getUint32(0, true);
    const len = e.data.byteLength;

    // Ping response
    if (msgType === MSG.PING && len === 24) {
        const clientSendUs = Number(view.getBigUint64(8, true));
        const serverTimeUs = Number(view.getBigUint64(16, true));

        updateClockOffset(clientSendUs, serverTimeUs, arrivalUs);
        S.rtt = (arrivalUs - clientSendUs) / 1000;
        recordPacket(len, 'control');
        return;
    }

    // Host info
    if (msgType === MSG.HOST_INFO && len === 6) {
        S.hostFps = view.getUint16(4, true);
        updateFpsOpts();

        if (!S.fpsSent) {
            setTimeout(() => applyFps(selDefFps()), 50);
        }

        if (!hasConnected) {
            updateLoadingStage(Stage.OK);
            waitFirstFrame = true;
        }

        recordPacket(len, 'control');
        return;
    }

    // FPS acknowledgment
    if (msgType === MSG.FPS_ACK && len === 7) {
        S.currentFps = view.getUint16(4, true);
        S.currentFpsMode = view.getUint8(6);
        recordPacket(len, 'control');
        return;
    }

    // Monitor list
    if (msgType === MSG.MONITOR_LIST && len >= 6 && len < 1000) {
        const count = view.getUint8(4);
        if (count >= 1 && count <= 16) {
            recordPacket(len, 'control');
            return parseMonList(e.data);
        }
    }

    // Audio data
    if (msgType === MSG.AUDIO_DATA && len >= C.AUDIO_HEADER) {
        const dataLen = view.getUint16(14, true);
        if (len === C.AUDIO_HEADER + dataLen && dataLen > 0) {
            recordPacket(len, 'audio');
            return handleAudioPkt(e.data);
        }
    }

    // Video frame data
    if (len < C.HEADER) {
        return;
    }

    const capTs = Number(view.getBigUint64(0, true));
    const encTime = view.getUint32(8, true);
    const frameId = view.getUint32(12, true);
    const chunkIdx = view.getUint16(16, true);
    const totalChunks = view.getUint16(18, true);
    const frameType = view.getUint8(20);
    const chunk = new Uint8Array(e.data, C.HEADER);

    // Validate chunk
    if (totalChunks === 0 || chunkIdx >= totalChunks || capTs <= 0) {
        return;
    }

    recordPacket(len, 'video');
    S.stats.chunksReceived++;
    S.stats.bytes += len;

    // Skip old frames
    if (S.lastFrameId > 0 && frameId < S.lastFrameId) {
        return;
    }

    // Clean up timed out frames
    for (const [id, fr] of S.chunks) {
        if (arrivalMs - fr.arrivalMs > C.FRAME_TIMEOUT_MS && fr.received < fr.total) {
            S.chunks.delete(id);
            S.stats.framesTimeout++;

            if (fr.isKey) {
                S.needKey = true;
                reqKey();
            }
        }
    }

    // Create new frame entry if needed
    if (!S.chunks.has(frameId)) {
        S.chunks.set(frameId, {
            parts: Array(totalChunks).fill(null),
            total: totalChunks,
            received: 0,
            capTs,
            encMs: encTime / 1000,
            arrivalMs,
            isKey: frameType === 1
        });

        // Limit max frames in buffer
        if (S.chunks.size > C.MAX_FRAMES) {
            const oldest = [...S.chunks.keys()].sort((a, b) => a - b)[0];
            if (oldest !== frameId) {
                S.chunks.delete(oldest);
            }
        }
    }

    // Add chunk to frame
    const frame = S.chunks.get(frameId);
    if (!frame || frame.parts[chunkIdx]) {
        return;
    }

    frame.parts[chunkIdx] = chunk;
    frame.received++;

    // Process complete frame
    if (frame.received === frame.total) {
        processFrame(frameId, frame);
    }
};

// ============================================
// Data Channel Setup
// ============================================

const setupDC = () => {
    S.dc.binaryType = 'arraybuffer';

    S.dc.onopen = async () => {
        S.fpsSent = false;
        S.authenticated = true;

        resetClockSync();
        resetSessionStats();

        updateLoadingStage(Stage.OK, 'Connected');

        await initDecoder();

        clearPing();
        startMetricsLogger();
        pingInterval = setInterval(sendPing, C.PING_MS);
    };

    S.dc.onclose = () => {
        S.fpsSent = false;
        clearPing();
        stopMetricsLogger();
        stopPresentLoop();
        stopKeyframeRetryTimer?.();
    };

    S.dc.onerror = () => {};

    S.dc.onmessage = handleMsg;
};

// ============================================
// Connection Management
// ============================================

// Reset all connection state
const resetState = () => {
    clearPing();
    stopMetricsLogger();

    S.dc?.close();
    S.pc?.close();

    stopPresentLoop();
    stopKeyframeRetryTimer?.();
    resetClockSync();

    try {
        if (S.decoder?.state !== 'closed') {
            S.decoder?.close();
        }
    } catch {}

    S.dc = null;
    S.pc = null;
    S.decoder = null;
    S.ready = false;
    S.fpsSent = false;

    waitFirstFrame = false;
    S.chunks.clear();
    S.lastFrameId = 0;
    S.frameMeta.clear();
    S.presentQueue = [];
    lastPingSendUs = 0;
};

// Start connection process
const startConnection = async () => {
    if (S.sessionToken) {
        updateLoadingStage(Stage.AUTH, 'Validating session...');

        if (await validateSession()) {
            updateLoadingStage(Stage.CONNECT, 'Connecting...');
            await connect();
            return;
        }
    }

    showAuthModal();
};

// Establish WebRTC connection
const connect = async () => {
    if (!S.sessionToken) {
        showAuthModal();
        return;
    }

    try {
        updateLoadingStage(Stage.CONNECT);
        resetState();

        // Create peer connection
        const pc = S.pc = new RTCPeerConnection({
            iceServers: [{ urls: 'stun:stun.l.google.com:19302' }],
            iceCandidatePoolSize: 4,
            bundlePolicy: 'max-bundle',
            rtcpMuxPolicy: 'require'
        });

        // Handle connection state changes
        pc.onconnectionstatechange = () => {
            if (pc.connectionState === 'connected') {
                connAttempts = 0;
            }

            if (pc.connectionState === 'failed' || pc.connectionState === 'disconnected') {
                if (authEl.overlay.classList.contains('visible')) {
                    return;
                }

                if (++connAttempts >= 3) {
                    clearSession();
                    showAuthModal('Connection failed. Please login again.');
                } else {
                    showLoading(true);
                    updateLoadingStage(Stage.ERR, 'Reconnecting...');
                    setTimeout(
                        () => startConnection(),
                        Math.min(1000 * connAttempts, 5000)
                    );
                }
            }
        };

        // Create data channel
        S.dc = pc.createDataChannel('screen', C.DC);
        setupDC();

        // Create and set local offer
        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);

        // Wait for ICE gathering
        await new Promise(resolve => {
            const timeout = setTimeout(resolve, 300);

            pc.addEventListener('icegatheringstatechange', () => {
                if (pc.iceGatheringState === 'complete') {
                    clearTimeout(timeout);
                    resolve();
                }
            });
        });

        // Send offer to server
        updateLoadingStage(Stage.CONNECT, 'Sending offer...');

        const res = await fetch(`${baseUrl}/api/offer`, {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                'Authorization': `Bearer ${S.sessionToken}`
            },
            body: JSON.stringify({
                sdp: pc.localDescription.sdp,
                type: pc.localDescription.type
            })
        });

        if (!res.ok) {
            const data = await res.json().catch(() => ({}));

            if (res.status === 401) {
                clearSession();
                showAuthModal('Session expired. Please login again.');
                return;
            }

            throw new Error(data.error || 'Server rejected offer');
        }

        // Set remote answer
        const answer = await res.json();
        await pc.setRemoteDescription(new RTCSessionDescription(answer));
    } catch (e) {
        throw e;
    }
};

// ============================================
// Client FPS Detection
// ============================================

export const detectFps = async () => {
    // Use screen API if available
    if (window.screen?.refreshRate) {
        return Math.round(window.screen.refreshRate);
    }

    // Fall back to frame timing measurement
    return new Promise(resolve => {
        let frameCount = 0;
        let lastTime = performance.now();
        const samples = [];

        const measure = now => {
            if (++frameCount > 1) {
                samples.push(now - lastTime);
            }
            lastTime = now;

            if (samples.length < 30) {
                requestAnimationFrame(measure);
            } else {
                // Calculate median interval
                samples.sort((a, b) => a - b);
                const median = samples[samples.length >> 1];

                // Match to common refresh rates
                const commonRates = [30, 60, 120, 144];
                const detected = commonRates.find(rate =>
                    Math.abs(rate - 1000 / median) <= 5
                ) || Math.round(1000 / median);

                resolve(detected);
            }
        };

        requestAnimationFrame(measure);
    });
};

// ============================================
// Cleanup
// ============================================

export const cleanup = () => {
    clearPing();
    stopMetricsLogger();
    stopPresentLoop();
    stopKeyframeRetryTimer?.();
    resetClockSync();
    S.dc?.close();
    S.pc?.close();
};

// ============================================
// Initialization
// ============================================

(async () => {
    // Set up network callbacks
    setNetCbs(applyFps, sendMonSel);

    // Detect client refresh rate
    S.clientFps = await detectFps();
    updateFpsOpts();
    enableControl();

    // Check for stored session
    const stored = getStoredSession();
    if (stored) {
        S.sessionToken = stored.token;
        S.username = stored.username;
    }

    showLoading(false);

    try {
        await startConnection();
    } catch (e) {
        showAuthModal('Connection failed: ' + e.message);
    }
})();

// Clean up on page unload
window.onbeforeunload = () => {
    cleanup();
    closeAudio();
};
