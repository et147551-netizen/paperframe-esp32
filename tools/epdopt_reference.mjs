// Generates the expected bytes for the epdoptimize port, by running epdoptimize.
//
//   node tools/epdopt_reference.mjs
//
// Writes test/test_epdopt/epdopt_fixtures.h, which test/test_epdopt/test_main.c compares
// src/core/epd_epdopt.c against byte for byte. This is what replaces judging a colour pipeline
// by eye on a 15.6 s refresh: the reference implementation runs here, on this machine, and
// disagreement is a failing assert naming the stage.
//
// Requires the library to be built once:
//
//   cd refs/epdoptimize && npm ci && npm run build
//
// Three things about how it drives the library, each of which would silently give the
// wrong answer if changed:
//
//   * `processingEngine: "js"`. The default "auto" takes a WebAssembly error-diffusion
//     path (dither.ts:777-780); the JS implementation is the one that was ported.
//   * The canvas is a hand-written stub. `CanvasLike` is three properties
//     (dither.ts:220-235) and the library fakes one itself at dither.ts:862-873, so no
//     node-canvas and no browser is needed -- and none is wanted, because a real canvas
//     would premultiply alpha.
//   * Stages are cumulative, matching how epd_epdopt_render() composes them. "tone" is
//     the balanced preset with range compression switched off; "range" is the preset as
//     it stands; "diffuse" runs ditherCanvas() over the "range" output, and ditherCanvas
//     deliberately does not re-run the adjustments.

