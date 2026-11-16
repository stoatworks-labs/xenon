/**
 * Xenon — browser demo.
 *
 * Every slice in the operator's Advanced Output is a xenon strobe head, and a
 * lighting desk's chase fires them. Each head is a capacitor bank charged
 * through a resistor and dumped through its tube when its step fires, so a
 * flash carries (1 − e^(−Δt/RC))² of a full bank's energy, Δt since that head
 * last fired, and its light dies away with the tube's own time constant. The
 * camera sees all the light that lands inside each frame.
 *
 * Three files, and they are not equally faithful:
 *
 *   **render.js runs the plugin's own GLSL**, unedited: the light pass (an
 *   even-odd crossing count per pixel over each lit head's edges, added into
 *   an R32F buffer) and the composite. shaders.js is spliced from
 *   source/Shaders.cpp by demo/tools/check_shaders.py --write, and the same
 *   script fails tools/verify.sh if a character or the way the pieces are
 *   joined drifts.
 *
 *   **port.js is a hand port of the CPU half, and only a reader checks it**:
 *   Controls.cpp, Rig.cpp, SliceMap.cpp (the reader, not the file side) and
 *   the CPU half of Xenon.cpp's ProcessOpenGL.
 *
 *   **This file is the page**: the parameter panel, the start gate, the host's
 *   stand-ins (Arena's saved Advanced Output and its tempo), the Preset File
 *   picker, the line under the picture and the page's presets. None of it is
 *   the plugin.
 *
 * There is no Arena here. The slices come from the repository's own fixture
 * presets (the files the plugin's harness reads to the corners Arena wrote),
 * a preset the visitor picks with Preset File, or the grid.
 *
 * Photosensitivity: nothing is mounted, so nothing draws, until the visitor
 * has pressed Start under the warning at the top of the page (index.html).
 */

import { mountDemo } from './vendor/demo.js';
import { SOURCES } from './vendor/sources.js';
import { createPasses } from './render.js';
import {
  f32, PT, Kind, InfoOf, Pattern, Trigger, Look, Slices, HeadOrder,
  ParsePreset, GridMap, SameHeads, describe, Xenon,
  RechargeFromParam, ParamFromRecharge, DecayFromParam, ParamFromDecay,
  IntensityFromParam, WidthFromParam, ParamFromWidth, RoomFromParam, BeatsPerStep, OptionIndex,
} from './port.js';

//===========================================================================
// The clip the plugin's harness uses: tools/xntest/main.cpp's lookClip, as a
// GLSL clip, so the defaults look here as they do in the README's picture.
// A slow diagonal sweep of colour bars over a dark ground, so a slice shows
// the clip under it and the strobe on top. Rows bottom first, as there.
//===========================================================================
const LOOK = `
uniform float Time;
uniform vec2 Resolution;
in vec2 uv;
out vec4 fragColor;

void main()
{
	vec2 px     = floor( gl_FragCoord.xy );
	float u     = ( px.x + 0.5 ) / Resolution.x;
	float v     = ( px.y + 0.5 ) / Resolution.y;
	float band  = mod( 3.0 * ( u + 0.6 * v ) - 0.25 * Time + 100.0, 1.0 );
	int bar     = int( band * 6.0 );
	vec3 c      = vec3( 0.20, 0.20, 0.22 );
	if( bar == 0 ) c = vec3( 0.55, 0.12, 0.10 );
	else if( bar == 1 ) c = vec3( 0.50, 0.38, 0.08 );
	else if( bar == 2 ) c = vec3( 0.12, 0.42, 0.18 );
	else if( bar == 3 ) c = vec3( 0.08, 0.30, 0.48 );
	else if( bar == 4 ) c = vec3( 0.30, 0.14, 0.46 );
	fragColor   = vec4( c * ( 0.55 + 0.45 * v ), 1.0 );
}
`;

if (!SOURCES.some((s) => s.id === 'look')) {
  SOURCES.unshift({
    id: 'look',
    name: 'Dark stripes (the harness’s clip)',
    hint: 'The plugin harness’s own clip (lookClip in xntest), drawn in the page: slow diagonal bars of colour over a dark ground, so a slice shows the clip under it and the strobe on top.',
    fragment: LOOK,
  });
}

