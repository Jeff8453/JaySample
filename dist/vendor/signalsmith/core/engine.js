import createModule, {} from "../wasm/signalsmith-stretch-wasm.js";
import { SignalsmithStretchDisposedError, SignalsmithStretchInitializationError, SignalsmithStretchProcessingError, SignalsmithStretchValidationError, SignalsmithStretchWasmLoadError } from "../errors.js";
import { semitonesToPitchScale } from "./math.js";
import { NativePlanarBuffer } from "./native-buffer.js";
import { qualityToNative, resolveAdvancedOptions } from "./options.js";
import { validateProcessOptions, validateStreamOptions } from "./validation.js";
function concatChunks(chunks, channels) {
    const total = chunks.reduce((sum, chunk) => sum + (chunk[0]?.length ?? 0), 0);
    const result = Array.from({ length: channels }, () => new Float32Array(total));
    let offset = 0;
    for (const chunk of chunks) {
        const frames = chunk[0]?.length ?? 0;
        for (let channel = 0; channel < channels; channel += 1) {
            const source = chunk[channel];
            if (source)
                result[channel]?.set(source, offset);
        }
        offset += frames;
    }
    return result;
}
function createNativeProcessor(module, sampleRate, channels, quality) {
    const handle = module._ss_create(channels, sampleRate, qualityToNative(quality));
    if (!handle)
        throw new SignalsmithStretchProcessingError("Signalsmith Stretch failed to create a native processor");
    return handle;
}
function configureProcessor(module, handle, sampleRate, pitchScale, advanced) {
    const resolved = resolveAdvancedOptions(sampleRate, advanced);
    module._ss_set_pitch_scale(handle, pitchScale, resolved.tonalityLimit);
    module._ss_set_formant_scale(handle, resolved.formantScale, resolved.formantCompensation ? 1 : 0);
    module._ss_set_formant_base(handle, resolved.formantBase);
}
class SignalsmithStretchEngine {
    module;
    isDisposed = false;
    version;
    constructor(module) {
        this.module = module;
        this.version = `${module._ss_version_major()}.${module._ss_version_minor()}.${module._ss_version_patch()}`;
    }
    get disposed() { return this.isDisposed; }
    async process(options) {
        this.assertLive();
        const { frames, timeRatio, pitchScale } = validateProcessOptions(options);
        const channels = options.channels.length;
        const outputFrames = Math.max(1, Math.round(frames * timeRatio));
        const handle = createNativeProcessor(this.module, options.sampleRate, channels, options.quality ?? "balanced");
        let input;
        let output;
        try {
            configureProcessor(this.module, handle, options.sampleRate, pitchScale, options.advanced);
            // Signalsmith's exact helper needs enough pre-roll for its analysis window.
            // Zero-padding very short clips keeps the public result at the requested length.
            const playbackRate = frames / outputFrames;
            const minimumInput = Math.ceil(this.module._ss_input_latency(handle) + playbackRate * this.module._ss_output_latency(handle)) + 2;
            const paddedFrames = Math.max(frames, minimumInput);
            const paddedOutputFrames = Math.max(outputFrames, Math.ceil(paddedFrames / playbackRate));
            input = new NativePlanarBuffer(this.module, channels, paddedFrames);
            output = new NativePlanarBuffer(this.module, channels, paddedOutputFrames);
            input.writeSilence(paddedFrames);
            input.write(options.channels, 0, frames);
            const ok = this.module._ss_exact(handle, input.channelPointers, paddedFrames, output.channelPointers, paddedOutputFrames);
            if (!ok)
                throw new SignalsmithStretchProcessingError("Signalsmith Stretch could not process this audio buffer");
            const processed = output.read(paddedOutputFrames).map((channel) => channel.slice(0, outputFrames));
            return { sampleRate: options.sampleRate, channels: processed };
        }
        catch (error) {
            if (error instanceof SignalsmithStretchValidationError || error instanceof SignalsmithStretchProcessingError)
                throw error;
            throw new SignalsmithStretchProcessingError("Signalsmith Stretch offline processing failed", { cause: error });
        }
        finally {
            output?.dispose();
            input?.dispose();
            this.module._ss_delete(handle);
        }
    }
    async createStreamProcessor(options) {
        this.assertLive();
        return new StreamProcessor(this.module, options);
    }
    dispose() { this.isDisposed = true; }
    assertLive() {
        if (this.isDisposed)
            throw new SignalsmithStretchDisposedError("SignalsmithStretch instance is disposed");
    }
}
class StreamProcessor {
    module;
    options;
    handle;
    input;
    output;
    channelCount;
    maxBlockSize;
    timeRatio;
    totalInputFrames = 0;
    totalOutputFrames = 0;
    isDisposed = false;
    flushed = false;
    constructor(module, options) {
        this.module = module;
        this.options = options;
        const validated = validateStreamOptions(options);
        this.channelCount = options.channels;
        this.maxBlockSize = validated.maxBlockSize;
        this.timeRatio = validated.timeRatio;
        this.handle = createNativeProcessor(module, options.sampleRate, options.channels, options.quality ?? "balanced");
        configureProcessor(module, this.handle, options.sampleRate, validated.pitchScale, options.advanced);
        this.input = new NativePlanarBuffer(module, options.channels, validated.maxBlockSize);
        this.output = new NativePlanarBuffer(module, options.channels, this.requiredOutputCapacity(validated.maxBlockSize));
    }
    get disposed() { return this.isDisposed; }
    get latencyFrames() {
        this.assertLive();
        return Math.round(this.module._ss_output_latency(this.handle) + this.module._ss_input_latency(this.handle) * this.timeRatio);
    }
    process(channels) {
        this.assertUsable();
        if (channels.length !== this.channelCount)
            throw new SignalsmithStretchValidationError(`expected ${this.channelCount} channels`);
        const frames = channels[0]?.length ?? 0;
        if (frames === 0)
            return Array.from({ length: this.channelCount }, () => new Float32Array());
        if (frames > this.maxBlockSize)
            throw new SignalsmithStretchValidationError(`stream block cannot exceed ${this.maxBlockSize} frames`);
        for (const channel of channels) {
            if (!(channel instanceof Float32Array) || channel.length !== frames) {
                throw new SignalsmithStretchValidationError("stream channels must be equal-length Float32Array instances");
            }
        }
        this.totalInputFrames += frames;
        const targetTotal = Math.round(this.totalInputFrames * this.timeRatio);
        const outputFrames = Math.max(0, targetTotal - this.totalOutputFrames);
        this.ensureOutputCapacity(outputFrames);
        this.input.write(channels, 0, frames);
        const ok = this.module._ss_process(this.handle, this.input.channelPointers, frames, this.output.channelPointers, outputFrames);
        if (!ok)
            throw new SignalsmithStretchProcessingError("Signalsmith Stretch streaming process failed");
        this.totalOutputFrames += outputFrames;
        return this.output.read(outputFrames);
    }
    flush() {
        this.assertUsable();
        const chunks = [];
        let remainingInput = this.module._ss_input_latency(this.handle);
        while (remainingInput > 0) {
            const inputFrames = Math.min(remainingInput, this.maxBlockSize);
            const outputFrames = Math.max(1, Math.round(inputFrames * this.timeRatio));
            this.ensureOutputCapacity(outputFrames);
            this.input.writeSilence(inputFrames);
            if (!this.module._ss_process(this.handle, this.input.channelPointers, inputFrames, this.output.channelPointers, outputFrames)) {
                throw new SignalsmithStretchProcessingError("Signalsmith Stretch tail processing failed");
            }
            chunks.push(this.output.read(outputFrames));
            remainingInput -= inputFrames;
        }
        const tailFrames = this.module._ss_output_latency(this.handle);
        this.ensureOutputCapacity(tailFrames);
        if (!this.module._ss_flush(this.handle, this.output.channelPointers, tailFrames)) {
            throw new SignalsmithStretchProcessingError("Signalsmith Stretch flush failed");
        }
        chunks.push(this.output.read(tailFrames));
        this.flushed = true;
        return concatChunks(chunks, this.channelCount);
    }
    setTimeRatio(value) {
        this.assertUsable();
        if (!Number.isFinite(value) || value < 0.02 || value > 50) {
            throw new SignalsmithStretchValidationError("timeRatio must be between 0.02 and 50");
        }
        this.timeRatio = value;
    }
    setPitchScale(value) {
        this.assertUsable();
        if (!Number.isFinite(value) || value < 0.125 || value > 8) {
            throw new SignalsmithStretchValidationError("pitchScale must be between 0.125 and 8");
        }
        const resolved = resolveAdvancedOptions(this.options.sampleRate, this.options.advanced);
        this.module._ss_set_pitch_scale(this.handle, value, resolved.tonalityLimit);
    }
    setPitchSemitones(value) { this.setPitchScale(semitonesToPitchScale(value)); }
    reset() {
        this.assertLive();
        this.module._ss_reset(this.handle);
        this.totalInputFrames = 0;
        this.totalOutputFrames = 0;
        this.flushed = false;
    }
    dispose() {
        if (this.isDisposed)
            return;
        this.isDisposed = true;
        this.output.dispose();
        this.input.dispose();
        this.module._ss_delete(this.handle);
    }
    requiredOutputCapacity(inputFrames) {
        return Math.max(1, Math.ceil(inputFrames * this.timeRatio) + 2);
    }
    ensureOutputCapacity(frames) {
        if (frames <= this.output.capacity)
            return;
        this.output.dispose();
        this.output = new NativePlanarBuffer(this.module, this.channelCount, frames);
    }
    assertLive() {
        if (this.isDisposed)
            throw new SignalsmithStretchDisposedError("stream processor is disposed");
    }
    assertUsable() {
        this.assertLive();
        if (this.flushed)
            throw new SignalsmithStretchDisposedError("stream processor is flushed; call reset() before reusing it");
    }
}
/** Loads the bundled WebAssembly module and creates a safe Signalsmith Stretch wrapper. */
export async function createSignalsmithStretch(options = {}) {
    const defaultWasmUrl = new URL("../wasm/signalsmith-stretch-wasm.wasm", import.meta.url).href;
    try {
        const module = await createModule({
            locateFile(path) {
                if (!path.endsWith(".wasm"))
                    return options.locateFile?.(path) ?? path;
                return options.wasmUrl?.toString() ?? options.locateFile?.(path) ?? defaultWasmUrl;
            }
        });
        if (!module || typeof module._ss_create !== "function") {
            throw new SignalsmithStretchInitializationError("loaded module does not expose the Signalsmith Stretch API");
        }
        return new SignalsmithStretchEngine(module);
    }
    catch (error) {
        if (error instanceof SignalsmithStretchInitializationError)
            throw error;
        throw new SignalsmithStretchWasmLoadError("Unable to load signalsmith-stretch-wasm.wasm", { cause: error });
    }
}
//# sourceMappingURL=engine.js.map