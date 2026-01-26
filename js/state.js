// Message Types
export const MSG = {
    PING: 0x504E4750,
    FPS_SET: 0x46505343,
    HOST_INFO: 0x484F5354,
    FPS_ACK: 0x46505341,
    REQUEST_KEY: 0x4B455952,
    MONITOR_LIST: 0x4D4F4E4C,
    MONITOR_SET: 0x4D4F4E53,
    AUDIO_DATA: 0x41554449,
    MOUSE_MOVE: 0x4D4F5645,
    MOUSE_BTN: 0x4D42544E,
    MOUSE_WHEEL: 0x4D57484C,
    KEY: 0x4B455920
};

// Constants
export const C = {
    HEADER: 21,
    AUDIO_HEADER: 16,
    PING_MS: 200,
    CODEC: 'av01.0.05M.08',
    MAX_FRAMES: 6,
    FRAME_TIMEOUT_MS: 100,
    AUDIO_RATE: 48000,
    AUDIO_CH: 2,
    AUDIO_BUF: 0.04,
    DC: { ordered: false, maxRetransmits: 0 },
    JITTER_TARGET_FRAMES: 1,
    JITTER_MAX_FRAMES: 2,
    JITTER_MAX_AGE_MS: 50,
    PRESENT_INTERVAL_WINDOW: 60,
    CLOCK_OFFSET_SAMPLES: 8,
    METRICS_LOG_INTERVAL_MS: 1000
};

// Connection Stages
export const Stage = {
    IDLE: 'idle',
    CONNECT: 'connect',
    AUTH: 'auth',
    OK: 'connected',
    ERR: 'error'
};

// Clock Sync Factory
const mkClockSync = () => ({
    offset: 0,
    offsetSamples: [],
    rttSamples: [],
    lastOffset: 0,
    driftPerSecond: 0,
    lastUpdateMs: 0,
    valid: false,
    sampleCount: 0,
    maxDriftSeen: 0,
    avgRttUs: 0,
    minRttUs: Infinity,
    maxRttUs: 0
});

// Jitter Metrics Factory
const mkJitterMetrics = () => ({
    queueDepthSum: 0,
    queueDepthSamples: 0,
    maxQueueDepth: 0,
    framesDroppedLate: 0,
    framesDroppedOverflow: 0,
    frameAgeSum: 0,
    frameAgeSamples: 0,
    presentIntervals: [],
    lastPresentTs: 0,
    serverAgeSum: 0,
    serverAgeSamples: 0,
    avgQueueDepth: 0,
    avgFrameAgeMs: 0,
    avgServerAgeMs: 0,
    intervalStdDev: 0,
    intervalMean: 0,
    minFrameAgeMs: Infinity,
    maxFrameAgeMs: 0,
    minServerAgeMs: Infinity,
    maxServerAgeMs: 0
});