//===========================================================================
// The page's disk, in place of the plugin's Documents folder.
//
// The plugin follows Preferences/AdvancedOutput.xml to the preset Arena has
// active and stats both files twice a second (MapSource). A page has no
// Documents, so "Arena's saved Advanced Output" is a choice in the host row
// among the repository's fixture presets, and Preset File reads a file the
// visitor picks. The decisions are MapSource::Poll's: Grid, the override, the
// active preset, the fall back to the grid with the reason, and a re-read
// that finds the same regions is not a new map. What differs: there is no
// pointer to follow, and a pick is read at once rather than at the next
// half-second poll.
//===========================================================================
const FIXTURES = [
  { file: 'unmaptest-7.28.1-polygon-masks.xml', name: 'unmaptest (Arena 7.28.1): a frame, a tilted slice, a triangle, two masked slices' },
  { file: 'unmaptest.xml', name: 'unmaptest (Arena 7.27.1): four slices, one tilted' },
  { file: 'output_map_1.xml', name: 'output_map_1: three slices on a 2944 × 1280 composition' },
  { file: 'LPP slicer proof A.xml', name: 'LPP slicer proof A: three slices across two screens, 4920 × 1920' },
  { file: 'LPP slicer proof B.xml', name: 'LPP slicer proof B: the same three, cropped, 1920 × 1080' },
];
const NO_MAP = '';

/** UTF-8, keeping a byte-order mark as a character: the plugin's reader sees it too, and refuses it. */
const decode = (bytes) => new TextDecoder('utf-8', { ignoreBOM: true }).decode(bytes);

const fixtureTexts = new Map();
const fixturesLoaded = Promise.all(FIXTURES.map(async ({ file }) => {
  const response = await fetch(new URL(`./presets/${encodeURIComponent(file)}`, import.meta.url));
  if (!response.ok) throw new Error(`presets/${file}: HTTP ${response.status}`);
  fixtureTexts.set(file, decode(await response.arrayBuffer()));
}));

class PageMapSource {
  constructor() {
    this.current = { ok: false, source: '', error: '', compWidth: 0, compHeight: 0, heads: [] };
    this.last = null;
    this.forced = true;
    this.rescan = false;
    this.fromPreset = false;
    this.lastPoll = -1e300;
    this.pointerStamp = null;
    this.presetStamp = null;
    this.reads = 0;

    /** Arena's saved Advanced Output, by fixture file name; '' is none saved. */
    this.active = FIXTURES[0].file;
    this.activeGeneration = 0;
    /** What Preset File can name: files the visitor picked, by name. */
    this.picked = new Map();
  }

  /** Arena saved another map (the host row): read at once. */
  setActive(file) {
    this.active = file;
    this.activeGeneration += 1;
    this.rescan = true;
  }

  /** The visitor picked a file for Preset File: read at once. */
  pick(name, text) {
    const generation = (this.picked.get(name)?.generation ?? 0) + 1;
    this.picked.set(name, { text, generation });
    this.rescan = true;
  }

  Map() { return this.current; }
  FromPreset() { return this.fromPreset; }
  Reads() { return this.reads; }

  /** MapSource::Poll, with the page's files in place of stat and fopen. */
  Poll(now, settings) {
    const last = this.last;
    const settingsChanged = this.forced || last === null || settings.useMap !== last.useMap
      || settings.presetFile !== last.presetFile || settings.rows !== last.rows || settings.columns !== last.columns;
    // A clock that ran backwards (a clip restarted) polls at once.
    if (!settingsChanged && !this.rescan && now >= this.lastPoll && now - this.lastPoll < 0.5) return false;
    this.rescan = false;
    this.lastPoll = now;

    if (!settings.useMap) {
      if (!settingsChanged) return false;
      this.current = GridMap(settings.rows, settings.columns);
      this.fromPreset = false;
      this.last = { ...settings };
      this.forced = false;
      this.pointerStamp = this.presetStamp = null;
      return true;
    }

    let pointerPath = '';
    let preset = '';
    let text = null;
    let generation = -1;
    if (settings.presetFile !== '') {
      preset = settings.presetFile;
      const picked = this.picked.get(preset);
      if (picked) ({ text, generation } = picked);
    } else if (this.active !== NO_MAP) {
      pointerPath = 'Preferences/AdvancedOutput.xml';
      preset = `Presets/Advanced Output/${this.active}`;
      text = fixtureTexts.get(this.active) ?? null;
      generation = text === null ? -1 : 0;
    }
    const ps = `${pointerPath}#${pointerPath === '' ? -1 : this.activeGeneration}`;
    const ss = `${preset}#${generation}`;
    if (!settingsChanged && ps === this.pointerStamp && ss === this.presetStamp) return false;
    this.pointerStamp = ps;
    this.presetStamp = ss;
    this.last = { ...settings };
    this.forced = false;

    let m;
    if (preset === '') {
      m = { ok: false, error: 'no AdvancedOutput.xml' };
    } else if (text === null) {
      m = { ok: false, error: `no file ${preset}` };
    } else {
      m = ParsePreset(text, preset);
      this.reads += 1;
    }
    if (m.ok) {
      // A save that leaves the regions as they were is not a new rig: the
      // chase and the banks carry on.
      const same = this.fromPreset && SameHeads(this.current, m);
      this.current = m;
      this.fromPreset = true;
      return !same;
    }
    const g = GridMap(settings.rows, settings.columns);
    g.error = m.error;
    g.source = `${g.source} (no map: ${m.error})`;
    this.current = g;
    this.fromPreset = false;
    return true;
  }
}

