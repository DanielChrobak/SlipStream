import { MSG, CURSOR_TYPES, CODECS, CODEC_KEYS, C } from './constants.js';

export { MSG, CURSOR_TYPES, CODECS, CODEC_KEYS, C };
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
const JITTER_STAGE_FIELDS = [
    ['sourceCapture', 'avgSourceCaptureMs', 'sourceCaptureMs'],
    ['captureEncode', 'avgCaptureEncodeMs', 'captureEncodeMs'],
    ['encodeSend', 'avgEncodeSendMs', 'encodeSendMs'],
    ['sendReceive', 'avgSendReceiveMs', 'sendReceiveMs'],
    ['receiveAssemble', 'avgReceiveAssembleMs', 'receiveAssembleMs'],
    ['assembleDecode', 'avgAssembleDecodeMs', 'assembleDecodeMs'],
    ['decodePresent', 'avgDecodePresentMs', 'decodePresentMs'],
    ['e2eLatency', 'avgE2eLatencyMs', 'e2eMs']
];
const zeroMetric = (...keys) => Object.fromEntries(keys.map(key => [key, 0]));
const incrementMetric = (bucket, key, amount = 1) => {
    bucket[key] += amount;
    return bucket[key];
};

const logDrop = (module, prefix, reason, data, counterBucket, counterKey, options = {}) => {
    let payload = data;
    if (counterBucket && counterKey) {
        const total = options.countDropped === false
            ? counterBucket[counterKey]
            : incrementMetric(counterBucket, counterKey);
        payload = { ...data, [options.totalKey || 'total']: total };
    }
    log.warn(module, `${prefix}: ${reason}`, payload);
};

export const logVideoDrop = (reason, data, options = {}) =>
    logDrop('VIDEO', 'Drop', reason, data, S.stats, 'framesDropped', { ...options, totalKey: 'droppedTotal' });

export const logAudioDrop = (reason, data) =>
    logDrop('AUDIO', 'Drop', reason, data, S.audioMetrics, 'packetsDropped');

