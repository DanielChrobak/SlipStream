
export const MSG = {
    PING: 0x504E4750, FPS_SET: 0x46505343, HOST_INFO: 0x484F5354, FPS_ACK: 0x46505341,
    REQUEST_KEY: 0x4B455952, MONITOR_LIST: 0x4D4F4E4C, MONITOR_SET: 0x4D4F4E53,
    AUDIO_DATA: 0x41554449, MOUSE_MOVE: 0x4D4F5645, MOUSE_BTN: 0x4D42544E,
    MOUSE_WHEEL: 0x4D57484C, KEY: 0x4B455920, CODEC_SET: 0x434F4443, CODEC_ACK: 0x434F4441,
    CODEC_CAPS: 0x434F4350, MOUSE_MOVE_REL: 0x4D4F5652, CLIPBOARD_DATA: 0x434C4950,
    CLIPBOARD_GET: 0x434C4754, KICKED: 0x4B49434B, CURSOR_CAPTURE: 0x43555243,
    CURSOR_SHAPE: 0x43555253, AUDIO_ENABLE: 0x41554445, MIC_DATA: 0x4D494344, MIC_ENABLE: 0x4D494345,
    VERSION: 0x56455253
};

export const CURSOR_TYPES = ['default','text','pointer','wait','progress','crosshair','move',
    'ew-resize','ns-resize','nwse-resize','nesw-resize','not-allowed','help','none'];

export const CODECS = {
    AV1:  { id: 0, name: 'AV1',   codec: 'av01.0.05M.08' },
    H265: { id: 1, name: 'H.265', codec: 'hev1.1.6.L93.B0' },
    H264: { id: 2, name: 'H.264', codec: 'avc1.42001f' }
};

export const CODEC_KEYS = ['av1', 'h265', 'h264'];
const LOG_LEVEL = { ERROR: 0, WARN: 1, INFO: 2, DEBUG: 3 };
const CURRENT_LOG_LEVEL = 2;
const LOG_METHODS = ['error', 'warn', 'info', 'log'];

const _log = (level, module, msg, data) => {
    if (level > CURRENT_LOG_LEVEL) return;
    const prefix = `[${module}]`;
    const hasData = data && Object.keys(data).length > 0;
    console[LOG_METHODS[level]](prefix, msg, hasData ? data : '');
};

export const log = {
    error: (m, msg, d) => _log(LOG_LEVEL.ERROR, m, msg, d),
    warn:  (m, msg, d) => _log(LOG_LEVEL.WARN, m, msg, d),
    info:  (m, msg, d) => _log(LOG_LEVEL.INFO, m, msg, d),
    debug: (m, msg, d) => _log(LOG_LEVEL.DEBUG, m, msg, d)
};
export const safe = (fn, fallback, context = 'SAFE') => {
    try { return fn(); }
    catch (e) {
        log.error(context, 'Exception caught', { error: e.message, stack: e.stack?.split('\n')[1]?.trim() });
        return fallback;
    }
};

export const safeAsync = async (fn, fallback, context = 'ASYNC') => {
    try { return await fn(); }
    catch (e) {
        log.error(context, 'Async exception caught', { error: e.message, stack: e.stack?.split('\n')[1]?.trim() });
        return fallback;
    }
};
export const logVideoDrop = (reason, data) => {
    S.stats.framesDropped++;
    log.warn('VIDEO', `Drop: ${reason}`, { ...data, total: S.stats.framesDropped });
};

export const logAudioDrop = (reason, data) => {
    S.audioMetrics.packetsDropped++;
    log.warn('AUDIO', `Drop: ${reason}`, { ...data, total: S.audioMetrics.packetsDropped });
};

export const logNetworkDrop = (reason, data) => {
    log.warn('NET', `Issue: ${reason}`, data);
};
let codecCache = null;

