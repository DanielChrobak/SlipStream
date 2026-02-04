import { C, S, $, recordDecodeTime, recordAudioPacket, recordAudioDecoded, recordAudioDrop, recordAudioBufferHealth, recordAudioUnderrun, recordAudioOverflow, logVideoDrop } from './state.js';
import { queueFrameForPresentation, resetRenderer } from './renderer.js';

let reqKey = null, keyframeRetryInterval = null, lastCapTs = 0;
let audioWorkletNode = null, audioWorkletReady = false;

export const setReqKeyFn = fn => { reqKey = fn; };
const stopKeyframeRetry = () => { clearInterval(keyframeRetryInterval); keyframeRetryInterval = null; };

const startKeyframeRetry = () => {
    stopKeyframeRetry();
    keyframeRetryInterval = setInterval(() => {
        if (S.needKey && S.ready && reqKey) reqKey();
        else if (!S.needKey) stopKeyframeRetry();
    }, 250);
};

document.addEventListener('visibilitychange', () => {
    const wasVisible = S.tabVisible;
    S.tabVisible = document.visibilityState === 'visible';
    if (S.tabVisible && !wasVisible) { S.needKey = true; reqKey?.(); startKeyframeRetry(); }
});
S.tabVisible = document.visibilityState === 'visible';

const CODEC_MAP = { 0: 'av01.0.05M.08', 1: 'hev1.1.6.L93.B0', 2: 'avc1.42001f' };

export const initDecoder = async (force = false) => {
    if (!window.VideoDecoder) return;
    S.ready = false; S.needKey = true; lastCapTs = 0;
    if (force) S.W = S.H = 0;
    try { if (S.decoder?.state !== 'closed') S.decoder.close(); } catch (e) { console.warn('[Media] Failed to close decoder:', e.message); }

    const codec = CODEC_MAP[S.currentCodec] || CODEC_MAP[2];
    const decoder = S.decoder = new VideoDecoder({
        output: frame => {
            const now = performance.now(), meta = S.frameMeta.get(frame.timestamp);
            if (meta?.decodeStartMs) recordDecodeTime(now - meta.decodeStartMs, S.decoder?.decodeQueueSize || 0);
            queueFrameForPresentation({ frame, meta, queuedAt: now, timestamp: frame.timestamp, serverCapTs: meta?.capTs || frame.timestamp });
        },
        error: e => {
            console.warn('[Media] Decoder error:', e.message);
            S.ready = true; S.needKey = true; S.stats.decodeErrors++;
            logVideoDrop('Decoder error');
            reqKey?.(); startKeyframeRetry();
            if (!S.reinit) { S.reinit = true; setTimeout(async () => { await initDecoder(); S.reinit = false; }, 100); }
        }
    });

    let configured = false;
    for (const [hw, label] of [[true, 'HW'], [false, 'SW']]) {
        const cfg = { codec, optimizeForLatency: true, latencyMode: 'realtime', hardwareAcceleration: hw ? 'prefer-hardware' : 'prefer-software' };
        try {
            const sup = await VideoDecoder.isConfigSupported(cfg);
            if (sup.supported) { decoder.configure(sup.config); S.hwAccel = label; configured = true; break; }
        } catch (e) { console.warn('[Media] Decoder config check failed:', e.message); }
    }

    if (!configured || decoder.state !== 'configured') { S.hwAccel = 'NONE'; return; }
    S.ready = true;
    reqKey?.(); startKeyframeRetry(); resetRenderer();
};

