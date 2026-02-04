export const MSG = {
    PING: 0x504E4750, FPS_SET: 0x46505343, HOST_INFO: 0x484F5354, FPS_ACK: 0x46505341,
    REQUEST_KEY: 0x4B455952, MONITOR_LIST: 0x4D4F4E4C, MONITOR_SET: 0x4D4F4E53,
    AUDIO_DATA: 0x41554449, MOUSE_MOVE: 0x4D4F5645, MOUSE_BTN: 0x4D42544E,
    MOUSE_WHEEL: 0x4D57484C, KEY: 0x4B455920, CODEC_SET: 0x434F4443, CODEC_ACK: 0x434F4441,
    MOUSE_MOVE_REL: 0x4D4F5652, CLIPBOARD_DATA: 0x434C4950, CLIPBOARD_GET: 0x434C4754,
    KICKED: 0x4B49434B, CURSOR_CAPTURE: 0x43555243, CURSOR_SHAPE: 0x43555253
};

export const CURSOR_TYPES = {
    0:'default',1:'text',2:'pointer',3:'wait',4:'progress',5:'crosshair',6:'move',
    7:'ew-resize',8:'ns-resize',9:'nwse-resize',10:'nesw-resize',11:'not-allowed',
    12:'help',13:'none',255:'default'
};

export const CODECS = {
    AV1: { id: 0, name: 'AV1', codec: 'av01.0.05M.08' },
    H265: { id: 1, name: 'H.265', codec: 'hev1.1.6.L93.B0' },
    H264: { id: 2, name: 'H.264', codec: 'avc1.42001f' }
};

let codecCache = null;
export const detectCodecs = async () => {
    if (codecCache) return codecCache;
    const support = { av1: false, h265: false, h264: false, av1Hw: false, h265Hw: false, h264Hw: false };
    if (!window.VideoDecoder) return codecCache = { support, best: { codecId: 2, hardwareAccel: false, codecName: 'H.264' } };
    for (const [key, info] of Object.entries(CODECS)) {
        const k = key.toLowerCase();
        for (const hw of [true, false]) {
            try {
                const r = await VideoDecoder.isConfigSupported({ codec: info.codec, optimizeForLatency: true, hardwareAcceleration: hw ? 'prefer-hardware' : 'prefer-software' });
                if (r.supported) { support[k] = true; if (hw) support[`${k}Hw`] = true; }
            } catch (e) { console.warn(`[State] Codec check failed for ${key} (${hw ? 'HW' : 'SW'}):`, e.message); }
        }
    }
    const best = support.av1Hw ? { codecId: 0, hardwareAccel: true, codecName: 'AV1' }
        : support.av1 ? { codecId: 0, hardwareAccel: false, codecName: 'AV1' }
        : support.h265Hw ? { codecId: 1, hardwareAccel: true, codecName: 'H.265' }
        : support.h265 ? { codecId: 1, hardwareAccel: false, codecName: 'H.265' }
        : support.h264Hw ? { codecId: 2, hardwareAccel: true, codecName: 'H.264' }
        : { codecId: 2, hardwareAccel: false, codecName: 'H.264' };
    return codecCache = { support, best };
};

export const C = {
    HEADER: 21, AUDIO_HEADER: 16, PING_MS: 200, MAX_FRAMES: 8, FRAME_TIMEOUT_MS: 200,
    AUDIO_RATE: 48000, AUDIO_CH: 2,
    DC_CONTROL: { ordered: true, maxRetransmits: 3 },
    DC_VIDEO: { ordered: false, maxRetransmits: 0 },
    DC_AUDIO: { ordered: false, maxRetransmits: 0 },
    DC_INPUT: { ordered: true, maxRetransmits: 3 },
    JITTER_MAX_AGE_MS: 50, JITTER_SAMPLES: 60, CLOCK_OFFSET_SAMPLES: 8, METRICS_LOG_INTERVAL_MS: 1000
};

const mkClockSync = () => ({ offset: 0, offsetSamples: [], rttSamples: [], valid: false, sampleCount: 0, avgRttUs: 0 });
const mkJitterMetrics = () => ({ framesDroppedLate: 0, frameAgeSum: 0, frameAgeSamples: 0, presentIntervals: [], lastPresentTs: 0, serverAgeSum: 0, serverAgeSamples: 0, avgFrameAgeMs: 0, avgServerAgeMs: 0, intervalStdDev: 0, intervalMean: 0 });
const mkStats = () => ({ bytes: 0, moves: 0, clicks: 0, keys: 0, framesComplete: 0, framesDropped: 0, framesTimeout: 0, keyframesReceived: 0, decodeErrors: 0, renderErrors: 0, lastUpdate: performance.now() });
const mkAudioMetrics = () => ({ packetsReceived: 0, packetsDecoded: 0, packetsDropped: 0, bufferUnderruns: 0, bufferOverflows: 0, bufferHealthSum: 0, bufferHealthSamples: 0 });

