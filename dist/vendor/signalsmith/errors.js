export class SignalsmithStretchError extends Error {
    constructor(message, options) {
        super(message, options);
        this.name = new.target.name;
    }
}
export class SignalsmithStretchInitializationError extends SignalsmithStretchError {
}
export class SignalsmithStretchWasmLoadError extends SignalsmithStretchInitializationError {
}
export class SignalsmithStretchValidationError extends SignalsmithStretchError {
}
export class SignalsmithStretchProcessingError extends SignalsmithStretchError {
}
export class SignalsmithStretchDisposedError extends SignalsmithStretchError {
}
//# sourceMappingURL=errors.js.map