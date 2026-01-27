export const MSG = {
    PING: 0x504E4750, FPS_SET: 0x46505343, HOST_INFO: 0x484F5354, FPS_ACK: 0x46505341,
    REQUEST_KEY: 0x4B455952, MONITOR_LIST: 0x4D4F4E4C, MONITOR_SET: 0x4D4F4E53,
    AUDIO_DATA: 0x41554449, MOUSE_MOVE: 0x4D4F5645, MOUSE_BTN: 0x4D42544E,
    MOUSE_WHEEL: 0x4D57484C, KEY: 0x4B455920, CODEC_SET: 0x434F4443, CODEC_ACK: 0x434F4441
};

export const CODECS = {
    H264: { id: 0, name: 'H.264', codec: 'avc1.42001f' },
    AV1: { id: 1, name: 'AV1', codec: 'av01.0.05M.08' }
};

export const detectBestCodec = async () => {
    if (!window.VideoDecoder) return { codecId: 0, hardwareAccel: false, codecName: 'H.264' };

    for (const { id, codec, name } of [{ id: 1, codec: CODECS.AV1.codec, name: 'AV1' }, { id: 0, codec: CODECS.H264.codec, name: 'H.264' }]) {
        for (const hw of [true, false]) {
            try {
                const r = await VideoDecoder.isConfigSupported({ codec, optimizeForLatency: true, hardwareAcceleration: hw ? 'prefer-hardware' : 'prefer-software' });
                if (r.supported) return { codecId: id, hardwareAccel: hw, codecName: name };
            } catch {}
        }
    }
    return { codecId: 0, hardwareAccel: false, codecName: 'H.264' };
};

export const getCodecSupport = async () => {
    if (!window.VideoDecoder) return { av1: false, h264: false, av1Hw: false, h264Hw: false };

    const support = { av1: false, h264: false, av1Hw: false, h264Hw: false };
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
    HEADER: 21, AUDIO_HEADER: 16, PING_MS: 200, MAX_FRAMES: 6, FRAME_TIMEOUT_MS: 100,
    AUDIO_RATE: 48000, AUDIO_CH: 2, AUDIO_BUF: 0.04,
    DC: { ordered: false, maxRetransmits: 0 },
    JITTER_MAX_FRAMES: 2, JITTER_MAX_AGE_MS: 50, PRESENT_INTERVAL_WINDOW: 60,
    CLOCK_OFFSET_SAMPLES: 8, METRICS_LOG_INTERVAL_MS: 1000
};

export const Stage = { IDLE: 'idle', CONNECT: 'connect', AUTH: 'auth', OK: 'connected', ERR: 'error' };

const mkClockSync = () => ({ offset: 0, offsetSamples: [], rttSamples: [], lastOffset: 0, driftPerSecond: 0, lastUpdateMs: 0, valid: false, sampleCount: 0, maxDriftSeen: 0, avgRttUs: 0, minRttUs: Infinity, maxRttUs: 0 });

const mkJitterMetrics = () => ({ queueDepthSum: 0, queueDepthSamples: 0, maxQueueDepth: 0, framesDroppedLate: 0, framesDroppedOverflow: 0, frameAgeSum: 0, frameAgeSamples: 0, presentIntervals: [], lastPresentTs: 0, serverAgeSum: 0, serverAgeSamples: 0, avgQueueDepth: 0, avgFrameAgeMs: 0, avgServerAgeMs: 0, intervalStdDev: 0, intervalMean: 0, minFrameAgeMs: Infinity, maxFrameAgeMs: 0, minServerAgeMs: Infinity, maxServerAgeMs: 0 });

const mkStats = () => ({ recv: 0, dec: 0, rend: 0, bytes: 0, audio: 0, moves: 0, clicks: 0, keys: 0, chunksReceived: 0, framesComplete: 0, framesDropped: 0, framesTimeout: 0, keyframesReceived: 0, deltaFramesReceived: 0, decodeErrors: 0, renderErrors: 0, lastUpdate: performance.now() });

