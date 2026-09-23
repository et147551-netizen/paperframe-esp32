// Runs assets/index.html's ink-preview quantiser outside the browser, writing one palette
// index per pixel so that tools/ink_preview_parity.py can compare it against the device's own
// row path in src/core/epd_dither.c.
//
//   node tools/ink_preview_quantise.js <config.json> <paletteId> <quality|none|diffuse> \
//        <raw.rgb> <width> <height> <out.idx>
//
// The functions are **extracted from the page**, not copied, so this cannot drift from what the
// browser runs. INK_CLAMP=1 restores the clampByte() the shipping preview applied to the biased
// value; it exists so the check can be shown to fail (see the parity script's --self-check).
const fs = require("fs");
const path = require("path");

const html = fs.readFileSync(path.join(__dirname, "..", "assets", "index.html"), "utf8");

function extract(name) {
  const start = html.indexOf("function " + name + "(");
  if (start < 0) throw new Error("not found: " + name);
  let depth = 0;
  for (let i = html.indexOf("{", start); i < html.length; i++) {
    if (html[i] === "{") depth++;
    else if (html[i] === "}" && --depth === 0) return html.slice(start, i + 1);
  }
  throw new Error("unterminated: " + name);
}

function constant(name) {
  const m = html.match(new RegExp("^const " + name + " = (.+);$", "m"));
  if (!m) throw new Error("not found: const " + name);
  return m[1];
}

function clampedQuantiser() {
  const s = extract("quantizeToEpdPalette");
  if (!process.env.INK_CLAMP) return s;
  const from = "const toned = applyInkTone(t, [r + bias + biasR, g + bias + biasG, b + bias + biasB]);";
  if (!s.includes(from)) throw new Error("INK_CLAMP anchor not found; update this script");
  return s.replace(from,
    "const cb = v => (v < 0 ? 0 : (v > 255 ? 255 : v));\n"
    + "      const toned = applyInkTone(t, [cb(r + bias + biasR), cb(g + bias + biasG), cb(b + bias + biasB)]);");
}

const [cfgPath, paletteId, mode, rawPath, wStr, hStr, outPath] = process.argv.slice(2);
const width = Number(wStr);
const height = Number(hStr);

const modeConfig = JSON.parse(fs.readFileSync(cfgPath, "utf8"));
modeConfig.palette = paletteId;

const api = eval([
  "const EPD_STEP_VALUE = " + constant("EPD_STEP_VALUE") + ";",
  "const EPD_STEP_DIFF = " + constant("EPD_STEP_DIFF") + ";",
  "const EPD_X_STEP = " + constant("EPD_X_STEP") + ";",
  "const EPD_Y_STEP = " + constant("EPD_Y_STEP") + ";",
  extract("currentInkTables"),
  extract("toneCompress"),
  extract("applyInkTone"),
  extract("inkNearestIndex"),
  extract("inkPairIndices"),
  clampedQuantiser(),
  extract("quantizeToEpdPaletteNearest"),
  extract("quantizeToEpdPaletteDiffused"),
  "({currentInkTables, quantizeToEpdPalette, quantizeToEpdPaletteNearest,"
    + " quantizeToEpdPaletteDiffused})",
].join("\n"));

const tables = api.currentInkTables();
if (!tables) throw new Error("currentInkTables() returned null for palette " + paletteId);

// getImageData() hands the quantiser a Uint8ClampedArray of RGBA.
const rgb = fs.readFileSync(rawPath);
if (rgb.length !== width * height * 3) throw new Error("raw size does not match width x height");
const data = new Uint8ClampedArray(width * height * 4);
for (let i = 0; i < width * height; i++) {
  data[i * 4] = rgb[i * 3];
  data[i * 4 + 1] = rgb[i * 3 + 1];
  data[i * 4 + 2] = rgb[i * 3 + 2];
  data[i * 4 + 3] = 255;
}
const imageData = { data, width, height };

if (mode === "quality") {
  api.quantizeToEpdPalette(imageData, tables);
} else if (mode === "none") {
  api.quantizeToEpdPaletteNearest(imageData, tables);
} else if (mode === "diffuse") {
  api.quantizeToEpdPaletteDiffused(imageData, tables);
} else {
  throw new Error("mode must be quality, none or diffuse");
}

// The six screen colours are distinct, so the map from drawn colour back to index is exact.
const byColour = new Map();
for (const [idx, c] of Object.entries(tables.screen)) {
  byColour.set(c.join(","), Number(idx));
}
const out = Buffer.alloc(width * height);
for (let i = 0; i < width * height; i++) {
  const key = data[i * 4] + "," + data[i * 4 + 1] + "," + data[i * 4 + 2];
  const idx = byColour.get(key);
  if (idx === undefined) throw new Error("pixel " + i + " is not a palette colour: " + key);
  out[i] = idx;
}
fs.writeFileSync(outPath, out);
console.log(`tone=${tables.tone} strength=${tables.strength}`);
