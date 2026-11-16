/**
 * Xenon's CPU half, ported to JavaScript by hand. ONLY A READER CHECKS IT.
 *
 * Nothing in the repository compares this file with the C++ it was written
 * from: demo/tools/check_shaders.py holds the shaders and the presets to the
 * plugin's, character for character, but this is a translation, and a
 * translation that is subtly wrong draws a strobe that is subtly wrong, which
 * nobody would notice. When you change one of the files below, change its
 * section here too.
 *
 *   Controls.cpp   the parameter table and every conversion to physical units
 *   Rig.cpp        the chase, each head's capacitor bank and tube, the exposure
 *   SliceMap.cpp   the XML reader, the preset's slices, polygons, curves and
 *                  masks, the collapse, the grid and the head order
 *   Xenon.cpp      the CPU half of ProcessOpenGL: the clock's frame, the rig's
 *                  inputs, the light, rebuildGeometry and the light pass's quads
 *
 * What is NOT here, because a page cannot have it: the file side of SliceMap
 * (the Documents folder, AdvancedOutput.xml's pointer, stat and fopen, and
 * MapSource's polling, which plugin.js replaces with files the page holds),
 * the host clock's seconds-or-milliseconds vote (this host's unit is known),
 * and the audio analyser (no spectrum reaches a page, and with none the
 * analyser never fires). The negative-control hooks are 0 in a shipped plugin
 * and are left out.
 *
 * C++ `float` is mirrored with Math.fround wherever the plugin stores or
 * computes in float (its parameters, the light, what goes to the GPU); the
 * rest is double, as there. std::lround is half away from zero, which
 * Math.round is not for negatives, so it is ported by hand.
 *
 * Pure: no DOM, no GL, so it runs under node as well as in the page.
 */

export const f32 = Math.fround;
const clampTo = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);
/** std::lround and std::llround: half away from zero. */
export const lround = (v) => (v < 0 ? -Math.round(-v) : Math.round(v));

//===========================================================================
// Controls.h / Controls.cpp
//===========================================================================

/** Every control, in the order Resolume shows them (ParamId). */
export const PT = {
  PATTERN: 0, TRIGGER: 1, FIRE: 2, AUDIO: 3, THRESHOLD: 4, SEED: 5,
  RECHARGE: 6, DECAY: 7, INTENSITY: 8, COLOUR_R: 9, COLOUR_G: 10, COLOUR_B: 11,
  LOOK: 12, WIDTH: 13, ROOM: 14,
  SLICES: 15, PRESET_FILE: 16, ORDER: 17, ROWS: 18, COLUMNS: 19,
  ABOUT_FIRST: 20,
};

export const Kind = {
  Standard: 'standard', Integer: 'integer', Option: 'option', Event: 'event', Buffer: 'buffer',
  File: 'file', Red: 'red', Green: 'green', Blue: 'blue',
};

export const Pattern = { Forward: 0, Reverse: 1, Bounce: 2, Build: 3, Random: 4, All: 5, Count: 6 };
export const Trigger = { Bar: 0, Beat: 1, HalfBeat: 2, QuarterBeat: 3, EighthBeat: 4, Onset: 5, FireOnly: 6, Count: 7 };
export const Look = { Fill: 0, Outline: 1, Gate: 2, Count: 3 };
export const Slices = { Map: 0, Grid: 1, Count: 2 };

export const kMinGrid = 1;
export const kMaxGrid = 32;
export const kMaxSeed = 9999;

function clamp01(v) {
  return Number.isFinite(v) ? clampTo(v, 0.0, 1.0) : 0.0;
}
function geometric(v, low, high) {
  return low * Math.pow(high / low, clamp01(v));
}
function inverseGeometric(value, low, high) {
  const lo = Math.min(low, high);
  const hi = Math.max(low, high);
  return f32(Math.log(clampTo(value, lo, hi) / low) / Math.log(high / low));
}
function linear(v, low, high) {
  return low + (high - low) * clamp01(v);
}
function inverseLinear(value, low, high) {
  return f32(clampTo((value - low) / (high - low), 0.0, 1.0));
}

const kRechargeLow = 5e-3, kRechargeHigh = 5.0;
const kDecayLow = 0.2e-3, kDecayHigh = 0.5;
const kIntensityMost = 2.0;
const kWidthLow = 1.0, kWidthHigh = 100.0;

export const kPatternNames = ['Forward', 'Reverse', 'Bounce', 'Build', 'Random', 'All'];
export const kTriggerNames = ['Bar', 'Beat', 'Half Beat', 'Quarter Beat', 'Eighth Beat', 'Onset', 'Fire Only'];
export const kLookNames = ['Fill', 'Outline', 'Gate'];
export const kSlicesNames = ['Map', 'Grid'];
export const kOrderNames = ['As Listed', 'Left to Right', 'Top to Bottom'];

export function RechargeFromParam(value) { return geometric(value, kRechargeLow, kRechargeHigh); }
export function ParamFromRecharge(seconds) { return inverseGeometric(seconds, kRechargeLow, kRechargeHigh); }
export function DecayFromParam(value) { return geometric(value, kDecayLow, kDecayHigh); }
export function ParamFromDecay(seconds) { return inverseGeometric(seconds, kDecayLow, kDecayHigh); }
export function IntensityFromParam(value) { return linear(value, 0.0, kIntensityMost); }
export function ParamFromIntensity(intensity) { return inverseLinear(intensity, 0.0, kIntensityMost); }
export function WidthFromParam(value) { return geometric(value, kWidthLow, kWidthHigh); }
export function ParamFromWidth(pixels) { return inverseGeometric(pixels, kWidthLow, kWidthHigh); }
export function RoomFromParam(value) { return clamp01(value); }
export function SensitivityFromThreshold(value) { return 1.0 - clamp01(value); }
export function IntegerFromParam(value, low, high, fallback) {
  if (!Number.isFinite(value)) return fallback;
  return clampTo(lround(value), low, high);
}

/** An option's element index from the host's value. */
export function OptionIndex(value, count) {
  if (!Number.isFinite(value)) return 0;
  return clampTo(lround(value), 0, count - 1);
}

