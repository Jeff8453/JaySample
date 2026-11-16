/// <reference lib="webworker" />
import { createSignalsmithStretch } from "../core/engine.js";
const scope = self;
let engine;
function post(message, transfer = []) {
    scope.postMessage(message, transfer);
}
scope.onmessage = async (event) => {
    const request = event.data;
    try {
        if (request.type === "init") {
            engine ??= await createSignalsmithStretch();
            post({ type: "ready" });
            return;
        }
        if (request.type === "dispose") {
            engine?.dispose();
            engine = undefined;
            scope.close();
            return;
        }
        engine ??= await createSignalsmithStretch();
        const result = await engine.process(request.options);
        post({ type: "result", id: request.id, result }, result.channels.map((channel) => channel.buffer));
    }
    catch (error) {
        const value = error instanceof Error ? error : new Error(String(error));
        post({
            type: "error",
            ...(request.type === "process" ? { id: request.id } : {}),
            name: value.name,
            message: value.message
        });
    }
};
//# sourceMappingURL=signalsmith-stretch.worker.js.map