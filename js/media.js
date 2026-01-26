import { C, S, $ } from './state.js';
import { queueFrameForPresentation, startPresentLoop } from './renderer.js';
import { recordDecodeTime } from './state.js';

// Keyframe request function reference
let reqKey = null;
let keyframeRetryInterval = null;
let lastCapTs = 0;

export const setReqKeyFn = fn => {
    reqKey = fn;
};

// Start keyframe retry timer
const startKeyframeRetry = () => {
    stopKeyframeRetry();

    keyframeRetryInterval = setInterval(() => {
        if (S.needKey && S.ready && reqKey) {
            reqKey();
        } else if (!S.needKey) {
            stopKeyframeRetry();
        }
    }, 500);
};

// Stop keyframe retry timer
const stopKeyframeRetry = () => {
    if (keyframeRetryInterval) {
        clearInterval(keyframeRetryInterval);
        keyframeRetryInterval = null;
    }
};

const MAX_DECODE_QUEUE_SIZE = 4;

// Initialize video decoder
export const initDecoder = async (force = false) => {
    if (!window.VideoDecoder) {
        return;
    }

    S.ready = false;
    S.needKey = true;
    lastCapTs = 0;

    if (force) {
        S.W = S.H = 0;
    }

    // Close existing frames in queue
    S.presentQueue.forEach(f => {
        try {
            f.frame.close();
        } catch {}
    });
    S.presentQueue = [];

    // Close existing decoder
    try {
        if (S.decoder?.state !== 'closed') {
            S.decoder.close();
        }
    } catch {}

    // Create new decoder
    const decoder = S.decoder = new VideoDecoder({
        output: frame => {
            const now = performance.now();
            const meta = S.frameMeta.get(frame.timestamp);

            if (meta?.decodeStartMs) {
                recordDecodeTime(now - meta.decodeStartMs, S.decoder?.decodeQueueSize || 0);
            }

            queueFrameForPresentation({
                frame,
                meta,
                queuedAt: now,
                timestamp: frame.timestamp,
                serverCapTs: meta?.capTs || frame.timestamp
            });

            S.stats.dec++;
        },

        error: () => {
            S.ready = true;
            S.needKey = true;
            S.stats.decodeErrors++;
            reqKey?.();
            startKeyframeRetry();

            if (!S.reinit) {
                S.reinit = true;
                setTimeout(async () => {
                    await initDecoder();
                    S.reinit = false;
                }, 100);
            }
        }
    });

    // Try hardware acceleration first, then software
    const configs = [
        [true, 'HW'],
        [false, 'SW']
    ];

    for (const [preferHardware, label] of configs) {
        const config = {
            codec: C.CODEC,
            optimizeForLatency: true,
            latencyMode: 'realtime',
            hardwareAcceleration: preferHardware ? 'prefer-hardware' : 'prefer-software'
        };

        try {
            const support = await VideoDecoder.isConfigSupported(config);
            if (support.supported) {
                decoder.configure(support.config);
                S.hwAccel = label;
                break;
            }
        } catch {}
    }

    if (decoder.state !== 'configured') {
        S.hwAccel = 'NONE';
        return;
    }

    S.ready = true;
    reqKey?.();
    startKeyframeRetry();
    startPresentLoop();
};

// Decode a video frame
export const decodeFrame = data => {
    if (!S.ready) {
        reqKey?.();
        return;
    }

    // Need keyframe but got delta
    if (!data.isKey && S.needKey) {
        reqKey?.();
        startKeyframeRetry();
        return;
    }

    // Decoder not ready
    if (!S.decoder || S.decoder.state !== 'configured') {
        if (!S.reinit) {
            S.reinit = true;
            setTimeout(async () => {
                await initDecoder();
                S.reinit = false;
            }, 100);
        }
        return;
    }

    try {
        const queueSize = S.decoder.decodeQueueSize || 0;

        // Drop delta frames if queue is too full
        if (queueSize > MAX_DECODE_QUEUE_SIZE && !data.isKey) {
            reqKey?.();
            return;
        }

        // Validate capture timestamp
        if (!Number.isFinite(data.capTs) || data.capTs < 0) {
            return;
        }

        // Store frame metadata
        S.frameMeta.set(data.capTs, {
            capTs: data.capTs,
            decodeStartMs: performance.now(),
            arrivalMs: data.arrivalMs
        });

        // Clean up old metadata
        if (S.frameMeta.size > 30) {
            const keys = [...S.frameMeta.keys()].sort((a, b) => a - b);
            keys.slice(0, keys.length - 20).forEach(k => S.frameMeta.delete(k));
        }

        // Calculate frame duration
        const duration = (lastCapTs > 0 && data.capTs > lastCapTs)
            ? data.capTs - lastCapTs
            : 16667; // ~60fps default
        lastCapTs = data.capTs;

        // Decode the frame
        S.decoder.decode(new EncodedVideoChunk({
            type: data.isKey ? 'key' : 'delta',
            timestamp: data.capTs,
            duration: duration,
            data: data.buf
        }));

        // Clear keyframe need on successful keyframe
        if (data.isKey) {
            S.needKey = false;
            stopKeyframeRetry();
        }
    } catch {
        S.stats.decodeErrors++;
        S.needKey = true;
        S.ready = true;
        reqKey?.();
        startKeyframeRetry();
    }
};