//===========================================================================
// The host's tempo. Resolume sends its BPM and bar phase every frame; here the
// page is the host, and its bar phase runs on the page's clock at the tempo
// in the host row, continuous when the tempo changes (as Resolume's is), in
// 4/4. Paused, it stands still.
//===========================================================================
const TEMPOS = [60, 90, 100, 120, 128, 140, 150, 174];
const host = { bpm: 120, phase: 0, lastTime: null };

//===========================================================================
// The renderer the kit drives.
//===========================================================================
const source = new PageMapSource();
let current = null; // { cpu, frame, passes }
let afterMapChange = null;

/** Seed, Rows and Columns are FF_TYPE_INTEGER: dropdowns here, whose slot is not always the value. */
const SEED_CHOICES = 100;
const valueOf = {
  seed: (slot) => slot,
  rows: (slot) => slot + 1,
  columns: (slot) => slot + 1,
};

function createRenderer(gl, quad) {
  const passes = createPasses(gl);
  passes.quad = quad;
  const cpu = new Xenon(source);
  current = { cpu, frame: null, passes };

  return {
    render({ input, params, width, height, time }) {
      // The host restates every parameter each frame.
      for (const p of PARAMS) {
        if (p.slot === PT.PRESET_FILE) {
          cpu.SetTextParameter(PT.PRESET_FILE, params.get(p.id));
          continue;
        }
        const raw = params.get(p.id);
        cpu.SetFloatParameter(p.slot, valueOf[p.id] ? valueOf[p.id](Math.round(raw)) : raw);
      }
      // Fire is a toggle here: the plugin counts its rising edge, and the page
      // lets go of it after this frame, as a host does with an event button.
      if (params.get('fire') > 0.5) queueMicrotask(() => params.set('fire', 0));

      if (host.lastTime !== null && time > host.lastTime) host.phase = (host.phase + (time - host.lastTime) * host.bpm / 240) % 1;
      host.lastTime = time;
      cpu.SetBeatInfo(host.bpm, host.phase);

      const frame = cpu.ProcessFrame(time, width, height);
      current.frame = frame;
      if (frame.mapChanged && afterMapChange) queueMicrotask(afterMapChange);
      passes.draw(frame, input, width, height, null);
    },
  };
}

//===========================================================================
// The parameters: the constructor's, in kTable's order and groups, with the
// plugin's names, element lists and defaults. Hints from the user guide.
//===========================================================================
const sig = (n) => (n >= 100 ? n.toFixed(0) : n >= 10 ? n.toFixed(1) : n >= 1 ? n.toFixed(2) : n.toFixed(3));
const seconds = (s) => (s >= 1 ? `${sig(s)} s` : `${sig(s * 1000)} ms`);

