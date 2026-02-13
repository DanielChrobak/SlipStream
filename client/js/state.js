export const MSG = {
    PING: 0x504E4750, FPS_SET: 0x46505343, HOST_INFO: 0x484F5354, FPS_ACK: 0x46505341,
    REQUEST_KEY: 0x4B455952, MONITOR_LIST: 0x4D4F4E4C, MONITOR_SET: 0x4D4F4E53,
    AUDIO_DATA: 0x41554449, MOUSE_MOVE: 0x4D4F5645, MOUSE_BTN: 0x4D42544E,
    MOUSE_WHEEL: 0x4D57484C, KEY: 0x4B455920, CODEC_SET: 0x434F4443, CODEC_ACK: 0x434F4441,
    MOUSE_MOVE_REL: 0x4D4F5652, CLIPBOARD_DATA: 0x434C4950, CLIPBOARD_GET: 0x434C4754,
    KICKED: 0x4B49434B, CURSOR_CAPTURE: 0x43555243, CURSOR_SHAPE: 0x43555253, AUDIO_ENABLE: 0x41554445,
    MIC_DATA: 0x4D494344, MIC_ENABLE: 0x4D494345
};

export const CURSOR_TYPES = {0:'default',1:'text',2:'pointer',3:'wait',4:'progress',5:'crosshair',6:'move',7:'ew-resize',8:'ns-resize',9:'nwse-resize',10:'nesw-resize',11:'not-allowed',12:'help',13:'none',255:'default'};

export const CODECS = {
    AV1: { id: 0, name: 'AV1', codec: 'av01.0.05M.08' },
    H265: { id: 1, name: 'H.265', codec: 'hev1.1.6.L93.B0' },
    H264: { id: 2, name: 'H.264', codec: 'avc1.42001f' }
};

// Simplified logging - level 0=err,1=warn,2=info,3=dbg
const LL = 2, LM = ['error','warn','info','log'];
const _log = (l, m, msg, d) => l <= LL && console[LM[l]](`[${m}] ${msg}`, d && Object.keys(d).length ? d : '');
export const log = {
    error: (m, msg, d) => _log(0, m, msg, d),
    warn: (m, msg, d) => _log(1, m, msg, d),
    info: (m, msg, d) => _log(2, m, msg, d),
    debug: (m, msg, d) => _log(3, m, msg, d)
};

// Universal safe executor - replaces all try-catch patterns
export const safe = (fn, fallback) => { try { return fn(); } catch(e) { log.warn('ERR', e.message); return fallback; } };
export const safeAsync = async (fn, fallback) => { try { return await fn(); } catch(e) { log.warn('ERR', e.message); return fallback; } };

// Drop loggers with stats increment
export const logVideoDrop = (r, d) => { log.warn('VIDEO', r, d); S.stats.framesDropped++; };
export const logAudioDrop = (r, d) => { log.warn('AUDIO', r, d); S.audioMetrics.packetsDropped++; };
export const logNetworkDrop = (r, d) => log.warn('NET', r, d);

let codecCache = null;
export const detectCodecs = async () => {
    if (codecCache) return codecCache;
    const sup = { av1: 0, h265: 0, h264: 0, av1Hw: 0, h265Hw: 0, h264Hw: 0 };
    if (!window.VideoDecoder) return codecCache = { support: sup, best: { codecId: 2, hardwareAccel: 0, codecName: 'H.264' } };
    for (const [key, info] of Object.entries(CODECS)) {
        const k = key.toLowerCase();
        for (const hw of [1, 0]) {
            const r = await safeAsync(() => VideoDecoder.isConfigSupported({ codec: info.codec, optimizeForLatency: 1, hardwareAcceleration: hw ? 'prefer-hardware' : 'prefer-software' }));
            if (r?.supported) { sup[k] = 1; if (hw) sup[k+'Hw'] = 1; }
        }
    }
    const best = sup.av1Hw ? { codecId: 0, hardwareAccel: 1, codecName: 'AV1' }
        : sup.av1 ? { codecId: 0, hardwareAccel: 0, codecName: 'AV1' }
        : sup.h265Hw ? { codecId: 1, hardwareAccel: 1, codecName: 'H.265' }
        : sup.h265 ? { codecId: 1, hardwareAccel: 0, codecName: 'H.265' }
        : { codecId: 2, hardwareAccel: sup.h264Hw, codecName: 'H.264' };
    return codecCache = { support: sup, best };
};