export const detectCodecs = async () => {
    if (codecCache) return codecCache;

    const sup = { av1: 0, h265: 0, h264: 0, av1Hw: 0, h265Hw: 0, h264Hw: 0 };

    if (!window.VideoDecoder) {
        log.warn('CODEC', 'VideoDecoder API not available');
        codecCache = { support: sup, best: { codecId: 2, hardwareAccel: 0, codecName: 'H.264' } };
        return codecCache;
    }

    for (const [key, info] of Object.entries(CODECS)) {
        const k = key.toLowerCase();
        for (const hw of [1, 0]) {
            const cfg = {
                codec: info.codec,
                optimizeForLatency: 1,
                hardwareAcceleration: hw ? 'prefer-hardware' : 'prefer-software'
            };
            const result = await safeAsync(
                () => VideoDecoder.isConfigSupported(cfg),
                null,
                'CODEC'
            );
            if (result?.supported) {
                sup[k] = 1;
                if (hw) sup[k + 'Hw'] = 1;
                log.debug('CODEC', `${info.name} supported`, { hw: hw ? 'HW' : 'SW' });
            }
        }
    }

    const best = sup.av1Hw  ? { codecId: 0, hardwareAccel: 1, codecName: 'AV1' }
                : sup.av1   ? { codecId: 0, hardwareAccel: 0, codecName: 'AV1' }
                : sup.h265Hw? { codecId: 1, hardwareAccel: 1, codecName: 'H.265' }
                : sup.h265  ? { codecId: 1, hardwareAccel: 0, codecName: 'H.265' }
                : { codecId: 2, hardwareAccel: sup.h264Hw, codecName: 'H.264' };

    log.info('CODEC', 'Detection complete', { best: best.codecName, hw: best.hardwareAccel });
    codecCache = { support: sup, best };
    return codecCache;
};
export const C = {
    HEADER: 31, AUDIO_HEADER: 16, PING_MS: 200, MAX_FRAMES: 20, FRAME_TIMEOUT_MS: 900,
    KEY_REQ_MIN_INTERVAL_MS: 350, KEY_RETRY_INTERVAL_MS: 700,
    FEC_GROUP_SIZE: 4,
    AUDIO_RATE: 48000, AUDIO_CH: 2,
    MIC_HEADER: 16, MIC_RATE: 48000, MIC_CH: 1, MIC_FRAME_MS: 10,
    DC_CONTROL: { ordered: 1, maxRetransmits: 3 },
    DC_VIDEO:   { ordered: 1, maxRetransmits: 0 },
    DC_AUDIO:   { ordered: 1, maxRetransmits: 1 },
    DC_INPUT:   { ordered: 1, maxRetransmits: 3 },
    DC_MIC:     { ordered: 1, maxRetransmits: 1 },
    JITTER_MAX_AGE_MS: 50, JITTER_SAMPLES: 60,
    CLOCK_OFFSET_SAMPLES: 8, METRICS_LOG_INTERVAL_MS: 1000
};
const mkClockSync = () => ({ offset: 0, offsetSamples: [], rttSamples: [], valid: 0, sampleCount: 0, avgRttUs: 0 });
const mkJitter = () => ({ framesDroppedLate: 0, frameAgeSum: 0, frameAgeSamples: 0, presentIntervals: [],
    lastPresentTs: 0, serverAgeSum: 0, serverAgeSamples: 0, avgFrameAgeMs: 0, avgServerAgeMs: 0,
    intervalStdDev: 0, intervalMean: 0 });
const mkStats = () => ({ bytes: 0, moves: 0, clicks: 0, keys: 0, framesComplete: 0, framesDropped: 0,
    framesTimeout: 0, keyframesReceived: 0, decodeErrors: 0, renderErrors: 0, lastUpdate: performance.now() });
const mkAudio = () => ({ packetsReceived: 0, packetsDecoded: 0, packetsDropped: 0,
    bufferUnderruns: 0, bufferOverflows: 0, bufferHealthSum: 0, bufferHealthSamples: 0 });
const mkNetwork = () => ({ packetsReceived: 0, videoPackets: 0, controlPackets: 0, audioPackets: 0,
    micPackets: 0, bytesReceived: 0 });
