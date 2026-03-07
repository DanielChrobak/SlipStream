
import { C, MSG, CODECS } from './constants.js';
import { S, $, mkBuf, log, safe, safeAsync,
    recordDecodeTime, recordAudioPacket, recordAudioDecoded, recordAudioBufferHealth,
    recordAudioUnderrun, recordAudioOverflow, logVideoDrop, logAudioDrop } from './state.js';
import { queueFrameForPresentation, resetRenderer } from './renderer.js';
import { requestKeyframe, requestRecoveryKeyframe, sendAudioEnable, handleDecodePressure } from './protocol.js';
let kfRetryInterval = null;
let lastCaptureTs = 0;
let workletNode = null;
let workletReady = false;
let lastWaitingKeyLogAt = 0;
let decodeQueuePressureCount = 0;
let lastDecodeQueuePressureAt = 0;

const notifyDecodeQueuePressure = queueSize => {
    const now = performance.now();
    if (now - lastDecodeQueuePressureAt > 1500) {
        decodeQueuePressureCount = 0;
    }
    lastDecodeQueuePressureAt = now;
    decodeQueuePressureCount++;

    log.warn('MEDIA', 'Decode queue pressure', {
        queueSize, pressureCount: decodeQueuePressureCount, threshold: 12
    });

    if (decodeQueuePressureCount >= 3 && !S.needKey) {
        log.warn('MEDIA', 'Sustained decode pressure - requesting recovery keyframe', {
            queueSize, pressureCount: decodeQueuePressureCount
        });
        requestRecoveryKeyframe('decode-pressure');
    }

    if (decodeQueuePressureCount >= 12) {
        decodeQueuePressureCount = 0;
        log.error('MEDIA', 'Decode queue pressure threshold exceeded - triggering adaptive quality');
        handleDecodePressure({ reason: 'decode-queue-full', queueSize });
    }
};

const stopKeyframeRetry = () => {
    if (kfRetryInterval) {
        clearInterval(kfRetryInterval);
        kfRetryInterval = null;
        log.debug('MEDIA', 'Keyframe retry stopped');
    }
};

const startKeyframeRetry = () => {
    if (kfRetryInterval) return;
    kfRetryInterval = setInterval(() => {
        if (S.needKey && S.ready) {
            log.debug('MEDIA', 'Requesting keyframe (retry)');
            requestKeyframe('retry');
        } else if (!S.needKey) {
            stopKeyframeRetry();
        }
    }, C.KEY_RETRY_INTERVAL_MS);
    log.debug('MEDIA', 'Keyframe retry started');
};
const tryReinitDecoder = () => {
    if (!S.reinit) {
        S.reinit = 1;
        log.info('MEDIA', 'Scheduling decoder reinit');
        setTimeout(async () => {
            await initDecoder();
            S.reinit = 0;
        }, 100);
    }
};
const CODEC_MAP = Object.fromEntries(Object.values(CODECS).map(c => [c.id, c.codec]));