import { execFileSync } from "node:child_process";
import { inflateSync } from "node:zlib";
import { readFileSync, writeFileSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

import {
  applyImageDataAdjustments,
  ditherCanvas,
} from "../refs/epdoptimize/dist/index.mjs";

const here = dirname(fileURLToPath(import.meta.url));
const repo = join(here, "..");

// The two palettes the parity test covers: the one epdoptimize's demo selects, and the
// library's current general-purpose Spectra 6 calibration. The C side maps these ids to
// EPD_PALETTE_EPDOPT_AITJCIZE and EPD_PALETTE_EPDOPT_SPECTRA6.
const PALETTES = [
  { id: 0, name: "aitjcize-spectra6" },
  { id: 1, name: "spectra6" },
];

const baseOptions = (paletteName) => ({
  processingPreset: "balanced",
  palette: paletteName,
  ditheringType: "errorDiffusion",
  errorDiffusionMatrix: "floydSteinberg",
  serpentine: false,
  colorMatching: "rgb",
  processingEngine: "js",
});

// ------------------------------------------------------------------------ PNG decode
//
// Enough of PNG to read the factory photographs: 8-bit truecolour, non-interlaced, which
// is what all four of .scratch/digital-frame/fixtures/ are. Anything else throws rather
// than guessing -- a fixture that decoded wrongly would make the C look wrong.

const decodePng = (path) => {
  const bytes = readFileSync(path);
  if (bytes.readUInt32BE(0) !== 0x89504e47) throw new Error(`${path}: not a PNG`);

  const width = bytes.readUInt32BE(16);
  const height = bytes.readUInt32BE(20);
  const depth = bytes[24];
  const colourType = bytes[25];
  const interlace = bytes[28];
  if (depth !== 8 || colourType !== 2 || interlace !== 0) {
    throw new Error(
      `${path}: only 8-bit truecolour non-interlaced is supported (depth=${depth} colour=${colourType} interlace=${interlace})`
    );
  }

  const chunks = [];
  let offset = 8;
  while (offset < bytes.length) {
    const length = bytes.readUInt32BE(offset);
    const type = bytes.toString("ascii", offset + 4, offset + 8);
    if (type === "IDAT") chunks.push(bytes.subarray(offset + 8, offset + 8 + length));
    if (type === "IEND") break;
    offset += length + 12;
  }

  const raw = inflateSync(Buffer.concat(chunks));
  const stride = width * 3;
  const out = Buffer.alloc(stride * height);

  // Per-row filters, PNG spec 9.2. `a` is the pixel to the left, `b` above, `c` above-left.
  for (let y = 0; y < height; y += 1) {
    const filter = raw[y * (stride + 1)];
    const src = raw.subarray(y * (stride + 1) + 1, y * (stride + 1) + 1 + stride);
    const row = out.subarray(y * stride, (y + 1) * stride);
    const prev = y > 0 ? out.subarray((y - 1) * stride, y * stride) : null;

    for (let i = 0; i < stride; i += 1) {
      const a = i >= 3 ? row[i - 3] : 0;
      const b = prev ? prev[i] : 0;
      const c = prev && i >= 3 ? prev[i - 3] : 0;
      let value = src[i];

      if (filter === 0) {
        /* none */
      } else if (filter === 1) {
        value += a;
      } else if (filter === 2) {
        value += b;
      } else if (filter === 3) {
        value += (a + b) >> 1;
      } else if (filter === 4) {
        const p = a + b - c;
        const pa = Math.abs(p - a);
        const pb = Math.abs(p - b);
        const pc = Math.abs(p - c);
        value += pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
      } else {
        throw new Error(`${path}: unknown row filter ${filter}`);
      }

      row[i] = value & 0xff;
    }
  }

  return { width, height, rgb: out };
};

// Nearest-neighbour, the same rule as epd_canvas_blit(): index from the destination
// extent rather than from a scale factor, so the last row cannot round off the end.
const downscale = (image, width, height) => {
  const rgb = Buffer.alloc(width * height * 3);
  for (let y = 0; y < height; y += 1) {
    const sy = Math.floor((y * image.height) / height);
    for (let x = 0; x < width; x += 1) {
      const sx = Math.floor((x * image.width) / width);
      const from = (sy * image.width + sx) * 3;
      const to = (y * width + x) * 3;
      rgb[to] = image.rgb[from];
      rgb[to + 1] = image.rgb[from + 1];
      rgb[to + 2] = image.rgb[from + 2];
    }
  }
  return { width, height, rgb };
};

// ----------------------------------------------------------------------- test images

const hsvToRgb = (h, s, v) => {
  const c = v * s;
  const x = c * (1 - Math.abs(((h / 60) % 2) - 1));
  const m = v - c;
  const sector = Math.floor(h / 60) % 6;
  const table = [
    [c, x, 0],
    [x, c, 0],
    [0, c, x],
    [0, x, c],
    [x, 0, c],
    [c, 0, x],
  ][sector];
  return table.map((channel) => Math.round((channel + m) * 255));
};

const build = (width, height, fn) => {
  const rgb = Buffer.alloc(width * height * 3);
  for (let y = 0; y < height; y += 1) {
    for (let x = 0; x < width; x += 1) {
      const [r, g, b] = fn(x, y);
      const at = (y * width + x) * 3;
      rgb[at] = r;
      rgb[at + 1] = g;
      rgb[at + 2] = b;
    }
  }
  return { width, height, rgb };
};

const cases = () => {
  const list = [];

  // Every byte value once, so the tone LUTs and the L*a*b* round trip are covered
  // exhaustively rather than sampled.
  list.push({ name: "ramp16", image: build(16, 16, (x, y) => {
    const v = y * 16 + x;
    return [v, v, v];
  })});

  // Fully saturated blocks: this is what drives the chroma protection and sends the guard
  // into its bisection, which is the most intricate arithmetic in the port.
  const primaries = [
    [255, 0, 0], [0, 255, 0], [0, 0, 255],
    [255, 255, 0], [0, 255, 255], [255, 0, 255],
  ];
  list.push({ name: "primaries", image: build(12, 8, (x) => primaries[Math.floor(x / 2)]) });

  // A hue sweep at full saturation and again at half value, for colours that sit between
  // palette entries and have to dither rather than land on one.
  list.push({ name: "hue-sweep", image: build(32, 8, (x, y) =>
    hsvToRgb((x * 360) / 32, 1, y < 4 ? 1 : 0.5)) });

  // Odd width: the packed output pads a final low nibble with white, and 5 is the
  // smallest width that makes that happen twice over three rows.
  list.push({ name: "odd-width", image: build(5, 3, (x, y) => [x * 50, y * 80, 255 - x * 40]) });

  // Flat mid grey. Must come out as a stable texture; a bug in the error feedback shows
  // here as a gradient or as a solid colour.
  list.push({ name: "flat-grey", image: build(8, 8, () => [128, 128, 128]) });

  // Deterministic noise, for error propagation over an area with no structure to hide a
  // mistake behind. A 32-bit LCG so the fixture is reproducible on any machine.
  let seed = 12345;
  const lcg = () => {
    seed = (seed * 1103515245 + 12345) & 0x7fffffff;
    return (seed >> 16) & 0xff;
  };
  list.push({ name: "noise", image: build(24, 24, () => [lcg(), lcg(), lcg()]) });

  // A real photograph, downscaled: the same arithmetic, but over an area big enough for
  // the diffusion to accumulate the way it does on the panel.
  const photo = decodePng(join(repo, ".scratch/digital-frame/fixtures/imaged001.png"));
  list.push({ name: "photo32x48", image: downscale(photo, 32, 48) });

  return list;
};

// ------------------------------------------------------- images for the classifier port
//
// `node tools/epdopt_reference.mjs --classify` writes test/test_classify/classify_fixtures.h
// instead, holding what classifyImageStyle() returns for each image below.
//
// **These images carry no pixels into the header.** Every one is defined by an integer-only
// rule that the C test reproduces, and each fixture carries an FNV-1a checksum of the bytes so
// that a divergence between the two definitions fails as "checksum" rather than as a metric
// that looks like a bug in the port. The reason not to emit the pixels is size: the classifier
// only says something interesting on images big enough to be downsampled onto its 160-sample
// grid, and a 200 x 300 input is 700 KB of C source per image.

const CLASSIFY_IMAGES = [
  // Deterministic noise, large enough that the sample grid downsamples (200 x 300 -> 107 x 160,
  // the same reduction the panel's own canvas gets). No structure at all: maximum unique
  // colours, no flat regions.
  {
    name: "noise-200x300",
    width: 200,
    height: 300,
    // **xorshift32, not the LCG the parity fixtures use.** That one multiplies a 31-bit seed by
    // 1103515245, which exceeds 2^53, so JavaScript rounds it and the sequence is a property of
    // double arithmetic rather than of the algorithm -- fine where the pixels are emitted into
    // the header, impossible to reproduce in C where they are not. xorshift32 is shifts and
    // xors on a uint32 and is therefore exactly the same sequence in both languages.
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      let state = 0x12345678;
      for (let i = 0; i < rgb.length; i += 1) {
        state = (state ^ (state << 13)) >>> 0;
        state = (state ^ (state >>> 17)) >>> 0;
        state = (state ^ (state << 5)) >>> 0;
        rgb[i] = state & 0xff;
      }
      return rgb;
    },
  },

  // Smooth in both axes: soft neighbour changes everywhere, few strong edges, which is what
  // gradientTileRatio and softChangeRatio are for.
  {
    name: "gradient-200x300",
    width: 200,
    height: 300,
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
          const at = (y * width + x) * 3;
          rgb[at] = Math.floor((x * 255) / (width - 1));
          rgb[at + 1] = Math.floor((y * 255) / (height - 1));
          rgb[at + 2] = Math.floor(((x + y) * 255) / (width + height - 2));
        }
      }
      return rgb;
    },
  },

  // Six saturated flat bands: flatRatio and topColorCoverage both high, highSaturationRatio
  // high. This is what `flatIllustration` and `pixelArt` score on.
  {
    name: "flat-blocks-160x120",
    width: 160,
    height: 120,
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      const bands = [
        [255, 0, 0], [0, 255, 0], [0, 0, 255],
        [255, 255, 0], [0, 255, 255], [255, 0, 255],
      ];
      for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
          const band = bands[Math.min(5, Math.floor((x * 6) / width))];
          const at = (y * width + x) * 3;
          rgb[at] = band[0];
          rgb[at + 1] = band[1];
          rgb[at + 2] = band[2];
        }
      }
      return rgb;
    },
  },

  // Grey strokes in bands, which is what a screenshot of text looks like to these metrics:
  // high edge density, grayRatio ~1, flat between the strokes.
  {
    name: "text-like-160x120",
    width: 160,
    height: 120,
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      rgb.fill(255);
      for (let y = 0; y < height; y += 1) {
        if (y % 20 >= 10) continue;
        for (let x = 0; x < width; x += 1) {
          if (x % 6 >= 2) continue;
          const at = (y * width + x) * 3;
          rgb[at] = 16;
          rgb[at + 1] = 16;
          rgb[at + 2] = 16;
        }
      }
      return rgb;
    },
  },

  // Thin diagonals on white: edges without area, which separates `lineArt` from `textOrUi`.
  {
    name: "line-art-160x120",
    width: 160,
    height: 120,
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      rgb.fill(255);
      for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
          if ((x + y) % 17 !== 0) continue;
          const at = (y * width + x) * 3;
          rgb[at] = 0;
          rgb[at + 1] = 0;
          rgb[at + 2] = 0;
        }
      }
      return rgb;
    },
  },

  // Smaller than one tile. Every tile is dropped for holding too few samples
  // (image-style.ts:631), so all four tile ratios come out 0 -- the degenerate path, which is
  // reachable on the device by an image that fits the panel in one dimension only.
  {
    name: "tiny-5x3",
    width: 5,
    height: 3,
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
          const at = (y * width + x) * 3;
          rgb[at] = x * 50;
          rgb[at + 1] = y * 80;
          rgb[at + 2] = 255 - x * 40;
        }
      }
      return rgb;
    },
  },
];

// -------------------------------------------- extra images, for the suggestion port only
//
// `--auto` writes test/test_auto/auto_fixtures.h, which pins src/core/epd_auto.c against
// buildLayeredSuggestion(). **That fixture carries no pixels and needs none**: the C test feeds
// the port the *metrics* out of the header rather than classifying anything, which is what makes
// a failure localise to the suggestion rather than to epd_classify.c's tolerances. So unlike
// CLASSIFY_IMAGES above, these do not have to be reproducible in C and are not checksummed --
// they only have to reach parts of auto-processing.ts the other seven do not.
//
// Each one names the branch it is here for. `--auto` prints a coverage table and refuses to
// write the header if any of the nine required branches is unreached, because threshold code
// over inputs that all land on one side is a suite that cannot fail.