const mkDecode = () => ({ decodeCount: 0, decodeTimeSum: 0, maxQueueSize: 0 });
const mkRender = () => ({ renderCount: 0, renderTimeSum: 0 });
const mkMic = () => ({ packetsSent: 0, packetsDropped: 0, encodeErrors: 0, bytesSent: 0 });
export const S = {
    pc: null, dcControl: null, dcVideo: null, dcAudio: null, dcInput: null, dcMic: null,
    decoder: null, ready: 0, needKey: 1, reinit: 0, hwAccel: 'unknown',
    W: 0, H: 0, hostFps: 60, currentFps: 60, currentFpsMode: 0, fpsSent: 0,
    authenticated: 0, monitors: [], currentMon: 0, tabbedMode: 0, username: null,
    audioCtx: null, audioEnabled: 0, audioDecoder: null, audioGain: null,
    controlEnabled: 0, lastVp: { x: 0, y: 0, w: 0, h: 0 },
    relativeMouseMode: 0, pointerLocked: 0,
    isReconnecting: 0, firstFrameReceived: 0,
    currentCodec: 1, codecSent: 0, hostCodecs: 0x07,
    clipboardSyncEnabled: 0,
    chunks: new Map(), frameMeta: new Map(), lastFrameId: 0,
    stats: mkStats(), clockSync: mkClockSync(), jitterMetrics: mkJitter(),
    networkMetrics: mkNetwork(), decodeMetrics: mkDecode(),
    renderMetrics: mkRender(), audioMetrics: mkAudio(),
    micMetrics: mkMic(), micEnabled: 0, micStream: null,
    hostVersion: null
};
export const $ = id => document.querySelector(`#${id}`);
export const mkBuf = (sz, fn) => { const b = new ArrayBuffer(sz); fn(new DataView(b)); return b; };
export const clientTimeUs = () => Math.floor((performance.timeOrigin + performance.now()) * 1000);

export const serverFrameAgeMs = ts => {
    if (!S.clockSync.valid) return 0;
    return Math.max(0, (clientTimeUs() - (ts - S.clockSync.offset)) / 1000);
};

const median = arr => {
    if (!arr.length) return 0;
    const sorted = [...arr].sort((a, b) => a - b);
    return sorted[Math.floor(sorted.length / 2)];
};
export const updateClockOffset = (clientSendUs, serverTimeUs, clientRecvUs) => {
    const cs = S.clockSync;
    const rttUs = clientRecvUs - clientSendUs;

    if (rttUs < 0 || rttUs > 5000000) {
        log.warn('CLOCK', 'Invalid RTT', { rttUs, clientSendUs, serverTimeUs, clientRecvUs });
        return;
    }

    cs.offsetSamples.push(serverTimeUs - (clientSendUs + rttUs / 2));
    cs.rttSamples.push(rttUs);

    if (cs.offsetSamples.length > C.CLOCK_OFFSET_SAMPLES) cs.offsetSamples.shift();
    if (cs.rttSamples.length > C.CLOCK_OFFSET_SAMPLES) cs.rttSamples.shift();

    cs.offset = median(cs.offsetSamples);
    cs.avgRttUs = median(cs.rttSamples);
    cs.sampleCount++;
    cs.valid = cs.offsetSamples.length >= 2;

    if (cs.sampleCount % 10 === 0) {
        log.debug('CLOCK', 'Sync update', { offsetMs: (cs.offset/1000).toFixed(2), rttMs: (cs.avgRttUs/1000).toFixed(2) });
    }
};

export const resetClockSync = () => {
    S.clockSync = mkClockSync();
    log.debug('CLOCK', 'Reset');
};

export const getClockSyncStats = () => ({
    offsetMs: S.clockSync.offset / 1000,
    rttMs: S.clockSync.avgRttUs / 1000,
    samples: S.clockSync.sampleCount,
    valid: S.clockSync.valid
});
export const resetStats = () => {
    const elapsed = performance.now() - S.stats.lastUpdate;
    const result = { ...S.stats, elapsed };
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
        m.intervalStdDev = Math.sqrt(
            m.presentIntervals.reduce((s, v) => s + (v - mean) ** 2, 0) / m.presentIntervals.length
        );
    }

    const result = {
        avgFrameAgeMs: m.avgFrameAgeMs, avgServerAgeMs: m.avgServerAgeMs,
        intervalMean: m.intervalMean, intervalStdDev: m.intervalStdDev,
        framesDroppedLate: m.framesDroppedLate
    };

    const lastTs = m.lastPresentTs;
    S.jitterMetrics = mkJitter();
    S.jitterMetrics.lastPresentTs = lastTs;
    return result;
};

const avg = (sum, count) => count > 0 ? sum / count : 0;

const resetMetric = (key, mkFn, avgKey, sumKey, countKey) => {
    const m = S[key];
    const result = { ...m };
    if (avgKey) result[avgKey] = avg(m[sumKey], m[countKey]);
    S[key] = mkFn();
    return result;
};