export const initDecoder = async (force = false) => {
    if (!window.VideoDecoder) {
        logVideoDrop('VideoDecoder API not available');
        return;
    }

    S.ready = 0;
    S.needKey = 1;
    lastCaptureTs = 0;
    decodeQueuePressureCount = 0;
    lastDecodeQueuePressureAt = 0;

    if (force) {
        S.W = S.H = 0;
        log.info('MEDIA', 'Force reinitializing decoder');
    }
    if (S.decoder && S.decoder.state !== 'closed') {
        safe(() => S.decoder.close(), undefined, 'MEDIA');
    }

    const codec = CODEC_MAP[S.currentCodec] || CODEC_MAP[2];
    log.info('MEDIA', 'Initializing decoder', { codec, codecId: S.currentCodec });

    const decoder = S.decoder = new VideoDecoder({
        output: frame => {
            const now = performance.now();
            const meta = S.frameMeta.get(frame.timestamp);

            if (meta?.decodeStartMs) {
                const decodeTime = now - meta.decodeStartMs;
                recordDecodeTime(decodeTime, S.decoder?.decodeQueueSize || 0);
                if (decodeTime > 50) {
                    log.warn('MEDIA', 'Slow decode', {
                        decodeMs: decodeTime.toFixed(1),
                        queueSize: S.decoder?.decodeQueueSize || 0,
                        w: frame.displayWidth, h: frame.displayHeight,
                        ts: frame.timestamp
                    });
                }
            }

            log.debug('MEDIA', 'Frame decoded', {
                ts: frame.timestamp, w: frame.displayWidth, h: frame.displayHeight,
                queueSize: S.decoder?.decodeQueueSize || 0
            });
            if (meta) meta.decodeOutputMs = now;

            queueFrameForPresentation({
                frame,
                meta,
                queuedAt: now,
                timestamp: frame.timestamp,
                sourceTs: meta?.sourceTs || meta?.capTs || frame.timestamp
            });
        },
        error: e => {
            log.error('MEDIA', 'Decoder error', {
                error: e.message, codec,
                decoderState: S.decoder?.state,
                queueSize: S.decoder?.decodeQueueSize || 0,
                decodeErrors: S.stats.decodeErrors
            });
            S.stats.decodeErrors++;
            S.ready = 1;
            S.needKey = 1;
            requestKeyframe('decoder-error');
            startKeyframeRetry();
            tryReinitDecoder();
        }
    });
    let configured = false;
    const decodeModes = [[true, 'HW'], [false, 'SW']];
    for (const [preferHw, label] of decodeModes) {
        const config = {
            codec,
            optimizeForLatency: true,
            latencyMode: 'realtime',
            hardwareAcceleration: preferHw ? 'prefer-hardware' : 'prefer-software'
        };

        const supported = await safeAsync(
            () => VideoDecoder.isConfigSupported(config),
            null,
            'MEDIA'
        );

        if (supported?.supported) {
            decoder.configure(supported.config);
            S.hwAccel = label;
            configured = true;
            log.info('MEDIA', 'Decoder configured', { codec, accel: label });
            break;
        }
    }

    if (!configured || decoder.state !== 'configured') {
        S.hwAccel = 'NONE';
        log.error('MEDIA', 'Decoder configuration failed', { codec, state: decoder.state });
        logVideoDrop('Decoder configuration failed', { codec });
        return;
    }

    S.ready = 1;
    requestKeyframe('decoder-init');
    startKeyframeRetry();
    resetRenderer();
};

export const decodeFrame = data => {
    if (!S.ready) {
        logVideoDrop('Decoder not ready', { decoderState: S.decoder?.state, needKey: S.needKey });
        requestKeyframe('decoder-not-ready');
        return false;
    }

    if (!data.isKey && S.needKey) {
        const now = performance.now();
        if (now - lastWaitingKeyLogAt >= 1000) {
            logVideoDrop('Waiting for keyframe', {
                capTs: data.capTs, timeSinceLastLog: (now - lastWaitingKeyLogAt).toFixed(0)
            });
            lastWaitingKeyLogAt = now;
        }
        startKeyframeRetry();
        return false;
    }

    if (!S.decoder || S.decoder.state !== 'configured') {
        logVideoDrop('Decoder not configured', {
            hasDecoder: !!S.decoder, state: S.decoder?.state,
            needKey: S.needKey, ready: S.ready
        });
        tryReinitDecoder();
        return false;
    }

    const queueSize = S.decoder.decodeQueueSize || 0;
    if (queueSize > 6 && !data.isKey) {
        logVideoDrop('Decode queue full', {
            queueSize, isKey: data.isKey ? 1 : 0, capTs: data.capTs
        });
        notifyDecodeQueuePressure(queueSize);
        return false;
    }

    if (!Number.isFinite(data.capTs) || data.capTs < 0) {
        logVideoDrop('Invalid timestamp', {
            capTs: data.capTs, isKey: data.isKey ? 1 : 0,
            bufSize: data.buf?.byteLength
        });
        return false;
    }
    S.frameMeta.set(data.capTs, {
        capTs: data.capTs,
        sourceTs: data.sourceTs || data.capTs,
        encodeEndTs: data.encodeEndTs || 0,
        enqueueTs: data.enqueueTs || 0,
        firstPacketMs: data.firstPacketMs,
        lastPacketMs: data.lastPacketMs,
        reassemblyCompleteMs: data.reassemblyCompleteMs,
        frameKey: data.capTs,
        decodeStartMs: performance.now(),
        arrivalMs: data.arrivalMs
    });
    if (S.frameMeta.size > 30) {
        while (S.frameMeta.size > 20) {
            const oldest = S.frameMeta.keys().next().value;
            if (oldest === undefined) break;
            S.frameMeta.delete(oldest);
        }
    }
    const duration = (lastCaptureTs > 0 && data.capTs > lastCaptureTs)
        ? data.capTs - lastCaptureTs
        : 16667;
    lastCaptureTs = data.capTs;

    try {
        const chunk = new EncodedVideoChunk({
            type: data.isKey ? 'key' : 'delta',
            timestamp: data.capTs,
            duration,
            data: data.buf
        });
        S.decoder.decode(chunk);

        if (data.isKey) {
            S.needKey = 0;
            lastWaitingKeyLogAt = 0;
            stopKeyframeRetry();
            log.info('MEDIA', 'Keyframe decoded', {
                capTs: data.capTs, size: data.buf?.byteLength,
                queueSize: S.decoder?.decodeQueueSize || 0
            });
        } else {
            log.debug('MEDIA', 'Delta frame submitted', {
                capTs: data.capTs, size: data.buf?.byteLength,
                duration, queueSize: S.decoder?.decodeQueueSize || 0
            });
        }
        return true;
    } catch (e) {
        logVideoDrop('Decode exception', {
            error: e.message, isKey: data.isKey ? 1 : 0,
            capTs: data.capTs, size: data.buf?.byteLength,
            decoderState: S.decoder?.state,
            queueSize: S.decoder?.decodeQueueSize || 0
        }, { countDropped: false });
        S.stats.decodeErrors++;
        S.needKey = 1;
        S.ready = 1;
        requestKeyframe('decode-exception');
        startKeyframeRetry();
        return false;
    }
};
const resetAudioState = () => {
    if (workletNode) {
        safe(() => workletNode.port.postMessage({ type: 'clear' }), undefined, 'AUDIO');
        log.debug('AUDIO', 'Worklet state cleared');
    }
};
const AUDIO_WORKLET_URL = new URL('./audio-worklet.js', import.meta.url).href;

