export const MSG = {
    PING: 0x504E4750, FPS_SET: 0x46505343, HOST_INFO: 0x484F5354, FPS_ACK: 0x46505341,
    REQUEST_KEY: 0x4B455952, MONITOR_LIST: 0x4D4F4E4C, MONITOR_SET: 0x4D4F4E53,
    AUDIO_DATA: 0x41554449, MOUSE_MOVE: 0x4D4F5645, MOUSE_BTN: 0x4D42544E,
    MOUSE_WHEEL: 0x4D57484C, KEY: 0x4B455920, CODEC_SET: 0x434F4443, CODEC_ACK: 0x434F4441,
    MOUSE_MOVE_REL: 0x4D4F5652
};

export const CODECS = {
    H264: { id: 0, name: 'H.264', codec: 'avc1.42001f' },
    AV1: { id: 1, name: 'AV1', codec: 'av01.0.05M.08' }
};

const checkCodec = async (id, codec, name) => {
    for (const hw of [true, false]) {
        try {
            const r = await VideoDecoder.isConfigSupported({ codec, optimizeForLatency: true, hardwareAcceleration: hw ? 'prefer-hardware' : 'prefer-software' });
            if (r.supported) return { codecId: id, hardwareAccel: hw, codecName: name };
        } catch {}
    }
    return null;
};

export const detectBestCodec = async () => {
    if (!window.VideoDecoder) return { codecId: 0, hardwareAccel: false, codecName: 'H.264' };
    return await checkCodec(1, CODECS.AV1.codec, 'AV1') || await checkCodec(0, CODECS.H264.codec, 'H.264') || { codecId: 0, hardwareAccel: false, codecName: 'H.264' };
};

export const getCodecSupport = async () => {
    const support = { av1: false, h264: false, av1Hw: false, h264Hw: false };
    if (!window.VideoDecoder) return support;
    for (const [key, info] of Object.entries(CODECS)) {
        const k = key.toLowerCase();
        for (const hw of [true, false]) {
            try {
                const r = await VideoDecoder.isConfigSupported({ codec: info.codec, optimizeForLatency: true, hardwareAcceleration: hw ? 'prefer-hardware' : 'prefer-software' });
                if (r.supported) { support[k] = true; if (hw) support[`${k}Hw`] = true; }
            } catch {}
        }
    }
    return support;
};

export const C = {
    HEADER: 21, AUDIO_HEADER: 16, PING_MS: 200, MAX_FRAMES: 8, FRAME_TIMEOUT_MS: 200,
    AUDIO_RATE: 48000, AUDIO_CH: 2,
    DC_CONTROL: { ordered: true, maxRetransmits: 3 },
    DC_VIDEO: { ordered: false, maxRetransmits: 0 },
    DC_AUDIO: { ordered: false, maxRetransmits: 0 },
    DC_INPUT: { ordered: true, maxRetransmits: 3 },
    JITTER_MAX_FRAMES: 2, JITTER_MAX_AGE_MS: 50, PRESENT_INTERVAL_WINDOW: 60,
    CLOCK_OFFSET_SAMPLES: 8, METRICS_LOG_INTERVAL_MS: 1000
};

export const Stage = { IDLE: 'idle', CONNECT: 'connect', AUTH: 'auth', OK: 'connected', ERR: 'error' };

const mkClockSync = () => ({ offset: 0, offsetSamples: [], rttSamples: [], valid: false, sampleCount: 0, avgRttUs: 0 });
const mkJitterMetrics = () => ({ queueDepthSum: 0, queueDepthSamples: 0, maxQueueDepth: 0, framesDroppedLate: 0, framesDroppedOverflow: 0, frameAgeSum: 0, frameAgeSamples: 0, presentIntervals: [], lastPresentTs: 0, serverAgeSum: 0, serverAgeSamples: 0, avgQueueDepth: 0, avgFrameAgeMs: 0, avgServerAgeMs: 0, intervalStdDev: 0, intervalMean: 0, minFrameAgeMs: Infinity, maxFrameAgeMs: 0, minServerAgeMs: Infinity, maxServerAgeMs: 0 });
const mkStats = () => ({ recv: 0, dec: 0, rend: 0, bytes: 0, audio: 0, moves: 0, clicks: 0, keys: 0, chunksReceived: 0, framesComplete: 0, framesDropped: 0, framesTimeout: 0, keyframesReceived: 0, deltaFramesReceived: 0, decodeErrors: 0, renderErrors: 0, lastUpdate: performance.now() });
const mkAudioMetrics = () => ({ packetsReceived: 0, packetsDecoded: 0, packetsDropped: 0, bufferUnderruns: 0, bufferOverflows: 0, bufferHealthSum: 0, bufferHealthSamples: 0, minBufferMs: Infinity, maxBufferMs: 0, playbackRateSum: 0, playbackRateSamples: 0 });