/** Beats per step for a beat-type trigger; 0 for Onset and Fire Only. */
export function BeatsPerStep(trigger) {
  switch (trigger) {
    case Trigger.Bar: return 4.0;
    case Trigger.Beat: return 1.0;
    case Trigger.HalfBeat: return 0.5;
    case Trigger.QuarterBeat: return 0.25;
    case Trigger.EighthBeat: return 0.125;
    default: return 0.0;
  }
}

const standard = (id, name, group, value, kind = Kind.Standard) =>
  ({ id, name, group, kind, defaultValue: f32(value), options: null, minimum: 0, maximum: 1 });
const option = (id, name, group, value, names) =>
  ({ id, name, group, kind: Kind.Option, defaultValue: f32(value), options: names, minimum: 0, maximum: names.length - 1 });
const event = (id, name, group) => ({ id, name, group, kind: Kind.Event, defaultValue: 0, options: null, minimum: 0, maximum: 1 });
const integer = (id, name, group, value, low, high) =>
  ({ id, name, group, kind: Kind.Integer, defaultValue: f32(value), options: null, minimum: low, maximum: high });
const buffer = (id, name, group) => ({ id, name, group, kind: Kind.Buffer, defaultValue: 0, options: null, minimum: 0, maximum: 1 });
const file = (id, name, group) => ({ id, name, group, kind: Kind.File, defaultValue: 0, options: null, minimum: 0, maximum: 1 });

// The defaults. The active map's slices stepped through one a beat, each a
// white strobe with a quarter-second bank and a 2 ms tube, so each flash
// lands in one frame; flashing over the clip at full room light.
const kTable = [
  option(PT.PATTERN, 'Pattern', 'Chase', 0.0, kPatternNames),
  option(PT.TRIGGER, 'Trigger', 'Chase', 1.0, kTriggerNames),
  event(PT.FIRE, 'Fire', 'Chase'),
  buffer(PT.AUDIO, 'Audio', 'Chase'),
  standard(PT.THRESHOLD, 'Threshold', 'Chase', 0.4),
  integer(PT.SEED, 'Seed', 'Chase', 1, 0, kMaxSeed),
  standard(PT.RECHARGE, 'Recharge', 'Strobe', ParamFromRecharge(0.25)),
  standard(PT.DECAY, 'Decay', 'Strobe', ParamFromDecay(2e-3)),
  standard(PT.INTENSITY, 'Intensity', 'Strobe', ParamFromIntensity(1.0)),
  standard(PT.COLOUR_R, 'Colour', 'Strobe', 1.0, Kind.Red),
  standard(PT.COLOUR_G, 'Colour_Green', 'Strobe', 1.0, Kind.Green),
  standard(PT.COLOUR_B, 'Colour_Blue', 'Strobe', 1.0, Kind.Blue),
  option(PT.LOOK, 'Look', 'Look', 0.0, kLookNames),
  standard(PT.WIDTH, 'Width', 'Look', ParamFromWidth(12.0)),
  standard(PT.ROOM, 'Room Light', 'Look', 1.0),
  option(PT.SLICES, 'Slices', 'Slices', 0.0, kSlicesNames),
  file(PT.PRESET_FILE, 'Preset File', 'Slices'),
  option(PT.ORDER, 'Order', 'Slices', 0.0, kOrderNames),
  integer(PT.ROWS, 'Rows', 'Slices', 2, kMinGrid, kMaxGrid),
  integer(PT.COLUMNS, 'Columns', 'Slices', 4, kMinGrid, kMaxGrid),
];
if (kTable.length !== PT.ABOUT_FIRST || kTable.some((row, i) => row.id !== i)) {
  throw new Error('every control needs a row in the table, in ParamId order');
}

/** The table, indexed by ParamId, for every id below PT.ABOUT_FIRST. */
export function InfoOf(id) {
  return kTable[Math.min(id, PT.ABOUT_FIRST - 1)];
}

//===========================================================================
// Rig.h / Rig.cpp
//===========================================================================

/** The most steps one frame may fire: more was a seek or a stall, not music. */
const kMaxPerFrame = 64;
/** A bar is four beats (Resolume's bar phase runs 0..1 over a 4/4 bar). */
const kBeatsPerBar = 4.0;
/** A flash is dropped once its light has fallen below e^-20 of its start. */
const kKeepTaus = 20.0;

/** A 32-bit integer hash (PCG's output permutation of an LCG step). */
export function HashInt(v) {
  const state = (Math.imul(v >>> 0, 747796405) + 2891336453) >>> 0;
  const word = Math.imul(((state >>> ((state >>> 28) + 4)) ^ state) >>> 0, 277803737) >>> 0;
  return ((word >>> 22) ^ word) >>> 0;
}

/** The heads step k of a chase over `count` heads lights, by chase position. */
export function HeadsForStep(pattern, step, count, seed, out) {
  out.length = 0;
  if (count <= 0 || step < 0) return out;
  const n = count;
  switch (pattern) {
    case Pattern.Forward: out.push(step % n); break;
    case Pattern.Reverse: out.push(n - 1 - (step % n)); break;
    case Pattern.Bounce: {
      if (n === 1) {
        out.push(0);
        break;
      }
      const period = 2 * n - 2;
      const p = step % period;
      out.push(p < n ? p : period - p);
      break;
    }
    case Pattern.Build:
      for (let i = 0; i <= step % n; ++i) out.push(i);
      break;
    case Pattern.Random: {
      // Every head once a cycle: Fisher-Yates over the cycle's own hash, so
      // the order is fixed by ( Seed, cycle ) and nothing else.
      const cycle = Math.floor(step / n) >>> 0;
      const perm = new Array(n);
      for (let i = 0; i < count; ++i) perm[i] = i;
      const base = HashInt((Math.imul(seed >>> 0, 0x9E3779B9) ^ HashInt((cycle + 0x632BE5AB) >>> 0)) >>> 0);
      for (let i = count - 1; i > 0; --i) {
        const j = HashInt((base ^ Math.imul(i >>> 0, 0x85EBCA6B)) >>> 0) % ((i + 1) >>> 0);
        const swap = perm[i];
        perm[i] = perm[j];
        perm[j] = swap;
      }
      out.push(perm[step % n]);
      break;
    }
    case Pattern.All:
      for (let i = 0; i < count; ++i) out.push(i);
      break;
    default: break;
  }
  return out;
}

