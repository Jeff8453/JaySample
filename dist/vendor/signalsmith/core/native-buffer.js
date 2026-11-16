import { SignalsmithStretchProcessingError } from "../errors.js";
/** Owns one planar PCM allocation and its native channel-pointer table. */
export class NativePlanarBuffer {
    module;
    channels;
    capacity;
    dataPointer;
    channelPointers;
    freed = false;
    constructor(module, channels, capacity) {
        this.module = module;
        this.channels = channels;
        this.capacity = capacity;
        const sampleBytes = channels * capacity * Float32Array.BYTES_PER_ELEMENT;
        this.dataPointer = module._malloc(sampleBytes);
        this.channelPointers = module._malloc(channels * Uint32Array.BYTES_PER_ELEMENT);
        if (!this.dataPointer || !this.channelPointers) {
            if (this.dataPointer)
                module._free(this.dataPointer);
            if (this.channelPointers)
                module._free(this.channelPointers);
            throw new SignalsmithStretchProcessingError("WebAssembly memory allocation failed");
        }
        this.refreshPointers();
    }
    write(input, sourceOffset, frames) {
        this.assertLive();
        if (frames > this.capacity)
            throw new SignalsmithStretchProcessingError("native input exceeds allocated capacity");
        this.refreshPointers();
        for (let channel = 0; channel < this.channels; channel += 1) {
            const source = input[channel];
            if (!source)
                throw new SignalsmithStretchProcessingError(`missing channel ${channel}`);
            this.module.HEAPF32.set(source.subarray(sourceOffset, sourceOffset + frames), this.sampleIndex(channel));
        }
    }
    writeSilence(frames) {
        this.assertLive();
        if (frames > this.capacity)
            throw new SignalsmithStretchProcessingError("silence block exceeds allocated capacity");
        this.refreshPointers();
        for (let channel = 0; channel < this.channels; channel += 1) {
            this.module.HEAPF32.fill(0, this.sampleIndex(channel), this.sampleIndex(channel) + frames);
        }
    }
    read(frames) {
        this.assertLive();
        if (frames > this.capacity)
            throw new SignalsmithStretchProcessingError("native output exceeds allocated capacity");
        this.refreshPointers();
        return Array.from({ length: this.channels }, (_, channel) => {
            const start = this.sampleIndex(channel);
            return this.module.HEAPF32.slice(start, start + frames);
        });
    }
    dispose() {
        if (this.freed)
            return;
        this.freed = true;
        this.module._free(this.channelPointers);
        this.module._free(this.dataPointer);
    }
    sampleIndex(channel) {
        return (this.dataPointer >>> 2) + channel * this.capacity;
    }
    refreshPointers() {
        const base = this.channelPointers >>> 2;
        for (let channel = 0; channel < this.channels; channel += 1) {
            this.module.HEAPU32[base + channel] = this.dataPointer + channel * this.capacity * Float32Array.BYTES_PER_ELEMENT;
        }
    }
    assertLive() {
        if (this.freed)
            throw new SignalsmithStretchProcessingError("native buffer is already disposed");
    }
}
//# sourceMappingURL=native-buffer.js.map