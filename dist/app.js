import { spectralProcess } from "./engine.mjs";
import {
  encodeProject,
  decodeProject,
  validateProject,
  writeRecovery,
  readRecovery,
  History,
} from "./project.mjs";
import { zipSync, strToU8 } from "./fflate.mjs";

const $ = (s) => document.querySelector(s),
  keys = [
    "q",
    "w",
    "e",
    "r",
    "a",
    "s",
    "d",
    "f",
    "z",
    "x",
    "c",
    "v",
    "1",
    "2",
    "3",
    "4",
  ],
  colors = ["#ff875c", "#dba967", "#9dba8c", "#9fa4d5"];
let ctx,
  buffer,
  bank = 0,
  selected = 0,
  banks = Array.from({ length: 4 }, () => Array(16).fill(null)),
  voices = new Set(),
  keyboard = false,
  cursor = 0,
  epoch = 0,
  loadId = 0,
  worker,
  analysisId = 0;
let latestTrigger = 0;
let cache = new Map(),
  pending = new Set(),
  held = new Map(),
  pointerTokens = new Map(),
  activeNote = null;
let masterGain, limiter, analyser;
function audio() {
  if (!ctx) {
    ctx = new AudioContext({ sampleRate: 44100, latencyHint: "interactive" });
    masterGain = ctx.createGain();
    masterGain.gain.value = +$("#master").value / 100;
    limiter = ctx.createDynamicsCompressor();
    limiter.threshold.value = -2;
    limiter.knee.value = 0;
    limiter.ratio.value = 20;
    limiter.attack.value = 0.003;
    limiter.release.value = 0.08;
    analyser = ctx.createAnalyser();
    analyser.fftSize = 256;
    masterGain.connect(limiter).connect(analyser).connect(ctx.destination);
  }
  return ctx;
}
const msg = (s) => ($("#message").textContent = s),
  current = () => banks[bank][selected];