/** id, ParamId, display, hint. Audio (PT.AUDIO) is not here: see differences. */
const SPEC = [
  ['pattern', PT.PATTERN, null, 'Which heads step k lights, in their Order. Forward: one head a step, round and round. Reverse: the same backwards. Bounce: there and back, the end heads once a lap. Build: every head from the first up to the step’s, at once. Random: every head once a lap, in an order Seed fixes. All: every head on every step.'],
  ['trigger', PT.TRIGGER, null, 'When a step happens. Bar, Beat, Half, Quarter and Eighth Beat follow the host’s bar phase (here the page’s, at the tempo in the host row), each step at the moment inside the frame where its beat fell. Onset listens to the Audio input, which a browser does not have, so here Onset steps only on Fire. Fire Only: only Fire.'],
  ['fire', PT.FIRE, null, 'One step now, in any Trigger mode, on the press. FF_TYPE_EVENT in the plugin; a toggle the page lets go of after one frame here.'],
  ['threshold', PT.THRESHOLD, (v) => `${f32(v).toFixed(2)} · no audio`, 'How far a hit has to stand out from what came before it for Onset. It sets the audio analyser’s bar, and no audio reaches this page, so here it changes nothing.'],
  ['seed', PT.SEED, null, 'Random’s order: the same Seed gives the same sequence every time. FF_TYPE_INTEGER 0–9999 in the plugin; 0–99 here.'],

  ['recharge', PT.RECHARGE, (v) => seconds(RechargeFromParam(f32(v))), 'The bank’s time constant RC, 5 ms to 5 s, geometrically. After a flash the bank charges as 1 − e^(−t/RC) and the next flash carries the square of that: 40% of a full one after 1 RC, 75% after 2, 90% after 3. A long Recharge makes a fast chase fade.'],
  ['decay', PT.DECAY, (v) => seconds(DecayFromParam(f32(v))), 'The tube’s time constant, 0.2 ms to 500 ms, geometrically. Short puts each flash in one frame; long spreads it over several as a fading glow. It never changes how much light a flash gives in all.'],
  ['intensity', PT.INTENSITY, (v) => IntensityFromParam(f32(v)).toFixed(2), 'A full bank’s flash, 0 to 2. At 1 a full flash caught in one frame adds white at full strength.'],
  ['colourR', PT.COLOUR_R, null, 'The tube’s colour (sRGB, decoded to linear light). One colour picker, as Arena shows FF_TYPE_RED, GREEN and BLUE.'],
  ['colourG', PT.COLOUR_G, null, ''],
  ['colourB', PT.COLOUR_B, null, ''],

  ['look', PT.LOOK, null, 'Fill: each lit slice is a lamp in front of the clip. Outline: the same light only within Width of the slice’s edge, inside it. Gate: the clip lit by the strobe, the clip times (Room Light plus the flash); at Room Light 0 you see the clip only in the flashes.'],
  ['width', PT.WIDTH, (v) => `${sig(WidthFromParam(f32(v)))} px`, 'Outline’s band, 1 to 100 composition pixels (the units Arena shows a slice in), geometrically. On the grid there is no composition and Width is output pixels.'],
  ['room', PT.ROOM, (v) => `${(100 * RoomFromParam(f32(v))).toFixed(0)}%`, 'How much of the clip shows between flashes.'],

  ['slices', PT.SLICES, null, 'Map: the Advanced Output preset (Arena’s saved one, chosen in the host row under the picture, or Preset File). Grid: Rows × Columns. With no map to read the plugin falls back to the grid, and the line under the picture says why.'],
  ['presetFile', PT.PRESET_FILE, null, 'Another Advanced Output preset to read instead of Arena’s: an .xml from Presets/Advanced Output. FF_TYPE_FILE in the plugin (Arena’s file picker); here your own file, read in the page and never uploaded.'],
  ['order', PT.ORDER, null, 'The order the chase goes through the heads. As Listed: Arena’s slice list, screen by screen. Left to Right and Top to Bottom: by each slice’s centre.'],
  ['rows', PT.ROWS, null, 'The grid’s rows, 1 to 32, numbered row by row from the top left. FF_TYPE_INTEGER in the plugin; a dropdown of every value here.'],
  ['columns', PT.COLUMNS, null, 'The grid’s columns, 1 to 32. FF_TYPE_INTEGER in the plugin; a dropdown of every value here.'],
];

const numbers = (from, count) => Array.from({ length: count }, (_, i) => String(from + i));