const AUTO_EXTRA_IMAGES = [
  // A faded neutral scan: soft 2D structure inside a narrow band. Targets
  // isRestorableLowContrastSource() from a kind that is *not* lowContrastPhoto -- lumaRange
  // <= 96, lumaStdDev <= 32, neutral, and enough soft transition to satisfy the "recoverable
  // structure" clause. That is the arm where applyLowContrastRestoreTuning overrides a preset
  // the kind had already chosen, and the early return at the head of
  // applyLayeredAutoAdjustments is what lets it.
  //
  // **Shallow is not the same as soft, and two attempts got that wrong before the metric said
  // so.** getNeighborMetrics (image-style.ts:370-398) bins each adjacent pair by colour
  // difference: <= 4 is flat, <= 28 is a soft change, more is a strong edge. A 100 -> 160 ramp
  // across 160 pixels moves by 0 or 1 per step and a low-frequency wave of the same amplitude
  // does no better, so both came out flatRatio 1.0 and softChangeRatio 0.0 -- flatIllustration
  // with the range left off, not restorable at all. The wave below has a period of about seven
  // pixels, so neighbours differ by roughly 15: soft by that definition, while the whole image
  // still spans under 50 luma levels.
  {
    name: "faded-grey-160x120",
    width: 160,
    height: 120,
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
          const wave =
            18 * Math.sin(x * 0.9) * Math.cos(y * 0.7) + 6 * Math.sin((x + y) * 0.5);
          const grey = 128 + Math.round(wave);
          const at = (y * width + x) * 3;
          rgb[at] = grey;
          rgb[at + 1] = grey;
          rgb[at + 2] = grey;
        }
      }
      return rgb;
    },
  },

  // Warm paper with black strokes and a red block. Targets isWarmPosterScanSource(), the only
  // route to paper normalisation. The three colours are chosen against the predicates rather
  // than by eye: paper (222,210,178) has luma 210.3 and saturation 0.198, inside
  // isWarmPaperPixel's [92,230] and [0.06,0.56] with red >= blue + 10; the ink (35,35,35) is
  // luma 35 at saturation 0, inside the darkNeutral test; the red (190,40,30) has saturation
  // 0.842 with red >= green + 24 and red >= blue + 28, so isRedPixel holds.
  {
    name: "warm-poster-160x120",
    width: 160,
    height: 120,
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
          let colour = [222, 210, 178];
          if (x >= 110 && x < 150 && y >= 10 && y < 40) {
            colour = [190, 40, 30];
          } else if (y % 16 >= 12 && x % 5 >= 2) {
            colour = [35, 35, 35];
          }
          const at = (y * width + x) * 3;
          rgb[at] = colour[0];
          rgb[at + 1] = colour[1];
          rgb[at + 2] = colour[2];
        }
      }
      return rgb;
    },
  },

  // Coloured, textured, and centred on mid grey with a small amplitude: aimed at the
  // `lumaStdDev <= 42` branch of the photo arm, which is the one that asks for range mode
  // "auto" and a non-neutral saturation. The xorshift dither is what keeps flatRatio down.
  {
    name: "photo-soft-160x120",
    width: 160,
    height: 120,
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      let state = 0x2468ace0;
      const next = () => {
        state = (state ^ (state << 13)) >>> 0;
        state = (state ^ (state >>> 17)) >>> 0;
        state = (state ^ (state << 5)) >>> 0;
        return state & 0xff;
      };
      for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
          const wave = Math.sin((x / width) * 3.1 + (y / height) * 2.2);
          const base = 128 + Math.round(wave * 26);
          const at = (y * width + x) * 3;
          rgb[at] = Math.max(0, Math.min(255, base + 14 + (next() % 9) - 4));
          rgb[at + 1] = Math.max(0, Math.min(255, base + (next() % 9) - 4));
          rgb[at + 2] = Math.max(0, Math.min(255, base - 18 + (next() % 9) - 4));
        }
      }
      return rgb;
    },
  },

  // The same shape lifted until the kind comes out `photo` rather than `lowContrastPhoto`,
  // while lumaStdDev stays under 42: the photo arm's first branch, which is the only one that
  // asks for range mode "auto" and a non-neutral saturation, and so the one an ordinary
  // photograph is most likely to take on this frame.
  {
    name: "photo-lift-160x120",
    width: 160,
    height: 120,
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      let state = 0x0f1e2d3c;
      const next = () => {
        state = (state ^ (state << 13)) >>> 0;
        state = (state ^ (state >>> 17)) >>> 0;
        state = (state ^ (state << 5)) >>> 0;
        return state & 0xff;
      };
      for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
          const wave = Math.sin((x / width) * 3.1 + (y / height) * 2.2);
          const base = 128 + Math.round(wave * 52);
          const at = (y * width + x) * 3;
          rgb[at] = Math.max(0, Math.min(255, base + 18 + (next() % 21) - 10));
          rgb[at + 1] = Math.max(0, Math.min(255, base + (next() % 21) - 10));
          rgb[at + 2] = Math.max(0, Math.min(255, base - 22 + (next() % 21) - 10));
        }
      }
      return rgb;
    },
  },

  // A screenshot: grey text strokes over most of the frame with one colourful noisy panel.
  // Targets `textOrUi`, whose five tone constants are reachable from nowhere else.
  //
  // **It needs the panel, and that is not decoration.** getImageKindScores (image-style.ts:721,
  // :729) gives lineArt and textOrUi the same 1.0 ceiling, and getBestKind's reduce keeps the
  // earlier entry on a tie -- lineArt is declared first, so a plain grey text image ties and
  // comes out lineArt, which is what text-like-160x120 above does. The only term that separates
  // them is lineArt's `photoTileLineArtPenalty`, so the image has to contain tiles that satisfy
  // getTileMetrics' photo test (uniqueColorRatio >= 0.18, lumaStdDev >= 18, flatRatio <= 0.68).
  {
    name: "screenshot-160x120",
    width: 160,
    height: 120,
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      rgb.fill(250);
      let state = 0x77aa33cc;
      const next = () => {
        state = (state ^ (state << 13)) >>> 0;
        state = (state ^ (state >>> 17)) >>> 0;
        state = (state ^ (state << 5)) >>> 0;
        return state & 0xff;
      };
      for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
          const at = (y * width + x) * 3;
          if (x >= 112) {
            rgb[at] = next();
            rgb[at + 1] = next();
            rgb[at + 2] = next();
            continue;
          }
          if (y % 8 >= 5 && x % 4 >= 1) {
            rgb[at] = 20;
            rgb[at + 1] = 20;
            rgb[at + 2] = 20;
          }
        }
      }
      return rgb;
    },
  },

  // The photo arm's `lumaStdDev >= 70` branch, which takes display-mode compression at 0.78 and
  // leaves the preset's tone curve alone.
  //
  // **A two-axis product does not get there.** `sin(x) * cos(y)` at amplitude 125 measured
  // lumaStdDev 63.4, because multiplying two waves spends most of the image near zero -- the
  // spread of a product is the product of the spreads. One dominant axis reaches 70 at a
  // *smaller* amplitude than the product needed.
  {
    name: "photo-wide-160x120",
    width: 160,
    height: 120,
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      let state = 0x13579bdf;
      const next = () => {
        state = (state ^ (state << 13)) >>> 0;
        state = (state ^ (state >>> 17)) >>> 0;
        state = (state ^ (state << 5)) >>> 0;
        return state & 0xff;
      };
      for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
          const base =
            128 + Math.round(112 * Math.sin(x * 0.11) + 26 * Math.sin(y * 0.07));
          const at = (y * width + x) * 3;
          rgb[at] = Math.max(0, Math.min(255, base + 22 + (next() % 13) - 6));
          rgb[at + 1] = Math.max(0, Math.min(255, base + (next() % 13) - 6));
          rgb[at + 2] = Math.max(0, Math.min(255, base - 26 + (next() % 13) - 6));
        }
      }
      return rgb;
    },
  },

  // The same again with the wave driven far past the ends so a large share of the frame clips to
  // near-black and near-white. That is what `highContrastPhoto` scores on -- its
  // contrastEndpointRatio term (image-style.ts:708-713) reads the dark and light ratios rather
  // than the spread, which photo-wide-160x120 has plenty of without ever reaching the ends.
  // The arm it selects is the only user of the `soft` preset and of stucki diffusion.
  //
  // **The channel offsets lean cool on purpose.** The first version leaned warm, the way the two
  // photo images above do, and applyPosterScanTuning took the whole suggestion over: every pixel
  // satisfied isWarmPaperPixel's `red >= blue + 10`, the clipped shadows supplied the ink, and
  // the kind came out highContrastPhoto with a posterScan plan attached -- so the arm this image
  // exists for never ran. A fixture can reach the intended kind and still test something else.
  {
    name: "photo-clipped-160x120",
    width: 160,
    height: 120,
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      let state = 0x5a5aa5a5;
      const next = () => {
        state = (state ^ (state << 13)) >>> 0;
        state = (state ^ (state >>> 17)) >>> 0;
        state = (state ^ (state << 5)) >>> 0;
        return state & 0xff;
      };
      for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
          const base =
            128 + Math.round(260 * Math.sin(x * 0.09) + 60 * Math.sin(y * 0.13));
          const at = (y * width + x) * 3;
          rgb[at] = Math.max(0, Math.min(255, base - 20 + (next() % 17) - 8));
          rgb[at + 1] = Math.max(0, Math.min(255, base + (next() % 17) - 8));
          rgb[at + 2] = Math.max(0, Math.min(255, base + 18 + (next() % 17) - 8));
        }
      }
      return rgb;
    },
  },
];