export const decodeFrame = data => {
    if (!S.tabVisible) { logVideoDrop('Tab hidden'); return; }
    if (!S.ready) { logVideoDrop('Decoder not ready'); reqKey?.(); return; }
    if (!data.isKey && S.needKey) { logVideoDrop('Waiting for keyframe'); reqKey?.(); startKeyframeRetry(); return; }
    if (!S.decoder || S.decoder.state !== 'configured') {
        logVideoDrop('Decoder not configured');
        if (!S.reinit) { S.reinit = true; setTimeout(async () => { await initDecoder(); S.reinit = false; }, 100); }
        return;
    }
    try {
        const queueSize = S.decoder.decodeQueueSize || 0;
        if (queueSize > 6 && !data.isKey) { logVideoDrop('Decode queue full'); reqKey?.(); return; }
        if (!Number.isFinite(data.capTs) || data.capTs < 0) { logVideoDrop('Invalid timestamp'); return; }

        S.frameMeta.set(data.capTs, { capTs: data.capTs, decodeStartMs: performance.now(), arrivalMs: data.arrivalMs });
        if (S.frameMeta.size > 30) [...S.frameMeta.keys()].sort((a, b) => a - b).slice(0, -20).forEach(k => S.frameMeta.delete(k));

        const dur = (lastCapTs > 0 && data.capTs > lastCapTs) ? data.capTs - lastCapTs : 16667;
        lastCapTs = data.capTs;
        S.decoder.decode(new EncodedVideoChunk({ type: data.isKey ? 'key' : 'delta', timestamp: data.capTs, duration: dur, data: data.buf }));
        if (data.isKey) { S.needKey = false; stopKeyframeRetry(); }
    } catch (e) {
        console.warn('[Media] Decode exception:', e.message);
        S.stats.decodeErrors++; S.needKey = true; S.ready = true;
        logVideoDrop('Decode exception');
        reqKey?.(); startKeyframeRetry();
    }
};

const resetAudioState = () => {
    audioWorkletNode?.port.postMessage({ type: 'clear' });
};

const WORKLET_CODE = `
class RingBuf {
    constructor(ch, cap) {
        this.ch = ch;
        this.cap = cap;
        this.bufs = [];
        for (let i = 0; i < ch; i++) this.bufs.push(new Float32Array(cap));
        this.rp = 0;
        this.wp = 0;
        this.avail = 0;
    }

    write(cd, f) {
        if (f <= 0) return { w: 0, o: 0 };
        let o = 0;
        const space = this.cap - this.avail;
        if (f > space) {
            o = f - space;
            this.rp = (this.rp + o) % this.cap;
            this.avail -= o;
        }
        const tw = Math.min(f, this.cap - this.avail);
        if (tw <= 0) return { w: 0, o };
        for (let c = 0; c < this.ch; c++) {
            const s = cd[c] || cd[0];
            for (let i = 0; i < tw; i++) this.bufs[c][(this.wp + i) % this.cap] = s[i] || 0;
        }
        this.wp = (this.wp + tw) % this.cap;
        this.avail += tw;
        return { w: tw, o };
    }

    read(ob, f) {
        const tr = Math.min(f, this.avail);
        for (let c = 0; c < this.ch; c++) {
            const o = ob[c];
            for (let i = 0; i < f; i++) o[i] = i < tr ? this.bufs[c][(this.rp + i) % this.cap] : 0;
        }
        if (tr > 0) {
            this.rp = (this.rp + tr) % this.cap;
            this.avail -= tr;
        }
        return tr;
    }

    skip(n) {
        const toSkip = Math.min(n, this.avail);
        if (toSkip > 0) {
            this.rp = (this.rp + toSkip) % this.cap;
            this.avail -= toSkip;
        }
        return toSkip;
    }

    get len() { return this.avail; }

    clear() {
        this.rp = 0;
        this.wp = 0;
        this.avail = 0;
    }
}

class StreamAudioProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        this.rb = new RingBuf(2, 9600);
        this.targetSamples = 3360;
        this.maxSamples = 6720;
        this.prebufferThreshold = 2400;
        this.prebuffering = true;
        this.vol = 1;
        this.mute = false;
        this.sp = 0;
        this.ur = 0;
        this.of = 0;
        this.lr = 0;
        this.consecutiveUnderruns = 0;

        this.port.onmessage = e => {
            const { type, data } = e.data;
            if (type === 'audio') {
                const bufferLevel = this.rb.len;
                if (bufferLevel > this.maxSamples) {
                    const toSkip = bufferLevel - this.targetSamples;
                    if (toSkip > 0) {
                        this.rb.skip(toSkip);
                        this.of++;
                    }
                }
                const result = this.rb.write(data.channels, data.frames);
                if (result.o > 0) this.of++;
                if (result.w > 0) this.consecutiveUnderruns = 0;
                if (this.prebuffering && this.rb.len >= this.prebufferThreshold) {
                    this.prebuffering = false;
                }
            } else if (type === 'volume') {
                this.vol = Math.max(0, Math.min(1, data));
            } else if (type === 'mute') {
                this.mute = data;
            } else if (type === 'clear') {
                this.rb.clear();
                this.ur = 0;
                this.of = 0;
                this.consecutiveUnderruns = 0;
                this.prebuffering = true;
            }
        };

        this.port.postMessage({ type: 'ready' });
    }

    process(i, o) {
        const out = o[0];
        if (!out || !out.length) return true;

        const f = out[0].length;
        const bufferMs = (this.rb.len / 48000) * 1000;

        if (this.mute || this.prebuffering) {
            for (let c = 0; c < out.length; c++) out[c].fill(0);
            this.sp += f;
            return true;
        }

        const r = this.rb.read(out, f);

        if (this.vol !== 1) {
            for (let c = 0; c < out.length; c++) {
                for (let i = 0; i < f; i++) out[c][i] *= this.vol;
            }
        }

        if (r < f) {
            this.ur++;
            this.consecutiveUnderruns++;
            if (this.consecutiveUnderruns >= 5) {
                this.prebuffering = true;
                this.consecutiveUnderruns = 0;
            }
        } else {
            this.consecutiveUnderruns = 0;
        }

        this.sp += f;

        if (this.sp - this.lr >= 4800) {
            this.port.postMessage({
                type: 'stats',
                bufferMs,
                underruns: this.ur,
                overflows: this.of
            });
            this.lr = this.sp;
            this.ur = 0;
            this.of = 0;
        }

        return true;
    }
}

registerProcessor('stream-audio-processor', StreamAudioProcessor);
`;

