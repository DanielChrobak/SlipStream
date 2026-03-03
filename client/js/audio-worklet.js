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
        const first = Math.min(toWrite, this.cap - this.wp);
        const second = toWrite - first;
        for (let c = 0; c < this.ch; c++) {
            const src = channelData[c] || channelData[0];
            if (first > 0) this.bufs[c].set(src.subarray(0, first), this.wp);
            if (second > 0) this.bufs[c].set(src.subarray(first, first + second), 0);
        }
        this.wp = (this.wp + toWrite) % this.cap; this.avail += toWrite;
        return { written: toWrite, overflow };
    }
    read(output, frames) {
        const toRead = Math.min(frames, this.avail);
        const first = Math.min(toRead, this.cap - this.rp);
        const second = toRead - first;
        for (let c = 0; c < this.ch; c++) {
            const out = output[c];
            if (toRead > 0) {
                if (first > 0) out.set(this.bufs[c].subarray(this.rp, this.rp + first), 0);
                if (second > 0) out.set(this.bufs[c].subarray(0, second), first);
            }
            if (toRead < frames) out.fill(0, toRead);
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
        this.sampleRate = 48000;
        this.rb = new RingBuffer(2, this.sampleRate / 2);
        this.target = Math.floor(this.sampleRate * 0.06);
        this.max = Math.floor(this.sampleRate * 0.12);
        this.prebufThreshold = Math.floor(this.sampleRate * 0.04);
        this.minTarget = Math.floor(this.sampleRate * 0.03);
        this.maxTarget = Math.floor(this.sampleRate * 0.14);
        this.prebuffering = true; this.volume = 1; this.muted = false;
        this.samplesProcessed = 0; this.underruns = 0; this.overflows = 0;
        this.lastReport = 0; this.consecutiveUnderruns = 0;
        this.lastHealthAdjust = 0;
        this.port.onmessage = e => {
            const { type, data } = e.data;
            if (type === 'audio') {
                const bufLen = this.rb.length;
                if (bufLen > this.max) {
                    const toSkip = bufLen - this.target;
                    if (toSkip > 0) { this.rb.skip(toSkip); this.overflows++; this.port.postMessage({ type: 'drop', reason: 'Overflow' }); }
                }
                const result = this.rb.write(data.channels, data.frames);
                if (result.overflow > 0) { this.overflows++; this.port.postMessage({ type: 'drop', reason: 'Write overflow' }); }
                if (result.written > 0) this.consecutiveUnderruns = 0;
                if (this.prebuffering && this.rb.length >= this.prebufThreshold) this.prebuffering = false;
            } else if (type === 'volume') this.volume = Math.max(0, Math.min(1, data));
            else if (type === 'mute') this.muted = data;
            else if (type === 'clear') {
                this.rb.clear();
                this.underruns = 0;
                this.overflows = 0;
                this.consecutiveUnderruns = 0;
                this.target = Math.floor(this.sampleRate * 0.06);
                this.max = Math.floor(this.sampleRate * 0.12);
                this.prebufThreshold = Math.floor(this.sampleRate * 0.04);
                this.prebuffering = true;
            }
        };
        this.port.postMessage({ type: 'ready' });
    }
    process(inputs, outputs) {
        const out = outputs[0];
        if (!out || !out.length) return true;
        const frames = out[0].length;
        if (!this.prebuffering && this.rb.length > this.max) {
            const skipped = this.rb.skip(this.rb.length - this.target);
            if (skipped > 0) {
                this.overflows++;
                this.port.postMessage({ type: 'drop', reason: 'Catch-up' });
            }
        }
        const bufferMs = (this.rb.length / 48000) * 1000;
        if (this.muted || this.prebuffering) { for (let c = 0; c < out.length; c++) out[c].fill(0); this.samplesProcessed += frames; return true; }
        const read = this.rb.read(out, frames);
        if (this.volume !== 1) for (let c = 0; c < out.length; c++) for (let i = 0; i < frames; i++) out[c][i] *= this.volume;
        if (read < frames) {
            this.underruns++; this.consecutiveUnderruns++;
            this.port.postMessage({ type: 'drop', reason: 'Underrun' });
            if (read > 0) {
                for (let c = 0; c < out.length; c++) {
                    const hold = out[c][Math.max(0, read - 1)] || 0;
                    for (let i = read; i < frames; i++) out[c][i] = hold;
                }
            }
            if (this.consecutiveUnderruns >= 4) {
                this.target = Math.min(this.maxTarget, this.target + Math.floor(this.sampleRate * 0.01));
                this.max = Math.max(this.target + Math.floor(this.sampleRate * 0.02), this.max);
                this.prebufThreshold = Math.max(Math.floor(this.target * 0.7), this.prebufThreshold);
                this.prebuffering = true;
                this.consecutiveUnderruns = 0;
            }
        } else this.consecutiveUnderruns = 0;
        this.samplesProcessed += frames;
        if (this.samplesProcessed - this.lastHealthAdjust >= this.sampleRate * 2) {
            this.lastHealthAdjust = this.samplesProcessed;
            if (bufferMs > 90) {
                this.target = Math.max(this.minTarget, this.target - Math.floor(this.sampleRate * 0.005));
                this.prebufThreshold = Math.max(Math.floor(this.target * 0.7), Math.floor(this.sampleRate * 0.025));
                this.max = Math.max(this.target + Math.floor(this.sampleRate * 0.03), this.max);
            }
        }
        if (this.samplesProcessed - this.lastReport >= 4800) {
            this.port.postMessage({ type: 'stats', bufferMs, underruns: this.underruns, overflows: this.overflows });
            this.lastReport = this.samplesProcessed; this.underruns = 0; this.overflows = 0;
        }
        return true;
    }
}
registerProcessor('stream-audio-processor', StreamAudioProcessor);
