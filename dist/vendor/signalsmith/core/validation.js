import { SignalsmithStretchValidationError } from "../errors.js";
import { semitonesToPitchScale } from "./math.js";
export const DEFAULT_BLOCK_SIZE = 4096;
function positiveFinite(name, value) {
    if (!Number.isFinite(value) || value <= 0) {
        throw new SignalsmithStretchValidationError(`${name} must be a positive finite number`);
    }
}
export function validateSampleRate(sampleRate) {
    if (!Number.isInteger(sampleRate) || sampleRate < 8_000 || sampleRate > 192_000) {
        throw new SignalsmithStretchValidationError("sampleRate must be an integer between 8000 and 192000 Hz");
    }
}
export function validateChannelCount(channels) {
    if (!Number.isInteger(channels) || channels < 1 || channels > 32) {
        throw new SignalsmithStretchValidationError("channel count must be an integer between 1 and 32");
    }
}
export function validateAudioData(data) {
    validateSampleRate(data.sampleRate);
    validateChannelCount(data.channels.length);
    const length = data.channels[0]?.length ?? 0;
    if (length === 0)
        throw new SignalsmithStretchValidationError("audio input must contain at least one frame");
    for (const [index, channel] of data.channels.entries()) {
        if (!(channel instanceof Float32Array)) {
            throw new SignalsmithStretchValidationError(`channel ${index} must be a Float32Array`);
        }
        if (channel.length !== length) {
            throw new SignalsmithStretchValidationError("all channels must contain the same number of frames");
        }
    }
    return length;
}
export function resolvePitch(options) {
    if (options.pitchScale !== undefined && options.pitchSemitones !== undefined) {
        throw new SignalsmithStretchValidationError("pitchScale and pitchSemitones are mutually exclusive");
    }
    const scale = options.pitchSemitones === undefined
        ? options.pitchScale ?? 1
        : semitonesToPitchScale(options.pitchSemitones);
    positiveFinite("pitchScale", scale);
    if (scale < 0.125 || scale > 8) {
        throw new SignalsmithStretchValidationError("pitchScale must be between 0.125 and 8 (±36 semitones)");
    }
    return scale;
}
export function validateBlockSize(blockSize) {
    if (!Number.isInteger(blockSize) || blockSize < 128 || blockSize > 1_048_576) {
        throw new SignalsmithStretchValidationError("maxBlockSize must be an integer between 128 and 1048576 frames");
    }
}
function resolveTimeRatio(timeRatio) {
    const ratio = timeRatio ?? 1;
    positiveFinite("timeRatio", ratio);
    if (ratio < 0.02 || ratio > 50) {
        throw new SignalsmithStretchValidationError("timeRatio must be between 0.02 and 50");
    }
    return ratio;
}
export function validateProcessOptions(options) {
    return {
        frames: validateAudioData(options),
        timeRatio: resolveTimeRatio(options.timeRatio),
        pitchScale: resolvePitch(options)
    };
}
export function validateStreamOptions(options) {
    validateSampleRate(options.sampleRate);
    validateChannelCount(options.channels);
    const maxBlockSize = options.maxBlockSize ?? DEFAULT_BLOCK_SIZE;
    validateBlockSize(maxBlockSize);
    return { timeRatio: resolveTimeRatio(options.timeRatio), pitchScale: resolvePitch(options), maxBlockSize };
}
//# sourceMappingURL=validation.js.map