// FNV-1a, 32-bit. Chosen because it is four lines in both languages and needs no table.
const fnv1a = (buffer) => {
  let hash = 0x811c9dc5;
  for (const byte of buffer) {
    hash = (hash ^ byte) >>> 0;
    hash = Math.imul(hash, 0x01000193) >>> 0;
  }
  return hash >>> 0;
};

const KIND_ORDER = [
  "photo",
  "lowContrastPhoto",
  "highContrastPhoto",
  "flatIllustration",
  "lineArt",
  "textOrUi",
  "pixelArt",
  "unknown",
];

const STYLE_ORDER = ["photo", "illustration", "unknown"];

// The metric fields the C struct carries, in its declaration order. `transparentRatio` is not
// among them: epd_canvas_t has no alpha channel, so it is always 0 there.
const METRIC_FIELDS = [
  "uniqueColorRatio",
  "topColorCoverage",
  "paletteEntropy",
  "flatRatio",
  "softChangeRatio",
  "strongEdgeRatio",
  "edgeDensity",
  "horizontalEdgeRatio",
  "verticalEdgeRatio",
  "lumaStdDev",
  "lumaP05",
  "lumaP95",
  "lumaRange",
  "saturationMean",
  "saturationStdDev",
  "darkRatio",
  "lightRatio",
  "grayRatio",
  "highSaturationRatio",
  "warmPaperRatio",
  "redRatio",
  "darkNeutralRatio",
  "photoTileRatio",
  "flatTileRatio",
  "textTileRatio",
  "gradientTileRatio",
];

// **Full precision, not toFixed(9), and that was learned the hard way.** Two kind scores for
// the flat-blocks image are 0.9999999995 and 1; at nine decimals both print as "1.000000000",
// which hides the fact that upstream's choice between two kinds rests on 5e-10 and makes the C
// side's disagreement look like a port bug. String() gives the shortest representation that
// round-trips to the same double, and the C compiler rounds it to a float itself.
// `${String(1)}f` is "1f", which C reads as an octal constant with a bad digit rather than as a
// float -- so an integral value needs the decimal point put back. An exponent form ("1e-7")
// already is a valid float literal.
const cFloat = (value) => {
  const text = String(value);
  return /[.eE]/.test(text) ? `${text}f` : `${text}.0f`;
};

const emitClassifyHeader = async (version, commit) => {
  const { classifyImageStyle } = await import("../refs/epdoptimize/dist/index.mjs");
  const rows = [];

  for (const spec of CLASSIFY_IMAGES) {
    const rgb = spec.fn(spec.width, spec.height);
    const result = classifyImageStyle(
      toImageData({ width: spec.width, height: spec.height, rgb })
    );

    const metrics = METRIC_FIELDS.map((field) => {
      const value = result.metrics[field];
      if (typeof value !== "number" || !Number.isFinite(value)) {
        throw new Error(`${spec.name}: metric ${field} is ${value}`);
      }
      return cFloat(value);
    });
    const scores = KIND_ORDER.map((kind) => cFloat(result.kindScores[kind]));

    rows.push(
      `    {"${spec.name}", ${spec.width}, ${spec.height}, 0x${fnv1a(rgb)
        .toString(16)
        .padStart(8, "0")}u,\n` +
        `     ${STYLE_ORDER.indexOf(result.style)}, ${KIND_ORDER.indexOf(result.kind)}, ` +
        `${cFloat(result.photoScore)}, ${cFloat(result.confidence)},\n` +
        `     {${scores.join(", ")}},\n` +
        `     {${metrics.join(", ")}}},`
    );
  }

  const header = `// GENERATED by tools/epdopt_reference.mjs --classify -- do not edit.
//
// What epdoptimize ${version} (${commit}) classifyImageStyle() returns for each of the images
// defined in that generator's CLASSIFY_IMAGES. The images themselves are NOT here: each is an
// integer-only rule that test/test_classify/test_main.c reproduces, and \`checksum\` is an
// FNV-1a over the bytes so that a divergence between the two definitions fails as a checksum
// rather than as a metric.
//
// Regenerate after changing the generator or updating refs/epdoptimize. Do not hand-edit a
// value to make a test pass -- these numbers are the specification.

#ifndef CLASSIFY_FIXTURES_H
#define CLASSIFY_FIXTURES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    int32_t width;
    int32_t height;
    uint32_t checksum;
    int style;   // index into epd_image_style_t
    int kind;    // index into epd_image_kind_t
    float photo_score;
    float confidence;
    float kind_scores[8];
    // In the declaration order of epd_classify_metrics_t, from unique_colour_ratio onwards.
    float metrics[${METRIC_FIELDS.length}];
} classify_fixture_t;

static const classify_fixture_t CLASSIFY_FIXTURES[] = {
${rows.join("\n")}
};

static const size_t CLASSIFY_FIXTURE_COUNT =
    sizeof(CLASSIFY_FIXTURES) / sizeof(CLASSIFY_FIXTURES[0]);

#define CLASSIFY_METRIC_COUNT ${METRIC_FIELDS.length}

#endif // CLASSIFY_FIXTURES_H
`;

  const outDir = join(repo, "test/test_classify");
  mkdirSync(outDir, { recursive: true });
  writeFileSync(join(outDir, "classify_fixtures.h"), header);
  console.log(
    `wrote test/test_classify/classify_fixtures.h: ${rows.length} fixtures from epdoptimize ${version}`
  );
};

// ------------------------------------------------------- fixtures for the suggestion port
//
// The two palettes are this repository's own tables, not upstream's names, because the
// suggestion depends on them: applyPaletteTuning() (auto-processing.ts:1145-1174) forces
// display-mode range compression at strength >= 0.8 for any palette whose Rec. 709 luma range is
// <= 150, overriding whatever the image kind chose. `aitjcize` spans 195.9 and does not trip it;
// `manual` spans 130.6 and does. The generator asserts that the two really do straddle 150 --
// without that they are one arm run twice.

