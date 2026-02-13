import { C, S, MSG, mkBuf, log, safe, safeAsync, recordMicPacket, recordMicEncodeError } from './state.js';

let micStream = null, micCtx = null, micWorklet = null, micEncoder = null, dcMicRef = null, micEnableCb = null, frameCtr = 0;

export const setDcMic = dc => { dcMicRef = dc; };
export const setMicEnableCallback = cb => { micEnableCb = cb; };

const MIC_WORKLET = `class P extends AudioWorkletProcessor{constructor(){super();this.bs=480;this.buf=new Float32Array(this.bs);this.bi=0}process(i){const s=i[0]?.[0];if(!s)return 1;for(let j=0;j<s.length;j++){this.buf[this.bi++]=s[j];if(this.bi>=this.bs){this.port.postMessage({type:'samples',data:this.buf.slice()});this.bi=0}}return 1}}registerProcessor('mic-processor',P);`;

const sendMicPkt = (opus, ts, samples) => {
    if (!dcMicRef || dcMicRef.readyState !== 'open') return 0;
    return safe(() => {
        const pkt = new ArrayBuffer(C.MIC_HEADER + opus.byteLength), v = new DataView(pkt);
        v.setUint32(0, MSG.MIC_DATA, 1); v.setBigUint64(4, BigInt(ts), 1);
        v.setUint16(12, samples, 1); v.setUint16(14, opus.byteLength, 1);
        new Uint8Array(pkt, C.MIC_HEADER).set(new Uint8Array(opus));
        dcMicRef.send(pkt); recordMicPacket(pkt.byteLength); return 1;
    }, 0);
};

const initEncoder = () => {
    if (!window.AudioEncoder) { log.error('MIC', 'No AudioEncoder'); return null; }
    const enc = new AudioEncoder({
        output: (chunk) => { const d = new ArrayBuffer(chunk.byteLength); chunk.copyTo(d); sendMicPkt(d, Math.floor(chunk.timestamp), C.MIC_RATE * C.MIC_FRAME_MS / 1000); },
        error: e => { log.error('MIC', 'Enc err', { e: e.message }); recordMicEncodeError(); }
    });
    enc.configure({ codec: 'opus', sampleRate: C.MIC_RATE, numberOfChannels: C.MIC_CH, bitrate: 32000, opus: { application: 'voip', frameDuration: 10000, complexity: 5, signal: 'voice', usedtx: 0, useinbandfec: 0 } });
    return enc;
};

const procSamples = samples => {
    if (!micEncoder || micEncoder.state !== 'configured') return;
    const ts = frameCtr++ * C.MIC_FRAME_MS * 1000;
    safe(() => { const ad = new AudioData({ format: 'f32', sampleRate: C.MIC_RATE, numberOfFrames: samples.length, numberOfChannels: C.MIC_CH, timestamp: ts, data: samples }); micEncoder.encode(ad); ad.close(); }, recordMicEncodeError());
};

export const startMic = async () => {
    if (S.micEnabled) return 1;
    try {
        micStream = await navigator.mediaDevices.getUserMedia({ audio: { sampleRate: C.MIC_RATE, channelCount: C.MIC_CH, echoCancellation: 1, noiseSuppression: 1, autoGainControl: 1 } });
        S.micStream = micStream;
        micCtx = new AudioContext({ sampleRate: C.MIC_RATE, latencyHint: 'interactive' });
        const blob = new Blob([MIC_WORKLET], { type: 'application/javascript' }), url = URL.createObjectURL(blob);
        try { await micCtx.audioWorklet.addModule(url); } finally { URL.revokeObjectURL(url); }
        micWorklet = new AudioWorkletNode(micCtx, 'mic-processor', { numberOfInputs: 1, numberOfOutputs: 0, channelCount: C.MIC_CH });
        micWorklet.port.onmessage = e => { if (e.data.type === 'samples' && S.micEnabled) procSamples(e.data.data); };
        micEncoder = initEncoder();
        if (!micEncoder) throw new Error('Enc init fail');
        micCtx.createMediaStreamSource(micStream).connect(micWorklet);
        if (micCtx.state === 'suspended') await micCtx.resume();
        S.micEnabled = 1; frameCtr = 0; micEnableCb?.(1);
        log.info('MIC', 'Started', { rate: C.MIC_RATE });
        return 1;
    } catch (e) { log.error('MIC', 'Start fail', { e: e.message }); stopMic(); return 0; }
};

export const stopMic = () => {
    S.micEnabled = 0;
    safe(() => micEncoder?.state !== 'closed' && micEncoder.close()); micEncoder = null;
    safe(() => micWorklet?.disconnect()); micWorklet = null;
    safe(() => micCtx?.close()); micCtx = null;
    if (micStream) { micStream.getTracks().forEach(t => t.stop()); micStream = null; S.micStream = null; }
    frameCtr = 0; micEnableCb?.(0);
    log.info('MIC', 'Stopped');
};

export const toggleMic = async () => S.micEnabled ? (stopMic(), 0) : await startMic();
export const isMicSupported = () => !!(navigator.mediaDevices?.getUserMedia && window.AudioEncoder && window.AudioData);
export const closeMic = stopMic;