const PARAMS = SPEC.map(([id, slot, display, hint]) => {
  const info = InfoOf(slot);
  if (info.id !== slot) throw new Error(`${id}: not a declared parameter`);
  const p = { id, slot, name: info.name, group: info.group, hint };
  switch (info.kind) {
    case Kind.Option: return { ...p, type: 'option', elements: info.options.slice(), default: info.defaultValue };
    case Kind.Event: return { ...p, type: 'boolean', default: 0 };
    case Kind.Red: case Kind.Green: case Kind.Blue: return { ...p, type: 'colour', default: info.defaultValue };
    case Kind.File: return { ...p, type: 'text', default: '', placeholder: 'none: Arena’s saved map' };
    case Kind.Integer:
      if (slot === PT.SEED) return { ...p, type: 'option', elements: numbers(0, SEED_CHOICES), default: info.defaultValue };
      return { ...p, type: 'option', elements: numbers(info.minimum, info.maximum - info.minimum + 1), default: info.defaultValue - info.minimum };
    default: return { ...p, type: 'standard', display, default: info.defaultValue };
  }
});
{
  const slots = PARAMS.map((p) => p.slot);
  const want = Array.from({ length: PT.ABOUT_FIRST }, (_, i) => i).filter((i) => i !== PT.AUDIO);
  if (slots.join() !== want.join()) throw new Error('the panel must carry every control but Audio, in the plugin’s order');
}

// Presets: whole states on top of the constructor's defaults. Mostly the user
// guide's recipes, chosen and set so that no head flashes more than twice a
// second at the default tempo on the page's maps: the guide's own "strobe
// that dies away" (Pattern All on Eighth Beat, sixteen full-frame flashes a
// second) is left out on purpose, Bounce runs at Half Beat rather than the
// guide's Quarter, and the fast chase runs on 32 small cells.
const amber = { colourR: 1.0, colourG: 0.62, colourB: 0.2 };
const PRESETS = {
  'The defaults: one slice a beat': {},
  'Screen flash on the beat (All, twice a second)': { pattern: Pattern.All },
  'Outlines chasing round the stage': { look: Look.Outline, width: ParamFromWidth(16), pattern: Pattern.Bounce, trigger: Trigger.HalfBeat },
  'The clip, strobed slice by slice (Gate)': { look: Look.Gate, room: 0.0, pattern: Pattern.Random, decay: ParamFromDecay(0.03) },
  'Bounce’s weak-strong, on a row of eight': { slices: Slices.Grid, rows: 0, columns: 7, pattern: Pattern.Bounce, trigger: Trigger.HalfBeat, recharge: ParamFromRecharge(0.3) },
  'Build, on a row of six': { slices: Slices.Grid, rows: 0, columns: 5, pattern: Pattern.Build },
  'A fast chase runs out of charge (4 × 8 grid)': { slices: Slices.Grid, rows: 3, columns: 7, trigger: Trigger.QuarterBeat, recharge: ParamFromRecharge(4.0) },
  'Amber tubes with a tail, left to right': { look: Look.Outline, order: HeadOrder.LeftToRight, decay: ParamFromDecay(0.06), room: 0.6, ...amber },
  'From a pad: Fire Only': { trigger: Trigger.FireOnly },
};