// Initialize audio context and decoder
export const initAudio = async () => {
    if (S.audioCtx) {
        return true;
    }

    try {
        const ctx = S.audioCtx = new AudioContext({
            sampleRate: C.AUDIO_RATE,
            latencyHint: 'interactive'
        });

        const gain = S.audioGain = ctx.createGain();
        gain.gain.value = 1;
        gain.connect(ctx.destination);

        if (ctx.state === 'suspended') {
            await ctx.resume();
        }

        // Initialize audio decoder if available
        if (window.AudioDecoder) {
            const decoder = S.audioDecoder = new AudioDecoder({
                output: audioData => {
                    if (S.audioEnabled) {
                        playAudio(audioData);
                    }
                },
                error: () => {}
            });

            decoder.configure({
                codec: 'opus',
                sampleRate: C.AUDIO_RATE,
                numberOfChannels: C.AUDIO_CH
            });
        }

        return true;
    } catch {
        return false;
    }
};

// Play decoded audio data
const playAudio = audioData => {
    const { audioCtx: ctx, audioGain: gain } = S;

    if (!ctx || !gain) {
        return;
    }

    try {
        const {
            numberOfFrames: numFrames,
            numberOfChannels: numChannels,
            format,
            sampleRate
        } = audioData;

        const buffer = ctx.createBuffer(numChannels, numFrames, sampleRate);
        const isPlanar = format.includes('planar');
        const isS16 = format.includes('s16');

        for (let ch = 0; ch < numChannels; ch++) {
            const planeIndex = isPlanar ? ch : 0;
            const allocSize = audioData.allocationSize({ planeIndex });

            const tempView = isS16
                ? new Int16Array(allocSize / 2)
                : new Float32Array(allocSize / 4);

            const channelData = new Float32Array(numFrames);
            audioData.copyTo(tempView, { planeIndex });

            for (let i = 0; i < numFrames; i++) {
                const sampleIndex = isPlanar ? i : i * numChannels + ch;
                channelData[i] = isS16
                    ? tempView[sampleIndex] / 32768
                    : tempView[sampleIndex];
            }

            buffer.copyToChannel(channelData, ch);
        }

        const source = ctx.createBufferSource();
        source.buffer = buffer;
        source.connect(gain);
        source.start(ctx.currentTime + C.AUDIO_BUF);
    } catch {
        // Ignore playback errors
    } finally {
        audioData.close();
    }
};

// Handle incoming audio packet
export const handleAudioPkt = data => {
    if (!S.audioEnabled || !S.audioCtx) {
        return;
    }

    S.stats.audio++;

    const view = new DataView(data);
    const length = view.getUint16(14, true);

    if (length > data.byteLength - C.AUDIO_HEADER || S.audioDecoder?.state !== 'configured') {
        return;
    }

    try {
        S.audioDecoder.decode(new EncodedAudioChunk({
            type: 'key',
            timestamp: performance.now() * 1000,
            duration: 20000, // 20ms in microseconds
            data: new Uint8Array(data, C.AUDIO_HEADER, length)
        }));
    } catch {}
};

// Toggle audio on/off
export const toggleAudio = () => {
    const btn = $('aBtn');
    const btnText = $('aTxt');

    if (!S.audioEnabled) {
        initAudio().then(ok => {
            if (ok) {
                S.audioEnabled = true;
                btn.classList.add('on');
                btnText.textContent = 'Mute';
            }
        });
    } else {
        S.audioEnabled = false;
        btn.classList.remove('on');
        btnText.textContent = 'Enable';
    }
};

// Close audio context
export const closeAudio = () => {
    S.audioCtx?.close();
};

// Export timer stop function
export const stopKeyframeRetryTimer = stopKeyframeRetry;