export class Rig {
  constructor() {
    this.charge = [];     // v / V_s per head
    this.chargedAt = [];  // the time `charge` holds for
    this.flashes = [];    // still lighting frames: { head, time, energy, step }
    this.fresh = [];
    this.lit = [];
    this.lastPhase = -1.0;
    this.step = 0;
    this.fired = 0;
    this.longestTau = 0.5;
  }

  /** A new set of heads: every bank full, the chase back at step 0. */
  Reset(headCount) {
    headCount = Math.max(headCount, 0);
    this.charge = new Array(headCount).fill(1.0);
    this.chargedAt = new Array(headCount).fill(0.0);
    this.flashes = [];
    this.fresh = [];
    this.step = 0;
  }

  HeadCount() { return this.charge.length; }
  Charge(head) { return head >= 0 && head < this.HeadCount() ? this.charge[head] : 0.0; }
  Steps() { return this.step; }
  LastFlashes() { return this.fresh; }
  Fired() { return this.fired; }

  crossings(p0, p1, stepSize, frameStart, dt, at) {
    if (!(stepSize > 0.0) || !(p1 > p0)) return;
    let boundary = (Math.floor(p0 / stepSize + 1e-9) + 1.0) * stepSize;
    for (let k = 0; boundary <= p1 + 1e-12 && k < kMaxPerFrame; ++k, boundary += stepSize) {
      const inside = (boundary - p0) / (p1 - p0);
      at.push(frameStart + clampTo(inside, 0.0, 1.0) * dt);
    }
  }

  fireStep(at, input, order) {
    const count = this.HeadCount();
    if (count === 0) {
      ++this.step;
      return;
    }
    HeadsForStep(input.pattern, this.step, count, input.seed, this.lit);
    const rc = Math.max(input.recharge, 1e-6);
    for (const position of this.lit) {
      const head = position >= 0 && position < order.length ? order[position] : position;
      if (head < 0 || head >= count) continue;
      // The bank charges on from where it was to the moment it fires.
      const gap = Math.max(at - this.chargedAt[head], 0.0);
      const q = 1.0 - (1.0 - this.charge[head]) * Math.exp(-gap / rc);
      const e = q * q;
      const f = { head, time: at, energy: e, step: this.step };
      this.flashes.push(f);
      this.fresh.push(f);
      ++this.fired;
      this.charge[head] = 0.0;
      this.chargedAt[head] = at;
    }
    ++this.step;
  }

  /**
   * One host frame ( now - dt, now ]. Fires every step due in it, in time
   * order, through `order` (chase position -> head index), then advances
   * every bank's charge to `now`.
   */
  Advance(now, dt, input, order) {
    this.fresh = [];
    const frameStart = now - dt;
    this.longestTau = Math.max(input.decay, 1e-3);

    // The bar phase: the host's, or a clock of the plugin's own at its BPM, so
    // a host that never moves it still has beats.
    const tempo = input.bpm > 1.0 ? input.bpm : 120.0;
    const phaseNow = input.hostBeat ? clampTo(input.barPhase, 0.0, 1.0) : (now * tempo / 240.0) % 1.0;
    const p0 = this.lastPhase;
    let p1 = phaseNow;
    const phaseValid = this.lastPhase >= 0.0 && dt > 0.0;
    if (phaseValid && p1 < p0) p1 += 1.0;
    this.lastPhase = phaseNow;

    const at = [];
    const beats = BeatsPerStep(input.trigger);
    if (beats > 0.0 && phaseValid) this.crossings(p0, p1, beats / kBeatsPerBar, frameStart, dt, at);
    // An onset or a press fires at the start of the frame that heard it.
    const unannounced = frameStart;
    if (input.trigger === Trigger.Onset && input.onset) at.push(unannounced);
    for (let i = 0; i < input.presses && i < kMaxPerFrame; ++i) at.push(unannounced);
    at.sort((a, b) => a - b);
    for (const t of at) this.fireStep(t, input, order);

    // Every bank on to the end of the frame, with this frame's RC.
    const rc = Math.max(input.recharge, 1e-6);
    for (let h = 0; h < this.charge.length; ++h) {
      const gap = now - this.chargedAt[h];
      if (gap > 0.0) {
        this.charge[h] = 1.0 - (1.0 - this.charge[h]) * Math.exp(-gap / rc);
        this.chargedAt[h] = now;
      } else if (gap < -1.0) {
        this.chargedAt[h] = now; // the clock ran backwards: a clip restart
      }
    }

    // Flashes whose light no frame will see again.
    while (this.flashes.length > 0
      && (now - this.flashes[0].time > kKeepTaus * this.longestTau || this.flashes[0].time > now + 1.0)) {
      this.flashes.shift();
    }
  }

  /** Each head's light over ( now - dt, now ], a full flash caught whole = 1. As float. */
  Exposure(now, dt, tau) {
    const out = new Float32Array(this.charge.length);
    tau = Math.max(tau, 1e-7);
    const t0 = now - Math.max(dt, 0.0);
    const sum = new Array(this.charge.length).fill(0.0);
    for (const f of this.flashes) {
      if (f.head < 0 || f.head >= sum.length || f.time > now) continue;
      sum[f.head] += f.energy * (Math.exp(-Math.max(0.0, t0 - f.time) / tau) - Math.exp(-(now - f.time) / tau));
    }
    for (let h = 0; h < sum.length; ++h) out[h] = sum[h];
    return out;
  }
}

//===========================================================================
// SliceMap.h / SliceMap.cpp: the preset reader, the grid and the head order
//===========================================================================

export const HeadOrder = { AsListed: 0, LeftToRight: 1, TopToBottom: 2 };

export const kMaxHeads = 256;
export const kMaxEdges = 8192;
/** A cubic Bezier segment becomes this many straight edges. */
export const kBezierSteps = 16;
/** Two regions closer than this, point for point, in composition pixels, are one. */
export const kSameRegion = 0.01;

