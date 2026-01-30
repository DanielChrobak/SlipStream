import { C, S, $, CODECS } from './state.js';
import { queueFrameForPresentation, startPresentLoop } from './renderer.js';
import { recordDecodeTime } from './state.js';

let reqKey = null, keyframeRetryInterval = null, lastCapTs = 0;

export const setReqKeyFn = fn => { reqKey = fn; };
const stopKeyframeRetry = () => { clearInterval(keyframeRetryInterval); keyframeRetryInterval = null; };

const startKeyframeRetry = () => {
    stopKeyframeRetry();
    keyframeRetryInterval = setInterval(() => {
        if (S.needKey && S.ready && reqKey) reqKey();
        else if (!S.needKey) stopKeyframeRetry();
    }, 500);
};

export const initDecoder = async (force = false) => {
    if (!window.VideoDecoder) return console.error('[DECODER] VideoDecoder API not available');

    S.ready = false;
    S.needKey = true;
    lastCapTs = 0;
    if (force) S.W = S.H = 0;

    S.presentQueue.forEach(f => { try { f.frame.close(); } catch {} });
    S.presentQueue = [];
    try { if (S.decoder?.state !== 'closed') S.decoder.close(); } catch {}

    const codecInfo = S.currentCodec === 1 ? CODECS.AV1 : CODECS.H264;
    console.log(`[DECODER] Initializing with codec: ${codecInfo.name} (${codecInfo.codec})`);

    const decoder = S.decoder = new VideoDecoder({
        output: frame => {
            const now = performance.now(), meta = S.frameMeta.get(frame.timestamp);
            if (meta?.decodeStartMs) recordDecodeTime(now - meta.decodeStartMs, S.decoder?.decodeQueueSize || 0);
            queueFrameForPresentation({ frame, meta, queuedAt: now, timestamp: frame.timestamp, serverCapTs: meta?.capTs || frame.timestamp });
            S.stats.dec++;
        },
        error: () => {
            S.ready = true; S.needKey = true; S.stats.decodeErrors++;
            reqKey?.(); startKeyframeRetry();
            if (!S.reinit) { S.reinit = true; setTimeout(async () => { await initDecoder(); S.reinit = false; }, 100); }
        }
    });

    let configured = false;
    for (const [hw, label] of [[true, 'HW'], [false, 'SW']]) {
        const cfg = { codec: codecInfo.codec, optimizeForLatency: true, latencyMode: 'realtime', hardwareAcceleration: hw ? 'prefer-hardware' : 'prefer-software' };
        try {
            const sup = await VideoDecoder.isConfigSupported(cfg);
            if (sup.supported) { decoder.configure(sup.config); S.hwAccel = label; configured = true; console.log(`[DECODER] Configured ${codecInfo.name} with ${label} acceleration`); break; }
        } catch {}
    }

    if (!configured || decoder.state !== 'configured') { S.hwAccel = 'NONE'; return; }
    S.ready = true;
    console.log(`[DECODER] Ready: ${codecInfo.name} (${S.hwAccel})`);
    reqKey?.(); startKeyframeRetry(); startPresentLoop();
};

export const decodeFrame = data => {
    if (!S.ready) { reqKey?.(); return; }
    if (!data.isKey && S.needKey) { reqKey?.(); startKeyframeRetry(); return; }

    if (!S.decoder || S.decoder.state !== 'configured') {
        if (!S.reinit) { S.reinit = true; setTimeout(async () => { await initDecoder(); S.reinit = false; }, 100); }
        return;
    }

    try {
        if ((S.decoder.decodeQueueSize || 0) > 4 && !data.isKey) { reqKey?.(); return; }
        if (!Number.isFinite(data.capTs) || data.capTs < 0) return;

        S.frameMeta.set(data.capTs, { capTs: data.capTs, decodeStartMs: performance.now(), arrivalMs: data.arrivalMs });
        if (S.frameMeta.size > 30) [...S.frameMeta.keys()].sort((a, b) => a - b).slice(0, -20).forEach(k => S.frameMeta.delete(k));

        const dur = (lastCapTs > 0 && data.capTs > lastCapTs) ? data.capTs - lastCapTs : 16667;
        lastCapTs = data.capTs;

        S.decoder.decode(new EncodedVideoChunk({ type: data.isKey ? 'key' : 'delta', timestamp: data.capTs, duration: dur, data: data.buf }));
        if (data.isKey) { S.needKey = false; stopKeyframeRetry(); }
    } catch { S.stats.decodeErrors++; S.needKey = true; S.ready = true; reqKey?.(); startKeyframeRetry(); }
};

export const initAudio = async () => {
    if (S.audioCtx) return true;
    try {
        const ctx = S.audioCtx = new AudioContext({ sampleRate: C.AUDIO_RATE, latencyHint: 'interactive' });
        const gain = S.audioGain = ctx.createGain();
        gain.gain.value = 1; gain.connect(ctx.destination);
        if (ctx.state === 'suspended') await ctx.resume();

        if (window.AudioDecoder) {
            const dec = S.audioDecoder = new AudioDecoder({ output: ad => { if (S.audioEnabled) playAudio(ad); }, error: () => {} });
            dec.configure({ codec: 'opus', sampleRate: C.AUDIO_RATE, numberOfChannels: C.AUDIO_CH });
        }
        return true;
    } catch { return false; }
};

const playAudio = ad => {
    const { audioCtx: ctx, audioGain: gain } = S;
    if (!ctx || !gain) return;

    try {
        const { numberOfFrames: nf, numberOfChannels: nc, format, sampleRate } = ad;
        const buf = ctx.createBuffer(nc, nf, sampleRate);
        const planar = format.includes('planar'), s16 = format.includes('s16');

        for (let ch = 0; ch < nc; ch++) {
            const pi = planar ? ch : 0, sz = ad.allocationSize({ planeIndex: pi });
            const tmp = s16 ? new Int16Array(sz / 2) : new Float32Array(sz / 4);
            const chData = new Float32Array(nf);
            ad.copyTo(tmp, { planeIndex: pi });
            for (let i = 0; i < nf; i++) { const si = planar ? i : i * nc + ch; chData[i] = s16 ? tmp[si] / 32768 : tmp[si]; }
            buf.copyToChannel(chData, ch);
        }

        const src = ctx.createBufferSource();
        src.buffer = buf; src.connect(gain); src.start(ctx.currentTime + C.AUDIO_BUF);
    } catch {} finally { ad.close(); }
};

export const handleAudioPkt = data => {
    if (!S.audioEnabled || !S.audioCtx) return;
    S.stats.audio++;
    const view = new DataView(data), len = view.getUint16(14, true);
    if (len > data.byteLength - C.AUDIO_HEADER || S.audioDecoder?.state !== 'configured') return;
    try { S.audioDecoder.decode(new EncodedAudioChunk({ type: 'key', timestamp: performance.now() * 1000, duration: 20000, data: new Uint8Array(data, C.AUDIO_HEADER, len) })); } catch {}
};

export const toggleAudio = () => {
    const btn = $('aBtn'), txt = $('aTxt');
    if (!S.audioEnabled) {
        initAudio().then(ok => { if (ok) { S.audioEnabled = true; btn.classList.add('on'); txt.textContent = 'Mute'; } });
    } else { S.audioEnabled = false; btn.classList.remove('on'); txt.textContent = 'Enable'; }
};

export const closeAudio = () => { S.audioCtx?.close(); };
export const stopKeyframeRetryTimer = stopKeyframeRetry;