function defaults(start, end, i) {
  return {
    start,
    end,
    pitch: 0,
    gain: 80,
    reverse: false,
    filter: 20000,
    attack: 5,
    release: 40,
    mode: "trigger",
    name: "Cue " + String(i + 1).padStart(2, "0"),
  };
}
const noteNames = [
  "C",
  "C♯",
  "D",
  "D♯",
  "E",
  "F",
  "F♯",
  "G",
  "G♯",
  "A",
  "A♯",
  "B",
];
for (const id of ["sourcekey", "targetkey"]) {
  const unknown = new Option("Unknown", "");
  $("#" + id).add(unknown);
  for (let i = 0; i < 24; i++)
    $("#" + id).add(
      new Option(
        noteNames[i % 12] + (i >= 12 ? " minor" : " major"),
        String(i),
      ),
    );
}
function keyShift() {
  const a = $("#sourcekey").value,
    b = $("#targetkey").value;
  if (a === "" || b === "") return 0;
  return (((+b % 12) - (+a % 12) + 18) % 12) - 6;
}
function tempo() {
  return $("#sync").classList.contains("active")
    ? +$("#targetbpm").value / (+$("#sourcebpm").value || 120)
    : 1;
}
function invalidate() {
  epoch++;
  pending.forEach((t) => (t.cancelled = true));
  pending.clear();
  cache.clear();
  cacheBytes = 0;
}
function stop() {
  epoch++;
  pending.forEach((t) => (t.cancelled = true));
  pending.clear();
  for (const v of [...voices]) endVoice(v, true);
  held.clear();
  pointerTokens.clear();
  activeNote = null;
  document.querySelectorAll(".hit").forEach((b) => b.classList.remove("hit"));
}
function resetControls() {
  stop();
  invalidate();
  banks = Array.from({ length: 4 }, () => Array(16).fill(null));
  bank = selected = 0;
  cursor = 0;
  $("#zoom").value = 1;
  $("#pan").value = 0;
  $("#sourcebpm").value = $("#targetbpm").value = 120;
  $("#sourcekey").value = $("#targetkey").value = "";
  $("#sync").classList.remove("active");
  $("#shift").textContent = "0 st";
  $("#analysisnote").textContent =
    "Detection is an estimate. Confirm or edit the source BPM and key.";
}
function demo() {
  loadId++;
  worker?.terminate();
  $("#analyze").disabled = false;
  buffer = audio().createBuffer(1, 44100 * 8, 44100);
  const d = buffer.getChannelData(0);
  let seed = 5;
  function noise() {
    seed = (seed * 16807) % 2147483647;
    return seed / 1073741824 - 1;
  }
  for (let i = 0; i < d.length; i++) {
    const t = i / 44100,
      b = t % 0.5,
      s = t % 0.25;
    d[i] =
      Math.sin(2 * Math.PI * (48 * b + 6 * (1 - Math.exp(-b * 28)))) *
        Math.exp(-b * 20) *
        0.8 +
      noise() * Math.exp(-s * 85) * 0.15 +
      (Math.floor(t / 0.5) % 2 ? noise() * Math.exp(-b * 24) * 0.38 : 0) +
      Math.sin(2 * Math.PI * 110 * t) * Math.exp(-b * 8) * 0.12;
  }
  resetControls();
  $("#filename").textContent = "Forge break · Demo";
  $("#duration").textContent = "8.00 s · 44.1 kHz";
  slice(false);
  msg("Demo ready. Trigger pads, or detect key + BPM.");
}
function transients() {
  const d = buffer.getChannelData(0),
    hop = Math.round(buffer.sampleRate * 0.01),
    energy = [];
  for (let i = 0; i < d.length; i += hop) {
    let sum = 0;
    for (let j = i; j < Math.min(d.length, i + hop); j++) sum += d[j] * d[j];
    energy.push(Math.sqrt(sum / hop));
  }
  const peaks = [];
  for (let i = 1; i < energy.length - 1; i++) {
    const prev =
      energy.slice(Math.max(0, i - 10), i).reduce((a, b) => a + b, 0) /
      Math.min(i, 10);
    if (energy[i] > prev * 1.5 + 0.008 && energy[i] >= energy[i + 1])
      peaks.push({
        t: (i * hop) / buffer.sampleRate,
        strength: energy[i] - prev,
      });
  }
  return peaks.sort((a, b) => b.strength - a.strength);
}
function slice(detect) {
  if (!buffer) return;
  stop();
  const n = +$("#count").value;
  let starts = [0];
  if (detect) {
    for (const p of transients()) {
      if (
        starts.every((t) => Math.abs(t - p.t) > 0.09) &&
        p.t < buffer.duration - 0.025
      )
        starts.push(p.t);
      if (starts.length >= n) break;
    }
    starts.sort((a, b) => a - b);
  } else
    starts = Array.from({ length: n }, (_, i) => (i * buffer.duration) / n);
  banks[bank] = Array.from({ length: 16 }, (_, i) =>
    i < starts.length
      ? defaults(starts[i], starts[i + 1] ?? buffer.duration, i)
      : null,
  );
  selected = 0;
  cursor = 0;
  render();
  msg(
    `${starts.length} ${detect ? "transient" : "even"} cues in bank ${"ABCD"[bank]}.`,
  );
}
function render() {
  const bs = $("#banks");
  bs.replaceChildren();
  for (let i = 0; i < 4; i++) {
    let b = document.createElement("button");
    b.textContent = "ABCD"[i];
    b.classList.toggle("active", i === bank);
    b.onclick = () => {
      stop();
      bank = i;
      selected = 0;
      render();
    };
    bs.append(b);
  }
  const pads = $("#pads");
  pads.replaceChildren();
  banks[bank].forEach((p, i) => {
    const b = document.createElement("button");
    b.className =
      "pad" +
      (keyboard ? " keypad" : "") +
      (!keyboard && i === selected ? " selected" : "");
    b.style.setProperty("--accent", colors[Math.floor(i / 4)]);
    b.innerHTML = `<span class="num">${String(i + 1).padStart(2, "0")}</span><span class="key">${keys[i].toUpperCase()}</span><b></b><span class="time"></span>`;
    b.querySelector("b").textContent = keyboard
      ? noteNames[i % 12] + (3 + Math.floor(i / 12))
      : (p?.name ?? "Empty cue");
    b.querySelector(".time").textContent = keyboard
      ? (i >= 0 ? "+" : "") + i + " st"
      : p
        ? (p.end - p.start).toFixed(2) + " s"
        : "Click to set cue";
    b.setAttribute(
      "aria-label",
      keyboard
        ? `Play selected cue at ${i} semitones`
        : `Cue ${i + 1}${p ? "" : " empty"}`,
    );
    b.onpointerdown = (e) => {
      if (e.button !== 0) return;
      e.preventDefault();
      b.setPointerCapture(e.pointerId);
      if (e.shiftKey && !keyboard) {
        selected = i;
        update();
        highlight();
        draw();
        return;
      }
      if (!keyboard) {
        selected = i;
        if (!current()) {
          banks[bank][i] = defaults(cursor, buffer.duration, i);
          msg("Cue set at " + cursor.toFixed(3) + " s");
        }
        update();
        highlight();
        draw();
      }
      const token = {};
      pointerTokens.set(e.pointerId, token);
      play(keyboard ? selected : i, keyboard ? i : 0, token);
    };
    b.onclick = (e) => {
      if (e.detail !== 0) return;
      if (!keyboard) {
        selected = i;
        update();
        highlight();
        draw();
      }
      play(keyboard ? selected : i, keyboard ? i : 0);
    };
    const release = (e) => {
      const t = pointerTokens.get(e.pointerId);
      if (t) {
        releaseToken(t);
        pointerTokens.delete(e.pointerId);
      }
    };
    b.onpointerup = release;
    b.onpointercancel = release;
    pads.append(b);
  });
  $("#padmode").classList.toggle("active", !keyboard);
  $("#keymode").classList.toggle("active", keyboard);
  $("#modehint").textContent = keyboard
    ? "Selected cue mapped chromatically · C3 = original pitch"
    : "Each pad plays its own cue";
  update();
  draw();
  commit();
}
function highlight() {
  [...$("#pads").children].forEach((b, i) =>
    b.classList.toggle("selected", !keyboard && i === selected),
  );
}
function update() {
  const p = current();
  $("#cuename").value = p?.name ?? "";
  $("#cuename").disabled = !p;
  $("#padtitle").textContent =
    "BANK " + "ABCD"[bank] + " / " + String(selected + 1).padStart(2, "0");
  $("#sliceName").textContent = p?.name ?? "Empty cue";
  for (const k of [
    "pitch",
    "gain",
    "start",
    "end",
    "filter",
    "attack",
    "release",
    "mode",
  ]) {
    $("#" + k).disabled = !p;
    $("#" + k).value = p ? p[k] : 0;
  }
  $("#pitchout").textContent = (p?.pitch ?? 0) + " st";
  $("#gainout").textContent = (p?.gain ?? 0) + "%";
  $("#filterout").textContent = p
    ? p.filter >= 20000
      ? "Open"
      : (p.filter / 1000).toFixed(1) + " kHz"
    : "—";
  $("#reverse").classList.toggle("active", !!p?.reverse);
}
function endVoice(v, immediate = false) {
  if (!voices.has(v)) return;
  voices.delete(v);
  const now = audio().currentTime;
  if (v.g.gain.cancelAndHoldAtTime) v.g.gain.cancelAndHoldAtTime(now);
  else v.g.gain.cancelScheduledValues(now);
  if (v.startTime > now) {
    try {
      v.s.stop();
    } catch {}
    v.token.cancelled = true;
    v.el?.classList.remove("hit");
    return;
  }
  v.g.gain.setValueAtTime(v.g.gain.value, now);
  v.g.gain.linearRampToValueAtTime(0, now + (immediate ? 0.005 : v.release));
  try {
    v.s.stop(now + (immediate ? 0.006 : v.release + 0.01));
  } catch {}
  v.token.cancelled = true;
  if (v.el) v.el.classList.remove("hit");
}
function releaseToken(token) {
  token.released = true;
  if (token.mode === "hold") {
    token.cancelled = true;
    for (const v of [...voices]) if (v.token === token) endVoice(v);
  }
}
const inflight = new Map();
let cacheBytes = 0;
function processingKey(p, extra = 0, raw = false) {
  return [
    loadId,
    p.start,
    p.end,
    p.reverse,
    p.mode,
    raw ? 0 : Math.max(-36, Math.min(36, p.pitch + keyShift() + extra)),
    raw ? 1 : tempo(),
    $("#quality").value,
    $("#formants").classList.contains("active"),
  ].join(":");
}
async function processed(p, extra = 0, raw = false) {
  const key = processingKey(p, extra, raw);
  if (cache.has(key)) {
    const result = cache.get(key);
    cache.delete(key);
    cache.set(key, result);
    return result;
  }
  if (inflight.has(key)) return inflight.get(key);
  if((p.end-p.start)/(raw?1:tempo())>600)throw Error("Rendered cue would exceed 10 minutes. Shorten it or increase target tempo.");
  const source = buffer,
    version = loadId,
    pitch = raw ? 0 : Math.max(-36, Math.min(36, p.pitch + keyShift() + extra)),
    ratio = raw ? 1 : tempo(),
    quality = $("#quality").value,
    formants = $("#formants").classList.contains("active");
  const task = (async () => {
    const sr = source.sampleRate,
      start = Math.floor(p.start * sr),
      n = Math.max(1, Math.round(p.end * sr) - start),
      channels = [0, Math.min(1, source.numberOfChannels - 1)].map((ch) => {
        const d = new Float32Array(n);
        d.set(source.getChannelData(ch).subarray(start, start + n));
        return p.reverse ? d.reverse() : d;
      });
    const result =
      pitch === 0 && Math.abs(ratio - 1) < 0.000001
        ? { channels, sampleRate: sr }
        : await spectralProcess(channels, sr, pitch, 1 / ratio, {
            fast: quality === "fast",
            formants,
          });
    const out = audio().createBuffer(2, result.channels[0].length, sr);
    out.copyToChannel(result.channels[0], 0);
    out.copyToChannel(result.channels[1], 1);
    if (!raw && p.mode === "loop") {
      const fade = Math.min(Math.floor(sr * 0.002), Math.floor(out.length / 4));
      for (let ch = 0; ch < 2; ch++) {
        const d = out.getChannelData(ch);
        for (let j = 0; j < fade; j++) {
          const gain = j / fade;
          d[j] *= gain;
          d[d.length - 1 - j] *= gain;
        }
      }
    }
    if (version === loadId) {
      const size = out.length * 8;
      while (cacheBytes + size > 96 * 1024 * 1024 && cache.size) {
        const old = cache.keys().next().value;
        cacheBytes -= cache.get(old).length * 8;
        cache.delete(old);
      }
      if (size <= 96 * 1024 * 1024) {
        cache.set(key, out);
        cacheBytes += size;
        markPrepared();
      }
    }
    return out;
  })();
  inflight.set(key, task);
  try {
    return await task;
  } finally {
    inflight.delete(key);
  }
}
async function play(i, extra = 0, token = {}, source = false) {
  if (!buffer) return;
  const p = source ? defaults(cursor, buffer.duration, 0) : banks[bank][i];
  if (!p) return msg("Select a loaded cue first.");
  const cueKey = `${bank}:${i}:${extra}`;
  if (!source && p.mode === "loop") {
    for (const t of pending)
      if (t.cueKey === cueKey) {
        t.cancelled = true;
        pending.delete(t);
        return;
      }
    const existing = [...voices].find((v) => v.cueKey === cueKey);
    if (existing) {
      endVoice(existing);
      return;
    }
  }
  token.serial = ++latestTrigger;
  token.mode = source ? "trigger" : p.mode;
  token.cueKey = cueKey;
  pending.add(token);
  const version = epoch;
  await audio().resume();
  if (token.cancelled) {
    pending.delete(token);
    return;
  }
  msg("Preparing " + (source ? "source" : p.name) + "…");
  try {
    const snapshot = { ...p },
      clip = await processed(snapshot, extra, source);
    if (
      ($("#voices").value === "mono" && token.serial !== latestTrigger) ||
      version !== epoch ||
      token.cancelled ||
      (token.released && token.mode === "hold")
    )
      return;
    if ($("#voices").value === "mono")
      for (const v of [...voices]) endVoice(v, true);
    while (voices.size >= 32) endVoice(voices.values().next().value, true);
    const s = ctx.createBufferSource(),
      g = ctx.createGain(),
      f = ctx.createBiquadFilter();
    s.buffer = clip;
    s.loop = !source && p.mode === "loop";
    f.type = "lowpass";
    f.frequency.value = source ? 20000 : p.filter;
    const attack = source ? 0.005 : p.attack / 1000,
      release = source ? 0.02 : p.release / 1000,
      now = source ? ctx.currentTime : quantizedTime(),
      level = (p.gain / 100) * 0.8 * (token.velocity ?? 1);
    g.gain.setValueAtTime(0, now);
    g.gain.linearRampToValueAtTime(
      level,
      now + Math.min(attack, clip.duration / 2),
    );
    if (!s.loop && token.mode !== "hold") {
      const rel = Math.min(release, clip.duration / 2);
      g.gain.setValueAtTime(
        level,
        now +
          Math.max(Math.min(attack, clip.duration / 2), clip.duration - rel),
      );
      g.gain.linearRampToValueAtTime(0, now + clip.duration);
    }
    s.connect(f).connect(g).connect(masterGain);
    const el = source ? null : $("#pads").children[keyboard ? extra : i];
    const v = { s, g, token, cueKey, release, el, startTime: now };
    voices.add(v);
    el?.classList.add("hit");
    s.onended = () => {
      voices.delete(v);
      s.disconnect();
      g.disconnect();
      f.disconnect();
      if (![...voices].some((w) => w.el === el)) el?.classList.remove("hit");
    };
    s.start(now);
    activeNote = {
      v,
      at: now,
      duration: clip.duration,
      start: p.start,
      end: p.end,
      reverse: p.reverse,
      loop: s.loop,
    };
    animate();
    msg(
      source
        ? "Source preview"
        : `${p.name} · ${p.mode} · ${Math.round(tempo() * 100)}% tempo`,
    );
  } catch (e) {
    msg("Audio processing failed. Try a shorter sample.");
    console.error(e);
  } finally {
    pending.delete(token);
  }
}
let animating = false;
function animate() {
  if (animating) return;
  animating = true;
  function frame() {
    draw();
    if (activeNote && voices.has(activeNote.v)) requestAnimationFrame(frame);
    else {
      animating = false;
      activeNote = null;
      draw();
    }
  }
  requestAnimationFrame(frame);
}
function viewport() {
  const span = buffer.duration / $("#zoom").value,
    offset = ($("#pan").value / 100) * (buffer.duration - span);
  return { span, offset };
}
function draw() {
  if (!buffer) return;
  const cv = $("#wave"),
    r = cv.getBoundingClientRect(),
    scale = devicePixelRatio || 1;
  cv.width = Math.round(r.width * scale);
  cv.height = Math.round(r.height * scale);
  const c = cv.getContext("2d");
  c.scale(scale, scale);
  const w = r.width,
    h = r.height,
    d = buffer.getChannelData(0),
    p = current(),
    { span, offset } = viewport(),
    x = (t) => ((t - offset) / span) * w;
  c.fillStyle = "#15171a";
  c.fillRect(0, 0, w, h);
  if (p) {
    c.fillStyle = "#ff744719";
    c.fillRect(x(p.start), 0, x(p.end) - x(p.start), h);
  }
  c.font = "9px monospace";
  for (let i = 0; i <= 8; i++) {
    const xx = (w * i) / 8;
    c.strokeStyle = "#34373b";
    c.beginPath();
    c.moveTo(xx, 0);
    c.lineTo(xx, h);
    c.stroke();
    c.fillStyle = "#92969d";
    c.fillText((offset + (span * i) / 8).toFixed(2) + "s", xx + 4, 15);
  }
  c.strokeStyle = "#ed966e";
  c.lineWidth = 1.5;
  for (let xx = 0; xx < w; xx += 2) {
    let a = 0,
      from = Math.floor((offset + (xx / w) * span) * buffer.sampleRate),
      to = Math.min(
        d.length,
        Math.floor((offset + ((xx + 2) / w) * span) * buffer.sampleRate),
      );
    for (let j = from; j < to; j += Math.max(1, Math.floor((to - from) / 70)))
      a = Math.max(a, Math.abs(d[j]));
    c.beginPath();
    c.moveTo(xx, h / 2 - a * h * 0.35);
    c.lineTo(xx, h / 2 + a * h * 0.35);
    c.stroke();
  }
  banks[bank].forEach((cue, i) => {
    if (!cue) return;
    const xx = x(cue.start);
    if (xx < 0 || xx > w) return;
    c.fillStyle = colors[Math.floor(i / 4)];
    c.fillRect(xx, 24, 1, h - 40);
    c.fillRect(xx, 24, 21, 16);
    c.fillStyle = "#151515";
    c.fillText(String(i + 1).padStart(2, "0"), xx + 4, 35);
  });
  if (p) {
    for (const [t, label] of [
      [p.start, "START"],
      [p.end, "END"],
    ]) {
      const xx = x(t);
      c.fillStyle = "#fff0d8";
      c.fillRect(xx - 1, 43, 2, h - 63);
      c.fillText(label, Math.max(2, Math.min(w - 38, xx + 4)), h - 25);
    }
  }
  c.fillStyle = "#d9e1e7";
  c.fillRect(x(cursor), 42, 1, h - 60);
  if (activeNote && voices.has(activeNote.v)) {
    let f = Math.max(
      0,
      (ctx.currentTime - activeNote.at) / activeNote.duration,
    );
    if (activeNote.loop) f %= 1;
    f = Math.min(1, f);
    const t = activeNote.reverse
      ? activeNote.end - f * (activeNote.end - activeNote.start)
      : activeNote.start + f * (activeNote.end - activeNote.start);
    c.fillStyle = "#b8e895";
    c.fillRect(x(t), 20, 2, h - 40);
  }
}
let dragging = null;
$("#wave").onpointerdown = (e) => {
  const r = e.currentTarget.getBoundingClientRect(),
    xx = e.clientX - r.left,
    { span, offset } = viewport(),
    p = current(),
    x = (t) => ((t - offset) / span) * r.width;
  dragging =
    p && Math.abs(xx - x(p.start)) < 10
      ? "start"
      : p && Math.abs(xx - x(p.end)) < 10
        ? "end"
        : null;
  cursor = Math.max(
    0,
    Math.min(buffer.duration - 0.001, offset + (xx / r.width) * span),
  );
  cursor = snapTime(cursor);
  e.currentTarget.setPointerCapture(e.pointerId);
  draw();
};
$("#wave").onpointermove = (e) => {
  if (!dragging) return;
  const r = e.currentTarget.getBoundingClientRect(),
    { span, offset } = viewport(),
    t = Math.max(
      0,
      Math.min(
        buffer.duration,
        offset + ((e.clientX - r.left) / r.width) * span,
      ),
    ),
    p = current();
  p[dragging] =
    dragging === "start"
      ? Math.min(snapTime(t), p.end - 0.001)
      : Math.max(snapTime(t), p.start + 0.001);
  update();
  draw();
};
$("#wave").onpointerup = () => {
  if (dragging) {
    invalidate();
    render();
  }
  dragging = null;
};
$("#wave").onpointercancel = $("#wave").onpointerup;
function addCue() {
  const i = banks[bank].findIndex((p) => !p);
  if (i < 0) return msg("Bank full. Clear a cue or switch banks.");
  banks[bank][i] = defaults(cursor, buffer.duration, i);
  selected = i;
  render();
  msg("Cue " + (i + 1) + " set at " + cursor.toFixed(3) + " s");
}
$("#wave").ondblclick = addCue;
$("#addcue").onclick = addCue;
$("#zoom").oninput = $("#pan").oninput = draw;
for (const k of [
  "pitch",
  "gain",
  "start",
  "end",
  "filter",
  "attack",
  "release",
  "mode",
])
  $("#" + k).onchange = (e) => {
    const p = current();
    if (!p) return;
    let v = k === "mode" ? e.target.value : Number(e.target.value);
    if (k !== "mode" && !Number.isFinite(v)) return;
    if (k === "start") v = Math.max(0, Math.min(v, p.end - 0.001));
    if (k === "end")
      v = Math.min(buffer.duration, Math.max(v, p.start + 0.001));
    if (k === "attack") v = Math.max(0, Math.min(2000, v));
    if (k === "release") v = Math.max(0, Math.min(5000, v));
    p[k] = v;
    invalidate();
    render();
  };