export const logNetworkDrop = (reason, data) =>
    logDrop('NET', 'Issue', reason, data);
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
const mkClockSync = () => ({ ...zeroMetric('offset', 'valid', 'sampleCount', 'avgRttUs'), offsetSamples: [], rttSamples: [] });
const mkJitter = () => {
    const metric = { ...zeroMetric('framesDroppedLate', 'lastPresentTs', 'intervalStdDev', 'intervalMean'), presentIntervals: [] };
    for (const [prefix, avgKey] of JITTER_STAGE_FIELDS) Object.assign(metric, zeroMetric(`${prefix}Sum`, `${prefix}Samples`, avgKey));
    return metric;
};
const mkStats = () => ({ ...zeroMetric('bytes', 'moves', 'clicks', 'keys', 'framesComplete', 'framesDropped', 'framesTimeout', 'keyframesReceived', 'decodeErrors', 'renderErrors'), lastUpdate: performance.now() });
const mkAudio = () => zeroMetric('packetsReceived', 'packetsDecoded', 'packetsDropped', 'bufferUnderruns', 'bufferOverflows', 'bufferHealthSum', 'bufferHealthSamples');
const mkNetwork = () => zeroMetric('packetsReceived', 'videoPackets', 'controlPackets', 'audioPackets', 'micPackets', 'bytesReceived');
const mkDecode = () => zeroMetric('decodeCount', 'decodeTimeSum', 'maxQueueSize');
const mkRender = () => zeroMetric('renderCount', 'renderTimeSum');
const mkMic = () => zeroMetric('packetsSent', 'packetsDropped', 'encodeErrors', 'bytesSent');
export const S = {
    pc: null, dcControl: null, dcVideo: null, dcAudio: null, dcInput: null, dcMic: null,
    decoder: null, ready: 0, needKey: 1, reinit: 0, hwAccel: 'unknown',
    W: 0, H: 0, hostFps: 60, currentFps: 60, currentFpsMode: 0, fpsSent: 0,
    authenticated: 0, monitors: [], currentMon: 0, tabbedMode: 0, username: null,
    audioCtx: null, audioEnabled: 0, audioDecoder: null, audioGain: null,
    controlEnabled: 0, lastVp: { x: 0, y: 0, w: 0, h: 0 },
    relativeMouseMode: 0, pointerLocked: 0, keyboardLockActive: 0,
    isReconnecting: 0, firstFrameReceived: 0,
    currentCodec: 1, codecSent: 0, hostCodecs: 0x07,
    hostEncoderName: null,
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
export const clientTimeUs = () => Math.floor(performance.now() * 1000);

export const bus = {
    _h: {},
    on(e, fn) { (this._h[e] ??= []).push(fn); },
    off(e, fn) { const a = this._h[e]; if (a) this._h[e] = a.filter(f => f !== fn); },
    emit(e, ...args) { (this._h[e] || []).forEach(fn => fn(...args)); }
};

export const syncedServerTimestampAgeMs = (ts, clientNowMs = performance.now()) => {
    if (!S.clockSync.valid) return 0;
    return Math.max(0, ((Math.floor(clientNowMs * 1000) - (ts - S.clockSync.offset)) / 1000));
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
    uncertaintyMs: S.clockSync.valid ? S.clockSync.avgRttUs / 2000 : 0,
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
    for (const [prefix, avgKey] of JITTER_STAGE_FIELDS) {
        m[avgKey] = avg(m[`${prefix}Sum`], m[`${prefix}Samples`]);
    }

    if (m.presentIntervals.length > 1) {
        const mean = m.presentIntervals.reduce((a, b) => a + b, 0) / m.presentIntervals.length;
        m.intervalMean = mean;
        m.intervalStdDev = Math.sqrt(
            m.presentIntervals.reduce((s, v) => s + (v - mean) ** 2, 0) / m.presentIntervals.length
        );
    }

    const result = {
        intervalMean: m.intervalMean,
        intervalStdDev: m.intervalStdDev,
        framesDroppedLate: m.framesDroppedLate
    };
    for (const [, avgKey] of JITTER_STAGE_FIELDS) result[avgKey] = m[avgKey];

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
export const recordAudioPacket = () => { incrementMetric(S.audioMetrics, 'packetsReceived'); };
export const recordAudioDecoded = () => { incrementMetric(S.audioMetrics, 'packetsDecoded'); };
export const recordAudioBufferHealth = h => {
    incrementMetric(S.audioMetrics, 'bufferHealthSum', h);
    incrementMetric(S.audioMetrics, 'bufferHealthSamples');
};
export const recordAudioUnderrun = () => { incrementMetric(S.audioMetrics, 'bufferUnderruns'); };
export const recordAudioOverflow = () => { incrementMetric(S.audioMetrics, 'bufferOverflows'); };
export const recordMicPacket = bytes => {
    incrementMetric(S.micMetrics, 'packetsSent');
    incrementMetric(S.micMetrics, 'bytesSent', bytes);
};
export const recordMicEncodeError = () => {
    incrementMetric(S.micMetrics, 'encodeErrors');
    log.warn('MIC', 'Encode error');
};

export const recordPacket = (size, type) => {
    const m = S.networkMetrics;
    incrementMetric(m, 'packetsReceived');
    incrementMetric(m, 'bytesReceived', size);
    incrementMetric(m, type === 'video' ? 'videoPackets' : type === 'audio' ? 'audioPackets' : type === 'mic' ? 'micPackets' : 'controlPackets');
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

const addLatencySample = (metrics, sumKey, countKey, value) => {
    if (!Number.isFinite(value) || value < 0) return;
    metrics[sumKey] += value;
    metrics[countKey]++;
};

export const recordVideoLatencySample = sample => {
    const m = S.jitterMetrics;
    for (const [prefix, , sampleKey] of JITTER_STAGE_FIELDS) {
        addLatencySample(m, `${prefix}Sum`, `${prefix}Samples`, sample[sampleKey]);
    }
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
        const [network, decode, render, audio, mic] = [resetNetworkMetrics, resetDecodeMetrics, resetRenderMetrics, resetAudioMetrics, resetMicMetrics].map(reset => reset());

        sesFrames += stats.framesComplete;
        sesBytes += stats.bytes;
        sesDrops += stats.framesDropped + stats.framesTimeout + jitter.framesDroppedLate + stats.decodeErrors;

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
    if (!metricsInt) return;
    clearInterval(metricsInt);
    metricsInt = null;
    log.info('METRICS', 'Logger stopped', { uptime });
};

export const resetSessionStats = () => { uptime = sesFrames = sesBytes = sesDrops = 0; log.debug('METRICS', 'Session stats reset'); };