export const initAudio = async () => {
    if (S.audioCtx && workletNode) {
        if (S.audioCtx.state === 'suspended') {
            await safeAsync(() => S.audioCtx.resume(), undefined, 'AUDIO');
        }
        log.debug('AUDIO', 'Resumed existing context');
        return true;
    }

    try {
        log.info('AUDIO', 'Initializing audio system');
        const ctx = S.audioCtx = new AudioContext({
            sampleRate: C.AUDIO_RATE,
            latencyHint: 'interactive'
        });
        const gain = S.audioGain = ctx.createGain();
        gain.gain.value = 1;
        gain.connect(ctx.destination);
        if (ctx.state === 'suspended') {
            await ctx.resume();
            log.debug('AUDIO', 'Context resumed');
        }
        await ctx.audioWorklet.addModule(AUDIO_WORKLET_URL);
        log.debug('AUDIO', 'Worklet module loaded');
        workletNode = new AudioWorkletNode(ctx, 'stream-audio-processor', {
            numberOfInputs: 0,
            numberOfOutputs: 1,
            outputChannelCount: [2]
        });

        workletNode.port.onmessage = e => {
            const { type } = e.data;
            if (type === 'ready') {
                workletReady = true;
                log.info('AUDIO', 'Worklet ready');
            } else if (type === 'stats') {
                recordAudioBufferHealth(e.data.bufferMs);
                if (e.data.underruns > 0) {
                    log.warn('AUDIO', 'Worklet underruns', {
                        count: e.data.underruns, bufferMs: e.data.bufferMs
                    });
                }
                for (let i = 0; i < e.data.underruns; i++) recordAudioUnderrun();
                if ((e.data.overflows || 0) > 0) {
                    log.warn('AUDIO', 'Worklet overflows', {
                        count: e.data.overflows, bufferMs: e.data.bufferMs
                    });
                }
                for (let i = 0; i < (e.data.overflows || 0); i++) recordAudioOverflow();
            } else if (type === 'drop') {
                logAudioDrop(e.data.reason);
            } else {
                log.warn('AUDIO', 'Unknown worklet message type', { type });
            }
        };

        workletNode.connect(gain);
        if (window.AudioDecoder) {
            const decoder = S.audioDecoder = new AudioDecoder({
                output: audioData => {
                    if (S.audioEnabled && workletReady) {
                        recordAudioDecoded();
                        sendToWorklet(audioData);
                    } else {
                        logAudioDrop('Discarded (disabled)');
                        audioData.close();
                    }
                },
                error: e => {
                    log.error('AUDIO', 'AudioDecoder error', {
                        error: e.message,
                        decoderState: S.audioDecoder?.state,
                        queueSize: S.audioDecoder?.decodeQueueSize || 0
                    });
                    logAudioDrop('Decoder error', { error: e.message });
                }
            });

            decoder.configure({
                codec: 'opus',
                sampleRate: C.AUDIO_RATE,
                numberOfChannels: C.AUDIO_CH
            });
            log.debug('AUDIO', 'Decoder configured');
        } else {
            log.warn('AUDIO', 'AudioDecoder API not available');
        }

        log.info('AUDIO', 'Audio system initialized');
        return true;

    } catch (e) {
        logAudioDrop('Init failed', { error: e.message });
        return false;
    }
};
const extractChannel = (audioData, frames, numChannels, planar, isS16, channel) => {
    const size = audioData.allocationSize({ planeIndex: planar ? channel : 0 });
    const output = new Float32Array(frames);

    if (planar) {
        if (isS16) {
            const temp = new Int16Array(size / 2);
            audioData.copyTo(temp, { planeIndex: channel });
            for (let i = 0; i < frames; i++) output[i] = temp[i] / 32768;
        } else {
            audioData.copyTo(output, { planeIndex: channel });
        }
    } else {
        if (isS16) {
            const temp = new Int16Array(size / 2);
            audioData.copyTo(temp, { planeIndex: 0 });
            for (let i = 0; i < frames; i++) output[i] = temp[i * numChannels + channel] / 32768;
        } else {
            const temp = new Float32Array(size / 4);
            audioData.copyTo(temp, { planeIndex: 0 });
            for (let i = 0; i < frames; i++) output[i] = temp[i * numChannels + channel];
        }
    }

    return output;
};
const sendToWorklet = audioData => {
    if (!workletNode || !workletReady) {
        logAudioDrop('Worklet not ready');
        audioData.close();
        return;
    }

    try {
        const { numberOfFrames: frames, numberOfChannels: numCh, format } = audioData;
        const planar = format.includes('planar');
        const isS16 = format.includes('s16');

        const channels = [];
        for (let c = 0; c < numCh && c < 2; c++) {
            channels.push(extractChannel(audioData, frames, numCh, planar, isS16, c));
        }

        workletNode.port.postMessage(
            { type: 'audio', data: { channels, frames } },
            channels.map(c => c.buffer)
        );
    } catch (e) {
        logAudioDrop('Send to worklet failed', { error: e.message });
    } finally {
        audioData.close();
    }
};