export const S = {
    pc: null, dc: null, decoder: null, ready: false, needKey: true, reinit: false, hwAccel: 'unknown',
    W: 0, H: 0, rtt: 0, hostFps: 60, clientFps: 60, currentFps: 60, currentFpsMode: 0, fpsSent: false,
    authenticated: false, monitors: [], currentMon: 0, tabbedMode: false,
    audioCtx: null, audioEnabled: false, audioDecoder: null, audioGain: null,
    controlEnabled: false, lastVp: { x: 0, y: 0, w: 0, h: 0 },
    stage: Stage.IDLE, isReconnecting: false, firstFrameReceived: false,
    currentCodec: 1, codecSent: false, stats: mkStats(),
    sessionStats: { startTime: 0, totalFrames: 0, totalBytes: 0, totalDrops: 0, totalKeyframes: 0, totalDecodeErrors: 0 },
    chunks: new Map(), frameMeta: new Map(), lastFrameId: 0, sessionToken: null, username: null,
    presentQueue: [], presentLoopRunning: false, clockSync: mkClockSync(), jitterMetrics: mkJitterMetrics(),
    networkMetrics: { packetsReceived: 0, videoPackets: 0, controlPackets: 0, audioPackets: 0, bytesReceived: 0, minPacketSize: Infinity, maxPacketSize: 0 },
    decodeMetrics: { decodeCount: 0, decodeTimeSum: 0, minDecodeTimeMs: Infinity, maxDecodeTimeMs: 0, maxQueueSize: 0 },
    renderMetrics: { renderCount: 0, renderTimeSum: 0, minRenderTimeMs: Infinity, maxRenderTimeMs: 0 }
};

export const $ = id => document.getElementById(id);
export const mkBuf = (sz, fn) => { const b = new ArrayBuffer(sz); fn(new DataView(b)); return b; };
export const clientTimeUs = () => Math.floor((performance.timeOrigin + performance.now()) * 1000);
export const serverFrameAgeMs = ts => S.clockSync.valid ? Math.max(0, (clientTimeUs() - (ts - S.clockSync.offset)) / 1000) : 0;

const median = arr => [...arr].sort((a, b) => a - b)[Math.floor(arr.length / 2)];

export const updateClockOffset = (clientSendUs, serverTimeUs, clientRecvUs) => {
    const cs = S.clockSync, rttUs = clientRecvUs - clientSendUs;
    cs.minRttUs = Math.min(cs.minRttUs, rttUs);
    cs.maxRttUs = Math.max(cs.maxRttUs, rttUs);
    cs.offsetSamples.push(serverTimeUs - (clientSendUs + rttUs / 2));
    cs.rttSamples.push(rttUs);
    if (cs.offsetSamples.length > C.CLOCK_OFFSET_SAMPLES) cs.offsetSamples.shift();
    if (cs.rttSamples.length > C.CLOCK_OFFSET_SAMPLES) cs.rttSamples.shift();

    const prevOffset = cs.offset;
    cs.offset = median(cs.offsetSamples);
    cs.avgRttUs = median(cs.rttSamples);

    const nowMs = performance.now();
    if (cs.valid && cs.lastUpdateMs > 0) {
        const elapsed = (nowMs - cs.lastUpdateMs) / 1000;
        if (elapsed > 0.1) { cs.driftPerSecond = Math.abs(cs.offset - prevOffset) / elapsed; cs.maxDriftSeen = Math.max(cs.maxDriftSeen, Math.abs(cs.offset - prevOffset)); }
    }
    cs.lastOffset = prevOffset;
    cs.lastUpdateMs = nowMs;
    cs.sampleCount++;
    cs.valid = cs.offsetSamples.length >= 2;
};

export const resetClockSync = () => { S.clockSync = mkClockSync(); };

export const getClockSyncStats = () => {
    const cs = S.clockSync;
    return { offsetMs: cs.offset / 1000, rttMs: cs.avgRttUs / 1000, minRttMs: cs.minRttUs === Infinity ? 0 : cs.minRttUs / 1000, maxRttMs: cs.maxRttUs / 1000, driftMsPerSec: cs.driftPerSecond / 1000, samples: cs.sampleCount, valid: cs.valid };
};

export const resetStats = () => {
    const now = performance.now(), result = { ...S.stats, elapsed: now - S.stats.lastUpdate };
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

    const result = { avgQueueDepth: m.avgQueueDepth, maxQueueDepth: m.maxQueueDepth, avgFrameAgeMs: m.avgFrameAgeMs, minFrameAgeMs: m.minFrameAgeMs === Infinity ? 0 : m.minFrameAgeMs, maxFrameAgeMs: m.maxFrameAgeMs, avgServerAgeMs: m.avgServerAgeMs, minServerAgeMs: m.minServerAgeMs === Infinity ? 0 : m.minServerAgeMs, maxServerAgeMs: m.maxServerAgeMs, intervalMean: m.intervalMean, intervalStdDev: m.intervalStdDev, framesDroppedLate: m.framesDroppedLate, framesDroppedOverflow: m.framesDroppedOverflow };
    Object.assign(m, mkJitterMetrics(), { lastPresentTs: m.lastPresentTs });
    return result;
};