// A small XML reader: elements, attributes, text, the five entities and
// numeric references, comments, the prolog and CDATA. Enough for Resolume's
// own files, which are plain machine-written XML; nothing here validates.
//
// The C++ walks bytes and this walks the UTF-16 units of the decoded text.
// The structure is ASCII, so the two agree on what is an element, an
// attribute or a name; a parse error's position is turned back into the
// byte offset the plugin would log. (An entity's twelve-unit window is
// counted in units here, in bytes there: it differs only for a non-ASCII
// character inside a malformed entity, and the text appended is the same.)

class Element {
  constructor() {
    this.name = '';
    this.attrs = [];
    this.children = [];
    this.text = '';
  }

  attr(key) {
    for (const [k, v] of this.attrs) if (k === key) return v;
    return null;
  }

  child(tag) {
    for (const c of this.children) if (c.name === tag) return c;
    return null;
  }

  /** The child `<tag name="key">`. */
  childNamed(tag, key) {
    for (const c of this.children) {
      const n = c.attr('name');
      if (c.name === tag && n !== null && n === key) return c;
    }
    return null;
  }
}

/** strtoul on what follows `&#` or `&#x`: leading digits, 0 if there are none. */
function strtoul(text, radix) {
  const match = (radix === 16 ? /^[\t\n\v\f\r ]*([+-]?)([0-9A-Fa-f]+)/ : /^[\t\n\v\f\r ]*([+-]?)([0-9]+)/).exec(text);
  if (!match) return 0;
  const v = parseInt(match[2], radix);
  // A negative number wraps to a huge unsigned one, which appendUtf8 drops.
  return match[1] === '-' && v !== 0 ? Number.MAX_SAFE_INTEGER : v;
}

/** appendUtf8: a code point, or nothing past U+10FFFF. */
function codePoint(cp) {
  return cp < 0x110000 ? String.fromCodePoint(cp) : '';
}

function unescape(s, from, count) {
  const n = from + count;
  if (s.indexOf('&', from) < 0 || s.indexOf('&', from) >= n) return s.slice(from, n);
  let out = '';
  for (let i = from; i < n; ++i) {
    if (s[i] !== '&') {
      out += s[i];
      continue;
    }
    let end = i + 1;
    while (end < n && end - i < 12 && s[end] !== ';') ++end;
    if (end >= n || s[end] !== ';') {
      out += '&';
      continue;
    }
    const ent = s.slice(i + 1, end);
    if (ent === 'amp') out += '&';
    else if (ent === 'lt') out += '<';
    else if (ent === 'gt') out += '>';
    else if (ent === 'quot') out += '"';
    else if (ent === 'apos') out += "'";
    else if (ent.length > 1 && ent[0] === '#') {
      const hex = ent[1] === 'x' || ent[1] === 'X';
      out += codePoint(strtoul(ent.slice(hex ? 2 : 1), hex ? 16 : 10));
    } else {
      out += s.slice(i, end + 1);
    }
    i = end;
  }
  return out;
}

class XmlReader {
  constructor(text) {
    this.s = text;
    this.n = text.length;
    this.i = 0;
    this.error = '';
  }

  Document(root) {
    this.skipMisc();
    if (this.i >= this.n || this.s[this.i] !== '<') return this.fail('no root element');
    if (!this.element(root, 0)) return false;
    return true;
  }

  fail(why) {
    if (this.error === '') {
      const bytes = new TextEncoder().encode(this.s.slice(0, this.i)).length;
      this.error = `${why} at byte ${bytes}`;
    }
    return false;
  }

  startsWith(lit) {
    return this.i + lit.length <= this.n && this.s.startsWith(lit, this.i);
  }

  skipSpace() {
    const { s, n } = this;
    while (this.i < n && (s[this.i] === ' ' || s[this.i] === '\t' || s[this.i] === '\r' || s[this.i] === '\n')) ++this.i;
  }

  skipPast(lit) {
    const at = this.s.indexOf(lit, this.i);
    if (at < 0) {
      this.i = this.n;
      return false;
    }
    this.i = at + lit.length;
    return true;
  }

  /** The prolog, comments, doctype and processing instructions between elements. */
  skipMisc() {
    for (;;) {
      this.skipSpace();
      if (this.startsWith('<?')) this.skipPast('?>');
      else if (this.startsWith('<!--')) this.skipPast('-->');
      else if (this.startsWith('<!DOCTYPE')) this.skipPast('>');
      else return;
    }
  }

  static nameChar(c) {
    return !(c === ' ' || c === '\t' || c === '\r' || c === '\n' || c === '/' || c === '>' || c === '=' || c === '<');
  }

  name() {
    const start = this.i;
    while (this.i < this.n && XmlReader.nameChar(this.s[this.i])) ++this.i;
    return this.s.slice(start, this.i);
  }

  element(e, depth) {
    const { s, n } = this;
    if (depth > 200) return this.fail('nesting too deep');
    ++this.i; // '<'
    e.name = this.name();
    if (e.name === '') return this.fail('empty element name');
    for (;;) {
      this.skipSpace();
      if (this.i >= n) return this.fail('unterminated start tag');
      if (s[this.i] === '/') {
        if (this.i + 1 < n && s[this.i + 1] === '>') {
          this.i += 2;
          return true;
        }
        return this.fail("stray '/' in a tag");
      }
      if (s[this.i] === '>') {
        ++this.i;
        break;
      }
      const key = this.name();
      if (key === '') return this.fail('bad attribute');
      this.skipSpace();
      if (this.i >= n || s[this.i] !== '=') return this.fail("attribute without '='");
      ++this.i;
      this.skipSpace();
      if (this.i >= n || (s[this.i] !== '"' && s[this.i] !== "'")) return this.fail('unquoted attribute');
      const quote = s[this.i++];
      const end = s.indexOf(quote, this.i);
      if (end < 0) return this.fail('unterminated attribute');
      e.attrs.push([key, unescape(s, this.i, end - this.i)]);
      this.i = end + 1;
    }
    // Content.
    for (;;) {
      const lt = s.indexOf('<', this.i);
      if (lt < 0) return this.fail('unterminated element');
      if (lt > this.i) e.text += unescape(s, this.i, lt - this.i);
      this.i = lt;
      if (this.startsWith('</')) {
        this.i += 2;
        const closing = this.name();
        this.skipSpace();
        if (closing !== e.name || this.i >= n || s[this.i] !== '>') return this.fail('mismatched end tag');
        ++this.i;
        return true;
      }
      if (this.startsWith('<!--')) {
        if (!this.skipPast('-->')) return this.fail('unterminated comment');
        continue;
      }
      if (this.startsWith('<![CDATA[')) {
        const start = this.i + 9;
        const end = s.indexOf(']]>', start);
        if (end < 0) return this.fail('unterminated CDATA');
        e.text += s.slice(start, end);
        this.i = end + 3;
        continue;
      }
      if (this.startsWith('<?')) {
        this.skipPast('?>');
        continue;
      }
      const child = new Element();
      e.children.push(child);
      if (!this.element(child, depth + 1)) return false;
    }
  }
}

