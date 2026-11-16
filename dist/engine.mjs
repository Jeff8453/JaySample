import { SignalsmithStretchWorkerClient } from "./vendor/signalsmith/worker/index.js";
let clientPromise;
export async function spectralProcess(
  channels,
  sampleRate,
  pitchSemitones,
  timeRatio,
  { fast = false, formants = false } = {},
) {
  clientPromise ??= SignalsmithStretchWorkerClient.create().catch((e) => {
    clientPromise = null;
    throw e;
  });
  const client = await clientPromise;
  return client.process({
    channels,
    sampleRate,
    pitchSemitones: Math.max(-36, Math.min(36, pitchSemitones)),
    timeRatio,
    quality: fast ? "fast" : "balanced",
    advanced: {
      formantCompensation: formants,
      tonalityLimitHz: Math.min(8000, sampleRate / 2),
    },
  });
}