const AUTO_PALETTES = [
  {
    id: 0,
    name: "aitjcize",
    // EPD_PALETTE_EPDOPT_AITJCIZE, epd_dither.c:70-77.
    hex: ["020202", "bec8c8", "05409e", "27663c", "871300", "cdca00"],
    expectWideRange: true,
  },
  {
    id: 1,
    name: "manual",
    // EPD_PALETTE_MANUAL, epd_dither.c:56-63.
    hex: ["211d2f", "9aa4a2", "a19903", "7b1913", "18528b", "345b3a"],
    expectWideRange: false,
  },
];

const TONE_MODE_ORDER = ["unset", "off", "contrast", "scurve"];
const RANGE_MODE_ORDER = ["off", "display", "auto"];

const paletteLumaRange = (hexList) => {
  const lumas = hexList.map((hex) => {
    const r = parseInt(hex.slice(0, 2), 16);
    const g = parseInt(hex.slice(2, 4), 16);
    const b = parseInt(hex.slice(4, 6), 16);
    return r * 0.2126 + g * 0.7152 + b * 0.0722;
  });
  return Math.max(...lumas) - Math.min(...lumas);
};

// What this fixture set must reach. A suite over inputs that all land on one side of a threshold
// cannot fail for the right reason, so the generator refuses to write the header rather than
// leaving that to be assumed.
//
// Every arm of applyLayeredAutoAdjustments is here -- all seven kinds, the photo arm's three
// lumaStdDev branches by their range strengths, both sides of the quantization guard, both sides
// of white preservation, the level and paper stages, and the palette override. **Three things
// upstream can express are absent, and each is a claim rather than a gap:**
//
//   * `unknown`. getBestKind only returns it when every score is <= 0, which needs an image with
//     no visible samples at all -- classifyImageStyle returns its `unknown` default before the
//     metrics exist. src/core/epd_auto.c's arm for it is reached by the `default:` label instead.
//   * **lineArt with lumaRange <= 96**, the only route to level compression at 6/248. It looks
//     reachable and is not: `lumaRange <= 96` is also the first clause of
//     isRestorableLowContrastSource, so escaping that predicate needs `lumaStdDev > 32` inside a
//     96-level range, which is a near-bimodal image -- and such an image scores as pixelArt or
//     flatIllustration rather than lineArt. applyLowContrastRestoreTuning wins whenever the
//     branch would fire.
//   * The `colorCount <= 2` half of applyPaletteTuning. Every palette in epd_dither.c has six
//     entries, so it cannot be reached from this frame at all.
const REQUIRED_BRANCHES = [
  "kind:photo",
  "kind:lowContrastPhoto",
  "kind:highContrastPhoto",
  "kind:flatIllustration",
  "kind:lineArt",
  "kind:textOrUi",
  "kind:pixelArt",
  // The photo arm's three lumaStdDev branches, told apart by the strength each one asks for.
  "photoArm:auto@0.68",
  "photoArm:display@0.7",
  "photoArm:display@0.78",
  "range:off",
  "range:display",
  "range:auto",
  "quantizationOnly:kept",
  "quantizationOnly:overridden",
  "preserveWhite:on",
  "preserveWhite:off",
  "level:on",
  "paper:on",
  "paletteOverridesRangeMode",
];