for (const k of ["pitch", "gain", "filter"])
  $("#" + k).oninput = (e) => {
    if (!current()) return;
    current()[k] = +e.target.value;
    update();
  };
$("#reverse").onclick = () => {
  if (current()) {
    current().reverse = !current().reverse;
    invalidate();
    update();
    commit();
  }
};
$("#clear").onclick = () => {
  stop();
  banks[bank][selected] = null;
  render();
  msg("Cue cleared.");
};
$("#chop").onclick = () => slice(false);
$("#transients").onclick = () => slice(true);
$("#demo").onclick = demo;
$("#preview").onclick = () => play(0, 0, {}, true);
$("#stop").onclick = stop;
$("#playpad").onpointerdown = (e) => {
  e.preventDefault();
  e.currentTarget.setPointerCapture(e.pointerId);
  const t = {};
  pointerTokens.set(e.pointerId, t);
  play(selected, 0, t);
};
$("#playpad").onclick = (e) => {
  if (e.detail === 0) play(selected);
};
$("#playpad").onpointerup = $("#playpad").onpointercancel = (e) => {
  const t = pointerTokens.get(e.pointerId);
  if (t) releaseToken(t);
  pointerTokens.delete(e.pointerId);
};
for (const [id, value] of [
  ["padmode", false],
  ["keymode", true],
])
  $("#" + id).onclick = () => {
    stop();
    keyboard = value;
    render();
  };