export const initAudio = async () => {
    if (S.audioCtx && audioWorkletNode) {
        if (S.audioCtx.state === 'suspended') try { await S.audioCtx.resume(); } catch (e) { console.warn('[Audio] Failed to resume audio context:', e.message); }
        return true;
    }
    try {
        const ctx = S.audioCtx = new AudioContext({ sampleRate: C.AUDIO_RATE, latencyHint: 'interactive' });
        const gain = S.audioGain = ctx.createGain();
        gain.gain.value = 1; gain.connect(ctx.destination);

        if (ctx.state === 'suspended') await ctx.resume();

        try {
            const blob = new Blob([WORKLET_CODE], { type: 'application/javascript' });
            const url = URL.createObjectURL(blob);
            await ctx.audioWorklet.addModule(url);
            URL.revokeObjectURL(url);

            audioWorkletNode = new AudioWorkletNode(ctx, 'stream-audio-processor', {
                numberOfInputs: 0,
                numberOfOutputs: 1,
                outputChannelCount: [2]
            });

            audioWorkletNode.port.onmessage = e => {
                const { type } = e.data;
                if (type === 'ready') {
                    audioWorkletReady = true;
                } else if (type === 'stats') {
                    recordAudioBufferHealth(e.data.bufferMs);
                    for (let i = 0; i < e.data.underruns; i++) recordAudioUnderrun();
                    for (let i = 0; i < (e.data.overflows || 0); i++) recordAudioOverflow();
                }
            };

            audioWorkletNode.connect(gain);
        } catch (e) {
            console.warn('[Audio] Worklet initialization failed:', e.message);
            return false;
        }

        if (window.AudioDecoder) {
            const dec = S.audioDecoder = new AudioDecoder({
                output: ad => {
                    if (S.audioEnabled && audioWorkletReady) {
                        recordAudioDecoded();
                        sendToWorklet(ad);
                    } else {
                        ad.close();
                    }
                },
                error: e => {
                    console.warn('[Audio] Audio decoder error:', e.message);
                    recordAudioDrop();
                }
            });
            dec.configure({ codec: 'opus', sampleRate: C.AUDIO_RATE, numberOfChannels: C.AUDIO_CH });
        }

        return true;
    } catch (e) {
        console.warn('[Audio] Audio initialization failed:', e.message);
        return false;
    }
};