const emitAutoHeader = async (version, commit) => {
  const { suggestLayeredProcessingOptions } = await import(
    "../refs/epdoptimize/dist/index.mjs"
  );

  for (const palette of AUTO_PALETTES) {
    const range = paletteLumaRange(palette.hex);
    if (range > 150 !== palette.expectWideRange) {
      throw new Error(
        `palette ${palette.name} has luma range ${range.toFixed(1)}, which is on the wrong ` +
          `side of applyPaletteTuning's 150 -- the two-palette arm would test nothing`
      );
    }
  }

  const rows = [];
  const reached = new Set();
  const report = [];

  for (const spec of [...CLASSIFY_IMAGES, ...AUTO_EXTRA_IMAGES]) {
    const rgb = spec.fn(spec.width, spec.height);
    const image = toImageData({ width: spec.width, height: spec.height, rgb });
    const perPalette = [];

    for (const palette of AUTO_PALETTES) {
      // A fresh ImageDataLike per call: classifyImageStyle only reads, but relying on that
      // across a library update is not worth the two lines it saves.
      const suggestion = suggestLayeredProcessingOptions(
        toImageData({ width: spec.width, height: spec.height, rgb }),
        palette.hex
      );
      const o = suggestion.ditherOptions;
      const c = suggestion.classification;
      const tm = o.toneMapping;
      const drc = o.dynamicRangeCompression;
      const lvl = o.levelCompression;
      const pn = o.paperNormalization;

      if (!tm || !drc) {
        throw new Error(`${spec.name}/${palette.name}: layered auto left tone or range unset`);
      }

      const toneMode = TONE_MODE_ORDER.indexOf(tm.mode ?? "unset");
      const rangeMode = RANGE_MODE_ORDER.indexOf(drc.mode ?? "display");
      if (toneMode < 0 || rangeMode < 0) {
        throw new Error(`${spec.name}: unknown mode ${tm.mode} / ${drc.mode}`);
      }

      const nearest = o.ditheringType === "quantizationOnly";
      const preserveWhite = drc.preserveWhite === true;

      reached.add(`kind:${c.kind}`);
      reached.add(`range:${drc.mode ?? "display"}`);
      reached.add(preserveWhite ? "preserveWhite:on" : "preserveWhite:off");
      if (lvl) {
        reached.add("level:on");
      }
      if (pn) {
        reached.add("paper:on");
      }
      // Only for a `photo` that neither tuning function took over, so the strength really is the
      // one applyLayeredAutoAdjustments' photo arm chose.
      if (c.kind === "photo" && !lvl && !pn) {
        reached.add(`photoArm:${drc.mode}@${drc.strength}`);
      }
      // `nearest` is the kind's own choice only when the guard let it through; a kind that asks
      // for quantizationOnly and does not get it is the guard firing.
      const kindAsksQuantization =
        c.kind === "textOrUi" || c.kind === "lineArt" || c.kind === "pixelArt";
      if (kindAsksQuantization) {
        reached.add(nearest ? "quantizationOnly:kept" : "quantizationOnly:overridden");
      }

      const metrics = METRIC_FIELDS.map((field) => cFloat(c.metrics[field]));

      rows.push(
        `    {"${spec.name}", ${palette.id},\n` +
          `     ${STYLE_ORDER.indexOf(c.style)}, ${KIND_ORDER.indexOf(c.kind)}, ` +
          `${cFloat(c.photoScore)},\n` +
          `     {${metrics.join(", ")}},\n` +
          `     ${toneMode}, {${cFloat(tm.exposure ?? 0)}, ${cFloat(tm.saturation ?? 0)}, ` +
          `${cFloat(tm.contrast ?? 0)}, ${cFloat(tm.strength ?? 0)}, ` +
          `${cFloat(tm.shadowBoost ?? 0)}, ${cFloat(tm.highlightCompress ?? 0)}, ` +
          `${cFloat(tm.midpoint ?? 0)}},\n` +
          `     ${rangeMode}, {${cFloat(drc.strength ?? 0)}, ` +
          `${cFloat(drc.lowPercentile ?? 0)}, ${cFloat(drc.highPercentile ?? 0)}},\n` +
          `     ${lvl ? 1 : 0}, {${cFloat(lvl?.black ?? 0)}, ${cFloat(lvl?.white ?? 0)}},\n` +
          `     ${pn ? 1 : 0}, {${cFloat(pn?.strength ?? 0)}, ${cFloat(pn?.minLuma ?? 0)}, ` +
          `${cFloat(pn?.saturationThreshold ?? 0)}, ` +
          `${cFloat(pn?.warmBiasThreshold ?? 0)}, ${cFloat(pn?.blackAnchor ?? 0)}, ` +
          `${cFloat(pn?.preserveRed ?? 0)}},\n` +
          `     {${(pn?.paperWhite ?? [0, 0, 0]).join(", ")}},\n` +
          `     ${preserveWhite ? 1 : 0}, {${cFloat(drc.whitePreservePercentile ?? 0.99)}, ` +
          `${cFloat(drc.whitePreserveMinLuma ?? 150)}, ` +
          `${cFloat(drc.whitePreserveMaxSaturation ?? 0.18)}},\n` +
          `     ${nearest ? 1 : 0}, ${o.colorMatching === "lab" ? 1 : 0}, ` +
          `${o.errorDiffusionMatrix === "stucki" ? 1 : 0}, ` +
          `${o.serpentine === true ? 1 : 0}},`
      );

      perPalette.push({ palette: palette.name, kind: c.kind, rangeMode: drc.mode, nearest });
      // lumaStdDev and lumaRange are on the line because they are the two metrics the arms
      // above are steered by: 42 and 70 split the photo arm three ways, and 96 splits lineArt
      // and gates isRestorableLowContrastSource. Reading them is how an image gets aimed at a
      // branch instead of guessed at.
      report.push(
        `  ${spec.name.padEnd(22)} ${palette.name.padEnd(9)} ${String(c.kind).padEnd(18)} ` +
          `sd=${c.metrics.lumaStdDev.toFixed(1).padStart(5)} ` +
          `rng=${c.metrics.lumaRange.toFixed(0).padStart(3)} ` +
          `range=${String(drc.mode).padEnd(8)}@${String(drc.strength ?? 0).padEnd(5)} ` +
          `nearest=${nearest ? "y" : "n"} ` +
          `white=${preserveWhite ? "y" : "n"} level=${lvl ? "y" : "n"} paper=${pn ? "y" : "n"}`
      );
    }

    if (perPalette[0].rangeMode !== perPalette[1].rangeMode) {
      reached.add("paletteOverridesRangeMode");
    }
  }

  console.log("kind and plan per image and palette:");
  for (const line of report) {
    console.log(line);
  }

  const missing = REQUIRED_BRANCHES.filter((b) => !reached.has(b));
  if (missing.length) {
    throw new Error(
      `these branches are unreached by the fixture set, so test_auto cannot fail for them: ` +
        `${missing.join(", ")}. Add images to AUTO_EXTRA_IMAGES rather than dropping the check.`
    );
  }
  console.log(`branch coverage: all ${REQUIRED_BRANCHES.length} required branches reached`);

  const header = `// GENERATED by tools/epdopt_reference.mjs --auto -- do not edit.
//
// What epdoptimize ${version} (${commit}) buildLayeredSuggestion() decides, for each of that
// generator's CLASSIFY_IMAGES plus AUTO_EXTRA_IMAGES, against two palettes.
//
// **The classification here is upstream's, not this project's.** test/test_auto/test_main.c
// builds an epd_classification_t out of these numbers and hands it straight to
// epd_auto_suggest(), so a failure is a fault in src/core/epd_auto.c and cannot be one in
// src/core/epd_classify.c. That is also why no pixels and no checksum are here: nothing in the C
// test looks at an image.
//
// Regenerate after changing the generator or updating refs/epdoptimize. Do not hand-edit a
// value to make a test pass -- these numbers are the specification.

#ifndef AUTO_FIXTURES_H
#define AUTO_FIXTURES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *name;
    int palette;  // 0 = EPD_PALETTE_EPDOPT_AITJCIZE, 1 = EPD_PALETTE_MANUAL

    // The classification, restricted to what the suggestion actually reads: style, kind,
    // photoScore and the metrics. kindScores and confidence are not among them.
    int style;
    int kind;
    float photo_score;
    float metrics[${METRIC_FIELDS.length}];

    // The expected plan.
    int tone_mode;   // index into epd_tone_mode_t
    // exposure, saturation, contrast, strength, shadow_boost, highlight_compress, midpoint
    float tone[7];
    int range_mode;  // index into epd_range_mode_t
    float range[3];  // strength, low_percentile, high_percentile
    int level_enabled;
    float level[2];  // black, white
    int paper_enabled;
    // strength, min_luma, saturation_threshold, warm_bias_threshold, black_anchor, preserve_red
    float paper[6];
    int paper_white[3];
    int white_enabled;
    float white[3];  // percentile, min_luma, max_saturation
    int nearest;
    int wanted_lab;
    int wanted_stucki;
    // buildLayeredSuggestion adds serpentine: true to every errorDiffusion arm and omits it
    // entirely for quantizationOnly (auto-processing.ts:388-391), so this tracks the inverse of
    // nearest -- worth a fixture column rather than a comment, because src/core/epd_auto.c asserts
    // the relation rather than deriving it.
    int serpentine;
} auto_fixture_t;

static const auto_fixture_t AUTO_FIXTURES[] = {
${rows.join("\n")}
};

static const size_t AUTO_FIXTURE_COUNT = sizeof(AUTO_FIXTURES) / sizeof(AUTO_FIXTURES[0]);

#define AUTO_METRIC_COUNT ${METRIC_FIELDS.length}

#endif // AUTO_FIXTURES_H
`;

  const outDir = join(repo, "test/test_auto");
  mkdirSync(outDir, { recursive: true });
  writeFileSync(join(outDir, "auto_fixtures.h"), header);
  console.log(
    `wrote test/test_auto/auto_fixtures.h: ${rows.length} fixtures from epdoptimize ${version}`
  );
};

// ------------------------------------------- fixtures for the three added pixel stages
//
// `--stages` writes test/test_adjust/stage_fixtures.h. Paper normalisation, luma level
// compression and white preservation have **no double-precision counterpart in
// src/core/epd_epdopt.c**, so the two-level chain the other two stages use (float vs double here,
// double vs library in test_epdopt) does not exist for them. These are pinned straight to the
// library within a tolerance instead.
//
// Each stage is isolated by passing only its own option: mergeImageProcessingOptions
// (dither.ts:422-440) returns undefined when none of the five is present and applyToneMapping
// returns early without one, and getPresetDefaults (dither.ts:463-467) contributes nothing
// unless `processingPreset` is set -- which none of these do.
//
// **White preservation cannot be isolated, by construction.** getWhitePreservationPlan refuses
// unless the range mode is non-off (dither.ts:333-340), so it is emitted as a pair: range alone,
// and range with preserveWhite. The difference between the two arrays is the stage.
//
// `quality: "fast"` on every range-bearing call. Upstream's auto flow leaves quality unset and so
// takes the L*a*b* path; the device has no L*a*b* path and epd_adjust_range() is the fast
// algorithm. Generating these against the accurate one would pin the port to an algorithm it does
// not implement.

