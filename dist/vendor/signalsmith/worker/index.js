import { SignalsmithStretchDisposedError, SignalsmithStretchInitializationError, SignalsmithStretchProcessingError } from "../errors.js";
/** Runs full-buffer Signalsmith Stretch processing in a dedicated Web Worker. */
export class SignalsmithStretchWorkerClient {
    worker;
    pending = new Map();
    nextId = 1;
    isDisposed = false;
    constructor(worker) {
        this.worker = worker;
        worker.addEventListener("message", (event) => this.onMessage(event.data));
        worker.addEventListener("error", (event) => {
            this.rejectAll(new SignalsmithStretchProcessingError(event.message || "Signalsmith Stretch worker failed"));
        });
    }
    static async create(workerUrl) {
        const worker = workerUrl === undefined
            ? new Worker(new URL("./signalsmith-stretch.worker.js", import.meta.url), { type: "module", name: "signalsmith-stretch-js" })
            : new Worker(workerUrl, { type: "module", name: "signalsmith-stretch-js" });
        const client = new SignalsmithStretchWorkerClient(worker);
        try {
            await new Promise((resolve, reject) => {
                const onMessage = (event) => {
                    if (event.data.type === "ready") {
                        cleanup();
                        resolve();
                    }
                    else if (event.data.type === "error") {
                        cleanup();
                        reject(new SignalsmithStretchInitializationError(event.data.message));
                    }
                };
                const onError = (event) => {
                    cleanup();
                    reject(new SignalsmithStretchInitializationError(event.message || "worker initialization failed"));
                };
                const cleanup = () => {
                    worker.removeEventListener("message", onMessage);
                    worker.removeEventListener("error", onError);
                };
                worker.addEventListener("message", onMessage);
                worker.addEventListener("error", onError);
                worker.postMessage({ type: "init" });
            });
        }
        catch (error) {
            worker.terminate();
            throw error;
        }
        return client;
    }
    get disposed() { return this.isDisposed; }
    process(options) {
        if (this.isDisposed)
            return Promise.reject(new SignalsmithStretchDisposedError("worker client is disposed"));
        const id = this.nextId++;
        const copiedChannels = options.channels.map((channel) => channel.slice());
        const request = { type: "process", id, options: { ...options, channels: copiedChannels } };
        const promise = new Promise((resolve, reject) => this.pending.set(id, { resolve, reject }));
        this.worker.postMessage(request, copiedChannels.map((channel) => channel.buffer));
        return promise;
    }
    dispose() {
        if (this.isDisposed)
            return;
        this.isDisposed = true;
        this.worker.postMessage({ type: "dispose" });
        this.worker.terminate();
        this.rejectAll(new SignalsmithStretchDisposedError("worker client was disposed"));
    }
    onMessage(response) {
        if (response.type === "ready")
            return;
        if (response.type === "error") {
            if (response.id === undefined) {
                this.rejectAll(new SignalsmithStretchProcessingError(response.message));
                return;
            }
            const pending = this.pending.get(response.id);
            this.pending.delete(response.id);
            pending?.reject(new SignalsmithStretchProcessingError(response.message));
            return;
        }
        const pending = this.pending.get(response.id);
        this.pending.delete(response.id);
        pending?.resolve(response.result);
    }
    rejectAll(error) {
        for (const pending of this.pending.values())
            pending.reject(error);
        this.pending.clear();
    }
}
//# sourceMappingURL=index.js.map