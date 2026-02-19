
import { C, S, $, mkBuf, MSG, CODECS, log, safe, safeAsync,
    recordDecodeTime, recordAudioPacket, recordAudioDecoded, recordAudioBufferHealth,
    recordAudioUnderrun, recordAudioOverflow, logVideoDrop, logAudioDrop } from './state.js';
import { queueFrameForPresentation, resetRenderer } from './renderer.js';
let reqKeyFn = null;
let sendAudioEnableFn = null;
let kfRetryInterval = null;
let lastCaptureTs = 0;
let workletNode = null;
let workletReady = false;

export const setReqKeyFn = fn => { reqKeyFn = fn; };
export const setSendAudioEnableFn = fn => { sendAudioEnableFn = fn; };
const stopKeyframeRetry = () => {
    if (kfRetryInterval) {
        clearInterval(kfRetryInterval);
        kfRetryInterval = null;
        log.debug('MEDIA', 'Keyframe retry stopped');
    }
};

const startKeyframeRetry = () => {
    stopKeyframeRetry();
    kfRetryInterval = setInterval(() => {
        if (S.needKey && S.ready && reqKeyFn) {
            log.debug('MEDIA', 'Requesting keyframe (retry)');
            reqKeyFn();
        } else if (!S.needKey) {
            stopKeyframeRetry();
        }
    }, 250);
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
                recordDecodeTime(now - meta.decodeStartMs, S.decoder?.decodeQueueSize || 0);
            }

            queueFrameForPresentation({
                frame,
                meta,
                queuedAt: now,
                timestamp: frame.timestamp,
                serverCapTs: meta?.capTs || frame.timestamp
            });
        },
        error: e => {
            logVideoDrop('Decoder error', { error: e.message, codec });
            S.stats.decodeErrors++;
            S.ready = 1;
            S.needKey = 1;
            reqKeyFn?.();
            startKeyframeRetry();
            tryReinitDecoder();
        }
    });
    let configured = false;
    for (const [preferHw, label] of [[true, 'HW'], [false, 'SW']]) {
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
        logVideoDrop('Decoder configuration failed', { codec });
        return;
    }

    S.ready = 1;
    reqKeyFn?.();
    startKeyframeRetry();
    resetRenderer();
};

