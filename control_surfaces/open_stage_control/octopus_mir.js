/*
 * octopus_mir.js - Open Stage Control custom module
 *
 * Bridges the Octopus/Nemo engine's single /mir OSC message (a 170-byte hex
 * string encoding the full LED framebuffer) into per-widget property updates
 * that Open Stage Control clients can render.
 *
 * For every LED in the hardware map, this module:
 *   1. computes the lit state (0/1) and sends /key <keyidx> <lit>  so decoupled
 *      push buttons reflect engine state, and
 *   2. computes the colour and sends /<widget_id>/colorWidget <colour>  so the
 *      accent colour tracks red/green/yellow.
 *
 * Both are emitted only when they change (diffed against the previous frame),
 * keeping bandwidth low (~5-30 messages/frame on localhost).
 *
 * Colour mapping mirrors web_gui.html's setL():
 *   red+green (6) -> yellow   red (2) -> red   green (4) -> green   0 -> off
 */

var COLOR_MAP = {0: 'auto', 2: '#dd2222', 4: '#33dd33', 6: '#ddcc22'};

// Build the widget <-> MIR position table once on load.
// Each entry: { id: widgetId, key: keyIndex, s: mirSet, r: mirRow, b: mirBit }
var ENTRIES = [];

// --- Step pads (Octopus: rows 0-9, 160 cells; ids pad_0 .. pad_159) ---
// Cell $ fills top-to-bottom, left-to-right. Top row = sequencer row 9.
// seqRow = 9 - floor($/16), seqCol = $ % 16, key = 11 + seqCol*11 + seqRow
// MIR: cols 0-7 in set 0, cols 8-15 in set 1 (same row).
for (var i = 0; i < 160; i++) {
    var col = i % 16;
    var row = 9 - ((i - col) / 16);
    var key = 11 + col * 11 + row;
    if (col < 8) ENTRIES.push({id: 'pad_' + i, key: key, s: 0, r: row, b: col});
    else         ENTRIES.push({id: 'pad_' + i, key: key, s: 1, r: row, b: col - 8});
}

// --- Step pads (Nemo: rows 0-7, 128 cells; ids npad_0 .. npad_127) ---
for (var i = 0; i < 128; i++) {
    var col = i % 16;
    var row = 7 - ((i - col) / 16);
    var key = 11 + col * 11 + row;
    if (col < 8) ENTRIES.push({id: 'npad_' + i, key: key, s: 0, r: row, b: col});
    else         ENTRIES.push({id: 'npad_' + i, key: key, s: 1, r: row, b: col - 8});
}

// --- Attribute buttons (matrix left column), keys 1-10 ---
// keys 1-5 -> set0 row11 bits 4..0 ; keys 6-10 -> set0 row12 bits 5..1
var ATTR = [
    [1, 0, 11, 4], [2, 0, 11, 3], [3, 0, 11, 2], [4, 0, 11, 1], [5, 0, 11, 0],
    [6, 0, 12, 5], [7, 0, 12, 4], [8, 0, 12, 3], [9, 0, 12, 2], [10, 0, 12, 1]
];
for (var j = 0; j < ATTR.length; j++) {
    var e = ATTR[j];
    ENTRIES.push({id: 'attr_' + e[0], key: e[0], s: e[1], r: e[2], b: e[3]});
}

// --- Mutator buttons (matrix right column), keys 187-196 ---
// keys 187-191 -> set1 row11 bits 1..5 ; keys 192-196 -> set1 row12 bits 0..4
var MUT = [
    [187, 1, 11, 1], [188, 1, 11, 2], [189, 1, 11, 3], [190, 1, 11, 4], [191, 1, 11, 5],
    [192, 1, 12, 0], [193, 1, 12, 1], [194, 1, 12, 2], [195, 1, 12, 3], [196, 1, 12, 4]
];
for (var j = 0; j < MUT.length; j++) {
    var e = MUT[j];
    ENTRIES.push({id: 'mut_' + e[0], key: e[0], s: e[1], r: e[2], b: e[3]});
}

// --- Mix target strip, keys at multiples of 11 plus offset ---
// first 8 -> set0 row10 bits 0..7 ; last 8 -> set1 row10 bits 0..7
var MIX_KEYS = [21, 32, 43, 54, 65, 76, 87, 98, 109, 120, 131, 142, 153, 164, 175, 186];
for (var j = 0; j < MIX_KEYS.length; j++) {
    var k = MIX_KEYS[j];
    if (j < 8) ENTRIES.push({id: 'mix_' + k, key: k, s: 0, r: 10, b: j});
    else       ENTRIES.push({id: 'mix_' + k, key: k, s: 1, r: 10, b: j - 8});
}