export const handleAudioPacket = data => {
    if (!S.audioEnabled || !S.audioCtx) {
        log.debug('AUDIO', 'Packet dropped - audio not enabled', {
            enabled: S.audioEnabled, hasCtx: !!S.audioCtx
        });
        return;
    }

    recordAudioPacket();

    const view = new DataView(data);
    const timestamp = Number(view.getBigUint64(4, true));
    const samples = view.getUint16(16, true);
    const payloadLength = view.getUint16(18, true);

    if (payloadLength > data.byteLength - C.AUDIO_HEADER) {
        logAudioDrop('Invalid packet length', { length: payloadLength, total: data.byteLength });
        return;
    }

    if (S.audioDecoder?.state !== 'configured') {
        logAudioDrop('Decoder not configured', {
            state: S.audioDecoder?.state,
            hasDecoder: !!S.audioDecoder
        });
        return;
    }

    const result = safe(() => {
        const chunk = new EncodedAudioChunk({
            type: 'key',
            timestamp,
            duration: Math.round((samples / C.AUDIO_RATE) * 1e6),
            data: new Uint8Array(data, C.AUDIO_HEADER, payloadLength)
        });
        S.audioDecoder.decode(chunk);
        return true;
    }, false, 'AUDIO');

    if (!result) {
        logAudioDrop('Decode failed', {
            timestamp, samples, length: payloadLength,
            decoderState: S.audioDecoder?.state,
            queueSize: S.audioDecoder?.decodeQueueSize || 0
        });
    }
};

export const toggleAudio = async () => {
    const btn = $('aBtn');
    const txt = $('aTxt');

    if (!S.audioEnabled) {
        try {
            const ok = await initAudio();
            if (ok) {
                S.audioEnabled = 1;
                btn.classList.add('on');
                txt.textContent = 'Mute';
                sendAudioEnable(1, { suppressIfClosed: true });
                log.info('MEDIA', 'Audio enabled');
            } else {
                log.error('MEDIA', 'Failed to enable audio');
            }
        } catch (e) {
            log.error('MEDIA', 'Failed to enable audio', { error: e?.message });
        }
    } else {
        S.audioEnabled = 0;
        btn.classList.remove('on');
        txt.textContent = 'Enable';
        resetAudioState();
        sendAudioEnable(0, { suppressIfClosed: true });
        log.info('MEDIA', 'Audio disabled');
    }
};

export const closeAudio = () => {
    if (workletNode) {
        safe(() => workletNode.disconnect(), undefined, 'AUDIO');
        workletNode = null;
        workletReady = false;
    }

    safe(() => S.audioCtx?.close(), undefined, 'AUDIO');

    resetAudioState();
    log.info('MEDIA', 'Audio closed');
};

export { stopKeyframeRetry };