/**
 * strtod on an attribute's text, or the fallback when nothing converts or the
 * result is not finite. Decimal and hexadecimal, as strtod reads them;
 * "inf" and "nan" convert to something not finite, so they fall back too.
 */
function number(text, fallback) {
  if (text === null || text === '') return fallback;
  const dec = /^[\t\n\v\f\r ]*([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)/.exec(text);
  const hex = /^[\t\n\v\f\r ]*([+-]?)0[xX]([0-9A-Fa-f]*)(?:\.([0-9A-Fa-f]*))?(?:[pP]([+-]?\d+))?/.exec(text);
  let v = NaN;
  if (hex && (hex[2] !== '' || (hex[3] ?? '') !== '')) {
    const whole = hex[2] === '' ? 0 : parseInt(hex[2], 16);
    const frac = (hex[3] ?? '').split('').reduce((acc, c, k) => acc + parseInt(c, 16) / 16 ** (k + 1), 0);
    v = (whole + frac) * 2 ** (hex[4] ? Number(hex[4]) : 0) * (hex[1] === '-' ? -1 : 1);
  } else if (dec) {
    v = Number(dec[1]);
  }
  return Number.isFinite(v) ? v : fallback;
}

/** `<Params name=group><Param name=key value=…/>` → value, or null. */
function paramValue(owner, group, key) {
  const params = group !== null ? owner.childNamed('Params', group) : owner.child('Params');
  if (params === null) return null;
  const p = params.childNamed('Param', key);
  return p !== null ? p.attr('value') : null;
}

function isOff(v) {
  return v !== null && (v === '0' || v === 'false');
}

function vertices(list) {
  const out = [];
  if (list === null) return out;
  for (const v of list.children) {
    if (v.name === 'v') out.push({ x: number(v.attr('x'), 0.0), y: number(v.attr('y'), 0.0) });
  }
  return out;
}

function bezier(a, b, c, d, t) {
  const u = 1.0 - t;
  const w0 = u * u * u, w1 = 3.0 * u * u * t, w2 = 3.0 * u * t * t, w3 = t * t * t;
  return { x: w0 * a.x + w1 * b.x + w2 * c.x + w3 * d.x, y: w0 * a.y + w1 * b.y + w2 * c.y + w3 * d.y };
}

/**
 * A `<Contour>`/`<InputContour>`: its points and its segment letters, one a
 * segment. L is a line from its one point; C a cubic Bezier from its start
 * through two controls to the next segment's start. A count that does not
 * add up falls back to the raw points.
 */
function flattenContour(contour) {
  const out = [];
  if (contour === null) return out;
  const pts = vertices(contour.child('points'));
  let segs = '';
  const s = contour.child('segments');
  if (s !== null) {
    for (const c of s.text) if (c !== ' ' && c !== '\t' && c !== '\r' && c !== '\n') segs += c;
  }
  const n = pts.length;
  if (segs === '' || n === 0) return pts;
  let idx = 0;
  for (const c of segs) {
    if (idx >= n) return pts;
    if (c === 'C' || c === 'c') {
      if (idx + 2 >= n) return pts;
      const a = pts[idx];
      const b = pts[idx + 1];
      const d = pts[idx + 2];
      const e = pts[(idx + 3) % n];
      out.push(a);
      for (let k = 1; k < kBezierSteps; ++k) out.push(bezier(a, b, d, e, k / kBezierSteps));
      idx += 3;
    } else {
      out.push(pts[idx]);
      idx += 1;
    }
  }
  return idx === n ? out : pts;
}

/** Drop a closing point that repeats the first, and consecutive repeats. */
function tidy(p) {
  const out = [];
  for (const q of p) {
    const last = out[out.length - 1];
    if (out.length === 0 || Math.abs(q.x - last.x) > 1e-9 || Math.abs(q.y - last.y) > 1e-9) out.push(q);
  }
  while (out.length > 1
    && Math.abs(out[0].x - out[out.length - 1].x) <= 1e-9 && Math.abs(out[0].y - out[out.length - 1].y) <= 1e-9) {
    out.pop();
  }
  return out;
}

function sameRegion(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; ++i) {
    if (Math.abs(a[i].x - b[i].x) > kSameRegion || Math.abs(a[i].y - b[i].y) > kSameRegion) return false;
  }
  return true;
}

function newHead() {
  return {
    name: '',
    outline: [],          // the slice's own shape, normalised, y down
    mask: [],             // empty for none
    maskShowsInside: true, // Arena's Invert 1: the slice is seen inside the mask
    centroid: { x: 0, y: 0 },
    minX: 0, minY: 0, maxX: 0, maxY: 0,
    screen: 0,
    listed: 0,
    polygon: false,
  };
}

