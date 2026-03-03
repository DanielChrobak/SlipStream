
import { C, MSG } from './constants.js';
import { S, mkBuf, log, safe, safeAsync, recordMicPacket, recordMicEncodeError } from './state.js';
import { sendMicEnable } from './protocol.js';
let micStream = null;
let micContext = null;
let micWorklet = null;
let micEncoder = null;
let frameCounter = 0;
const MIC_WORKLET_URL = new URL('./mic-worklet.js', import.meta.url).href;
const sendMicPacket = (opusData, timestamp, samples) => {
    if (!S.dcMic || S.dcMic.readyState !== 'open') {
        log.debug('MIC', 'Channel not open, packet dropped');
        return false;
    }

    return safe(() => {
        const payload = opusData instanceof Uint8Array ? opusData : new Uint8Array(opusData);
        const packet = new ArrayBuffer(C.MIC_HEADER + payload.byteLength);
        const view = new DataView(packet);

        view.setUint32(0, MSG.MIC_DATA, true);
        view.setBigUint64(4, BigInt(timestamp), true);
        view.setUint16(12, samples, true);
        view.setUint16(14, payload.byteLength, true);
        new Uint8Array(packet, C.MIC_HEADER).set(payload);

        S.dcMic.send(packet);
        recordMicPacket(packet.byteLength);
        return true;
    }, false, 'MIC');
};
const initEncoder = () => {
    if (!window.AudioEncoder) {
        log.error('MIC', 'AudioEncoder API not available');
        return null;
    }

    try {
        const encoder = new AudioEncoder({
            output: chunk => {
                const data = new ArrayBuffer(chunk.byteLength);
                chunk.copyTo(data);
                const samples = C.MIC_RATE * C.MIC_FRAME_MS / 1000;
                sendMicPacket(data, Math.floor(chunk.timestamp), samples);
            },
            error: e => {
                log.error('MIC', 'Encoder error', { error: e.message });
                recordMicEncodeError();
            }
        });

        encoder.configure({
            codec: 'opus',
            sampleRate: C.MIC_RATE,
            numberOfChannels: C.MIC_CH,
            bitrate: 32000,
            opus: {
                application: 'voip',
                frameDuration: 10000,
                complexity: 5,
                signal: 'voice',
                usedtx: 0,
                useinbandfec: 0
            }
        });

        log.debug('MIC', 'Encoder initialized');
        return encoder;

    } catch (e) {
        log.error('MIC', 'Failed to create encoder', { error: e.message });
        return null;
    }
};
const processSamples = samples => {
    if (!micEncoder || micEncoder.state !== 'configured') {
        log.debug('MIC', 'Encoder not configured');
        return;
    }

    const timestamp = frameCounter++ * C.MIC_FRAME_MS * 1000;

    const result = safe(() => {
        const audioData = new AudioData({
            format: 'f32',
            sampleRate: C.MIC_RATE,
            numberOfFrames: samples.length,
            numberOfChannels: C.MIC_CH,
            timestamp,
            data: samples
        });

        micEncoder.encode(audioData);
        audioData.close();
        return true;
    }, false, 'MIC');

    if (!result) {
        recordMicEncodeError();
    }
};

export const startMic = async () => {
    if (S.micEnabled) {
        log.debug('MIC', 'Already enabled');
        return true;
    }

    try {
        log.info('MIC', 'Starting microphone capture');
        micStream = await navigator.mediaDevices.getUserMedia({
            audio: {
                sampleRate: C.MIC_RATE,
                channelCount: C.MIC_CH,
                echoCancellation: true,
                noiseSuppression: true,
                autoGainControl: true
            }
        });
        S.micStream = micStream;
        log.debug('MIC', 'Got media stream');
        micContext = new AudioContext({
            sampleRate: C.MIC_RATE,
            latencyHint: 'interactive'
        });
        await micContext.audioWorklet.addModule(MIC_WORKLET_URL);
        log.debug('MIC', 'Worklet loaded');
        micWorklet = new AudioWorkletNode(micContext, 'mic-processor', {
            numberOfInputs: 1,
            numberOfOutputs: 0,
            channelCount: C.MIC_CH
        });

        micWorklet.port.onmessage = e => {
            if (e.data.type === 'samples' && S.micEnabled) {
                processSamples(new Float32Array(e.data.data));
            }
        };
        micEncoder = initEncoder();
        if (!micEncoder) {
            throw new Error('Encoder initialization failed');
        }
        micContext.createMediaStreamSource(micStream).connect(micWorklet);
        if (micContext.state === 'suspended') {
            await micContext.resume();
        }

        S.micEnabled = 1;
        frameCounter = 0;
        sendMicEnable(1);

        log.info('MIC', 'Started', { sampleRate: C.MIC_RATE, channels: C.MIC_CH });
        return true;

    } catch (e) {
        log.error('MIC', 'Start failed', { error: e.message });
        stopMic();
        return false;
    }
};

export const stopMic = () => {
    log.info('MIC', 'Stopping microphone');

    S.micEnabled = 0;
    if (micEncoder) {
        if (micEncoder.state !== 'closed') {
            safe(() => micEncoder.close(), undefined, 'MIC');
        }
        micEncoder = null;
    }
    if (micWorklet) {
        safe(() => micWorklet.disconnect(), undefined, 'MIC');
        micWorklet = null;
    }
    if (micContext) {
        safe(() => micContext.close(), undefined, 'MIC');
        micContext = null;
    }
    if (micStream) {
        micStream.getTracks().forEach(track => {
            track.stop();
            log.debug('MIC', 'Track stopped', { kind: track.kind });
        });
        micStream = null;
        S.micStream = null;
    }

    frameCounter = 0;
    sendMicEnable(0);

    log.info('MIC', 'Stopped');
};

export const toggleMic = async () => {
    if (S.micEnabled) {
        stopMic();
        return false;
    }
    return await startMic();
};

export const isMicSupported = () => {
    const supported = !!(
        navigator.mediaDevices?.getUserMedia &&
        window.AudioEncoder &&
        window.AudioData
    );

    if (!supported) {
        log.debug('MIC', 'Not supported', {
            getUserMedia: !!navigator.mediaDevices?.getUserMedia,
            AudioEncoder: !!window.AudioEncoder,
            AudioData: !!window.AudioData
        });
    }

    return supported;
};