const sendToWorklet = ad => {
    if (!audioWorkletNode || !audioWorkletReady) {
        ad.close();
        return;
    }
    try {
        const { numberOfFrames: frames, numberOfChannels: nc, format } = ad;

        const channels = [], planar = format.includes('planar'), s16 = format.includes('s16');
        if (planar) {
            for (let ch = 0; ch < nc && ch < 2; ch++) {
                const sz = ad.allocationSize({ planeIndex: ch });
                if (s16) {
                    const tmp = new Int16Array(sz / 2); ad.copyTo(tmp, { planeIndex: ch });
                    const chData = new Float32Array(frames); for (let i = 0; i < frames; i++) chData[i] = tmp[i] / 32768; channels.push(chData);
                } else {
                    const chData = new Float32Array(frames); ad.copyTo(chData, { planeIndex: ch }); channels.push(chData);
                }
            }
        } else {
            const sz = ad.allocationSize({ planeIndex: 0 });
            if (s16) {
                const tmp = new Int16Array(sz / 2); ad.copyTo(tmp, { planeIndex: 0 });
                for (let ch = 0; ch < nc && ch < 2; ch++) {
                    const chData = new Float32Array(frames); for (let i = 0; i < frames; i++) chData[i] = tmp[i * nc + ch] / 32768; channels.push(chData);
                }
            } else {
                const tmp = new Float32Array(sz / 4); ad.copyTo(tmp, { planeIndex: 0 });
                for (let ch = 0; ch < nc && ch < 2; ch++) {
                    const chData = new Float32Array(frames); for (let i = 0; i < frames; i++) chData[i] = tmp[i * nc + ch]; channels.push(chData);
                }
            }
        }

        audioWorkletNode.port.postMessage({ type: 'audio', data: { channels, frames } }, channels.map(c => c.buffer));

    } catch (e) {
        console.warn('[Audio] Failed to send to worklet:', e.message);
        recordAudioDrop();
    } finally {
        ad.close();
    }
};

export const handleAudioPkt = data => {
    if (!S.audioEnabled || !S.audioCtx) return;

    recordAudioPacket();
    const view = new DataView(data), len = view.getUint16(14, true);
    const timestamp = Number(view.getBigUint64(4, true));
    const samples = view.getUint16(12, true);

    if (len > data.byteLength - C.AUDIO_HEADER || S.audioDecoder?.state !== 'configured') {
        recordAudioDrop();
        return;
    }

    const durationUs = Math.round((samples / C.AUDIO_RATE) * 1000000);

    try {
        S.audioDecoder.decode(new EncodedAudioChunk({
            type: 'key',
            timestamp,
            duration: durationUs,
            data: new Uint8Array(data, C.AUDIO_HEADER, len)
        }));
    } catch (e) {
        console.warn('[Audio] Audio decode failed:', e.message);
        recordAudioDrop();
    }
};

export const toggleAudio = () => {
    const btn = $('aBtn'), txt = $('aTxt');
    if (!S.audioEnabled) {
        initAudio().then(ok => {
            if (ok) {
                S.audioEnabled = true;
                btn.classList.add('on');
                txt.textContent = 'Mute';
            }
        });
    } else {
        S.audioEnabled = false;
        btn.classList.remove('on');
        txt.textContent = 'Enable';
        resetAudioState();
    }
};

export const closeAudio = () => {
    if (audioWorkletNode) { audioWorkletNode.disconnect(); audioWorkletNode = null; }
    audioWorkletReady = false;
    S.audioCtx?.close();
    resetAudioState();
};

export const stopKeyframeRetryTimer = stopKeyframeRetry;