function finishHead(h) {
  h.minX = h.minY = 1e300;
  h.maxX = h.maxY = -1e300;
  for (const p of h.outline) {
    h.minX = Math.min(h.minX, p.x); h.maxX = Math.max(h.maxX, p.x);
    h.minY = Math.min(h.minY, p.y); h.maxY = Math.max(h.maxY, p.y);
  }
  // Area centroid; the vertex mean if it has no area.
  let a = 0.0, cx = 0.0, cy = 0.0;
  const n = h.outline.length;
  for (let i = 0; i < n; ++i) {
    const p = h.outline[i];
    const q = h.outline[(i + 1) % n];
    const c = p.x * q.y - q.x * p.y;
    a += c; cx += (p.x + q.x) * c; cy += (p.y + q.y) * c;
  }
  if (Math.abs(a) > 1e-15) {
    h.centroid = { x: cx / (3.0 * a), y: cy / (3.0 * a) };
  } else if (n > 0) {
    let sx = 0.0, sy = 0.0;
    for (const p of h.outline) { sx += p.x; sy += p.y; }
    h.centroid = { x: sx / n, y: sy / n };
  }
}

/** Signed area of a closed polygon (positive anticlockwise in a y-up frame). */
export function PolygonArea(p) {
  let a = 0.0;
  const n = p.length;
  for (let i = 0; i < n; ++i) {
    const u = p[i];
    const v = p[(i + 1) % n];
    a += u.x * v.y - v.x * u.y;
  }
  return 0.5 * a;
}

function emptyMap(source) {
  return {
    ok: false, source, error: '', compWidth: 0.0, compHeight: 0.0, heads: [],
    slicesSeen: 0, skippedDisabled: 0, collapsed: 0, skippedDegenerate: 0, masks: 0, dropped: 0, edges: 0,
  };
}

/** Parse one preset's text. `source` is only recorded. */
export function ParsePreset(xml, source) {
  const map = emptyMap(source);
  const root = new Element();
  const reader = new XmlReader(xml);
  if (!reader.Document(root)) {
    map.error = `not XML: ${reader.error}`;
    return map;
  }
  const setup = root.name === 'ScreenSetup' ? root : root.child('ScreenSetup');
  if (setup === null) {
    map.error = 'no <ScreenSetup>';
    return map;
  }
  const size = setup.child('CurrentCompositionTextureSize');
  map.compWidth = size !== null ? number(size.attr('width'), 0.0) : 0.0;
  map.compHeight = size !== null ? number(size.attr('height'), 0.0) : 0.0;
  if (!(map.compWidth > 0.0) || !(map.compHeight > 0.0)) {
    map.error = 'no <CurrentCompositionTextureSize>';
    return map;
  }
  const screens = setup.child('screens');
  if (screens === null) {
    map.error = 'no <screens>';
    return map;
  }

  const kept = [];
  let screenIndex = -1;
  for (const screen of screens.children) {
    if (screen.name !== 'Screen') continue;
    ++screenIndex;
    const screenOn = !isOff(paramValue(screen, 'Params', 'Enabled'));
    const layers = screen.child('layers');
    if (layers === null) continue;
    const these = [];
    for (const layer of layers.children) {
      const isSlice = layer.name === 'Slice';
      const isPolygon = layer.name === 'Polygon';
      if (!isSlice && !isPolygon) continue;
      ++map.slicesSeen;
      if (!screenOn || isOff(paramValue(layer, 'Common', 'Enabled'))) {
        ++map.skippedDisabled;
        continue;
      }
      const r = { head: newHead(), outline: [], mask: [] };
      const nm = paramValue(layer, 'Common', 'Name');
      r.head.name = nm !== null ? nm : layer.name;
      r.head.screen = screenIndex;
      r.head.polygon = isPolygon;
      if (isPolygon) r.outline = flattenContour(layer.child('InputContour'));
      if (r.outline.length < 3) r.outline = vertices(layer.child('InputRect'));
      r.outline = tidy(r.outline);
      const m = layer.child('SliceMask');
      if (m !== null) {
        let contour = null;
        const so = m.child('ShapeObject');
        if (so !== null) {
          const sh = so.child('Shape');
          if (sh !== null) contour = sh.child('Contour');
        }
        const mk = tidy(flattenContour(contour));
        if (!isOff(paramValue(m, 'Input Mask', 'Enabled')) && mk.length >= 3 && Math.abs(PolygonArea(mk)) > 1e-6) {
          r.mask = mk;
          r.head.maskShowsInside = !isOff(paramValue(m, 'Input Mask', 'Invert'));
        }
      }
      if (r.outline.length < 3 || !(Math.abs(PolygonArea(r.outline)) > 1e-6)) {
        ++map.skippedDegenerate;
        continue;
      }
      these.push(r);
    }
    // The file lists a screen's slices bottom up; Arena's panel shows them
    // top down. The chase walks the panel.
    these.reverse();
    for (const r of these) {
      let duplicate = false;
      for (const k of kept) {
        if (k.head.maskShowsInside === r.head.maskShowsInside && sameRegion(k.outline, r.outline) && sameRegion(k.mask, r.mask)) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) {
        ++map.collapsed;
        continue;
      }
      const edges = r.outline.length + r.mask.length;
      if (kept.length >= kMaxHeads || map.edges + edges > kMaxEdges) {
        ++map.dropped;
        continue;
      }
      map.edges += edges;
      if (r.mask.length > 0) ++map.masks;
      kept.push(r);
    }
  }

  for (const r of kept) {
    const h = r.head;
    for (const p of r.outline) h.outline.push({ x: p.x / map.compWidth, y: p.y / map.compHeight });
    for (const p of r.mask) h.mask.push({ x: p.x / map.compWidth, y: p.y / map.compHeight });
    finishHead(h);
    h.listed = map.heads.length;
    map.heads.push(h);
  }
  map.ok = map.heads.length > 0;
  if (!map.ok && map.error === '') map.error = 'no enabled slices';
  return map;
}

/** A rows x columns grid of rectangles, row-major from the top left. */
export function GridMap(rows, columns) {
  rows = clampTo(rows, 1, 32);
  columns = clampTo(columns, 1, 32);
  const map = emptyMap(`grid ${rows} x ${columns}`);
  map.compWidth = map.compHeight = 1.0;
  for (let r = 0; r < rows; ++r) {
    for (let c = 0; c < columns; ++c) {
      const h = newHead();
      const x0 = c / columns, x1 = (c + 1) / columns;
      const y0 = r / rows, y1 = (r + 1) / rows;
      h.name = `Cell ${r + 1},${c + 1}`;
      h.outline = [{ x: x0, y: y0 }, { x: x1, y: y0 }, { x: x1, y: y1 }, { x: x0, y: y1 }];
      finishHead(h);
      h.listed = map.heads.length;
      map.heads.push(h);
      map.edges += 4;
    }
  }
  map.slicesSeen = rows * columns;
  map.ok = true;
  return map;
}

