import { C, S, $, mkBuf, MSG, CODECS, log, safe, safeAsync, recordDecodeTime, recordAudioPacket, recordAudioDecoded, recordAudioBufferHealth, recordAudioUnderrun, recordAudioOverflow, logVideoDrop, logAudioDrop } from './state.js';
import { queueFrameForPresentation, resetRenderer } from './renderer.js';

let reqKey = null, kfRetryInt = null, lastCapTs = 0, awNode = null, awReady = 0, sendAudioEnFn = null;

export const setReqKeyFn = fn => { reqKey = fn; };
export const setSendAudioEnableFn = fn => { sendAudioEnFn = fn; };
const stopKfRetry = () => { clearInterval(kfRetryInt); kfRetryInt = null; };

const startKfRetry = () => { stopKfRetry(); kfRetryInt = setInterval(() => { if (S.needKey && S.ready && reqKey) reqKey(); else if (!S.needKey) stopKfRetry(); }, 250); };

const CODEC_MAP = Object.fromEntries(Object.values(CODECS).map(c => [c.id, c.codec]));

export const initDecoder = async (force = 0) => {
    if (!window.VideoDecoder) { logVideoDrop('No VideoDecoder'); return; }
    S.ready = 0; S.needKey = 1; lastCapTs = 0;
    if (force) S.W = S.H = 0;
    safe(() => S.decoder?.state !== 'closed' && S.decoder.close());
    const codec = CODEC_MAP[S.currentCodec] || CODEC_MAP[2];
    const dec = S.decoder = new VideoDecoder({
        output: fr => {
            const now = performance.now(), m = S.frameMeta.get(fr.timestamp);
            if (m?.decodeStartMs) recordDecodeTime(now - m.decodeStartMs, S.decoder?.decodeQueueSize || 0);
            queueFrameForPresentation({ frame: fr, meta: m, queuedAt: now, timestamp: fr.timestamp, serverCapTs: m?.capTs || fr.timestamp });
        },
        error: e => {
            logVideoDrop('Dec err', { e: e.message, codec });
            S.stats.decodeErrors++; S.ready = 1; S.needKey = 1; reqKey?.(); startKfRetry();
            if (!S.reinit) { S.reinit = 1; setTimeout(async () => { await initDecoder(); S.reinit = 0; }, 100); }
        }
    });
    let cfg = 0;
    for (const [hw, lbl] of [[1, 'HW'], [0, 'SW']]) {
        const c = { codec, optimizeForLatency: 1, latencyMode: 'realtime', hardwareAcceleration: hw ? 'prefer-hardware' : 'prefer-software' };
        const sup = await safeAsync(() => VideoDecoder.isConfigSupported(c));
        if (sup?.supported) { dec.configure(sup.config); S.hwAccel = lbl; cfg = 1; break; }
    }
    if (!cfg || dec.state !== 'configured') { S.hwAccel = 'NONE'; logVideoDrop('Dec not cfg', { codec }); return; }
    S.ready = 1; log.info('MEDIA', 'Dec init', { codec, hw: S.hwAccel });
    reqKey?.(); startKfRetry(); resetRenderer();
};

export const decodeFrame = d => {
    if (!S.ready) { logVideoDrop('Not ready'); reqKey?.(); return; }
    if (!d.isKey && S.needKey) { logVideoDrop('Need kf'); reqKey?.(); startKfRetry(); return; }
    if (!S.decoder || S.decoder.state !== 'configured') {
        logVideoDrop('Not cfg');
        if (!S.reinit) { S.reinit = 1; setTimeout(async () => { await initDecoder(); S.reinit = 0; }, 100); }
        return;
    }
    try {
        const qs = S.decoder.decodeQueueSize || 0;
        if (qs > 6 && !d.isKey) { logVideoDrop('Queue full', { qs }); reqKey?.(); return; }
        if (!Number.isFinite(d.capTs) || d.capTs < 0) { logVideoDrop('Bad ts'); return; }
        S.frameMeta.set(d.capTs, { capTs: d.capTs, decodeStartMs: performance.now(), arrivalMs: d.arrivalMs });
        if (S.frameMeta.size > 30) [...S.frameMeta.keys()].sort((a, b) => a - b).slice(0, -20).forEach(k => S.frameMeta.delete(k));
        const dur = (lastCapTs > 0 && d.capTs > lastCapTs) ? d.capTs - lastCapTs : 16667;
        lastCapTs = d.capTs;
        S.decoder.decode(new EncodedVideoChunk({ type: d.isKey ? 'key' : 'delta', timestamp: d.capTs, duration: dur, data: d.buf }));
        if (d.isKey) { S.needKey = 0; stopKfRetry(); }
    } catch (e) { logVideoDrop('Dec err', { e: e.message }); S.stats.decodeErrors++; S.needKey = 1; S.ready = 1; reqKey?.(); startKfRetry(); }
};

