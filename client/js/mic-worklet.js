class MicProcessor extends AudioWorkletProcessor {
    constructor() {
        super();
        this.bufferSize = 480;
        this.buffer = new Float32Array(this.bufferSize);
        this.bufferIndex = 0;
    }
    process(inputs) {
        const samples = inputs[0]?.[0];
        if (!samples) return true;
        for (let i = 0; i < samples.length; i++) {
            this.buffer[this.bufferIndex++] = samples[i];
            if (this.bufferIndex >= this.bufferSize) {
                this.port.postMessage({ type: 'samples', data: this.buffer.buffer }, [this.buffer.buffer]);
                this.buffer = new Float32Array(this.bufferSize);
                this.bufferIndex = 0;
            }
        }
        return true;
    }
}
registerProcessor('mic-processor', MicProcessor);