//===========================================================================
// What this page does not reproduce. The kit adds its four.
//===========================================================================
const DIFFERENCES = [
  'The CPU half is a PORT that only a reader checks: demo/port.js is a hand translation of Controls.cpp (every mapping, name, group and default), Rig.cpp (the chase’s steps on the bar phase, interpolated inside the frame; each head’s bank, v ← 1 − (1 − v)e^(−T/RC), and its flash E = q²; each flash’s light integrated over the frame’s own interval), SliceMap.cpp (the XML reader, slices, polygons, curves, masks, the collapse of identical regions, the limits, the grid and the head order) and the CPU half of Xenon.cpp (the frame, the light, the geometry and the quads). Nothing in the repository compares it with the C++. The plugin’s harness holds the C++ to the numbers in the README; it has never seen this page.',
  'The GPU half is not a port: the light and composite passes are the plugin’s own GLSL, joined as the plugin joins them, into the same R32F light buffer, blended, over the same RGBA32F edge texture. That needs EXT_color_buffer_float and EXT_float_blend, and without either the page stops and says so rather than drawing into something smaller. demo/tools/check_shaders.py fails the repository’s verify script if a character of the shaders, the way they are joined, or a preset the page offers drifts from the plugin’s.',
  'There is no Arena, so no Advanced Output. The plugin reads the preset Arena saved, following Preferences/AdvancedOutput.xml, and looks for a new Save & Close twice a second. Here “Arena’s saved Advanced Output”, in the host row under the picture, chooses among the repository’s own fixture presets (the files the plugin’s harness reads to the corners Arena wrote), or none, which sends the plugin to its grid as it does with no map. The pointer is not followed and nothing is polled: a choice is read at once.',
  'Preset File is a file picker: your own Advanced Output .xml from Documents/Resolume Arena/Presets/Advanced Output, read in this page by the ported reader and never uploaded. Copy link carries only its name, so a link opened elsewhere has no such file and the plugin falls back to the grid, as it does for a path that is not there. Pick a file again to re-read it, as a Save & Close would be; a re-read that finds the same regions does not restart the chase.',
  'In Arena the effect renders at the composition’s size, where every slice is where Arena draws it. Here the picture takes the map’s shape at the width you choose (a 2944 × 1280 map at 960 wide is 960 × 417); choose a 16:9 Composition to see it stretched, as on a layer of another shape. Outline’s Width is converted from composition pixels by the output’s height, as the plugin does.',
  'No audio. The plugin declares an Audio buffer (Resolume’s FFT) for Trigger Onset, with Threshold setting its analyser’s bar. A browser has no Resolume FFT, so Audio is not on the panel; with no spectrum the plugin’s analyser never fires, so Onset steps only on Fire, here as in a layer with no audio routed, and Threshold changes nothing.',
  'The tempo and bar phase are the page’s: Resolume sends its own every frame. The tempo is chosen in the host row (120 BPM by default, as the plugin assumes when a host sends none); the bar phase runs on the page’s clock, 4/4, continuous across a tempo change, and stands still when paused. The clock is seconds from frame deltas, capped at a tenth of a second a frame by the page (the plugin caps a frame at a quarter); the plugin’s vote on whether its host sends seconds or milliseconds is not ported. Restart is a clock jump backwards, which the plugin takes as no time passing.',
  'A flash with the default 2 ms Decay lands in one frame, and a frame here is one of the display’s refreshes (60 or 120 a second, and whenever the browser gets round to it), not Resolume’s output frame. Every frame’s light is still the integral of the flashes over that frame’s own interval, so a flash is never lost or doubled; only where frames fall differs.',
  'Seed is FF_TYPE_INTEGER 0–9999 in the plugin, a dropdown of 0–99 here; Rows and Columns are dropdowns of 1–32; Fire is FF_TYPE_EVENT, here a toggle the page lets go of after one frame; Colour is three FF_TYPE_RED/GREEN/BLUE controls, one swatch as in Arena. The About block is not on the panel.',
  'The warning at the top, the start button, the host row, the line under the picture and the Presets menu are the page’s, not the plugin’s. The line’s flash rate is the page’s arithmetic from Trigger, tempo, Pattern and the number of heads. The presets are the user guide’s recipes set so that no head flashes more than twice a second at 120 BPM on the page’s maps (Bounce at Half Beat where the guide says Quarter, the fast chase on 32 small cells); the guide’s “strobe that dies away” (sixteen flashes a second over the whole picture) is left out.',
  'The output is the page’s 8-bit canvas. The plugin clamps its output to 0..1 too, so nothing above white is lost here that it keeps. The plugin’s negative-control hooks (its harness’s deliberately wrong models) are 0 here, as in a shipped instance.',
  'Nothing here is measured. The plugin’s harness, xntest, reads four real Arena presets and a 7.28.1 save with a polygon and two masks to the corners Arena wrote (1e-12), holds a flash’s peak to (1 − e^(−Δt/RC))² within 3.5e-6, a flash’s light summed over every frame it lands in to its energy, every head lighting on the frame its beat falls in for every pattern, and every pixel of every head inside its slice exactly when an independent winding number says so, with 16 wrong models and 13 one-character mutants caught. That harness, not this page, is the reason to believe the strobe. What Arena itself has shown is in the README’s Status.',
];

//===========================================================================
// Start: nothing is mounted until the visitor has read the warning.
//===========================================================================
const startButton = document.getElementById('xn-start');
const startNote = document.getElementById('xn-start-note');
let mounted = null;

if (startButton) {
  startButton.hidden = false;
  startButton.addEventListener('click', async () => {
    startButton.disabled = true;
    try {
      await fixturesLoaded;
    } catch (error) {
      if (startNote) startNote.textContent = `The repository’s presets could not be loaded (${error.message}). The demo runs on the grid.`;
    }
    start();
    startButton.remove();
    document.getElementById('photosensitivity')?.classList.add('is-started');
    if (startNote && !startNote.textContent) startNote.textContent = 'Started. Pause, under the picture, stops it at once.';
  }, { once: true });
}