const resetAudioSt = () => safe(() => awNode?.port.postMessage({ type: 'clear' }));

const WORKLET_CODE = `class RingBuf{constructor(ch,cap){this.ch=ch;this.cap=cap;this.bufs=[];for(let i=0;i<ch;i++)this.bufs.push(new Float32Array(cap));this.rp=0;this.wp=0;this.avail=0}write(cd,f){if(f<=0)return{w:0,o:0};let o=0;const sp=this.cap-this.avail;if(f>sp){o=f-sp;this.rp=(this.rp+o)%this.cap;this.avail-=o}const tw=Math.min(f,this.cap-this.avail);if(tw<=0)return{w:0,o};for(let c=0;c<this.ch;c++){const s=cd[c]||cd[0];for(let i=0;i<tw;i++)this.bufs[c][(this.wp+i)%this.cap]=s[i]||0}this.wp=(this.wp+tw)%this.cap;this.avail+=tw;return{w:tw,o}}read(ob,f){const tr=Math.min(f,this.avail);for(let c=0;c<this.ch;c++){const o=ob[c];for(let i=0;i<f;i++)o[i]=i<tr?this.bufs[c][(this.rp+i)%this.cap]:0}if(tr>0){this.rp=(this.rp+tr)%this.cap;this.avail-=tr}return tr}skip(n){const ts=Math.min(n,this.avail);if(ts>0){this.rp=(this.rp+ts)%this.cap;this.avail-=ts}return ts}get len(){return this.avail}clear(){this.rp=0;this.wp=0;this.avail=0}}class StreamAudioProcessor extends AudioWorkletProcessor{constructor(){super();this.rb=new RingBuf(2,9600);this.tgt=2400;this.max=4800;this.pre=1440;this.prebuf=!0;this.vol=1;this.mute=!1;this.sp=0;this.ur=0;this.of=0;this.lr=0;this.cu=0;this.port.onmessage=e=>{const{type,data}=e.data;if(type==='audio'){const bl=this.rb.len;if(bl>this.max+this.tgt){const ts=bl-this.max;if(ts>0){this.rb.skip(ts);this.of++;this.port.postMessage({type:'drop',reason:'Overflow'})}}const r=this.rb.write(data.channels,data.frames);if(r.o>0){this.of++;this.port.postMessage({type:'drop',reason:'Write overflow'})}if(r.w>0)this.cu=0;if(this.prebuf&&this.rb.len>=this.pre)this.prebuf=!1}else if(type==='volume')this.vol=Math.max(0,Math.min(1,data));else if(type==='mute')this.mute=data;else if(type==='clear'){this.rb.clear();this.ur=0;this.of=0;this.cu=0;this.prebuf=!0}};this.port.postMessage({type:'ready'})}process(i,o){const out=o[0];if(!out||!out.length)return!0;const f=out[0].length,bms=(this.rb.len/48000)*1000;if(this.mute||this.prebuf){for(let c=0;c<out.length;c++)out[c].fill(0);this.sp+=f;return!0}const r=this.rb.read(out,f);if(this.vol!==1)for(let c=0;c<out.length;c++)for(let i=0;i<f;i++)out[c][i]*=this.vol;if(r<f){this.ur++;this.cu++;this.port.postMessage({type:'drop',reason:'Underrun'});if(this.cu>=5){this.prebuf=!0;this.cu=0}}else this.cu=0;this.sp+=f;if(this.sp-this.lr>=4800){this.port.postMessage({type:'stats',bufferMs:bms,underruns:this.ur,overflows:this.of});this.lr=this.sp;this.ur=0;this.of=0}return!0}}registerProcessor('stream-audio-processor',StreamAudioProcessor);`;

