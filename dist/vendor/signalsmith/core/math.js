import { SignalsmithStretchValidationError } from "../errors.js";
/** Converts semitones to a positive pitch multiplier. */
export function semitonesToPitchScale(semitones) {
    if (!Number.isFinite(semitones)) {
        throw new SignalsmithStretchValidationError("pitchSemitones must be a finite number");
    }
    return 2 ** (semitones / 12);
}
/** Converts a positive pitch multiplier to semitones. */
export function pitchScaleToSemitones(scale) {
    if (!Number.isFinite(scale) || scale <= 0) {
        throw new SignalsmithStretchValidationError("pitchScale must be a positive finite number");
    }
    return 12 * Math.log2(scale);
}
//# sourceMappingURL=math.js.map