export const C = {
    HEADER: 21, AUDIO_HEADER: 16, PING_MS: 200, MAX_FRAMES: 8, FRAME_TIMEOUT_MS: 200,
    AUDIO_RATE: 48000, AUDIO_CH: 2, MIC_HEADER: 16, MIC_RATE: 48000, MIC_CH: 1, MIC_FRAME_MS: 10,
    DC_CONTROL: { ordered: 1, maxRetransmits: 3 }, DC_VIDEO: { ordered: 1, maxRetransmits: 1 },
    DC_AUDIO: { ordered: 1, maxRetransmits: 1 }, DC_INPUT: { ordered: 1, maxRetransmits: 3 },
    DC_MIC: { ordered: 1, maxRetransmits: 1 },
    JITTER_MAX_AGE_MS: 50, JITTER_SAMPLES: 60, CLOCK_OFFSET_SAMPLES: 8, METRICS_LOG_INTERVAL_MS: 1000
};

// Generic metric factory
const mkMetric = defs => () => ({ ...defs });
const mkClockSync = mkMetric({ offset: 0, offsetSamples: [], rttSamples: [], valid: 0, sampleCount: 0, avgRttUs: 0 });
const mkJitter = mkMetric({ framesDroppedLate: 0, frameAgeSum: 0, frameAgeSamples: 0, presentIntervals: [], lastPresentTs: 0, serverAgeSum: 0, serverAgeSamples: 0, avgFrameAgeMs: 0, avgServerAgeMs: 0, intervalStdDev: 0, intervalMean: 0 });
const mkStats = mkMetric({ bytes: 0, moves: 0, clicks: 0, keys: 0, framesComplete: 0, framesDropped: 0, framesTimeout: 0, keyframesReceived: 0, decodeErrors: 0, renderErrors: 0, lastUpdate: 0 });
const mkAudio = mkMetric({ packetsReceived: 0, packetsDecoded: 0, packetsDropped: 0, bufferUnderruns: 0, bufferOverflows: 0, bufferHealthSum: 0, bufferHealthSamples: 0 });
const mkNetwork = mkMetric({ packetsReceived: 0, videoPackets: 0, controlPackets: 0, audioPackets: 0, micPackets: 0, bytesReceived: 0 });
const mkDecode = mkMetric({ decodeCount: 0, decodeTimeSum: 0, maxQueueSize: 0 });
const mkRender = mkMetric({ renderCount: 0, renderTimeSum: 0 });
const mkMic = mkMetric({ packetsSent: 0, packetsDropped: 0, encodeErrors: 0, bytesSent: 0 });

export const S = {
    pc: null, dcControl: null, dcVideo: null, dcAudio: null, dcInput: null, dcMic: null,
    decoder: null, ready: 0, needKey: 1, reinit: 0, hwAccel: 'unknown',
    W: 0, H: 0, hostFps: 60, currentFps: 60, currentFpsMode: 0, fpsSent: 0,
    authenticated: 0, monitors: [], currentMon: 0, tabbedMode: 0,
    audioCtx: null, audioEnabled: 0, audioDecoder: null, audioGain: null,
    controlEnabled: 0, lastVp: { x: 0, y: 0, w: 0, h: 0 },
    relativeMouseMode: 0, pointerLocked: 0, isReconnecting: 0, firstFrameReceived: 0,
    currentCodec: 1, codecSent: 0, stats: { ...mkStats(), lastUpdate: performance.now() }, clipboardSyncEnabled: 0,
    chunks: new Map(), frameMeta: new Map(), lastFrameId: 0, username: null,
    clockSync: mkClockSync(), jitterMetrics: mkJitter(),
    networkMetrics: mkNetwork(), decodeMetrics: mkDecode(),
    renderMetrics: mkRender(), audioMetrics: mkAudio(),
    micMetrics: mkMic(), micEnabled: 0, micStream: null
};

export const $ = id => document.getElementById(id);
export const mkBuf = (sz, fn) => { const b = new ArrayBuffer(sz); fn(new DataView(b)); return b; };
export const clientTimeUs = () => Math.floor((performance.timeOrigin + performance.now()) * 1000);
export const serverFrameAgeMs = ts => S.clockSync.valid ? Math.max(0, (clientTimeUs() - (ts - S.clockSync.offset)) / 1000) : 0;

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

// Unified metric reset with computed fields
export const resetStats = () => { const r = { ...S.stats, elapsed: performance.now() - S.stats.lastUpdate }; S.stats = { ...mkStats(), lastUpdate: performance.now() }; return r; };

export const resetJitterMetrics = () => {
    const m = S.jitterMetrics;
    m.avgFrameAgeMs = m.frameAgeSamples > 0 ? m.frameAgeSum / m.frameAgeSamples : 0;
    m.avgServerAgeMs = m.serverAgeSamples > 0 ? m.serverAgeSum / m.serverAgeSamples : 0;
    if (m.presentIntervals.length > 1) {
        const n = m.presentIntervals.reduce((a, b) => a + b, 0) / m.presentIntervals.length;
        m.intervalMean = n;
        m.intervalStdDev = Math.sqrt(m.presentIntervals.reduce((s, v) => s + (v - n) ** 2, 0) / m.presentIntervals.length);
    }
    const lt = m.lastPresentTs;
    const r = { avgFrameAgeMs: m.avgFrameAgeMs, avgServerAgeMs: m.avgServerAgeMs, intervalMean: m.intervalMean, intervalStdDev: m.intervalStdDev, framesDroppedLate: m.framesDroppedLate };
    S.jitterMetrics = { ...mkJitter(), lastPresentTs: lt };
    return r;
};