$("#voices").onchange = () => {
  stop();
  commit();
};
function musicalChange() {
  stop();
  commit();
  $("#shift").textContent = (keyShift() > 0 ? "+" : "") + keyShift() + " st";
  if (
    $("#sourcekey").value !== "" &&
    $("#targetkey").value !== "" &&
    Math.floor(+$("#sourcekey").value / 12) !==
      Math.floor(+$("#targetkey").value / 12)
  )
    msg("Root transposed. Pitch shifting does not convert major to minor.");
  else msg("Key and tempo settings updated.");
}
for (const id of ["sourcebpm", "targetbpm"])
  $("#" + id).onchange = (e) => {
    e.target.value = Math.max(30, Math.min(300, +e.target.value || 120));
    musicalChange();
  };
$("#sourcekey").onchange = $("#targetkey").onchange = musicalChange;
$("#sync").onclick = () => {
  stop();
  $("#sync").classList.toggle("active");
  commit();
  msg(
    $("#sync").classList.contains("active")
      ? "Tempo matched to " + $("#targetbpm").value + " BPM."
      : "Original tempo restored.",
  );
};
async function importFile(f) {
  if (!f) return;
  if (f.name.endsWith(".sforge")) return openProject(f);
  if (f.size > 128 * 1024 * 1024)
    return msg("Audio import limit: 128 MB / 10 minutes.");
  const id = ++loadId;
  worker?.terminate();
  $("#analyze").disabled = false;
  msg("Loading " + f.name + "…");
  try {
    const decoded = await audio().decodeAudioData(await f.arrayBuffer());
    if (id !== loadId) return;
    if (decoded.duration > 600 || decoded.numberOfChannels > 2)
      throw Error("Sample must be mono/stereo and at most 10 minutes.");
    buffer = decoded;
    resetControls();
    $("#filename").textContent = f.name;
    $("#duration").textContent =
      buffer.duration.toFixed(2) +
      " s · " +
      (buffer.sampleRate / 1000).toFixed(1) +
      " kHz";
    slice(false);
    msg("Loaded " + f.name + ". Detect key + BPM to begin matching.");
  } catch {
    msg("Could not decode this audio. Try WAV or MP3.");
  }
}
$("#file").onchange = (e) => {
  importFile(e.target.files[0]);
  e.target.value = "";
};
$("#importlabel").onkeydown = (e) => {
  if (e.key === "Enter" || e.key === " ") {
    e.preventDefault();
    $("#file").click();
  }
};
document.body.ondragover = (e) => e.preventDefault();
document.body.ondrop = (e) => {
  e.preventDefault();
  importFile(e.dataTransfer.files[0]);
};
document.addEventListener("keydown", (e) => {
  if (e.ctrlKey || e.metaKey) {
    const key = e.key.toLowerCase();
    if (key === "s") {
      e.preventDefault();
      saveProject();
      return;
    }
    if (key === "z" && !/INPUT|TEXTAREA/.test(e.target.tagName)) {
      e.preventDefault();
      travel(e.shiftKey ? "redo" : "undo");
      return;
    }
  }
  if (
    e.repeat ||
    e.ctrlKey ||
    e.metaKey ||
    /INPUT|SELECT|TEXTAREA/.test(e.target.tagName)
  )
    return;
  if (e.code === "Space") {
    if (e.target.tagName === "BUTTON") return;
    e.preventDefault();
    stop();
    return;
  }
  const i = keys.indexOf(e.key.toLowerCase());
  if (i < 0) return;
  e.preventDefault();
  if (!keyboard) {
    selected = i;
    update();
    highlight();
    draw();
  }
  const token = {};
  held.set(e.key.toLowerCase(), token);
  play(keyboard ? selected : i, keyboard ? i : 0, token);
});
document.addEventListener("keyup", (e) => {
  const k = e.key.toLowerCase(),
    t = held.get(k);
  if (t) releaseToken(t);
  held.delete(k);
});
window.addEventListener("blur", stop);
window.addEventListener("resize", draw);
$("#analyze").onclick = () => {
  if (!buffer) return;
  worker?.terminate();
  const id = ++analysisId,
    sourceId = loadId;
  $("#analyze").disabled = true;
  $("#analysisnote").textContent = "Analyzing up to 90 seconds of audio…";
  const rate = 11025,
    len = Math.min(Math.floor(buffer.duration * rate), rate * 90),
    mono = new Float32Array(len);
  for (let i = 0; i < len; i++) {
    const pos = Math.min(
      buffer.length - 1,
      Math.floor((i * buffer.sampleRate) / rate),
    );
    for (let ch = 0; ch < buffer.numberOfChannels; ch++)
      mono[i] += buffer.getChannelData(ch)[pos] / buffer.numberOfChannels;
  }
  worker = new Worker("analyze.js");
  worker.onmessage = (e) => {
    if (id !== analysisId || sourceId !== loadId) return;
    $("#analyze").disabled = false;
    const r = e.data;
    if (r.bpm) {
      $("#sourcebpm").value = r.bpm;
      $("#targetbpm").value = r.bpm;
    }
    $("#sourcekey").value = $("#targetkey").value =
      r.key === null ? "" : String(r.key);
    stop();
    $("#shift").textContent = "0 st";
    $("#analysisnote").textContent =
      `Estimated: ${r.bpm ? r.bpm + " BPM" : "tempo unclear"} · ${r.key === null ? "key unclear" : noteNames[r.key % 12] + (r.key >= 12 ? " minor" : " major")} · ${r.confidence} key / ${r.tempoConfidence ?? "low"} tempo confidence. Alternate tempos: ${(r.alternatives ?? []).map((n) => n.toFixed(1)).join(" / ") || "unclear"}. Confirm by ear.`;
    msg(
      "Analysis complete. Edit source estimates, then choose a target key or BPM.",
    );
    worker.terminate();
    commit();
  };
  worker.onerror = () => {
    $("#analyze").disabled = false;
    $("#analysisnote").textContent =
      "Detection failed. Set source BPM and key manually.";
  };
  worker.postMessage({ samples: mono, rate }, [mono.buffer]);
};
// Session/history. Audio is shared by history entries and embedded losslessly in .sforge downloads.
const history = new History(60);
let applying = false,
  initializing = true,
  historySource = null,
  recoveryTimer,
  recoverySource = null,
  recoveryWriting = Promise.resolve(),
  lastSavedState = "";