const STAGE_IMAGES = [
  // All three paper-normalisation branches plus two pixels that should fall through untouched, so
  // the fixture can fail for "changed something it should not" as well as for a wrong formula.
  // The colours are chosen against the predicates: (222,210,178) is warm paper, (35,35,35) is
  // dark neutral ink, (190,40,30) is red ink, (128,128,128) is mid grey whose dark-neutral mask
  // clamps to zero and whose warm bias is zero, and (20,40,200) is saturated blue which fails
  // every branch from the other side.
  {
    name: "warm-paper-24x8",
    width: 24,
    height: 8,
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      const cycle = [
        [222, 210, 178], [222, 210, 178], [35, 35, 35],
        [190, 40, 30], [128, 128, 128], [20, 40, 200],
      ];
      for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
          const colour = cycle[x % 6];
          const at = (y * width + x) * 3;
          rgb[at] = colour[0];
          rgb[at + 1] = colour[1];
          rgb[at + 2] = colour[2];
        }
      }
      return rgb;
    },
  },

  // Three coprime strides through 0-255, so the channels are uncorrelated and maxChannel takes
  // every value from 0 upwards. Pixel 0 is (0,0,0), which is the `y > 0` guard in both level
  // compression and range compression.
  {
    name: "mixed-24x8",
    width: 24,
    height: 8,
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      for (let i = 0; i < width * height; i += 1) {
        const at = i * 3;
        rgb[at] = (i * 4) % 256;
        rgb[at + 1] = (i * 7) % 256;
        rgb[at + 2] = (i * 11) % 256;
      }
      return rgb;
    },
  },

  // Half paper-white neutral, half mid colour: the p99 of the low-saturation pixels lands at 245,
  // comfortably above whitePreserveMinLuma, so the plan is active and the apply pass has both
  // marked and unmarked pixels to walk.
  //
  // **The bright half has to be exactly neutral, and finding out why is worth the paragraph.**
  // The first version used (245,246,244), whose luma is 245.6; getBytePercentileFromHistogram
  // returns a *bin index*, so the p99 came back as clampByte(245.6) = 246, and
  // applyWhitePreservation's test compares the **unrounded** luma against it (dither.ts:409) --
  // 245.6 < 246, so every pixel that filled that bin was then excluded from it and the stage did
  // nothing at all. With r = g = b the luma is the channel value to within float error, which is
  // exactly what upstream's `+ 0.0001` epsilon is there to absorb. So white preservation fires
  // only where the p99 pixel's luma does not round up past itself -- true of real paper white,
  // which is spread over many lumas, and easy to miss in a fixture of two colours.
  {
    name: "paper-white-24x8",
    width: 24,
    height: 8,
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      for (let y = 0; y < height; y += 1) {
        for (let x = 0; x < width; x += 1) {
          const i = y * width + x;
          const at = i * 3;
          if (x < 12) {
            rgb[at] = 245;
            rgb[at + 1] = 245;
            rgb[at + 2] = 245;
          } else {
            rgb[at] = (i * 13) % 160;
            rgb[at + 1] = (i * 29) % 160;
            rgb[at + 2] = (i * 7) % 160;
          }
        }
      }
      return rgb;
    },
  },

  // Wholly neutral and wholly dark: every pixel is a white candidate and the p99 still comes out
  // under 100, so getWhitePreservationPlan returns null at dither.ts:383. That is the other side
  // of white preservation, and without it the stage is only ever tested switched on.
  {
    name: "dim-24x8",
    width: 24,
    height: 8,
    fn: (width, height) => {
      const rgb = Buffer.alloc(width * height * 3);
      for (let i = 0; i < width * height; i += 1) {
        const grey = (i * 3) % 100;
        const at = i * 3;
        rgb[at] = grey;
        rgb[at + 1] = grey;
        rgb[at + 2] = grey;
      }
      return rgb;
    },
  },
];

// The posterScan arm's paper settings and the lowContrastPhoto arm's level settings, which is
// what src/core/epd_auto.c produces for them.
const STAGE_PAPER_OPTIONS = {
  paperNormalization: {
    mode: "warmPaper",
    strength: 0.95,
    minLuma: 82,
    saturationThreshold: 0.56,
    warmBiasThreshold: 8,
    blackAnchor: 0.95,
    preserveRed: 0.85,
    paperWhite: [248, 248, 246],
  },
};

const STAGE_LEVEL_OPTIONS = {
  levelCompression: { mode: "luma", black: 8, white: 245 },
};

const stageRangeOptions = (preserveWhite) => ({
  palette: AUTO_PALETTES[0].hex,
  dynamicRangeCompression: {
    mode: "display",
    strength: 1,
    quality: "fast",
    ...(preserveWhite
      ? {
          preserveWhite: true,
          whitePreservePercentile: 0.99,
          whitePreserveMinLuma: 150,
          whitePreserveMaxSaturation: 0.18,
        }
      : {}),
  },
});

const emitStageHeader = async (version, commit) => {
  const { applyImageDataAdjustments } = await import(
    "../refs/epdoptimize/dist/index.mjs"
  );

  const blocks = [];
  const rows = [];

  const run = (spec, rgb, options) => {
    const image = toImageData({ width: spec.width, height: spec.height, rgb });
    applyImageDataAdjustments(image, options);
    return toRgb(image);
  };

  for (const spec of STAGE_IMAGES) {
    const rgb = spec.fn(spec.width, spec.height);
    const symbol = cIdentifier(spec.name);

    blocks.push(emitArray(`${symbol}_in`, rgb));
    blocks.push(emitArray(`${symbol}_paper`, run(spec, rgb, STAGE_PAPER_OPTIONS)));
    blocks.push(emitArray(`${symbol}_level`, run(spec, rgb, STAGE_LEVEL_OPTIONS)));
    const range = run(spec, rgb, stageRangeOptions(false));
    const rangeWhite = run(spec, rgb, stageRangeOptions(true));
    blocks.push(emitArray(`${symbol}_range`, range));
    blocks.push(emitArray(`${symbol}_range_white`, rangeWhite));

    // Whether white preservation did anything, straight off the bytes. The C test asserts this
    // as a *relationship* against its own plan -- active must change something, inactive must
    // change nothing -- rather than the generator reimplementing dither.ts:378-385, which would
    // be a second thing that can be wrong.
    const whiteChanged = !range.equals(rangeWhite) ? 1 : 0;

    rows.push(
      `    {"${spec.name}", ${spec.width}, ${spec.height}, ${symbol}_in, ${symbol}_paper,\n` +
        `     ${symbol}_level, ${symbol}_range, ${symbol}_range_white, ${whiteChanged}},`
    );
    console.log(
      `  ${spec.name.padEnd(20)} white preservation ` +
        `${whiteChanged ? "changed pixels" : "did nothing"}`
    );
  }

  const changing = rows.filter((r) => r.trimEnd().endsWith("1},")).length;
  if (changing === 0 || changing === rows.length) {
    throw new Error(
      "every stage image agrees about white preservation, so the C test cannot tell an active " +
        "plan from an abandoned one. Keep one image with paper white in it and one without."
    );
  }

  const header = `// GENERATED by tools/epdopt_reference.mjs --stages -- do not edit.
//
// Expected output of epdoptimize ${version} (${commit}) for the three stages that arrived with
// the auto flow and have no double-precision counterpart in src/core/epd_epdopt.c: paper
// normalisation, luma level compression, and white preservation.
//
// Each array is that stage applied **alone** to \`in\`, except the last pair: white preservation
// cannot run without a live range stage, so \`range\` and \`range_white\` are the same range
// compression with the flag off and on, and their difference is the stage. Both are at
// \`quality: "fast"\`, which is the algorithm src/core/epd_adjust.c implements -- upstream's auto flow
// leaves quality unset and so takes the L*a*b* path the device does not have.
//
// Unlike test/test_epdopt/, these are compared within a tolerance rather than byte for byte:
// src/core/epd_adjust.c is single precision. The bounds are in test_main.c and were written down
// before the first run. Do not hand-edit a value here to make a test pass, and do not raise a
// bound to do it either -- both are ways of deleting the measurement.

#ifndef STAGE_FIXTURES_H
#define STAGE_FIXTURES_H

#include <stddef.h>
#include <stdint.h>

${blocks.join("\n\n")}

typedef struct {
    const char *name;
    int32_t width;
    int32_t height;
    const uint8_t *input;
    const uint8_t *after_paper;
    const uint8_t *after_level;
    const uint8_t *after_range;
    const uint8_t *after_range_white;
    // 1 when white preservation changed at least one pixel for this image, 0 when the plan was
    // abandoned. Derived from the two arrays above, not from a second implementation.
    int white_changed;
} stage_fixture_t;

static const stage_fixture_t STAGE_FIXTURES[] = {
${rows.join("\n")}
};

static const size_t STAGE_FIXTURE_COUNT = sizeof(STAGE_FIXTURES) / sizeof(STAGE_FIXTURES[0]);

#endif // STAGE_FIXTURES_H
`;

  const outDir = join(repo, "test/test_adjust");
  mkdirSync(outDir, { recursive: true });
  writeFileSync(join(outDir, "stage_fixtures.h"), header);
  console.log(
    `wrote test/test_adjust/stage_fixtures.h: ${rows.length} fixtures from epdoptimize ${version}`
  );
};