function start() {
  mounted = mountDemo({
    name: 'Xenon',
    pluginId: 'XN01',
    tagline:
      'Every slice in your Advanced Output is a xenon strobe head, and a lighting desk’s chase fires them. Each head is a capacitor bank charged through a resistor and dumped through its tube, so a head that fired a moment ago has less to give: a fast chase gets dimmer, a Bounce lights its inner heads weak and strong in turn, and nothing is animated.',
    repo: 'https://github.com/stoatworks-labs/xenon',

    blurb:
      'It is Xenon’s own GLSL, ported from the repository to WebGL2 unedited, fed by a JavaScript port of the plugin’s CPU half (the Advanced Output preset reader, the chase, each head’s capacitor bank and tube, and the exposure) that only a reader checks, running on the repository’s own presets and generated clips in this page — no install.',

    needFloat: true,
    showBackdrop: true,

    // The harness's clip first: the README's picture is on it.
    sources: ['look', 'scene', 'grid', 'bars', 'spot', 'alpha', 'ramp'],

    params: PARAMS,
    presets: PRESETS,
    differences: DIFFERENCES,
    createRenderer,
  });
  if (!mounted || !current) return;
  buildHostRow();
  buildPresetFileRow();
  buildStatusLine();
  afterMapChange = followMapShape;
  followMapShape();
}

//===========================================================================
// The host row: the stand-ins for what Resolume would hand the plugin.
//===========================================================================
function field(label, select, title) {
  const wrap = document.createElement('label');
  wrap.className = 'transport__field';
  if (title) wrap.title = title;
  const span = document.createElement('span');
  span.className = 'transport__label';
  span.textContent = label;
  wrap.append(span, select);
  return wrap;
}

function buildHostRow() {
  const transport = document.querySelector('.transport');
  if (!transport) return;
  const row = document.createElement('div');
  row.className = 'transport xn-host';
  const tag = document.createElement('span');
  tag.className = 'xn-host__tag';
  tag.textContent = 'The host';
  tag.title = 'Stand-ins for what Resolume would hand the plugin. None of these is a plugin control.';

  const map = document.createElement('select');
  map.className = 'transport__select xn-host__map';
  for (const f of FIXTURES) map.append(new Option(f.name, f.file));
  map.append(new Option('None saved (the plugin falls back to its grid)', NO_MAP));
  map.value = source.active;
  map.addEventListener('change', () => {
    source.setActive(map.value);
    mounted.redraw();
  });

  const tempo = document.createElement('select');
  tempo.className = 'transport__select';
  for (const bpm of TEMPOS) tempo.append(new Option(`${bpm} BPM`, String(bpm)));
  tempo.value = String(host.bpm);
  tempo.addEventListener('change', () => {
    host.bpm = Number(tempo.value);
    mounted.redraw();
  });

  row.append(
    tag,
    field('Arena’s saved Advanced Output', map, 'The preset Arena has active, which the plugin reads when Preset File is empty: one of the repository’s fixture presets. Slices must be on Map.'),
    field('Tempo', tempo, 'Resolume’s BPM. The bar phase runs at it, in 4/4, on the page’s clock.'),
  );
  transport.after(row);
}

//===========================================================================
// Preset File: the kit has no file control, so its text row becomes one.
//===========================================================================
function buildPresetFileRow() {
  const input = document.getElementById('p-presetFile');
  if (!input) return;
  const row = input.closest('.prow');
  input.hidden = true;

  const box = document.createElement('div');
  box.className = 'xn-file';
  const choose = document.createElement('label');
  choose.className = 'btn xn-file__choose';
  choose.textContent = 'Choose…';
  const picker = document.createElement('input');
  picker.type = 'file';
  picker.accept = '.xml,text/xml,application/xml';
  picker.hidden = true;
  choose.append(picker);
  const name = document.createElement('span');
  name.className = 'xn-file__name';
  const clear = document.createElement('button');
  clear.type = 'button';
  clear.className = 'btn xn-file__clear';
  clear.textContent = 'Clear';
  box.append(choose, name, clear);
  row.append(box);

  const show = () => {
    const value = mounted.params.get('presetFile');
    name.textContent = value || 'none: Arena’s saved map';
    name.title = value ? `Read in this page; never uploaded.${source.picked.has(value) ? '' : ' This page has no file by that name.'}` : '';
    clear.hidden = !value;
  };
  mounted.params.addEventListener('change', show);
  show();

  picker.addEventListener('change', async () => {
    const file = picker.files?.[0];
    picker.value = '';
    if (!file) return;
    source.pick(file.name, decode(await file.arrayBuffer()));
    mounted.params.set('presetFile', file.name);
    show();
    mounted.redraw();
  });
  clear.addEventListener('click', () => mounted.params.set('presetFile', ''));
}