const controlIds = [
  "sourcebpm",
  "targetbpm",
  "sourcekey",
  "targetkey",
  "voices",
  "quality",
  "snap",
  "quantize",
  "master",
];
function snapshot() {
  const controls = Object.fromEntries(
    controlIds.map((id) => [id, $("#" + id).value]),
  );
  controls.sync = $("#sync").classList.contains("active");
  controls.formants = $("#formants").classList.contains("active");
  return {
    version: 2,
    name: $("#projectname").value || "Untitled session",
    filename: $("#filename").textContent,
    banks: structuredClone(banks),
    controls,
    audio: {
      sampleRate: buffer.sampleRate,
      frames: buffer.length,
      channels: buffer.numberOfChannels,
    },
    analysis: $("#analysisnote").textContent,
  };
}
function updateHistory() {
  $("#undo").disabled = history.index <= 0;
  $("#redo").disabled = history.index >= history.items.length - 1;
}
function commit() {
  if (applying || !buffer) return;
  const state = snapshot();
  if (historySource !== buffer) {
    historySource = buffer;
    history.reset(state);
  } else history.push(state);
  updateHistory();
  if (!initializing) {
    $("#savestatus").textContent = "Changes · saving recovery…";
    clearTimeout(recoveryTimer);
    recoveryTimer = setTimeout(autosave, 750);
  }
  markPrepared();
}
function applyState(state) {
  applying = true;
  stop();
  invalidate();
  banks = structuredClone(state.banks);
  $("#projectname").value = state.name;
  $("#filename").textContent = state.filename;
  for (const id of controlIds)
    if (state.controls[id] !== undefined)
      $("#" + id).value = state.controls[id];
  $("#sync").classList.toggle("active", !!state.controls.sync);
  $("#formants").classList.toggle("active", !!state.controls.formants);
  $("#shift").textContent = keyShift() + " st";
  $("#analysisnote").textContent =
    state.analysis || "Project restored. Source estimates remain editable.";
  masterGain.gain.value = +$("#master").value / 100;
  render();
  applying = false;
  updateHistory();
}
function travel(direction) {
  const state = history[direction]();
  if (!state) return;
  applyState(state);
  clearTimeout(recoveryTimer);
  recoveryTimer = setTimeout(autosave, 200);
  msg(direction === "undo" ? "Edit undone." : "Edit restored.");
}
$("#undo").onclick = () => travel("undo");
$("#redo").onclick = () => travel("redo");
$("#projectname").onchange = commit;
$("#cuename").onchange = (e) => {
  if (current()) {
    current().name =
      e.target.value.trim().slice(0, 80) || "Cue " + (selected + 1);
    render();
  }
};
async function autosave() {
  if (initializing || !buffer) return;
  const source = buffer,
    meta = snapshot(),
    channels =
      recoverySource === source
        ? null
        : Array.from({ length: source.numberOfChannels }, (_, i) =>
            source.getChannelData(i).slice(),
          );
  recoveryWriting = recoveryWriting
    .catch(() => {})
    .then(async () => {
      try {
        await writeRecovery(meta, channels);
        recoverySource = source;
        $("#savestatus").textContent =
          JSON.stringify(meta) === lastSavedState
            ? "Project saved · recovery current"
            : "Recovery saved locally";
      } catch {
        $("#savestatus").textContent =
          "Recovery unavailable · download project";
      }
    });
  return recoveryWriting;
}
function download(blob, name) {
  const url = URL.createObjectURL(blob),
    a = document.createElement("a");
  a.href = url;
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(url), 10000);
}
function safeName(name) {
  return (
    name
      .replace(/[^a-zA-Z0-9 _.-]/g, "")
      .trim()
      .slice(0, 70) || "JaySample"
  );
}
async function saveProject() {
  try {
    const state = snapshot(),
      channels = Array.from({ length: buffer.numberOfChannels }, (_, i) =>
        buffer.getChannelData(i),
      );
    download(encodeProject(state, channels), safeName(state.name) + ".sforge");
    lastSavedState = JSON.stringify(state);
    $("#savestatus").textContent = "Project file downloaded";
    msg("Saved audio, banks, cues and settings in one lossless project.");
  } catch (e) {
    msg("Could not save project: " + e.message);
  }
}
$("#saveproject").onclick = saveProject;
async function restoreProject(meta, channels) {
  validateProject(
    meta,
    channels.reduce((s, c) => s + c.byteLength, 0),
  );
  const next = audio().createBuffer(
    channels.length,
    channels[0].length,
    meta.audio.sampleRate,
  );
  channels.forEach((d, i) => next.copyToChannel(d, i));
  stop();
  worker?.terminate();
  $("#analyze").disabled = false;
  loadId++;
  buffer = next;
  bank = selected = 0;
  cursor = 0;
  $("#zoom").value = 1;
  $("#pan").value = 0;
  historySource = buffer;
  applyState(meta);
  history.reset(snapshot());
  updateHistory();
  $("#duration").textContent =
    buffer.duration.toFixed(2) +
    " s · " +
    (buffer.sampleRate / 1000).toFixed(1) +
    " kHz";
  $("#recovery").hidden = true;
  commit();
  await autosave();
  msg("Project restored with embedded audio and all four banks.");
}
async function openProject(file) {
  if (!file) return;
  try {
    const { meta, channels } = decodeProject(await file.arrayBuffer());
    await restoreProject(meta, channels);
  } catch (e) {
    msg("Could not open project: " + e.message);
  }
}
$("#openproject").onchange = (e) => {
  openProject(e.target.files[0]);
  e.target.value = "";
};
$("#dismissrestore").onclick = () => {
  $("#recovery").hidden = true;
  msg("Recovery dismissed. Download projects to keep permanent copies.");
};
async function initializeRecovery() {
  try {
    const recovery = await readRecovery();
    if (recovery) {
      $("#recovery").hidden = false;
      $("#restore").onclick = () =>
        restoreProject(recovery.meta, recovery.channels).catch((e) =>
          msg("Recovery unavailable: " + e.message),
        );
    }
  } catch {
    $("#savestatus").textContent = "Download projects to keep your work";
  } finally {
    initializing = false;
  }
}
// Editing helpers: zero-crossing and beat snapping; one history entry per drag.
function snapTime(t) {
  t = Math.max(0, Math.min(buffer.duration - 1 / buffer.sampleRate, t));
  const mode = $("#snap").value;
  if (mode === "beat") {
    const step = 60 / +$("#sourcebpm").value / 4;
    return Math.max(
      0,
      Math.min(
        buffer.duration - 1 / buffer.sampleRate,
        Math.round(t / step) * step,
      ),
    );
  }
  if (mode === "zero") {
    const d = buffer.getChannelData(0),
      at = Math.round(t * buffer.sampleRate),
      radius = Math.round(buffer.sampleRate * 0.006);
    let best = at,
      distance = radius + 1;
    for (
      let i = Math.max(1, at - radius);
      i < Math.min(d.length, at + radius);
      i++
    )
      if (d[i - 1] * d[i] <= 0 && Math.abs(i - at) < distance) {
        best = i;
        distance = Math.abs(i - at);
      }
    return best / buffer.sampleRate;
  }
  return t;
}
$("#wave").addEventListener(
  "wheel",
  (e) => {
    e.preventDefault();
    if (e.shiftKey) {
      $("#pan").value = Math.max(
        0,
        Math.min(100, +$("#pan").value + Math.sign(e.deltaY) * 3),
      );
    } else {
      $("#zoom").value = Math.max(
        1,
        Math.min(200, +$("#zoom").value * (e.deltaY < 0 ? 1.25 : 0.8)),
      );
    }
    draw();
  },
  { passive: false },
);
for (const id of ["quality", "snap", "quantize"])
  $("#" + id).onchange = () => {
    stop();
    invalidate();
    commit();
  };