export const S = {
    pc: null, dcControl: null, dcVideo: null, dcAudio: null, dcInput: null,
    decoder: null, ready: false, needKey: true, reinit: false, hwAccel: 'unknown',
    W: 0, H: 0, hostFps: 60, currentFps: 60, currentFpsMode: 0, fpsSent: false,
    authenticated: false, monitors: [], currentMon: 0, tabbedMode: false,
    audioCtx: null, audioEnabled: false, audioDecoder: null, audioGain: null,
    controlEnabled: false, lastVp: { x: 0, y: 0, w: 0, h: 0 },
    relativeMouseMode: false, pointerLocked: false,
    isReconnecting: false, firstFrameReceived: false,
    currentCodec: 1, codecSent: false, stats: mkStats(), clipboardSyncEnabled: false,
    chunks: new Map(), frameMeta: new Map(), lastFrameId: 0, username: null,
    clockSync: mkClockSync(), jitterMetrics: mkJitterMetrics(),
    networkMetrics: { packetsReceived: 0, videoPackets: 0, controlPackets: 0, audioPackets: 0, bytesReceived: 0 },
    decodeMetrics: { decodeCount: 0, decodeTimeSum: 0, maxQueueSize: 0 },
    renderMetrics: { renderCount: 0, renderTimeSum: 0 },
    audioMetrics: mkAudioMetrics(), tabVisible: true
};

export const allChannelsOpen = () => S.dcControl?.readyState === 'open' && S.dcVideo?.readyState === 'open' && S.dcAudio?.readyState === 'open' && S.dcInput?.readyState === 'open';
export const $ = id => document.getElementById(id);
export const mkBuf = (sz, fn) => { const b = new ArrayBuffer(sz); fn(new DataView(b)); return b; };
export const clientTimeUs = () => Math.floor((performance.timeOrigin + performance.now()) * 1000);
export const serverFrameAgeMs = ts => S.clockSync.valid ? Math.max(0, (clientTimeUs() - (ts - S.clockSync.offset)) / 1000) : 0;
export const logVideoDrop = reason => console.warn(`[Video Drop] ${reason}`);

const median = arr => [...arr].sort((a, b) => a - b)[Math.floor(arr.length / 2)];

export const updateClockOffset = (clientSendUs, serverTimeUs, clientRecvUs) => {
    const cs = S.clockSync, rttUs = clientRecvUs - clientSendUs;
    cs.offsetSamples.push(serverTimeUs - (clientSendUs + rttUs / 2));
    cs.rttSamples.push(rttUs);
    if (cs.offsetSamples.length > C.CLOCK_OFFSET_SAMPLES) cs.offsetSamples.shift();
    if (cs.rttSamples.length > C.CLOCK_OFFSET_SAMPLES) cs.rttSamples.shift();
    cs.offset = median(cs.offsetSamples);
    cs.avgRttUs = median(cs.rttSamples);
    cs.sampleCount++;
    cs.valid = cs.offsetSamples.length >= 2;
};

export const resetClockSync = () => { S.clockSync = mkClockSync(); };
export const getClockSyncStats = () => ({ offsetMs: S.clockSync.offset / 1000, rttMs: S.clockSync.avgRttUs / 1000, samples: S.clockSync.sampleCount, valid: S.clockSync.valid });

export const resetStats = () => {
    const result = { ...S.stats, elapsed: performance.now() - S.stats.lastUpdate };
    S.stats = mkStats();
    return result;
};

export const resetJitterMetrics = () => {
    const m = S.jitterMetrics;
    m.avgFrameAgeMs = m.frameAgeSamples > 0 ? m.frameAgeSum / m.frameAgeSamples : 0;
    m.avgServerAgeMs = m.serverAgeSamples > 0 ? m.serverAgeSum / m.serverAgeSamples : 0;
    if (m.presentIntervals.length > 1) {
        const mean = m.presentIntervals.reduce((a, b) => a + b, 0) / m.presentIntervals.length;
        m.intervalMean = mean;
        m.intervalStdDev = Math.sqrt(m.presentIntervals.reduce((s, v) => s + (v - mean) ** 2, 0) / m.presentIntervals.length);
    }
    const result = { avgFrameAgeMs: m.avgFrameAgeMs, avgServerAgeMs: m.avgServerAgeMs, intervalMean: m.intervalMean, intervalStdDev: m.intervalStdDev, framesDroppedLate: m.framesDroppedLate };
    Object.assign(m, mkJitterMetrics(), { lastPresentTs: m.lastPresentTs });
    return result;
};

export const resetNetworkMetrics = () => {
    const m = S.networkMetrics;
    const result = { packetsReceived: m.packetsReceived, videoPackets: m.videoPackets, controlPackets: m.controlPackets, audioPackets: m.audioPackets, bytesReceived: m.bytesReceived, avgPacketSize: m.packetsReceived > 0 ? m.bytesReceived / m.packetsReceived : 0 };
    Object.assign(m, { packetsReceived: 0, videoPackets: 0, controlPackets: 0, audioPackets: 0, bytesReceived: 0 });
    return result;
};