export const decodeFrame = data => {
    if (!S.ready) {
        logVideoDrop('Decoder not ready');
        reqKeyFn?.();
        return;
    }

    if (!data.isKey && S.needKey) {
        logVideoDrop('Waiting for keyframe');
        reqKeyFn?.();
        startKeyframeRetry();
        return;
    }

    if (!S.decoder || S.decoder.state !== 'configured') {
        logVideoDrop('Decoder not configured');
        tryReinitDecoder();
        return;
    }

    const queueSize = S.decoder.decodeQueueSize || 0;
    if (queueSize > 6 && !data.isKey) {
        logVideoDrop('Decode queue full', { queueSize });
        reqKeyFn?.();
        return;
    }

    if (!Number.isFinite(data.capTs) || data.capTs < 0) {
        logVideoDrop('Invalid timestamp', { capTs: data.capTs });
        return;
    }
    S.frameMeta.set(data.capTs, {
        capTs: data.capTs,
        decodeStartMs: performance.now(),
        arrivalMs: data.arrivalMs
    });
    if (S.frameMeta.size > 30) {
        const sorted = [...S.frameMeta.keys()].sort((a, b) => a - b);
        sorted.slice(0, -20).forEach(k => S.frameMeta.delete(k));
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
            stopKeyframeRetry();
            log.debug('MEDIA', 'Keyframe decoded');
        }
    } catch (e) {
        logVideoDrop('Decode exception', { error: e.message });
        S.stats.decodeErrors++;
        S.needKey = 1;
        S.ready = 1;
        reqKeyFn?.();
        startKeyframeRetry();
    }
};
const resetAudioState = () => {
    if (workletNode) {
        safe(() => workletNode.port.postMessage({ type: 'clear' }), undefined, 'AUDIO');
        log.debug('AUDIO', 'Worklet state cleared');
    }
};
const WORKLET_CODE = `
class RingBuffer {
    constructor(channels, capacity) {
        this.ch = channels; this.cap = capacity;
        this.bufs = []; for (let i = 0; i < channels; i++) this.bufs.push(new Float32Array(capacity));
        this.rp = 0; this.wp = 0; this.avail = 0;
    }
    write(channelData, frames) {
        if (frames <= 0) return { written: 0, overflow: 0 };
        let overflow = 0;
        const space = this.cap - this.avail;
        if (frames > space) { overflow = frames - space; this.rp = (this.rp + overflow) % this.cap; this.avail -= overflow; }
        const toWrite = Math.min(frames, this.cap - this.avail);
        if (toWrite <= 0) return { written: 0, overflow };
        for (let c = 0; c < this.ch; c++) {
            const src = channelData[c] || channelData[0];
            for (let i = 0; i < toWrite; i++) this.bufs[c][(this.wp + i) % this.cap] = src[i] || 0;
        }
        this.wp = (this.wp + toWrite) % this.cap; this.avail += toWrite;
        return { written: toWrite, overflow };
    }
    read(output, frames) {
        const toRead = Math.min(frames, this.avail);
        for (let c = 0; c < this.ch; c++) {
            const out = output[c];
            for (let i = 0; i < frames; i++) out[i] = i < toRead ? this.bufs[c][(this.rp + i) % this.cap] : 0;
        }
        if (toRead > 0) { this.rp = (this.rp + toRead) % this.cap; this.avail -= toRead; }
        return toRead;
    }
    skip(n) { const toSkip = Math.min(n, this.avail); if (toSkip > 0) { this.rp = (this.rp + toSkip) % this.cap; this.avail -= toSkip; } return toSkip; }
    get length() { return this.avail; }
    clear() { this.rp = 0; this.wp = 0; this.avail = 0; }
}

class StreamAudioProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        this.rb = new RingBuffer(2, 9600);
        this.target = 2400; this.max = 4800; this.prebufThreshold = 1440;
        this.prebuffering = true; this.volume = 1; this.muted = false;
        this.samplesProcessed = 0; this.underruns = 0; this.overflows = 0;
        this.lastReport = 0; this.consecutiveUnderruns = 0;
        this.port.onmessage = e => {
            const { type, data } = e.data;
            if (type === 'audio') {
                const bufLen = this.rb.length;
                if (bufLen > this.max + this.target) {
                    const toSkip = bufLen - this.max;
                    if (toSkip > 0) { this.rb.skip(toSkip); this.overflows++; this.port.postMessage({ type: 'drop', reason: 'Overflow' }); }
                }
                const result = this.rb.write(data.channels, data.frames);
                if (result.overflow > 0) { this.overflows++; this.port.postMessage({ type: 'drop', reason: 'Write overflow' }); }
                if (result.written > 0) this.consecutiveUnderruns = 0;
                if (this.prebuffering && this.rb.length >= this.prebufThreshold) this.prebuffering = false;
            } else if (type === 'volume') this.volume = Math.max(0, Math.min(1, data));
            else if (type === 'mute') this.muted = data;
            else if (type === 'clear') { this.rb.clear(); this.underruns = 0; this.overflows = 0; this.consecutiveUnderruns = 0; this.prebuffering = true; }
        };
        this.port.postMessage({ type: 'ready' });
    }
    process(inputs, outputs) {
        const out = outputs[0];
        if (!out || !out.length) return true;
        const frames = out[0].length;
        const bufferMs = (this.rb.length / 48000) * 1000;
        if (this.muted || this.prebuffering) { for (let c = 0; c < out.length; c++) out[c].fill(0); this.samplesProcessed += frames; return true; }
        const read = this.rb.read(out, frames);
        if (this.volume !== 1) for (let c = 0; c < out.length; c++) for (let i = 0; i < frames; i++) out[c][i] *= this.volume;
        if (read < frames) {
            this.underruns++; this.consecutiveUnderruns++;
            this.port.postMessage({ type: 'drop', reason: 'Underrun' });
            if (this.consecutiveUnderruns >= 5) { this.prebuffering = true; this.consecutiveUnderruns = 0; }
        } else this.consecutiveUnderruns = 0;
        this.samplesProcessed += frames;
        if (this.samplesProcessed - this.lastReport >= 4800) {
            this.port.postMessage({ type: 'stats', bufferMs, underruns: this.underruns, overflows: this.overflows });
            this.lastReport = this.samplesProcessed; this.underruns = 0; this.overflows = 0;
        }
        return true;
    }
}
registerProcessor('stream-audio-processor', StreamAudioProcessor);
`;

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
        const blob = new Blob([WORKLET_CODE], { type: 'application/javascript' });
        const url = URL.createObjectURL(blob);
        try {
            await ctx.audioWorklet.addModule(url);
            log.debug('AUDIO', 'Worklet module loaded');
        } finally {
            URL.revokeObjectURL(url);
        }
        workletNode = new AudioWorkletNode(ctx, 'stream-audio-processor', {
            numberOfInputs: 0,
            numberOfOutputs: 1,
            outputChannelCount: [2]
        });

        workletNode.port.onmessage = e => {
            const { type } = e.data;
            if (type === 'ready') {
                workletReady = true;
                log.debug('AUDIO', 'Worklet ready');
            } else if (type === 'stats') {
                recordAudioBufferHealth(e.data.bufferMs);
                for (let i = 0; i < e.data.underruns; i++) recordAudioUnderrun();
                for (let i = 0; i < (e.data.overflows || 0); i++) recordAudioOverflow();
            } else if (type === 'drop') {
                logAudioDrop(e.data.reason);
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
    if (!S.audioEnabled || !S.audioCtx) return;

    recordAudioPacket();

    const view = new DataView(data);
    const length = view.getUint16(14, true);
    const timestamp = Number(view.getBigUint64(4, true));
    const samples = view.getUint16(12, true);

    if (length > data.byteLength - C.AUDIO_HEADER) {
        logAudioDrop('Invalid packet length', { length, total: data.byteLength });
        return;
    }

    if (S.audioDecoder?.state !== 'configured') {
        logAudioDrop('Decoder not configured');
        return;
    }

    const result = safe(() => {
        const chunk = new EncodedAudioChunk({
            type: 'key',
            timestamp,
            duration: Math.round((samples / C.AUDIO_RATE) * 1e6),
            data: new Uint8Array(data, C.AUDIO_HEADER, length)
        });
        S.audioDecoder.decode(chunk);
        return true;
    }, false, 'AUDIO');

    if (!result) {
        logAudioDrop('Decode failed');
    }
};

export const toggleAudio = () => {
    const btn = $('aBtn');
    const txt = $('aTxt');

    if (!S.audioEnabled) {
        initAudio().then(ok => {
            if (ok) {
                S.audioEnabled = 1;
                btn.classList.add('on');
                txt.textContent = 'Mute';
                sendAudioEnableFn?.(1);
                log.info('MEDIA', 'Audio enabled');
            } else {
                log.error('MEDIA', 'Failed to enable audio');
            }
        });
    } else {
        S.audioEnabled = 0;
        btn.classList.remove('on');
        txt.textContent = 'Enable';
        resetAudioState();
        sendAudioEnableFn?.(0);
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

export const stopKeyframeRetryTimer = stopKeyframeRetry;