$("#formants").onclick = () => {
  stop();
  invalidate();
  $("#formants").classList.toggle("active");
  commit();
};
$("#master").oninput = () => {
  audio();
  masterGain.gain.setTargetAtTime(
    +$("#master").value / 100,
    ctx.currentTime,
    0.01,
  );
};
$("#master").onchange = commit;
let prepareRun = 0;
async function prepare(kind) {
  const run = ++prepareRun,
    version = epoch,
    b = bank,
    items =
      kind === "keys"
        ? Array.from({ length: 16 }, (_, i) => ({ p: current(), extra: i }))
        : banks[b].map((p) => ({ p, extra: 0 }));
  const button = kind === "keys" ? $("#preparekeys") : $("#preparebank");
  button.disabled = true;
  try {
    for (let i = 0; i < items.length; i++) {
      if (run !== prepareRun || version !== epoch) break;
      const { p, extra } = items[i];
      if (p) await processed({ ...p }, extra);
      button.textContent = `Preparing ${i + 1}/${items.length}`;
      markPrepared();
    }
    msg(
      version === epoch
        ? "Prepared audio cached for immediate triggering."
        : "Preparation stopped because settings changed.",
    );
  } catch (e) {
    msg("Preparation failed: " + e.message);
  } finally {
    button.disabled = false;
    button.textContent = kind === "keys" ? "Prepare keyboard" : "Prepare bank";
  }
}
$("#preparebank").onclick = () => prepare("bank");
$("#preparekeys").onclick = () => prepare("keys");
function markPrepared() {
  if (!buffer) return;
  [...$("#pads").children].forEach((b, i) => {
    const p = keyboard ? current() : banks[bank][i];
    b.classList.toggle(
      "prepared",
      !!p && cache.has(processingKey(p, keyboard ? i : 0)),
    );
  });
}
// Internal transport or incoming 24-PPQN MIDI clock. Audio starts on AudioContext's sample clock.
let clockRunning = false,
  clockOrigin = 0,
  nextClick = 0,
  clicking = false,
  midiBeatTime = 0,
  midiTicks = 0,
  lastTick = 0,
  tickIntervals = [];