export const resetDecodeMetrics = () => {
    const m = S.decodeMetrics;
    const result = { decodeCount: m.decodeCount, maxQueueSize: m.maxQueueSize, avgDecodeTimeMs: m.decodeCount > 0 ? m.decodeTimeSum / m.decodeCount : 0 };
    Object.assign(m, { decodeCount: 0, decodeTimeSum: 0, maxQueueSize: 0 });
    return result;
};

export const resetRenderMetrics = () => {
    const m = S.renderMetrics;
    const result = { renderCount: m.renderCount, avgRenderTimeMs: m.renderCount > 0 ? m.renderTimeSum / m.renderCount : 0 };
    Object.assign(m, { renderCount: 0, renderTimeSum: 0 });
    return result;
};

export const resetAudioMetrics = () => {
    const m = S.audioMetrics;
    const result = { packetsReceived: m.packetsReceived, packetsDecoded: m.packetsDecoded, packetsDropped: m.packetsDropped, bufferUnderruns: m.bufferUnderruns, bufferOverflows: m.bufferOverflows, avgBufferHealth: m.bufferHealthSamples > 0 ? m.bufferHealthSum / m.bufferHealthSamples : 0 };
    Object.assign(m, mkAudioMetrics());
    return result;
};

export const recordAudioPacket = () => { S.audioMetrics.packetsReceived++; };
export const recordAudioDecoded = () => { S.audioMetrics.packetsDecoded++; };
export const recordAudioDrop = () => { S.audioMetrics.packetsDropped++; };
export const recordAudioBufferHealth = h => { S.audioMetrics.bufferHealthSum += h; S.audioMetrics.bufferHealthSamples++; };
export const recordAudioUnderrun = () => { S.audioMetrics.bufferUnderruns++; };
export const recordAudioOverflow = () => { S.audioMetrics.bufferOverflows++; };

export const recordPacket = (size, type) => {
    const m = S.networkMetrics;
    m.packetsReceived++; m.bytesReceived += size;
    m[type === 'video' ? 'videoPackets' : type === 'audio' ? 'audioPackets' : 'controlPackets']++;
};

export const recordDecodeTime = (timeMs, queueSize) => {
    S.decodeMetrics.decodeCount++; S.decodeMetrics.decodeTimeSum += timeMs;
    S.decodeMetrics.maxQueueSize = Math.max(S.decodeMetrics.maxQueueSize, queueSize);
};

export const recordRenderTime = timeMs => { S.renderMetrics.renderCount++; S.renderMetrics.renderTimeSum += timeMs; };

let metricsSubscribers = [], metricsLogInterval = null, uptimeSeconds = 0;
let sessionTotalFrames = 0, sessionTotalBytes = 0, sessionTotalDrops = 0, sessionTotalKeyframes = 0, sessionTotalDecodeErrors = 0;

export const subscribeToMetrics = callback => {
    metricsSubscribers.push(callback);
    return () => { metricsSubscribers = metricsSubscribers.filter(cb => cb !== callback); };
};

export const startMetricsLogger = () => {
    if (metricsLogInterval) return;
    uptimeSeconds = 0;
    metricsLogInterval = setInterval(() => {
        uptimeSeconds++;
        if (!allChannelsOpen()) return;
        const stats = resetStats(), jitter = resetJitterMetrics(), clock = getClockSyncStats();
        const network = resetNetworkMetrics(), decode = resetDecodeMetrics(), render = resetRenderMetrics(), audio = resetAudioMetrics();
        sessionTotalFrames += stats.framesComplete;
        sessionTotalBytes += stats.bytes;
        sessionTotalDrops += stats.framesDropped + jitter.framesDroppedLate;
        sessionTotalKeyframes += stats.keyframesReceived;
        sessionTotalDecodeErrors += stats.decodeErrors;
        const fps = stats.framesComplete, targetFps = S.currentFps || 60;
        const session = { totalFrames: sessionTotalFrames, totalBytes: sessionTotalBytes, totalDrops: sessionTotalDrops, totalKeyframes: sessionTotalKeyframes, totalDecodeErrors: sessionTotalDecodeErrors };
        const computed = { fps, targetFps, fpsEff: targetFps > 0 ? (fps / targetFps) * 100 : 0, mbps: (stats.bytes * 8 / 1048576).toFixed(2) };
        metricsSubscribers.forEach(cb => { try { cb({ uptime: uptimeSeconds, stats, jitter, clock, network, decode, render, audio, session, computed }); } catch (e) { console.warn('[State] Metrics subscriber error:', e.message); } });
    }, C.METRICS_LOG_INTERVAL_MS);
};

export const stopMetricsLogger = () => { clearInterval(metricsLogInterval); metricsLogInterval = null; };
export const resetSessionStats = () => { uptimeSeconds = sessionTotalFrames = sessionTotalBytes = sessionTotalDrops = sessionTotalKeyframes = sessionTotalDecodeErrors = 0; };