/** The heads' indices in the chase's order. */
export function OrderHeads(map, order) {
  const idx = map.heads.map((_, i) => i);
  if (order === HeadOrder.AsListed) return idx;
  // Quantised keys, so a column of grid cells sorts as one column whatever
  // the last bit of each centroid says, and the order is a strict weak one.
  const key = (v) => lround(v * 1e6);
  const less = (a, b) => {
    const p = map.heads[a];
    const q = map.heads[b];
    const px = key(p.centroid.x), py = key(p.centroid.y);
    const qx = key(q.centroid.x), qy = key(q.centroid.y);
    if (order === HeadOrder.LeftToRight) return px !== qx ? px < qx : py < qy;
    return py !== qy ? py < qy : px < qx;
  };
  // Array.prototype.sort is stable, as std::stable_sort is.
  return idx.sort((a, b) => (less(a, b) ? -1 : less(b, a) ? 1 : 0));
}

/** Two maps with the same heads, point for point (names, outlines, masks). */
export function SameHeads(a, b) {
  if (a.heads.length !== b.heads.length) return false;
  const samePoints = (p, q) => p.length === q.length && p.every((v, i) => v.x === q[i].x && v.y === q[i].y);
  for (let h = 0; h < a.heads.length; ++h) {
    const x = a.heads[h];
    const y = b.heads[h];
    if (x.name !== y.name || x.maskShowsInside !== y.maskShowsInside || !samePoints(x.outline, y.outline) || !samePoints(x.mask, y.mask)) {
      return false;
    }
  }
  return true;
}

/** Xenon.cpp's describe(): what the plugin logs about a map. */
export function describe(m) {
  return `${m.source}: ${m.heads.length} heads (${m.slicesSeen} slices, ${m.skippedDisabled} disabled, ${m.collapsed} collapsed, `
    + `${m.masks} masked, ${m.dropped} dropped, ${m.edges} edges)`;
}

//===========================================================================
// Shaders.h's constants, and the CPU half of Xenon.cpp
//===========================================================================

/** The light pass's vertex: position (2), runs (4), extra (2). */
export const kVertexFloats = 8;
/** Edge texture width. */
export const kEdgeRow = 1024;
export const kMaskNone = 0;
export const kMaskInside = 1;
export const kMaskOutside = 2;

/** Seconds of host time a single frame may advance by. */
const kMaxFrameDelta = 0.25;
/** Below this a head's light is not drawn. */
const kDark = f32(1e-6);

function toLinear(code) {
  code = clampTo(code, 0.0, 1.0);
  return code <= 0.04045 ? code / 12.92 : Math.pow((code + 0.055) / 1.055, 2.4);
}

/**
 * XenonPlugin, less its GL: the parameters as the host sets them, the beat as
 * SetBeatInfo says, and ProcessOpenGL's CPU half, which hands back everything
 * the two passes need. `source` is the map source: in the plugin a MapSource
 * reading files; on the page plugin.js's stand-in, with the same Poll, Map and
 * FromPreset.
 */
export class Xenon {
  constructor(source) {
    this.source = source;
    this.params = new Float32Array(PT.ABOUT_FIRST);
    for (let id = 0; id < PT.ABOUT_FIRST; ++id) this.params[id] = InfoOf(id).defaultValue;
    this.presetFile = '';

    this.rig = new Rig();
    this.pixels = [];
    this.exposure = new Float32Array(0);
    this.light = new Float32Array(0);
    this.order = [];
    this.vertices = new Float32Array(0);
    this.geometryDirty = true;
    this.geometryWidth = 0;
    this.geometryHeight = 0;
    this.band = 0.0;
    this.mapChanges = 0;
    this.edgeTexels = new Float32Array(0);
    this.edgeRows = 1;

    // Time: the host's clock in seconds, declared rather than voted on.
    this.now = 0.0;
    this.lastNow = -1.0;
    this.frameCount = 0;

    // The beat, as SetBeatInfo last said.
    this.bpm = f32(120.0);
    this.barPhase = f32(0.0);
    this.lastBarPhase = f32(-1.0);
    this.hostBeatSeen = false;

    this.firePresses = 0;
    this.fireHeld = false;
  }

  SetFloatParameter(index, value) {
    if (index < 0 || index >= PT.ABOUT_FIRST) return false;
    value = f32(value);
    // Fire acts on the press, once: an event arrives as 1 on press and 0 on
    // release, and a host is free to restate either.
    if (index === PT.FIRE) {
      const down = value >= 0.5;
      if (down && !this.fireHeld) ++this.firePresses;
      this.fireHeld = down;
    }
    this.params[index] = value;
    return true;
  }

  SetTextParameter(index, value) {
    if (index === PT.PRESET_FILE) this.presetFile = value ?? '';
  }

  SetBeatInfo(newBpm, newBarPhase) {
    newBpm = f32(newBpm);
    newBarPhase = f32(newBarPhase);
    if (newBpm > 0.0 && Number.isFinite(newBpm)) this.bpm = newBpm;
    if (Number.isFinite(newBarPhase)) this.barPhase = f32(newBarPhase - Math.floor(newBarPhase));
  }