function clockBpm() {
  return +$("#targetbpm").value || 120;
}
function quantizedTime() {
  const now = audio().currentTime,
    grid = +$("#quantize").value;
  if (!grid || !clockRunning) return now;
  const origin =
      $("#clocksource").value === "midi" ? midiBeatTime : clockOrigin,
    step = (60 / clockBpm()) * grid;
  return origin + Math.ceil((now + 0.004 - origin) / step) * step;
}
function setClock(running) {
  audio();
  clockRunning = running;
  if (running) {
    clockOrigin = ctx.currentTime + 0.025;
    nextClick = clockOrigin;
  }
  $("#clockplay").classList.toggle("active", running);
  $("#clockplay").textContent = running ? "■ Clock" : "▶ Clock";
}
$("#clockplay").onclick = async () => {
  await audio().resume();
  if ($("#clocksource").value === "midi")
    return msg("Waiting for MIDI Start/Continue from the selected input.");
  setClock(!clockRunning);
};
$("#clicktrack").onclick = () => {
  clicking = !clicking;
  $("#clicktrack").classList.toggle("active", clicking);
  $("#clicktrack").textContent = clicking ? "Click on" : "Click off";
};
$("#clocksource").onchange = () => {
  setClock(false);
  stop();
  msg(
    $("#clocksource").value === "midi"
      ? "Select a MIDI clock input and start its transport."
      : "Internal clock selected.",
  );
};
setInterval(() => {
  if (!ctx) return;
  $("#voicecount").textContent = voices.size + " voices";
  const data = new Float32Array(analyser.fftSize);
  analyser.getFloatTimeDomainData(data);
  $("#peak").value = Math.min(
    1,
    data.reduce((m, v) => Math.max(m, Math.abs(v)), 0),
  );
  if (!clockRunning || $("#clocksource").value !== "internal") return;
  const step = 60 / clockBpm();
  if (nextClick < ctx.currentTime - 0.1) nextClick = ctx.currentTime + 0.01;
  while (nextClick < ctx.currentTime + 0.1) {
    if (clicking) {
      const o = ctx.createOscillator(),
        g = ctx.createGain();
      o.frequency.value = 1000;
      g.gain.setValueAtTime(0.08, nextClick);
      g.gain.exponentialRampToValueAtTime(0.0001, nextClick + 0.025);
      o.connect(g).connect(masterGain);
      o.start(nextClick);
      o.stop(nextClick + 0.03);
      o.onended = () => {
        o.disconnect();
        g.disconnect();
      };
    }
    nextClick += step;
  }
}, 25);
const taps = [];
$("#taptempo").onclick = () => {
  const now = performance.now();
  if (taps.length && now - taps.at(-1) > 2500) taps.length = 0;
  taps.push(now);
  if (taps.length > 9) taps.shift();
  if (taps.length > 2) {
    const times = taps
        .slice(1)
        .map((t, i) => t - taps[i])
        .sort((a, b) => a - b),
      bpm = 60000 / times[Math.floor(times.length / 2)];
    $("#sourcebpm").value = Math.max(30, Math.min(300, bpm)).toFixed(1);
    musicalChange();
    msg("Tapped source tempo: " + $("#sourcebpm").value + " BPM");
  }
};
// Web MIDI: no sysex; explicit user connection, hotplug support and release-on-disconnect.
let midiAccess,
  midiPort,
  midiHeld = new Map();
