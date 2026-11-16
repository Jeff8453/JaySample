import { SignalsmithStretchValidationError } from "../errors.js";
import { semitonesToPitchScale } from "./math.js";
export function qualityToNative(quality) {
    return quality === "fast" ? 0 : 1;
}
export function resolveAdvancedOptions(sampleRate, advanced) {
    if (advanced?.formantScale !== undefined && advanced.formantSemitones !== undefined) {
        throw new SignalsmithStretchValidationError("formantScale and formantSemitones are mutually exclusive");
    }
    const formantScale = advanced?.formantSemitones === undefined
        ? advanced?.formantScale ?? 1
        : semitonesToPitchScale(advanced.formantSemitones);
    if (!Number.isFinite(formantScale) || formantScale <= 0 || formantScale < 0.125 || formantScale > 8) {
        throw new SignalsmithStretchValidationError("formantScale must be between 0.125 and 8");
    }
    const tonalityLimitHz = advanced?.tonalityLimitHz ?? 0;
    const formantBaseHz = advanced?.formantBaseHz ?? 0;
    if (!Number.isFinite(tonalityLimitHz) || tonalityLimitHz < 0 || tonalityLimitHz > sampleRate / 2) {
        throw new SignalsmithStretchValidationError("tonalityLimitHz must be between 0 and Nyquist");
    }
    if (!Number.isFinite(formantBaseHz) || formantBaseHz < 0 || formantBaseHz > sampleRate / 2) {
        throw new SignalsmithStretchValidationError("formantBaseHz must be between 0 and Nyquist");
    }
    return {
        tonalityLimit: tonalityLimitHz / sampleRate,
        formantScale,
        formantCompensation: advanced?.formantCompensation ?? false,
        formantBase: formantBaseHz / sampleRate
    };
}
//# sourceMappingURL=options.js.map