export const S = {
    pc: null, dcControl: null, dcVideo: null, dcAudio: null, dcInput: null,
    decoder: null, ready: false, needKey: true, reinit: false, hwAccel: 'unknown',
    W: 0, H: 0, hostFps: 60, currentFps: 60, currentFpsMode: 0, fpsSent: false,
    authenticated: false, monitors: [], currentMon: 0, tabbedMode: false,
    audioCtx: null, audioEnabled: false, audioDecoder: null, audioGain: null,
    controlEnabled: false, lastVp: { x: 0, y: 0, w: 0, h: 0 },
    relativeMouseMode: false, pointerLocked: false,
    stage: Stage.IDLE, isReconnecting: false, firstFrameReceived: false,
    currentCodec: 1, codecSent: false, stats: mkStats(),
    sessionStats: { startTime: 0, totalFrames: 0, totalBytes: 0, totalDrops: 0, totalKeyframes: 0, totalDecodeErrors: 0 },
    chunks: new Map(), frameMeta: new Map(), lastFrameId: 0, sessionToken: null, username: null,
    presentQueue: [], presentLoopRunning: false, clockSync: mkClockSync(), jitterMetrics: mkJitterMetrics(),
    networkMetrics: { packetsReceived: 0, videoPackets: 0, controlPackets: 0, audioPackets: 0, bytesReceived: 0 },
    decodeMetrics: { decodeCount: 0, decodeTimeSum: 0, maxQueueSize: 0 },
    renderMetrics: { renderCount: 0, renderTimeSum: 0 },
    audioMetrics: mkAudioMetrics(),
    tabVisible: true
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

export const getClockSyncStats = () => {
    const cs = S.clockSync;
    return { offsetMs: cs.offset / 1000, rttMs: cs.avgRttUs / 1000, samples: cs.sampleCount, valid: cs.valid };
};

export const resetStats = () => {
    const result = { ...S.stats, elapsed: performance.now() - S.stats.lastUpdate };
    S.stats = mkStats();
    return result;
};

export const resetJitterMetrics = () => {
    const m = S.jitterMetrics;
    m.avgQueueDepth = m.queueDepthSamples > 0 ? m.queueDepthSum / m.queueDepthSamples : 0;
    m.avgFrameAgeMs = m.frameAgeSamples > 0 ? m.frameAgeSum / m.frameAgeSamples : 0;
    m.avgServerAgeMs = m.serverAgeSamples > 0 ? m.serverAgeSum / m.serverAgeSamples : 0;
    if (m.presentIntervals.length > 1) {
        const mean = m.presentIntervals.reduce((a, b) => a + b, 0) / m.presentIntervals.length;
        m.intervalMean = mean;
        m.intervalStdDev = Math.sqrt(m.presentIntervals.reduce((s, v) => s + (v - mean) ** 2, 0) / m.presentIntervals.length);
    }
    const result = { avgQueueDepth: m.avgQueueDepth, maxQueueDepth: m.maxQueueDepth, avgFrameAgeMs: m.avgFrameAgeMs, avgServerAgeMs: m.avgServerAgeMs, intervalMean: m.intervalMean, intervalStdDev: m.intervalStdDev, framesDroppedLate: m.framesDroppedLate, framesDroppedOverflow: m.framesDroppedOverflow };
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
    const result = { packetsReceived: m.packetsReceived, packetsDecoded: m.packetsDecoded, packetsDropped: m.packetsDropped, bufferUnderruns: m.bufferUnderruns, bufferOverflows: m.bufferOverflows, avgBufferHealth: m.bufferHealthSamples > 0 ? m.bufferHealthSum / m.bufferHealthSamples : 0, avgPlaybackRate: m.playbackRateSamples > 0 ? m.playbackRateSum / m.playbackRateSamples : 1.0 };
    Object.assign(m, mkAudioMetrics());
    return result;
};

export const recordAudioPacket = () => { S.audioMetrics.packetsReceived++; };
export const recordAudioDecoded = () => { S.audioMetrics.packetsDecoded++; };
export const recordAudioDrop = () => { S.audioMetrics.packetsDropped++; };
export const recordAudioBufferHealth = (healthMs, playbackRate) => {
    const m = S.audioMetrics;
    m.bufferHealthSum += healthMs; m.bufferHealthSamples++;
    m.minBufferMs = Math.min(m.minBufferMs, healthMs);
    m.maxBufferMs = Math.max(m.maxBufferMs, healthMs);
    m.playbackRateSum += playbackRate; m.playbackRateSamples++;
};
export const recordAudioUnderrun = () => { S.audioMetrics.bufferUnderruns++; };
export const recordAudioOverflow = () => { S.audioMetrics.bufferOverflows++; };

export const recordPacket = (size, type) => {
    const m = S.networkMetrics;
    m.packetsReceived++; m.bytesReceived += size;
    m[type === 'video' ? 'videoPackets' : type === 'audio' ? 'audioPackets' : 'controlPackets']++;
};

export const recordDecodeTime = (timeMs, queueSize) => {
    const m = S.decodeMetrics;
    m.decodeCount++; m.decodeTimeSum += timeMs;
    m.maxQueueSize = Math.max(m.maxQueueSize, queueSize);
};

export const recordRenderTime = timeMs => {
    S.renderMetrics.renderCount++;
    S.renderMetrics.renderTimeSum += timeMs;
};

let metricsSubscribers = [], metricsLogInterval = null, uptimeSeconds = 0;

export const subscribeToMetrics = callback => {
    metricsSubscribers.push(callback);
    return () => { metricsSubscribers = metricsSubscribers.filter(cb => cb !== callback); };
};

export const startMetricsLogger = () => {
    if (metricsLogInterval) return;
    S.sessionStats.startTime = performance.now();
    uptimeSeconds = 0;
    metricsLogInterval = setInterval(() => {
        uptimeSeconds++;
        if (!allChannelsOpen()) return;
        const stats = resetStats(), jitter = resetJitterMetrics(), clock = getClockSyncStats();
        const network = resetNetworkMetrics(), decode = resetDecodeMetrics(), render = resetRenderMetrics();
        const audio = resetAudioMetrics(), ss = S.sessionStats;
        ss.totalFrames += stats.framesComplete;
        ss.totalBytes += stats.bytes;
        ss.totalDrops += stats.framesDropped + jitter.framesDroppedLate + jitter.framesDroppedOverflow;
        ss.totalKeyframes += stats.keyframesReceived;
        ss.totalDecodeErrors += stats.decodeErrors;
        const fps = stats.framesComplete, targetFps = S.currentFps || 60;
        const fpsEff = targetFps > 0 ? (fps / targetFps) * 100 : 0;
        const mbps = (stats.bytes * 8 / 1048576).toFixed(2);
        metricsSubscribers.forEach(cb => { try { cb({ uptime: uptimeSeconds, stats, jitter, clock, network, decode, render, audio, session: { ...ss }, computed: { fps, targetFps, fpsEff, mbps } }); } catch {} });
    }, C.METRICS_LOG_INTERVAL_MS);
};

export const stopMetricsLogger = () => { clearInterval(metricsLogInterval); metricsLogInterval = null; };

export const resetSessionStats = () => {
    uptimeSeconds = 0;
    Object.assign(S.sessionStats, { startTime: performance.now(), totalFrames: 0, totalBytes: 0, totalDrops: 0, totalKeyframes: 0, totalDecodeErrors: 0 });
};