// --- Circle / transport / big-knob / scale / chord (cm map from web_gui.html) ---
var CM = {
    // inner ring notes + scale nav
    225: [0, 15, 0], 217: [0, 15, 1], 208: [0, 15, 2], 209: [0, 15, 3],
    210: [0, 15, 4], 211: [0, 15, 5], 212: [0, 15, 6], 221: [0, 15, 7],
    222: [1, 15, 0], 230: [1, 15, 1], 239: [1, 15, 2], 238: [1, 15, 3],
    237: [1, 15, 4], 236: [1, 15, 5], 235: [1, 15, 6], 226: [1, 15, 7],
    // zoom cluster (centre)
    218: [0, 16, 0], 219: [0, 16, 1], 227: [0, 16, 2], 228: [0, 16, 3],
    220: [0, 16, 4], 229: [0, 16, 5],
    // transport + scale (outer, set1 row14)
    231: [1, 14, 0], 232: [1, 14, 1], 241: [1, 14, 2], 240: [1, 14, 3],
    250: [1, 14, 4], 249: [1, 14, 5], 248: [1, 14, 6], 247: [1, 14, 7],
    // channel / chain / record (outer, set0 row14)
    201: [0, 14, 0], 202: [0, 14, 1], 203: [0, 14, 2], 204: [0, 14, 3],
    205: [0, 14, 4], 213: [0, 14, 5], 214: [0, 14, 6], 223: [0, 14, 7],
    // big knob numerics (outer, set0 row13)
    215: [0, 13, 0], 216: [0, 13, 1], 206: [0, 13, 2], 207: [0, 13, 3],
    197: [0, 13, 4], 198: [0, 13, 5], 199: [0, 13, 6], 200: [0, 13, 7],
    // scale + program + tempo (outer, set1 row13)
    246: [1, 13, 0], 245: [1, 13, 1], 244: [1, 13, 2], 243: [1, 13, 3],
    242: [1, 13, 4], 234: [1, 13, 5], 233: [1, 13, 6], 224: [1, 13, 7],
    // align + chord size (set1 row16)
    251: [1, 16, 7], 252: [1, 16, 6], 253: [1, 16, 5], 254: [1, 16, 4],
    255: [1, 16, 3], 256: [1, 16, 2], 257: [1, 16, 1], 258: [1, 16, 0]
};
var cmKeys = Object.keys(CM);
for (var j = 0; j < cmKeys.length; j++) {
    var k = parseInt(cmKeys[j], 10);
    var p = CM[k];
    ENTRIES.push({id: 'k_' + k, key: k, s: p[0], r: p[1], b: p[2]});
}

// Diff caches: only emit when state changes.
var prevLit = {};
var prevColor = {};
var frameCount = 0;

module.exports = {

    oscInFilter: function (data) {
        var address = data.address;
        var args = data.args;

        if (address === '/mir') {
            var raw = args && args.length ? args[0].value : null;
            if (!raw) return false;

            // Engine sends /mir as an OSC blob (binary). Buffer extends
            // Uint8Array so we index it directly.
            var mir;
            if (typeof raw === 'string') {
                if (raw.length < 340) return false;
                mir = new Array(170);
                for (var n = 0; n < 170; n++) mir[n] = parseInt(raw.substr(n * 2, 2), 16);
            } else {
                if (!raw.length || raw.length < 170) return false;
                mir = new Array(170);
                for (var n = 0; n < 170; n++) mir[n] = raw[n];
            }

            // Every 30 frames, force-push all LEDs (safety net in case
            // individual updates were lost or the client reconnected).
            var forceAll = (frameCount % 30) === 0;
            frameCount++;

            for (var i = 0; i < ENTRIES.length; i++) {
                var e = ENTRIES[i];
                var base = e.s * 85 + e.r * 5;
                var red = (mir[base + 1] >> e.b & 1) ? 2 : 0;
                var grn = (mir[base + 2] >> e.b & 1) ? 4 : 0;
                var lit = (red + grn) > 0 ? 1 : 0;

                if (forceAll || lit !== prevLit[e.id]) {
                    receive('/key', e.key, lit);
                    prevLit[e.id] = lit;
                }
            }

            return false;
        }

        return data;
    }

};