// --------------------------------------------------------------------- running stages

const toImageData = (image) => {
  const data = new Uint8ClampedArray(image.width * image.height * 4);
  for (let i = 0, p = 0; i < data.length; i += 4, p += 3) {
    data[i] = image.rgb[p];
    data[i + 1] = image.rgb[p + 1];
    data[i + 2] = image.rgb[p + 2];
    data[i + 3] = 255;
  }
  return { width: image.width, height: image.height, data };
};

const toRgb = (imageData) => {
  const rgb = Buffer.alloc(imageData.width * imageData.height * 3);
  for (let i = 0, p = 0; i < imageData.data.length; i += 4, p += 3) {
    rgb[p] = imageData.data[i];
    rgb[p + 1] = imageData.data[i + 1];
    rgb[p + 2] = imageData.data[i + 2];
  }
  return rgb;
};

const cloneImageData = (imageData) => ({
  width: imageData.width,
  height: imageData.height,
  data: new Uint8ClampedArray(imageData.data),
});

const stubCanvas = (imageData) => {
  const context = {
    getImageData: () => imageData,
    putImageData: () => {},
  };
  return {
    width: imageData.width,
    height: imageData.height,
    getContext: () => context,
  };
};

const runStages = async (image, paletteName) => {
  const input = toImageData(image);
  const options = baseOptions(paletteName);

  const tone = cloneImageData(input);
  applyImageDataAdjustments(tone, { ...options, dynamicRangeCompression: { mode: "off" } });

  const range = cloneImageData(input);
  applyImageDataAdjustments(range, options);

  const diffused = cloneImageData(range);
  await ditherCanvas(stubCanvas(diffused), stubCanvas(diffused), options);

  // Serpentine scanning, which reverses every other row. Only the diffusion changes -- tone
  // mapping and range compression are per-pixel -- so it gets one extra array rather than a
  // second fixture. It is covered because plain Floyd-Steinberg leaves visible diagonal
  // banding in a smooth sky, and this is the standard remedy; having it verified is what
  // makes switching the default a decision rather than a gamble.
  const serpentine = cloneImageData(range);
  await ditherCanvas(stubCanvas(serpentine), stubCanvas(serpentine), {
    ...options,
    serpentine: true,
  });

  return {
    tone: toRgb(tone),
    range: toRgb(range),
    diffuse: toRgb(diffused),
    serpentine: toRgb(serpentine),
  };
};

// ------------------------------------------------------------------------ emit header

const cIdentifier = (name) => name.replace(/[^A-Za-z0-9]/g, "_");

const emitArray = (symbol, buffer) => {
  const lines = [`static const uint8_t ${symbol}[] = {`];
  for (let i = 0; i < buffer.length; i += 16) {
    const row = Array.from(buffer.subarray(i, i + 16)).map((v) => String(v).padStart(3, " "));
    lines.push(`    ${row.join(", ")},`);
  }
  lines.push("};");
  return lines.join("\n");
};

const main = async () => {
  const version = JSON.parse(
    readFileSync(join(repo, "refs/epdoptimize/package.json"), "utf8")
  ).version;
  const commit = execFileSync("git", ["-C", join(repo, "refs/epdoptimize"), "rev-parse", "HEAD"])
    .toString()
    .trim();

  if (process.argv.includes("--classify")) {
    await emitClassifyHeader(version, commit);
    return;
  }

  if (process.argv.includes("--auto")) {
    await emitAutoHeader(version, commit);
    return;
  }

  if (process.argv.includes("--stages")) {
    await emitStageHeader(version, commit);
    return;
  }

  const blocks = [];
  const rows = [];

  for (const testCase of cases()) {
    for (const palette of PALETTES) {
      const symbol = `${cIdentifier(testCase.name)}_p${palette.id}`;
      const stages = await runStages(testCase.image, palette.name);

      blocks.push(emitArray(`${symbol}_in`, testCase.image.rgb));
      blocks.push(emitArray(`${symbol}_tone`, stages.tone));
      blocks.push(emitArray(`${symbol}_range`, stages.range));
      blocks.push(emitArray(`${symbol}_diffuse`, stages.diffuse));
      blocks.push(emitArray(`${symbol}_serpentine`, stages.serpentine));

      rows.push(
        `    {"${testCase.name}", ${palette.id}, ${testCase.image.width}, ` +
          `${testCase.image.height}, ${symbol}_in, ${symbol}_tone, ${symbol}_range, ` +
          `${symbol}_diffuse, ${symbol}_serpentine},`
      );
    }
  }

  const header = `// GENERATED by tools/epdopt_reference.mjs -- do not edit.
//
// Expected output of epdoptimize ${version} (${commit}) for the "balanced" preset with
// Floyd-Steinberg error diffusion and rgb colour matching, against two of its Spectra 6
// calibrations. Stages are cumulative: \`tone\` is after tone mapping, \`range\` after tone
// mapping and range compression, \`diffuse\` after all three. \`diffuse_serpentine\` is the
// same as \`diffuse\` with serpentine row scanning, which changes only the diffusion pass.
//
// Regenerate after changing the generator or updating refs/epdoptimize. Do not hand-edit a
// value to make a test pass -- these bytes are the specification.

#ifndef EPDOPT_FIXTURES_H
#define EPDOPT_FIXTURES_H

#include <stddef.h>
#include <stdint.h>

${blocks.join("\n\n")}

typedef struct {
    const char *name;
    int palette_id; // 0 = aitjcize-spectra6, 1 = spectra6
    int32_t width;
    int32_t height;
    const uint8_t *input;
    const uint8_t *after_tone;
    const uint8_t *after_range;
    const uint8_t *after_diffuse;
    const uint8_t *after_diffuse_serpentine;
} epdopt_fixture_t;

static const epdopt_fixture_t EPDOPT_FIXTURES[] = {
${rows.join("\n")}
};

static const size_t EPDOPT_FIXTURE_COUNT =
    sizeof(EPDOPT_FIXTURES) / sizeof(EPDOPT_FIXTURES[0]);

#endif // EPDOPT_FIXTURES_H
`;

  const outDir = join(repo, "test/test_epdopt");
  mkdirSync(outDir, { recursive: true });
  writeFileSync(join(outDir, "epdopt_fixtures.h"), header);
  console.log(
    `wrote test/test_epdopt/epdopt_fixtures.h: ${rows.length} fixtures from epdoptimize ${version}`
  );
};

await main();
