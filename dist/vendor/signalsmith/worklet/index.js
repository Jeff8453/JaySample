let vendorFactoryPromise;
async function loadVendorFactory() {
    vendorFactoryPromise ??= import(
    // Generated from the pinned native sources by scripts/build-native.mjs.
    // @ts-expect-error The generated module exists in dist after the native build.
    "./SignalsmithStretch.mjs").then((loaded) => loaded.default);
    return vendorFactoryPromise;
}
/** Creates a live Signalsmith Stretch AudioWorkletNode for a Web Audio graph. */
export async function createSignalsmithStretchNode(context, options) {
    if (!context.audioWorklet)
        throw new Error("AudioWorklet is not available in this audio context");
    const factory = await loadVendorFactory();
    return factory(context, options);
}
export default createSignalsmithStretchNode;
//# sourceMappingURL=index.js.map