  /** The map's 0..1, y-down regions into output pixels, y up, and every edge into the edge texture. */
  rebuildGeometry(width, height) {
    const map = this.source.Map();
    this.pixels = [];
    const texels = [];
    let edgeCount = 0;
    const toPixels = (input) => input.map((p) => ({ x: p.x * width, y: (1.0 - p.y) * height }));
    const addRun = (p) => {
      const start = edgeCount;
      const count = p.length;
      for (let i = 0; i < p.length; ++i) {
        const a = p[i];
        const b = p[(i + 1) % p.length];
        texels.push(a.x, a.y, b.x, b.y);
      }
      edgeCount += count;
      return [start, count];
    };
    for (const head of map.heads) {
      const px = { outline: toPixels(head.outline), mask: toPixels(head.mask) };
      px.maskMode = head.mask.length === 0 ? kMaskNone : head.maskShowsInside ? kMaskInside : kMaskOutside;
      let x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
      for (const p of px.outline) {
        x0 = Math.min(x0, p.x); x1 = Math.max(x1, p.x); y0 = Math.min(y0, p.y); y1 = Math.max(y1, p.y);
      }
      // A pixel's centre is what is tested, so a pixel of padding is all
      // the quad needs; clipped to the frame.
      px.x0 = clampTo(Math.floor(x0) - 1.0, 0.0, width);
      px.x1 = clampTo(Math.ceil(x1) + 1.0, 0.0, width);
      px.y0 = clampTo(Math.floor(y0) - 1.0, 0.0, height);
      px.y1 = clampTo(Math.ceil(y1) + 1.0, 0.0, height);
      [px.outlineStart, px.outlineCount] = addRun(px.outline);
      [px.maskStart, px.maskCount] = addRun(px.mask);
      this.pixels.push(px);
    }
    const rows = Math.max(1, Math.floor((edgeCount + kEdgeRow - 1) / kEdgeRow));
    this.edgeTexels = new Float32Array(rows * kEdgeRow * 4);
    this.edgeTexels.set(texels);
    this.edgeRows = rows;

    this.geometryWidth = width;
    this.geometryHeight = height;
    this.geometryDirty = false;
  }

  /**
   * ProcessOpenGL's CPU half for one frame at host time `time` (seconds) and
   * a `width` x `height` raster. Returns what the passes need; `rebuilt` says
   * the edge texture must be uploaded again.
   */
  ProcessFrame(time, width, height) {
    // Time, (no audio) and the beat.
    this.now = time;
    const dt = this.lastNow >= 0.0 ? clampTo(this.now - this.lastNow, 0.0, kMaxFrameDelta) : 0.0;
    this.lastNow = this.now;
    ++this.frameCount;

    if (this.barPhase !== this.lastBarPhase && this.lastBarPhase >= 0.0) this.hostBeatSeen = true;
    this.lastBarPhase = this.barPhase;

    // The slices: the active map, re-read when Arena saves it.
    const p = this.params;
    const settings = {
      useMap: OptionIndex(p[PT.SLICES], Slices.Count) === Slices.Map,
      presetFile: this.presetFile,
      rows: IntegerFromParam(p[PT.ROWS], kMinGrid, kMaxGrid, 2),
      columns: IntegerFromParam(p[PT.COLUMNS], kMinGrid, kMaxGrid, 4),
    };
    let mapChanged = false;
    if (this.source.Poll(this.now, settings)) {
      this.rig.Reset(this.source.Map().heads.length);
      this.geometryDirty = true;
      ++this.mapChanges;
      mapChanged = true;
    }
    const map = this.source.Map();
    this.order = OrderHeads(map, OptionIndex(p[PT.ORDER], 3));

    // The rig: the chase's steps this frame, the banks, and what this frame
    // catches of every head's light.
    const input = {
      trigger: OptionIndex(p[PT.TRIGGER], Trigger.Count),
      pattern: OptionIndex(p[PT.PATTERN], Pattern.Count),
      presses: this.firePresses,
      onset: false, // analyser.Fired(): never, with no spectrum
      barPhase: this.barPhase,
      bpm: this.bpm > 1.0 ? this.bpm : 120.0,
      hostBeat: this.hostBeatSeen,
      recharge: RechargeFromParam(p[PT.RECHARGE]),
      decay: DecayFromParam(p[PT.DECAY]),
      seed: IntegerFromParam(p[PT.SEED], 0, kMaxSeed, 1) >>> 0,
    };
    this.firePresses = 0;
    this.rig.Advance(this.now, dt, input, this.order);
    this.exposure = this.rig.Exposure(this.now, dt, input.decay);
    const intensity = f32(IntensityFromParam(p[PT.INTENSITY]));
    this.light = new Float32Array(this.exposure.length);
    for (let h = 0; h < this.exposure.length; ++h) this.light[h] = this.exposure[h] * intensity;

    // Geometry, only when the map or the raster changes.
    if (width !== this.geometryWidth || height !== this.geometryHeight) this.geometryDirty = true;
    const rebuilt = this.geometryDirty;
    if (this.geometryDirty) this.rebuildGeometry(width, height);

    // Outline's Width is in composition pixels; a grid has no composition,
    // so its pixels are the output's.
    const scale = this.source.FromPreset() && map.compHeight > 0.0 ? height / map.compHeight : 1.0;
    this.band = WidthFromParam(p[PT.WIDTH]) * scale;
    const look = OptionIndex(p[PT.LOOK], Look.Count);

    // 1. The light: every lit head's quad.
    const v = [];
    for (let h = 0; h < this.pixels.length && h < this.light.length; ++h) {
      if (!(this.light[h] > kDark)) continue;
      const px = this.pixels[h];
      if (!(px.x1 > px.x0) || !(px.y1 > px.y0)) continue;
      const runs = [px.outlineStart, px.outlineCount, px.maskStart, px.maskCount, px.maskMode, this.light[h]];
      const corners = [[px.x0, px.y0], [px.x1, px.y0], [px.x1, px.y1], [px.x0, px.y0], [px.x1, px.y1], [px.x0, px.y1]];
      for (const c of corners) v.push(c[0], c[1], ...runs);
    }
    this.vertices = new Float32Array(v);

    return {
      mapChanged,
      rebuilt,
      edgeTexels: this.edgeTexels,
      edgeRows: this.edgeRows,
      vertices: this.vertices,
      outline: look === Look.Outline ? 1 : 0,
      band: f32(this.band),
      size: [f32(width), f32(height)],
      colour: [f32(toLinear(p[PT.COLOUR_R])), f32(toLinear(p[PT.COLOUR_G])), f32(toLinear(p[PT.COLOUR_B]))],
      room: f32(RoomFromParam(p[PT.ROOM])),
      look,
      steps: this.rig.Steps(),
      flashes: this.rig.LastFlashes(),
    };
  }
}