// Global State
export const S = {
    // WebRTC
    pc: null,
    dc: null,
    decoder: null,
    ready: false,
    needKey: true,
    reinit: false,
    hwAccel: 'unknown',

    // Video dimensions
    W: 0,
    H: 0,

    // Connection metrics
    rtt: 0,
    hostFps: 60,
    clientFps: 60,
    currentFps: 60,
    currentFpsMode: 0,
    fpsSent: false,
    authenticated: false,

    // Monitor state
    monitors: [],
    currentMon: 0,
    tabbedMode: false,

    // Audio state
    audioCtx: null,
    audioEnabled: false,
    audioDecoder: null,
    audioGain: null,

    // Input state
    controlEnabled: false,
    lastVp: { x: 0, y: 0, w: 0, h: 0 },

    // Connection state
    stage: Stage.IDLE,
    isReconnecting: false,
    firstFrameReceived: false,

    // Statistics
    stats: {
        recv: 0,
        dec: 0,
        rend: 0,
        bytes: 0,
        audio: 0,
        moves: 0,
        clicks: 0,
        keys: 0,
        chunksReceived: 0,
        framesComplete: 0,
        framesDropped: 0,
        framesTimeout: 0,
        keyframesReceived: 0,
        deltaFramesReceived: 0,
        decodeErrors: 0,
        renderErrors: 0,
        lastUpdate: performance.now()
    },

    // Session statistics
    sessionStats: {
        startTime: 0,
        totalFrames: 0,
        totalBytes: 0,
        totalDrops: 0,
        totalKeyframes: 0,
        totalDecodeErrors: 0
    },

    // Frame management
    chunks: new Map(),
    frameMeta: new Map(),
    lastFrameId: 0,

    // Authentication
    sessionToken: null,
    username: null,

    // Presentation
    presentQueue: [],
    presentLoopRunning: false,

    // Metrics
    clockSync: mkClockSync(),
    jitterMetrics: mkJitterMetrics(),

    networkMetrics: {
        packetsReceived: 0,
        videoPackets: 0,
        controlPackets: 0,
        audioPackets: 0,
        bytesReceived: 0,
        minPacketSize: Infinity,
        maxPacketSize: 0
    },

    decodeMetrics: {
        decodeCount: 0,
        decodeTimeSum: 0,
        minDecodeTimeMs: Infinity,
        maxDecodeTimeMs: 0,
        maxQueueSize: 0
    },

    renderMetrics: {
        renderCount: 0,
        renderTimeSum: 0,
        minRenderTimeMs: Infinity,
        maxRenderTimeMs: 0
    }
};

// Utility Functions
export const $ = id => document.getElementById(id);

export const mkBuf = (sz, fn) => {
    const b = new ArrayBuffer(sz);
    fn(new DataView(b));
    return b;
};

export const clientTimeUs = () => {
    return Math.floor((performance.timeOrigin + performance.now()) * 1000);
};

export const serverFrameAgeMs = ts => {
    if (!S.clockSync.valid) return 0;
    return Math.max(0, (clientTimeUs() - (ts - S.clockSync.offset)) / 1000);
};

// Clock Synchronization
export const updateClockOffset = (clientSendUs, serverTimeUs, clientRecvUs) => {
    const cs = S.clockSync;
    const rttUs = clientRecvUs - clientSendUs;
    const halfRtt = rttUs / 2;

    cs.minRttUs = Math.min(cs.minRttUs, rttUs);
    cs.maxRttUs = Math.max(cs.maxRttUs, rttUs);

    cs.offsetSamples.push(serverTimeUs - (clientSendUs + halfRtt));
    cs.rttSamples.push(rttUs);

    if (cs.offsetSamples.length > C.CLOCK_OFFSET_SAMPLES) {
        cs.offsetSamples.shift();
    }
    if (cs.rttSamples.length > C.CLOCK_OFFSET_SAMPLES) {
        cs.rttSamples.shift();
    }

    const sorted = arr => [...arr].sort((a, b) => a - b);
    const mid = Math.floor(cs.offsetSamples.length / 2);
    const prevOffset = cs.offset;

    cs.offset = sorted(cs.offsetSamples)[mid];
    cs.avgRttUs = sorted(cs.rttSamples)[mid];

    const nowMs = performance.now();
    if (cs.valid && cs.lastUpdateMs > 0) {
        const elapsed = (nowMs - cs.lastUpdateMs) / 1000;
        if (elapsed > 0.1) {
            const drift = Math.abs(cs.offset - prevOffset);
            cs.driftPerSecond = drift / elapsed;
            cs.maxDriftSeen = Math.max(cs.maxDriftSeen, drift);
        }
    }

    cs.lastOffset = prevOffset;
    cs.lastUpdateMs = nowMs;
    cs.sampleCount++;
    cs.valid = cs.offsetSamples.length >= 2;
};

export const resetClockSync = () => {
    S.clockSync = mkClockSync();
};

export const getClockSyncStats = () => {
    const cs = S.clockSync;
    return {
        offsetMs: cs.offset / 1000,
        rttMs: cs.avgRttUs / 1000,
        minRttMs: cs.minRttUs === Infinity ? 0 : cs.minRttUs / 1000,
        maxRttMs: cs.maxRttUs / 1000,
        driftMsPerSec: cs.driftPerSecond / 1000,
        samples: cs.sampleCount,
        valid: cs.valid
    };
};