export const resetNetworkMetrics = () => resetMetric('networkMetrics', mkNetwork, 'avgPacketSize', 'bytesReceived', 'packetsReceived');
export const resetDecodeMetrics = () => resetMetric('decodeMetrics', mkDecode, 'avgDecodeTimeMs', 'decodeTimeSum', 'decodeCount');
export const resetRenderMetrics = () => resetMetric('renderMetrics', mkRender, 'avgRenderTimeMs', 'renderTimeSum', 'renderCount');
export const resetAudioMetrics = () => resetMetric('audioMetrics', mkAudio, 'avgBufferHealth', 'bufferHealthSum', 'bufferHealthSamples');
export const resetMicMetrics = () => resetMetric('micMetrics', mkMic);
export const recordAudioPacket = () => { S.audioMetrics.packetsReceived++; };
export const recordAudioDecoded = () => { S.audioMetrics.packetsDecoded++; };
export const recordAudioBufferHealth = h => { S.audioMetrics.bufferHealthSum += h; S.audioMetrics.bufferHealthSamples++; };
export const recordAudioUnderrun = () => { S.audioMetrics.bufferUnderruns++; };
export const recordAudioOverflow = () => { S.audioMetrics.bufferOverflows++; };
export const recordMicPacket = bytes => { S.micMetrics.packetsSent++; S.micMetrics.bytesSent += bytes; };
export const recordMicEncodeError = () => { S.micMetrics.encodeErrors++; log.warn('MIC', 'Encode error'); };

export const recordPacket = (size, type) => {
    const m = S.networkMetrics;
    m.packetsReceived++;
    m.bytesReceived += size;
    if (type === 'video') m.videoPackets++;
    else if (type === 'audio') m.audioPackets++;
    else if (type === 'mic') m.micPackets++;
    else m.controlPackets++;
};

export const recordDecodeTime = (time, queueSize) => {
    S.decodeMetrics.decodeCount++;
    S.decodeMetrics.decodeTimeSum += time;
    S.decodeMetrics.maxQueueSize = Math.max(S.decodeMetrics.maxQueueSize, queueSize);
};

export const recordRenderTime = time => {
    S.renderMetrics.renderCount++;
    S.renderMetrics.renderTimeSum += time;
};
let metricsSubs = [];
let metricsInt = null;
let uptime = 0, sesFrames = 0, sesBytes = 0, sesDrops = 0;

export const subscribeToMetrics = cb => {
    metricsSubs.push(cb);
    log.debug('METRICS', 'Subscriber added', { count: metricsSubs.length });
    return () => {
        metricsSubs = metricsSubs.filter(c => c !== cb);
        log.debug('METRICS', 'Subscriber removed', { count: metricsSubs.length });
    };
};

const allChannelsOpen = () => {
    const channels = ['dcControl', 'dcVideo', 'dcAudio', 'dcInput', 'dcMic'];
    return channels.every(k => S[k]?.readyState === 'open');
};

export const startMetricsLogger = () => {
    if (metricsInt) {
        log.warn('METRICS', 'Logger already running');
        return;
    }

    uptime = 0;
    log.info('METRICS', 'Logger started');

    metricsInt = setInterval(() => {
        uptime++;
        if (!allChannelsOpen()) return;

        const stats = resetStats();
        const jitter = resetJitterMetrics();
        const clock = getClockSyncStats();
        const network = resetNetworkMetrics();
        const decode = resetDecodeMetrics();
        const render = resetRenderMetrics();
        const audio = resetAudioMetrics();
        const mic = resetMicMetrics();

        sesFrames += stats.framesComplete;
        sesBytes += stats.bytes;
        sesDrops += stats.framesDropped + jitter.framesDroppedLate;

        const fps = stats.framesComplete;
        const targetFps = S.currentFps || 60;
        const session = { totalFrames: sesFrames, totalBytes: sesBytes, totalDrops: sesDrops };
        const computed = {
            fps, targetFps,
            fpsEff: targetFps > 0 ? (fps / targetFps) * 100 : 0,
            mbps: (stats.bytes * 8 / 1048576).toFixed(2)
        };

        const payload = { uptime, stats, jitter, clock, network, decode, render, audio, mic, session, computed };

        metricsSubs.forEach(cb => {
            try { cb(payload); }
            catch (e) { log.error('METRICS', 'Subscriber callback failed', { error: e.message }); }
        });
    }, C.METRICS_LOG_INTERVAL_MS);
};

export const stopMetricsLogger = () => {
    if (metricsInt) {
        clearInterval(metricsInt);
        metricsInt = null;
        log.info('METRICS', 'Logger stopped', { uptime });
    }
};

export const resetSessionStats = () => {
    uptime = sesFrames = sesBytes = sesDrops = 0;
    log.debug('METRICS', 'Session stats reset');
};