export const resetNetworkMetrics = () => {
    const m = S.networkMetrics;
    const result = { packetsReceived: m.packetsReceived, videoPackets: m.videoPackets, controlPackets: m.controlPackets, audioPackets: m.audioPackets, bytesReceived: m.bytesReceived, minPacketSize: m.minPacketSize === Infinity ? 0 : m.minPacketSize, maxPacketSize: m.maxPacketSize, avgPacketSize: m.packetsReceived > 0 ? m.bytesReceived / m.packetsReceived : 0 };
    Object.assign(m, { packetsReceived: 0, videoPackets: 0, controlPackets: 0, audioPackets: 0, bytesReceived: 0, minPacketSize: Infinity, maxPacketSize: 0 });
    return result;
};

export const resetDecodeMetrics = () => {
    const m = S.decodeMetrics;
    const result = { decodeCount: m.decodeCount, maxQueueSize: m.maxQueueSize, avgDecodeTimeMs: m.decodeCount > 0 ? m.decodeTimeSum / m.decodeCount : 0, minDecodeTimeMs: m.minDecodeTimeMs === Infinity ? 0 : m.minDecodeTimeMs, maxDecodeTimeMs: m.maxDecodeTimeMs };
    Object.assign(m, { decodeCount: 0, decodeTimeSum: 0, minDecodeTimeMs: Infinity, maxDecodeTimeMs: 0, maxQueueSize: 0 });
    return result;
};

export const resetRenderMetrics = () => {
    const m = S.renderMetrics;
    const result = { renderCount: m.renderCount, avgRenderTimeMs: m.renderCount > 0 ? m.renderTimeSum / m.renderCount : 0, minRenderTimeMs: m.minRenderTimeMs === Infinity ? 0 : m.minRenderTimeMs, maxRenderTimeMs: m.maxRenderTimeMs };
    Object.assign(m, { renderCount: 0, renderTimeSum: 0, minRenderTimeMs: Infinity, maxRenderTimeMs: 0 });
    return result;
};

export const recordPacket = (size, type) => {
    const m = S.networkMetrics;
    m.packetsReceived++;
    m.bytesReceived += size;
    m.minPacketSize = Math.min(m.minPacketSize, size);
    m.maxPacketSize = Math.max(m.maxPacketSize, size);
    m[type === 'video' ? 'videoPackets' : type === 'audio' ? 'audioPackets' : 'controlPackets']++;
};

export const recordDecodeTime = (timeMs, queueSize) => {
    const m = S.decodeMetrics;
    m.decodeCount++;
    m.decodeTimeSum += timeMs;
    m.minDecodeTimeMs = Math.min(m.minDecodeTimeMs, timeMs);
    m.maxDecodeTimeMs = Math.max(m.maxDecodeTimeMs, timeMs);
    m.maxQueueSize = Math.max(m.maxQueueSize, queueSize);
};

export const recordRenderTime = timeMs => {
    const m = S.renderMetrics;
    m.renderCount++;
    m.renderTimeSum += timeMs;
    m.minRenderTimeMs = Math.min(m.minRenderTimeMs, timeMs);
    m.maxRenderTimeMs = Math.max(m.maxRenderTimeMs, timeMs);
};

let metricsLogInterval = null, uptimeSeconds = 0;