// Stats Reset Functions
export const resetStats = () => {
    const now = performance.now();
    const result = {
        ...S.stats,
        elapsed: now - S.stats.lastUpdate
    };

    Object.assign(S.stats, {
        recv: 0,
        dec: 0,
        rend: 0,
        bytes: 0,
        audio: 0,
        moves: 0,
        clicks: 0,
        keys: 0,
        chunksReceived: 0,
        framesComplete: 0,
        framesDropped: 0,
        framesTimeout: 0,
        keyframesReceived: 0,
        deltaFramesReceived: 0,
        decodeErrors: 0,
        renderErrors: 0,
        lastUpdate: now
    });

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
        m.intervalStdDev = Math.sqrt(
            m.presentIntervals.reduce((s, v) => s + (v - mean) ** 2, 0) / m.presentIntervals.length
        );
    }

    const result = {
        avgQueueDepth: m.avgQueueDepth,
        maxQueueDepth: m.maxQueueDepth,
        avgFrameAgeMs: m.avgFrameAgeMs,
        minFrameAgeMs: m.minFrameAgeMs === Infinity ? 0 : m.minFrameAgeMs,
        maxFrameAgeMs: m.maxFrameAgeMs,
        avgServerAgeMs: m.avgServerAgeMs,
        minServerAgeMs: m.minServerAgeMs === Infinity ? 0 : m.minServerAgeMs,
        maxServerAgeMs: m.maxServerAgeMs,
        intervalMean: m.intervalMean,
        intervalStdDev: m.intervalStdDev,
        framesDroppedLate: m.framesDroppedLate,
        framesDroppedOverflow: m.framesDroppedOverflow
    };

    Object.assign(m, {
        queueDepthSum: 0,
        queueDepthSamples: 0,
        maxQueueDepth: 0,
        framesDroppedLate: 0,
        framesDroppedOverflow: 0,
        frameAgeSum: 0,
        frameAgeSamples: 0,
        serverAgeSum: 0,
        serverAgeSamples: 0,
        presentIntervals: [],
        minFrameAgeMs: Infinity,
        maxFrameAgeMs: 0,
        minServerAgeMs: Infinity,
        maxServerAgeMs: 0
    });

    return result;
};

export const resetNetworkMetrics = () => {
    const m = S.networkMetrics;

    const result = {
        packetsReceived: m.packetsReceived,
        videoPackets: m.videoPackets,
        controlPackets: m.controlPackets,
        audioPackets: m.audioPackets,
        bytesReceived: m.bytesReceived,
        minPacketSize: m.minPacketSize === Infinity ? 0 : m.minPacketSize,
        maxPacketSize: m.maxPacketSize,
        avgPacketSize: m.packetsReceived > 0 ? m.bytesReceived / m.packetsReceived : 0
    };

    Object.assign(m, {
        packetsReceived: 0,
        videoPackets: 0,
        controlPackets: 0,
        audioPackets: 0,
        bytesReceived: 0,
        minPacketSize: Infinity,
        maxPacketSize: 0
    });

    return result;
};

export const resetDecodeMetrics = () => {
    const m = S.decodeMetrics;

    const result = {
        decodeCount: m.decodeCount,
        maxQueueSize: m.maxQueueSize,
        avgDecodeTimeMs: m.decodeCount > 0 ? m.decodeTimeSum / m.decodeCount : 0,
        minDecodeTimeMs: m.minDecodeTimeMs === Infinity ? 0 : m.minDecodeTimeMs,
        maxDecodeTimeMs: m.maxDecodeTimeMs
    };

    Object.assign(m, {
        decodeCount: 0,
        decodeTimeSum: 0,
        minDecodeTimeMs: Infinity,
        maxDecodeTimeMs: 0,
        maxQueueSize: 0
    });

    return result;
};