function releaseMidi() {
  for (const token of midiHeld.values()) releaseToken(token);
  midiHeld.clear();
}
function refreshMidi() {
  const wanted = $("#midiinput").value;
  $("#midiinput").replaceChildren(new Option("Select MIDI input", ""));
  for (const input of midiAccess.inputs.values())
    $("#midiinput").add(new Option(input.name || input.id, input.id));
  if ([...midiAccess.inputs.values()].some((p) => p.id === wanted))
    $("#midiinput").value = wanted;
  else if (midiPort) {
    releaseMidi();
    midiPort.onmidimessage = null;
    midiPort = null;
    $("#midistatus").textContent = "MIDI input disconnected";
    if ($("#clocksource").value === "midi") {
      setClock(false);
      stop();
    }
  }
}
$("#connectmidi").onclick = async () => {
  if (!navigator.requestMIDIAccess)
    return msg(
      "Web MIDI requires a compatible browser such as Chrome or Edge.",
    );
  try {
    audio().resume();
    midiAccess = await navigator.requestMIDIAccess({ sysex: false });
    refreshMidi();
    midiAccess.onstatechange = refreshMidi;
    $("#midistatus").textContent = midiAccess.inputs.size
      ? "Choose your MIDI input."
      : "No MIDI devices detected. Connect a controller and try again.";
    $("#connectmidi").textContent = "MIDI connected";
  } catch {
    msg(
      "MIDI access was unavailable or denied. Computer keyboard remains available.",
    );
  }
};
$("#midiinput").onchange = () => {
  releaseMidi();
  if (midiPort) midiPort.onmidimessage = null;
  midiPort = midiAccess?.inputs.get($("#midiinput").value);
  if (midiPort) {
    midiPort.onmidimessage = (e) => handleMidi(e.data, e.timeStamp);
    $("#midistatus").textContent = midiPort.name + " · MIDI active";
  }
};
export function handleMidi(data, timestamp = performance.now()) {
  const [status, note = 0, velocity = 0] = data;
  if (status >= 0xf8) {
    if ($("#clocksource").value !== "midi") return;
    if (status === 0xfa || status === 0xfb) {
      setClock(true);
      midiTicks = 0;
      lastTick = 0;
      tickIntervals = [];
      midiBeatTime = audio().currentTime + 0.015;
    } else if (status === 0xfc) {
      setClock(false);
      stop();
    } else if (status === 0xf8) {
      if (lastTick && timestamp - lastTick > 1 && timestamp - lastTick < 100) {
        tickIntervals.push(timestamp - lastTick);
        if (tickIntervals.length > 96) tickIntervals.shift();
        if (tickIntervals.length >= 24 && midiTicks % 24 === 0) {
          const sorted = [...tickIntervals].sort((a, b) => a - b),
            bpm = 60000 / (sorted[Math.floor(sorted.length / 2)] * 24);
          $("#targetbpm").value = Math.max(30, Math.min(300, bpm)).toFixed(1);
        }
      }
      lastTick = timestamp;
      if (midiTicks++ % 24 === 0) midiBeatTime = audio().currentTime + 0.005;
    }
    return;
  }
  const command = status & 0xf0,
    key = (status & 15) + ":" + note;
  if (command === 0xb0) {
    if (note === 123 || note === 120) {
      stop();
      midiHeld.clear();
    } else if (note === 7) {
      $("#master").value = Math.round((velocity / 127) * 100);
      $("#master").oninput();
    }
    return;
  }
  if (command === 0xc0) {
    stop();
    bank = note % 4;
    selected = 0;
    render();
    return;
  }
  if (command === 0x80 || (command === 0x90 && velocity === 0)) {
    const token = midiHeld.get(key);
    if (token) releaseToken(token);
    midiHeld.delete(key);
    return;
  }
  if (command !== 0x90) return;
  audio().resume();
  const extra = keyboard ? note - 48 : 0,
    i = keyboard ? selected : note - 36;
  if (keyboard ? extra < -24 || extra > 24 : i < 0 || i > 15) return;
  if (midiHeld.has(key)) releaseToken(midiHeld.get(key));
  if (!keyboard) {
    selected = i;
    update();
    highlight();
    draw();
  }
  const token = { velocity: velocity / 127 };
  midiHeld.set(key, token);
  play(i, extra, token);
  $("#midistatus").textContent = `MIDI note ${note} · velocity ${velocity}`;
}
// Shared rendered export path ensures exported and monitored cue effects match.
async function renderCue(snapshot) {
  const clip = await processed(snapshot),
    len = clip.length,
    sr = clip.sampleRate,
    o = new OfflineAudioContext(2, len, sr),
    s = o.createBufferSource(),
    g = o.createGain(),
    f = o.createBiquadFilter();
  s.buffer = clip;
  f.type = "lowpass";
  f.frequency.value = Math.min(sr / 2, snapshot.filter);
  const attack = Math.min(snapshot.attack / 1000, clip.duration / 2),
    release = Math.min(snapshot.release / 1000, clip.duration / 2),
    level = (snapshot.gain / 100) * 0.8;
  g.gain.setValueAtTime(0, 0);
  g.gain.linearRampToValueAtTime(level, attack);
  g.gain.setValueAtTime(level, Math.max(attack, clip.duration - release));
  g.gain.linearRampToValueAtTime(0, clip.duration);
  s.connect(f).connect(g).connect(o.destination);
  s.start();
  return o.startRendering();
}
function wav(out) {
  const len = out.length,
    channels = out.numberOfChannels,
    sr = out.sampleRate,
    ab = new ArrayBuffer(44 + len * channels * 2),
    v = new DataView(ab);
  function str(at, t) {
    for (let i = 0; i < t.length; i++) v.setUint8(at + i, t.charCodeAt(i));
  }
  str(0, "RIFF");
  v.setUint32(4, ab.byteLength - 8, true);
  str(8, "WAVEfmt ");
  v.setUint32(16, 16, true);
  v.setUint16(20, 1, true);
  v.setUint16(22, channels, true);
  v.setUint32(24, sr, true);
  v.setUint32(28, sr * channels * 2, true);
  v.setUint16(32, channels * 2, true);
  v.setUint16(34, 16, true);
  str(36, "data");
  v.setUint32(40, ab.byteLength - 44, true);
  for (let i = 0; i < len; i++)
    for (let c = 0; c < channels; c++)
      v.setInt16(
        44 + (i * channels + c) * 2,
        Math.max(-1, Math.min(1, out.getChannelData(c)[i])) * 32767,
        true,
      );
  return new Uint8Array(ab);
}
$("#export").onclick = async () => {
  const p = current();
  if (!p) return msg("Select a loaded cue.");
  const button = $("#export");
  button.disabled = true;
  try {
    download(
      new Blob([wav(await renderCue({ ...p }))], { type: "audio/wav" }),
      "JaySample-" + safeName(p.name) + ".wav",
    );
    msg(
      "Exported WAV with cue effects, pitch and tempo. Master level is monitor-only.",
    );
  } catch (e) {
    msg("Export failed: " + e.message);
  } finally {
    button.disabled = false;
  }
};
function midiFile(indices, bpm) {
  const track = [
    0,
    0xff,
    0x51,
    3,
    ...[
      (Math.round(60000000 / bpm) >> 16) & 255,
      (Math.round(60000000 / bpm) >> 8) & 255,
      Math.round(60000000 / bpm) & 255,
    ],
  ];
  for (const i of indices)
    track.push(0, 0x90, 36 + i, 100, 0x83, 0x60, 0x80, 36 + i, 0);
  track.push(0, 0xff, 0x2f, 0);
  const n = track.length;
  return new Uint8Array([
    77,
    84,
    104,
    100,
    0,
    0,
    0,
    6,
    0,
    0,
    0,
    1,
    1,
    0xe0,
    77,
    84,
    114,
    107,
    n >>> 24,
    (n >>> 16) & 255,
    (n >>> 8) & 255,
    n & 255,
    ...track,
  ]);
}
$("#exportbank").onclick = async () => {
  const button = $("#exportbank"),
    source = buffer,
    version = epoch,
    b = bank,
    items = structuredClone(banks[bank]),
    files = {},
    indices = [],
    map = [];
  const expectedBytes = items.reduce(
    (sum, p) =>
      sum + (p ? ((p.end - p.start) * buffer.sampleRate * 4) / tempo() : 0),
    0,
  );
  if (expectedBytes > 256 * 1024 * 1024)
    return msg(
      "Bank export would exceed 256 MB. Shorten cues or export them individually.",
    );
  button.disabled = true;
  try {
    for (let i = 0; i < items.length; i++) {
      if (epoch !== version || buffer !== source)
        throw Error("Settings changed during export. Export again.");
      if (!items[i]) continue;
      button.textContent = "Rendering " + (i + 1) + "/16…";
      const name =
        String(i + 1).padStart(2, "0") + "-" + safeName(items[i].name) + ".wav";
      files[name] = wav(await renderCue(items[i]));
      indices.push(i);
      map.push({ note: 36 + i, file: name, name: items[i].name });
    }
    if (!indices.length) throw Error("This bank is empty.");
    const bpm = $("#sync").classList.contains("active")
      ? +$("#targetbpm").value
      : +$("#sourcebpm").value;
    files["Bank-" + "ABCD"[b] + ".mid"] = midiFile(indices, bpm);
    files["mapping.json"] = strToU8(
      JSON.stringify({ bpm, bank: "ABCD"[b], samples: map }, null, 2),
    );
    files["README.txt"] = strToU8(
      "JaySample bank export\n\nEach WAV includes cue pitch, tempo, reverse, filter and envelope. One pass per cue; master volume is not baked in.\n\nAbleton: load a Drum Rack. Drop WAV files onto pads starting at MIDI note 36 (C1 in Ableton), following mapping.json. Import the MIDI file to that track to audition each cue.\n\nLogic: create a Drum Machine Designer track. Load each WAV into a Quick Sampler cell, assign MIDI notes according to mapping.json, then import the MIDI file. Logic octave labels can vary; use note numbers.\n\nThe MIDI file is an audition sequence with one cue per beat, not a reconstruction of your performance.\n",
    );
    download(
      new Blob([zipSync(files, { level: 0 })], { type: "application/zip" }),
      safeName($("#projectname").value) + "-Bank-" + "ABCD"[b] + ".zip",
    );
    msg(
      "Bank exported: rendered WAVs, MIDI audition sequence and note mapping.",
    );
  } catch (e) {
    msg("Bank export failed: " + e.message);
  } finally {
    button.disabled = false;
    button.textContent = "↓ Export bank + MIDI";
  }
};
window.addEventListener("pagehide", () => {
  releaseMidi();
  stop();
  if (clockRunning) setClock(false);
});
window.addEventListener("blur", () => {
  releaseMidi();
  midiHeld.clear();
});
$("#stop").onclick = () => {
  stop();
  setClock(false);
};
demo();
initializeRecovery();
export function diagnostics() {
  return {
    voices: voices.size,
    pending: pending.size,
    cacheBytes,
    prepared: cache.size,
    contextTime: ctx?.currentTime ?? 0,
    startTimes: [...voices].map((v) => v.startTime),
    clockRunning,
    clockOrigin,
    bpm: clockBpm(),
    bank,
    selected,
    keyboard,
  };
}
