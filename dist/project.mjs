// Portable, lossless project container: 8-byte magic, LE metadata length, JSON, planar float32 PCM.
const MAGIC = "SFORGE02";
export function validateProject(meta, byteLength) {
  if (!meta || meta.version !== 2) throw Error("Unsupported project version.");
  const a = meta.audio;
  if (
    !a ||
    !Number.isInteger(a.frames) ||
    a.frames < 1 ||
    !Number.isInteger(a.channels) ||
    a.channels < 1 ||
    a.channels > 2 ||
    !Number.isInteger(a.sampleRate) ||
    a.sampleRate < 8000 ||
    a.sampleRate > 192000 ||
    a.frames / a.sampleRate > 600
  )
    throw Error("Invalid audio dimensions (maximum 10 minutes, stereo).");
  if (a.frames * a.channels * 4 !== byteLength)
    throw Error("Project audio is truncated or has extra data.");
  if (
    !Array.isArray(meta.banks) ||
    meta.banks.length !== 4 ||
    meta.banks.some((b) => !Array.isArray(b) || b.length !== 16)
  )
    throw Error("Invalid cue banks.");
  const duration = a.frames / a.sampleRate;
  for (const b of meta.banks)
    for (const p of b) {
      if (p === null) continue;
      if (typeof p !== "object") throw Error("Invalid cue.");
      for (const [key, min, max] of [
        ["start", 0, duration],
        ["end", 0, duration],
        ["pitch", -24, 24],
        ["gain", 0, 100],
        ["filter", 100, 20000],
        ["attack", 0, 2000],
        ["release", 0, 5000],
      ])
        if (!Number.isFinite(p[key]) || p[key] < min || p[key] > max)
          throw Error("Invalid cue " + key + ".");
      if (
        p.end - p.start < 1 / a.sampleRate ||
        !["trigger", "hold", "loop"].includes(p.mode) ||
        typeof p.reverse !== "boolean" ||
        typeof p.name !== "string" ||
        p.name.length > 80
      )
        throw Error("Invalid cue settings.");
    }
  const c = meta.controls;
  if (
    !c ||
    !Number.isFinite(+c.sourcebpm) ||
    +c.sourcebpm < 30 ||
    +c.sourcebpm > 300 ||
    !Number.isFinite(+c.targetbpm) ||
    +c.targetbpm < 30 ||
    +c.targetbpm > 300
  )
    throw Error("Invalid tempo.");
  for (const k of ["sourcekey", "targetkey"])
    if (c[k] !== "" && !(Number.isInteger(+c[k]) && +c[k] >= 0 && +c[k] < 24))
      throw Error("Invalid key.");
  for (const [key, allowed] of Object.entries({
    voices: ["mono", "poly"],
    quality: ["studio", "fast"],
    snap: ["off", "zero", "beat"],
    quantize: ["0", "1", "0.5", "0.25"],
  }))
    if (c[key] !== undefined && !allowed.includes(String(c[key])))
      throw Error("Invalid " + key + ".");
  if (
    c.master !== undefined &&
    (!Number.isFinite(+c.master) || +c.master < 0 || +c.master > 100)
  )
    throw Error("Invalid master volume.");
  if (
    typeof meta.name !== "string" ||
    meta.name.length > 80 ||
    typeof meta.filename !== "string" ||
    meta.filename.length > 512
  )
    throw Error("Invalid project name.");
  return meta;
}
export function encodeProject(meta, channels) {
  const metadata = {
    ...meta,
    version: 2,
    audio: {
      sampleRate: meta.audio.sampleRate,
      channels: channels.length,
      frames: channels[0].length,
    },
  };
  validateProject(
    metadata,
    channels.reduce((n, c) => n + c.byteLength, 0),
  );
  const json = new TextEncoder().encode(JSON.stringify(metadata)),
    header = new Uint8Array(12);
  header.set(new TextEncoder().encode(MAGIC));
  new DataView(header.buffer).setUint32(8, json.length, true);
  return new Blob([header, json, ...channels], {
    type: "application/octet-stream",
  });
}
export function decodeProject(data) {
  if (data.byteLength < 12 || data.byteLength > 512 * 1024 * 1024)
    throw Error("Invalid project size.");
  const header = new Uint8Array(data, 0, 8);
  if (new TextDecoder().decode(header) !== MAGIC)
    throw Error("This is not a JaySample project.");
  const length = new DataView(data).getUint32(8, true);
  if (length > 1024 * 1024 || length + 12 > data.byteLength)
    throw Error("Invalid project header.");
  const meta = JSON.parse(
      new TextDecoder().decode(new Uint8Array(data, 12, length)),
    ),
    offset = 12 + length;
  validateProject(meta, data.byteLength - offset);
  const channels = [];
  for (let c = 0; c < meta.audio.channels; c++)
    channels.push(
      new Float32Array(
        data.slice(
          offset + c * meta.audio.frames * 4,
          offset + (c + 1) * meta.audio.frames * 4,
        ),
      ),
    );
  if (
    channels.some((c) => c.some((n) => !Number.isFinite(n) || Math.abs(n) > 32))
  )
    throw Error("Invalid audio samples.");
  return { meta, channels };
}
export function openRecovery() {
  return new Promise((resolve, reject) => {
    const req = indexedDB.open("sample-forge-v2", 1);
    req.onupgradeneeded = () => req.result.createObjectStore("session");
    req.onsuccess = () => resolve(req.result);
    req.onerror = () => reject(req.error);
  });
}
export async function writeRecovery(meta, channels) {
  const db = await openRecovery();
  try {
    await new Promise((resolve, reject) => {
      const t = db.transaction("session", "readwrite"),
        s = t.objectStore("session");
      s.put(meta, "meta");
      if (channels) s.put(channels, "audio");
      t.oncomplete = resolve;
      t.onabort = () => reject(t.error);
      t.onerror = () => reject(t.error);
    });
  } finally {
    db.close();
  }
}
export async function readRecovery() {
  const db = await openRecovery();
  try {
    return await new Promise((resolve, reject) => {
      const t = db.transaction("session", "readonly"),
        s = t.objectStore("session"),
        m = s.get("meta"),
        a = s.get("audio");
      t.oncomplete = () =>
        resolve(
          m.result && a.result ? { meta: m.result, channels: a.result } : null,
        );
      t.onerror = () => reject(t.error);
    });
  } finally {
    db.close();
  }
}
export class History {
  constructor(limit = 60) {
    this.limit = limit;
    this.items = [];
    this.index = -1;
  }
  reset(state) {
    this.items = [structuredClone(state)];
    this.index = 0;
  }
  push(state) {
    const s = structuredClone(state);
    if (JSON.stringify(s) === JSON.stringify(this.items[this.index]))
      return false;
    this.items = this.items.slice(0, this.index + 1);
    this.items.push(s);
    if (this.items.length > this.limit) this.items.shift();
    this.index = this.items.length - 1;
    return true;
  }
  undo() {
    return this.index > 0 ? structuredClone(this.items[--this.index]) : null;
  }
  redo() {
    return this.index < this.items.length - 1
      ? structuredClone(this.items[++this.index])
      : null;
  }
}