export const resetRenderMetrics = () => {
    const m = S.renderMetrics;

    const result = {
        renderCount: m.renderCount,
        avgRenderTimeMs: m.renderCount > 0 ? m.renderTimeSum / m.renderCount : 0,
        minRenderTimeMs: m.minRenderTimeMs === Infinity ? 0 : m.minRenderTimeMs,
        maxRenderTimeMs: m.maxRenderTimeMs
    };

    Object.assign(m, {
        renderCount: 0,
        renderTimeSum: 0,
        minRenderTimeMs: Infinity,
        maxRenderTimeMs: 0
    });

    return result;
};

// Metric Recording Functions
export const recordPacket = (size, type) => {
    const m = S.networkMetrics;
    m.packetsReceived++;
    m.bytesReceived += size;
    m.minPacketSize = Math.min(m.minPacketSize, size);
    m.maxPacketSize = Math.max(m.maxPacketSize, size);

    if (type === 'video') {
        m.videoPackets++;
    } else if (type === 'audio') {
        m.audioPackets++;
    } else {
        m.controlPackets++;
    }
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

// Metrics Logger
let metricsLogInterval = null;
let uptimeSeconds = 0;

export const startMetricsLogger = () => {
    if (metricsLogInterval) return;

    S.sessionStats.startTime = performance.now();
    uptimeSeconds = 0;

    metricsLogInterval = setInterval(() => {
        uptimeSeconds++;

        const stats = resetStats();
        const jitter = resetJitterMetrics();
        const clock = getClockSyncStats();
        const network = resetNetworkMetrics();
        const decode = resetDecodeMetrics();
        const render = resetRenderMetrics();

        // Update session stats
        const ss = S.sessionStats;
        ss.totalFrames += stats.framesComplete;
        ss.totalBytes += stats.bytes;
        ss.totalDrops += stats.framesDropped + jitter.framesDroppedLate + jitter.framesDroppedOverflow;
        ss.totalKeyframes += stats.keyframesReceived;
        ss.totalDecodeErrors += stats.decodeErrors;

        // Calculate metrics
        const fps = stats.framesComplete;
        const targetFps = S.currentFps || 60;
        const fpsEff = targetFps > 0 ? (fps / targetFps) * 100 : 0;
        const mbps = (stats.bytes * 8 / 1048576).toFixed(2);
        const e2e = clock.rttMs / 2 + jitter.avgServerAgeMs;

        // Status display
        const status = S.dc?.readyState === 'open'
            ? (S.ready ? '%c[LIVE]' : '%c[WAIT]')
            : '%c[DISC]';
        const statusColor = S.dc?.readyState === 'open'
            ? (S.ready ? 'color:#22c55e;font-weight:bold' : 'color:#f59e0b;font-weight:bold')
            : 'color:#ef4444;font-weight:bold';

        if (S.dc?.readyState !== 'open') return;

        const log = (tag, color, msg) => {
            console.log(
                `%c[${tag}]%c ${msg}`,
                `color:${color};font-weight:bold`,
                'color:inherit'
            );
        };

        // Log header
        console.log(
            `%c━━━ [${uptimeSeconds}s] ${status} ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━`,
            'color:#06b6d4;font-weight:bold',
            statusColor
        );

        // Throughput
        log('THROUGHPUT', '#eab308',
            `FPS: ${fps}/${targetFps} (${fpsEff.toFixed(1)}%) | ` +
            `Bitrate: ${mbps} Mbps | ` +
            `Frames: ${stats.recv} recv, ${stats.dec} dec, ${stats.rend} rend`
        );

        // Latency
        if (clock.valid) {
            log('LATENCY', '#eab308',
                `E2E: ${e2e.toFixed(1)}ms | ` +
                `RTT: ${clock.rttMs.toFixed(1)}ms (${clock.minRttMs.toFixed(1)}/${clock.maxRttMs.toFixed(1)}) | ` +
                `FrameAge: ${jitter.avgServerAgeMs.toFixed(1)}ms (${jitter.minServerAgeMs.toFixed(1)}/${jitter.maxServerAgeMs.toFixed(1)})`
            );
        }

        // Jitter
        if (jitter.avgQueueDepth > 0 || jitter.intervalMean > 0) {
            log('JITTER', '#eab308',
                `Queue: ${jitter.avgQueueDepth.toFixed(2)} (max:${jitter.maxQueueDepth}) | ` +
                `Interval: ${jitter.intervalMean.toFixed(1)}±${jitter.intervalStdDev.toFixed(1)}ms | ` +
                `LocalAge: ${jitter.avgFrameAgeMs.toFixed(1)}ms`
            );
        }

        // Decode
        if (decode.decodeCount > 0) {
            log('DECODE', '#eab308',
                `Count: ${decode.decodeCount} | ` +
                `Time: ${decode.avgDecodeTimeMs.toFixed(2)}ms (${decode.minDecodeTimeMs.toFixed(2)}/${decode.maxDecodeTimeMs.toFixed(2)}) | ` +
                `Queue: ${decode.maxQueueSize} | HW: ${S.hwAccel}`
            );
        }

        // Render
        if (render.renderCount > 0) {
            log('RENDER', '#eab308',
                `Count: ${render.renderCount} | ` +
                `Time: ${render.avgRenderTimeMs.toFixed(2)}ms (${render.minRenderTimeMs.toFixed(2)}/${render.maxRenderTimeMs.toFixed(2)}) | ` +
                `Resolution: ${S.W}x${S.H}`
            );
        }

        // Network
        if (network.packetsReceived > 0) {
            log('NETWORK', '#eab308',
                `Packets: ${network.packetsReceived} (V:${network.videoPackets} A:${network.audioPackets} C:${network.controlPackets}) | ` +
                `Size: ${network.avgPacketSize.toFixed(0)}B avg (${network.minPacketSize}-${network.maxPacketSize})`
            );
        }

        // Clock sync (every 5 seconds)
        if (clock.valid && uptimeSeconds % 5 === 0) {
            log('CLOCK', '#eab308',
                `Offset: ${clock.offsetMs.toFixed(2)}ms | ` +
                `Drift: ${clock.driftMsPerSec.toFixed(3)}ms/s | ` +
                `Samples: ${clock.samples}`
            );
        }

        // Drops
        const totalDrops = stats.framesDropped + stats.framesTimeout +
            jitter.framesDroppedLate + jitter.framesDroppedOverflow;
        if (totalDrops > 0 || stats.decodeErrors > 0) {
            log('DROPS', '#ef4444',
                `Network: ${stats.framesDropped} | ` +
                `Timeout: ${stats.framesTimeout} | ` +
                `Late: ${jitter.framesDroppedLate} | ` +
                `Overflow: ${jitter.framesDroppedOverflow} | ` +
                `DecodeErr: ${stats.decodeErrors}`
            );
        }

        // Input
        if (stats.moves > 0 || stats.clicks > 0 || stats.keys > 0) {
            log('INPUT', '#eab308',
                `Mouse: ${stats.moves} moves, ${stats.clicks} clicks | Keys: ${stats.keys}`
            );
        }

        // Audio
        if (stats.audio > 0) {
            log('AUDIO', '#eab308',
                `Packets: ${stats.audio} | Enabled: ${S.audioEnabled}`
            );
        }

        // Session summary (every 10 seconds)
        if (uptimeSeconds % 10 === 0) {
            log('SESSION', '#a855f7',
                `Uptime: ${uptimeSeconds}s | ` +
                `Frames: ${ss.totalFrames} (${(ss.totalFrames / uptimeSeconds).toFixed(1)} avg) | ` +
                `Data: ${(ss.totalBytes / 1048576).toFixed(2)}MB (${(ss.totalBytes * 8 / 1048576 / uptimeSeconds).toFixed(2)} Mbps avg) | ` +
                `Drops: ${ss.totalDrops} | Keys: ${ss.totalKeyframes}`
            );
        }
    }, C.METRICS_LOG_INTERVAL_MS);
};

export const stopMetricsLogger = () => {
    if (metricsLogInterval) {
        clearInterval(metricsLogInterval);
        metricsLogInterval = null;
    }
};

export const resetSessionStats = () => {
    uptimeSeconds = 0;
    Object.assign(S.sessionStats, {
        startTime: performance.now(),
        totalFrames: 0,
        totalBytes: 0,
        totalDrops: 0,
        totalKeyframes: 0,
        totalDecodeErrors: 0
    });
};
