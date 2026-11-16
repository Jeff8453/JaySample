import { createSignalsmithStretch } from "../core/engine.js";
import { validateAudioData } from "../core/validation.js";
/** Copies a browser AudioBuffer into framework-agnostic non-interleaved PCM data. */
export function audioBufferToAudioData(buffer) {
    const channels = Array.from({ length: buffer.numberOfChannels }, (_, channel) => {
        const copy = new Float32Array(buffer.length);
        buffer.copyFromChannel(copy, channel);
        return copy;
    });
    return { sampleRate: buffer.sampleRate, channels };
}
/** Copies non-interleaved PCM data into a new browser AudioBuffer. */
export function audioDataToAudioBuffer(context, data) {
    const length = validateAudioData(data);
    const buffer = context.createBuffer(data.channels.length, length, data.sampleRate);
    for (let channel = 0; channel < data.channels.length; channel += 1) {
        const samples = data.channels[channel];
        if (samples)
            buffer.copyToChannel(samples, channel);
    }
    return buffer;
}
/** Processes an AudioBuffer and returns a newly allocated AudioBuffer. */
export async function processAudioBuffer(context, input, options = {}) {
    const engine = await createSignalsmithStretch(options.initialization);
    try {
        const audio = audioBufferToAudioData(input);
        const result = await engine.process({
            ...audio,
            ...(options.timeRatio === undefined ? {} : { timeRatio: options.timeRatio }),
            ...(options.pitchScale === undefined ? {} : { pitchScale: options.pitchScale }),
            ...(options.pitchSemitones === undefined ? {} : { pitchSemitones: options.pitchSemitones }),
            ...(options.quality === undefined ? {} : { quality: options.quality }),
            ...(options.advanced === undefined ? {} : { advanced: options.advanced })
        });
        return audioDataToAudioBuffer(context, result);
    }
    finally {
        engine.dispose();
    }
}
//# sourceMappingURL=index.js.map