//===========================================================================
// The picture takes the map's shape, as the composition would in Arena.
//===========================================================================
const KIT_SIZES = { 640: 360, 960: 540, 1280: 720, 1920: 1080 };

function followMapShape() {
  const state = mounted.state;
  if (state.sourceId === 'user') return; // the visitor's own picture sets the raster
  const select = [...document.querySelectorAll('.transport__select')].find((s) => [...s.options].some((o) => o.value === '960'));
  const map = source.Map();
  const width = state.resolution.width;
  const aspect = map.compWidth / map.compHeight;
  let option = select ? [...select.options].find((o) => o.value === 'map') : null;

  if (source.FromPreset() && Number.isFinite(aspect) && Math.abs(aspect / (16 / 9) - 1) > 0.005) {
    const height = Math.max(2, Math.round(width / aspect / 2) * 2);
    state.resolution = { id: 'map', name: `${width} × ${height}`, width, height };
    if (select) {
      if (!option) {
        option = new Option('', 'map');
        select.append(option);
        select.addEventListener('change', () => {
          if (select.value === 'map') state.resolution = { ...mapResolution };
        });
      }
      option.textContent = `${width} × ${height} (the map’s ${map.compWidth} × ${map.compHeight} shape)`;
      select.value = 'map';
    }
    mapResolution = state.resolution;
  } else if (state.resolution.id === 'map') {
    const w = KIT_SIZES[width] ? width : 960;
    state.resolution = { id: String(w), name: `${w} × ${KIT_SIZES[w]}`, width: w, height: KIT_SIZES[w] };
    if (select) select.value = String(w);
    option?.remove();
  }
  mounted.redraw();
}
let mapResolution = null;

//===========================================================================
// The line under the picture. It reports; it measures nothing.
//===========================================================================
function busiestShare(pattern, n) {
  if (n <= 0) return 0;
  switch (pattern) {
    case Pattern.Bounce: return n === 1 ? 1 : n === 2 ? 0.5 : 1 / (n - 1);
    case Pattern.Build:
    case Pattern.All: return 1;
    default: return 1 / n;
  }
}

function buildStatusLine() {
  const stage = document.querySelector('.stage');
  if (!stage) return;
  const line = document.createElement('p');
  line.className = 'stage__status xn-status';
  line.setAttribute('aria-live', 'off');
  const caution = document.createElement('p');
  caution.className = 'stage__status xn-caution';
  caution.hidden = true;
  stage.append(line, caution);

  setInterval(() => {
    if (!current.frame) return;
    const { cpu } = current;
    const map = source.Map();
    const p = mounted.params;
    const trigger = OptionIndex(p.get('trigger'), Trigger.Count);
    const pattern = OptionIndex(p.get('pattern'), Pattern.Count);
    const beats = BeatsPerStep(trigger);
    const stepsPerSecond = beats > 0 ? host.bpm / 60 / beats : 0;
    const busiest = stepsPerSecond * busiestShare(pattern, map.heads.length);
    const what = map.error ? `${map.source}` : describe(map);
    const chase = beats > 0
      ? `${stepsPerSecond.toFixed(stepsPerSecond < 1 ? 2 : 1)} steps a second at ${host.bpm} BPM; the busiest head flashes about ${busiest.toFixed(busiest < 1 ? 2 : 1)} times a second`
      : trigger === Trigger.Onset ? 'Onset: no audio here, so only Fire steps the chase' : 'Fire Only: press Fire to step';
    line.textContent = `${what} · ${chase} · step ${cpu.rig.Steps()}`;
    const over = busiest > 3 || (stepsPerSecond > 3 && pattern === Pattern.All);
    caution.hidden = !over;
    caution.textContent = over
      ? 'Over the three-flashes-a-second line that broadcast and web guidance draw for a large flash. Keep a chase this fast to small slices, or Pause.'
      : '';
  }, 250);
}

export { source, current };