export const startMetricsLogger = () => {
    if (metricsLogInterval) return;
    S.sessionStats.startTime = performance.now();
    uptimeSeconds = 0;

    metricsLogInterval = setInterval(() => {
        uptimeSeconds++;
        if (S.dc?.readyState !== 'open') return;

        const stats = resetStats(), jitter = resetJitterMetrics(), clock = getClockSyncStats();
        const network = resetNetworkMetrics(), decode = resetDecodeMetrics(), render = resetRenderMetrics();
        const ss = S.sessionStats;

        ss.totalFrames += stats.framesComplete;
        ss.totalBytes += stats.bytes;
        ss.totalDrops += stats.framesDropped + jitter.framesDroppedLate + jitter.framesDroppedOverflow;
        ss.totalKeyframes += stats.keyframesReceived;
        ss.totalDecodeErrors += stats.decodeErrors;

        const fps = stats.framesComplete, targetFps = S.currentFps || 60;
        const fpsEff = targetFps > 0 ? (fps / targetFps) * 100 : 0;
        const mbps = (stats.bytes * 8 / 1048576).toFixed(2);
        const e2e = clock.rttMs / 2 + jitter.avgServerAgeMs;

        const status = S.ready ? '%c[LIVE]' : '%c[WAIT]';
        const statusColor = S.ready ? 'color:#22c55e;font-weight:bold' : 'color:#f59e0b;font-weight:bold';
        const log = (tag, color, msg) => console.log(`%c[${tag}]%c ${msg}`, `color:${color};font-weight:bold`, 'color:inherit');

        console.log(`%c━━━ [${uptimeSeconds}s] ${status} ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━`, 'color:#06b6d4;font-weight:bold', statusColor);
        log('THROUGHPUT', '#eab308', `FPS: ${fps}/${targetFps} (${fpsEff.toFixed(1)}%) | Bitrate: ${mbps} Mbps | Frames: ${stats.recv} recv, ${stats.dec} dec, ${stats.rend} rend`);

        if (clock.valid) log('LATENCY', '#eab308', `E2E: ${e2e.toFixed(1)}ms | RTT: ${clock.rttMs.toFixed(1)}ms (${clock.minRttMs.toFixed(1)}/${clock.maxRttMs.toFixed(1)}) | FrameAge: ${jitter.avgServerAgeMs.toFixed(1)}ms (${jitter.minServerAgeMs.toFixed(1)}/${jitter.maxServerAgeMs.toFixed(1)})`);
        if (jitter.avgQueueDepth > 0 || jitter.intervalMean > 0) log('JITTER', '#eab308', `Queue: ${jitter.avgQueueDepth.toFixed(2)} (max:${jitter.maxQueueDepth}) | Interval: ${jitter.intervalMean.toFixed(1)}±${jitter.intervalStdDev.toFixed(1)}ms | LocalAge: ${jitter.avgFrameAgeMs.toFixed(1)}ms`);
        if (decode.decodeCount > 0) log('DECODE', '#eab308', `Count: ${decode.decodeCount} | Time: ${decode.avgDecodeTimeMs.toFixed(2)}ms (${decode.minDecodeTimeMs.toFixed(2)}/${decode.maxDecodeTimeMs.toFixed(2)}) | Queue: ${decode.maxQueueSize} | HW: ${S.hwAccel}`);
        if (render.renderCount > 0) log('RENDER', '#eab308', `Count: ${render.renderCount} | Time: ${render.avgRenderTimeMs.toFixed(2)}ms (${render.minRenderTimeMs.toFixed(2)}/${render.maxRenderTimeMs.toFixed(2)}) | Resolution: ${S.W}x${S.H}`);
        if (network.packetsReceived > 0) log('NETWORK', '#eab308', `Packets: ${network.packetsReceived} (V:${network.videoPackets} A:${network.audioPackets} C:${network.controlPackets}) | Size: ${network.avgPacketSize.toFixed(0)}B avg (${network.minPacketSize}-${network.maxPacketSize})`);
        if (clock.valid && uptimeSeconds % 5 === 0) log('CLOCK', '#eab308', `Offset: ${clock.offsetMs.toFixed(2)}ms | Drift: ${clock.driftMsPerSec.toFixed(3)}ms/s | Samples: ${clock.samples}`);

        const totalDrops = stats.framesDropped + stats.framesTimeout + jitter.framesDroppedLate + jitter.framesDroppedOverflow;
        if (totalDrops > 0 || stats.decodeErrors > 0) log('DROPS', '#ef4444', `Network: ${stats.framesDropped} | Timeout: ${stats.framesTimeout} | Late: ${jitter.framesDroppedLate} | Overflow: ${jitter.framesDroppedOverflow} | DecodeErr: ${stats.decodeErrors}`);
        if (stats.moves > 0 || stats.clicks > 0 || stats.keys > 0) log('INPUT', '#eab308', `Mouse: ${stats.moves} moves, ${stats.clicks} clicks | Keys: ${stats.keys}`);
        if (stats.audio > 0) log('AUDIO', '#eab308', `Packets: ${stats.audio} | Enabled: ${S.audioEnabled}`);
        if (uptimeSeconds % 10 === 0) log('SESSION', '#a855f7', `Uptime: ${uptimeSeconds}s | Frames: ${ss.totalFrames} (${(ss.totalFrames / uptimeSeconds).toFixed(1)} avg) | Data: ${(ss.totalBytes / 1048576).toFixed(2)}MB (${(ss.totalBytes * 8 / 1048576 / uptimeSeconds).toFixed(2)} Mbps avg) | Drops: ${ss.totalDrops} | Keys: ${ss.totalKeyframes}`);
    }, C.METRICS_LOG_INTERVAL_MS);
};

export const stopMetricsLogger = () => { clearInterval(metricsLogInterval); metricsLogInterval = null; };

export const resetSessionStats = () => {
    uptimeSeconds = 0;
    Object.assign(S.sessionStats, { startTime: performance.now(), totalFrames: 0, totalBytes: 0, totalDrops: 0, totalKeyframes: 0, totalDecodeErrors: 0 });
};