export const initAudio = async () => {
    if (S.audioCtx && awNode) { await safeAsync(() => S.audioCtx.state === 'suspended' && S.audioCtx.resume()); return 1; }
    try {
        const ctx = S.audioCtx = new AudioContext({ sampleRate: C.AUDIO_RATE, latencyHint: 'interactive' });
        const g = S.audioGain = ctx.createGain(); g.gain.value = 1; g.connect(ctx.destination);
        if (ctx.state === 'suspended') await ctx.resume();
        const blob = new Blob([WORKLET_CODE], { type: 'application/javascript' }), url = URL.createObjectURL(blob);
        try { await ctx.audioWorklet.addModule(url); } finally { URL.revokeObjectURL(url); }
        awNode = new AudioWorkletNode(ctx, 'stream-audio-processor', { numberOfInputs: 0, numberOfOutputs: 1, outputChannelCount: [2] });
        awNode.port.onmessage = e => {
            const { type } = e.data;
            if (type === 'ready') awReady = 1;
            else if (type === 'stats') { recordAudioBufferHealth(e.data.bufferMs); for (let i = 0; i < e.data.underruns; i++) recordAudioUnderrun(); for (let i = 0; i < (e.data.overflows || 0); i++) recordAudioOverflow(); }
            else if (type === 'drop') logAudioDrop(e.data.reason);
        };
        awNode.connect(g);
        if (window.AudioDecoder) {
            const dec = S.audioDecoder = new AudioDecoder({
                output: ad => { if (S.audioEnabled && awReady) { recordAudioDecoded(); sendToWorklet(ad); } else { logAudioDrop('Discarded'); ad.close(); } },
                error: e => logAudioDrop('Dec err', { e: e.message })
            });
            dec.configure({ codec: 'opus', sampleRate: C.AUDIO_RATE, numberOfChannels: C.AUDIO_CH });
        }
        log.info('MEDIA', 'Audio init');
        return 1;
    } catch (e) { logAudioDrop('Init fail', { e: e.message }); return 0; }
};

const sendToWorklet = ad => {
    if (!awNode || !awReady) { logAudioDrop('Worklet !rdy'); ad.close(); return; }
    try {
        const { numberOfFrames: fr, numberOfChannels: nc, format } = ad;
        const chs = [], pl = format.includes('planar'), s16 = format.includes('s16');
        if (pl) { for (let c = 0; c < nc && c < 2; c++) { const sz = ad.allocationSize({ planeIndex: c }); if (s16) { const t = new Int16Array(sz / 2); ad.copyTo(t, { planeIndex: c }); const d = new Float32Array(fr); for (let i = 0; i < fr; i++) d[i] = t[i] / 32768; chs.push(d); } else { const d = new Float32Array(fr); ad.copyTo(d, { planeIndex: c }); chs.push(d); } } }
        else { const sz = ad.allocationSize({ planeIndex: 0 }); if (s16) { const t = new Int16Array(sz / 2); ad.copyTo(t, { planeIndex: 0 }); for (let c = 0; c < nc && c < 2; c++) { const d = new Float32Array(fr); for (let i = 0; i < fr; i++) d[i] = t[i * nc + c] / 32768; chs.push(d); } } else { const t = new Float32Array(sz / 4); ad.copyTo(t, { planeIndex: 0 }); for (let c = 0; c < nc && c < 2; c++) { const d = new Float32Array(fr); for (let i = 0; i < fr; i++) d[i] = t[i * nc + c]; chs.push(d); } } }
        awNode.port.postMessage({ type: 'audio', data: { channels: chs, frames: fr } }, chs.map(c => c.buffer));
    } catch (e) { logAudioDrop('Send fail', { e: e.message }); }
    finally { ad.close(); }
};

export const handleAudioPkt = data => {
    if (!S.audioEnabled || !S.audioCtx) return;
    recordAudioPacket();
    const v = new DataView(data), len = v.getUint16(14, 1), ts = Number(v.getBigUint64(4, 1)), smp = v.getUint16(12, 1);
    if (len > data.byteLength - C.AUDIO_HEADER) { logAudioDrop('Bad len'); return; }
    if (S.audioDecoder?.state !== 'configured') { logAudioDrop('Not cfg'); return; }
    safe(() => S.audioDecoder.decode(new EncodedAudioChunk({ type: 'key', timestamp: ts, duration: Math.round((smp / C.AUDIO_RATE) * 1e6), data: new Uint8Array(data, C.AUDIO_HEADER, len) })));
};

export const toggleAudio = () => {
    const btn = $('aBtn'), txt = $('aTxt');
    if (!S.audioEnabled) initAudio().then(ok => { if (ok) { S.audioEnabled = 1; btn.classList.add('on'); txt.textContent = 'Mute'; sendAudioEnFn?.(1); log.info('MEDIA', 'Audio on'); } });
    else { S.audioEnabled = 0; btn.classList.remove('on'); txt.textContent = 'Enable'; resetAudioSt(); sendAudioEnFn?.(0); log.info('MEDIA', 'Audio off'); }
};

export const closeAudio = () => {
    safe(() => awNode?.disconnect()); awNode = null; awReady = 0;
    safe(() => S.audioCtx?.close());
    resetAudioSt(); log.info('MEDIA', 'Audio closed');
};

export const stopKeyframeRetryTimer = stopKfRetry;