const avgMetric = (m, sum, count) => count > 0 ? sum / count : 0;
export const resetNetworkMetrics = () => { const m = S.networkMetrics; const r = { ...m, avgPacketSize: avgMetric(m, m.bytesReceived, m.packetsReceived) }; S.networkMetrics = mkNetwork(); return r; };
export const resetDecodeMetrics = () => { const m = S.decodeMetrics; const r = { ...m, avgDecodeTimeMs: avgMetric(m, m.decodeTimeSum, m.decodeCount) }; S.decodeMetrics = mkDecode(); return r; };
export const resetRenderMetrics = () => { const m = S.renderMetrics; const r = { ...m, avgRenderTimeMs: avgMetric(m, m.renderTimeSum, m.renderCount) }; S.renderMetrics = mkRender(); return r; };
export const resetAudioMetrics = () => { const m = S.audioMetrics; const r = { ...m, avgBufferHealth: avgMetric(m, m.bufferHealthSum, m.bufferHealthSamples) }; S.audioMetrics = mkAudio(); return r; };
export const resetMicMetrics = () => { const r = { ...S.micMetrics }; S.micMetrics = mkMic(); return r; };

// Simplified metric recorders
export const recordAudioPacket = () => S.audioMetrics.packetsReceived++;
export const recordAudioDecoded = () => S.audioMetrics.packetsDecoded++;
export const recordAudioBufferHealth = h => { S.audioMetrics.bufferHealthSum += h; S.audioMetrics.bufferHealthSamples++; };
export const recordAudioUnderrun = () => S.audioMetrics.bufferUnderruns++;
export const recordAudioOverflow = () => S.audioMetrics.bufferOverflows++;
export const recordMicPacket = b => { S.micMetrics.packetsSent++; S.micMetrics.bytesSent += b; };
export const recordMicEncodeError = () => S.micMetrics.encodeErrors++;

export const recordPacket = (sz, t) => {
    const m = S.networkMetrics;
    m.packetsReceived++; m.bytesReceived += sz;
    m[t === 'video' ? 'videoPackets' : t === 'audio' ? 'audioPackets' : t === 'mic' ? 'micPackets' : 'controlPackets']++;
};

export const recordDecodeTime = (t, q) => { S.decodeMetrics.decodeCount++; S.decodeMetrics.decodeTimeSum += t; S.decodeMetrics.maxQueueSize = Math.max(S.decodeMetrics.maxQueueSize, q); };
export const recordRenderTime = t => { S.renderMetrics.renderCount++; S.renderMetrics.renderTimeSum += t; };

let metricsSubs = [], metricsInt = null, uptime = 0, sesFrames = 0, sesBytes = 0, sesDrops = 0;

export const subscribeToMetrics = cb => { metricsSubs.push(cb); return () => { metricsSubs = metricsSubs.filter(c => c !== cb); }; };

const allOpen = () => ['dcControl', 'dcVideo', 'dcAudio', 'dcInput', 'dcMic'].every(k => S[k]?.readyState === 'open');

export const startMetricsLogger = () => {
    if (metricsInt) return;
    uptime = 0;
    metricsInt = setInterval(() => {
        uptime++;
        if (!allOpen()) return;
        const stats = resetStats(), jitter = resetJitterMetrics(), clock = getClockSyncStats();
        const network = resetNetworkMetrics(), decode = resetDecodeMetrics(), render = resetRenderMetrics(), audio = resetAudioMetrics(), mic = resetMicMetrics();
        sesFrames += stats.framesComplete; sesBytes += stats.bytes; sesDrops += stats.framesDropped + jitter.framesDroppedLate;
        const fps = stats.framesComplete, tFps = S.currentFps || 60;
        const session = { totalFrames: sesFrames, totalBytes: sesBytes, totalDrops: sesDrops };
        const computed = { fps, targetFps: tFps, fpsEff: tFps > 0 ? (fps / tFps) * 100 : 0, mbps: (stats.bytes * 8 / 1048576).toFixed(2) };
        metricsSubs.forEach(cb => safe(() => cb({ uptime, stats, jitter, clock, network, decode, render, audio, mic, session, computed })));
    }, C.METRICS_LOG_INTERVAL_MS);
};

export const stopMetricsLogger = () => { clearInterval(metricsInt); metricsInt = null; };
export const resetSessionStats = () => { uptime = sesFrames = sesBytes = sesDrops = 0; };
