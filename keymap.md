# Octopus Key & OSC Index Map

## OSC Messages

### Input — Index-based (existing)

| Type | OSC Address | Args |
|------|-------------|------|
| Key press | `/key` | `i` index, `i` press(1/0) |
| Rotary | `/rotary` | `i` index, `i` direction(1=dec,2=inc) |
| Transport | `/transport` | `s` cmd("start"/"stop"/"pause"/"continue") |
| Tempo | `/tempo` | `i` bpm |
| Zoom | `/zoom` | `i` level |
| Clock source | `/clocksource` | `i` (0=OFF,1=INT,2=EXT) |
| Save | `/save` | (none) |
| Quit | `/quit` | (none) |

### Input — Name-based (new)

| Type | OSC Address | Args |
|------|-------------|------|
| Key press | `/key/<NAME>` | `i` press(1/0) |
| Rotary | `/rotary/<NAME>` | `i` direction(1=dec,2=inc) |

Examples:
- `/key/REC 1` — arm record
- `/key/STP 1` — stop
- `/key/PAGE 1` — press PAGE zoom
- `/rotary/VEL 2` — increment velocity rotary
- `/rotary/TPO 1` — decrement tempo

### Output (engine → browser)

| Address | Args | Description |
|---------|------|-------------|
| `/mir` | `s` (170-byte hex string) | Full MIR LED frame, blink pre-applied |

### WebSocket JSON (browser → bridge)

Index-based:
```json
{"type":"key","index":231,"press":true}
{"type":"rotary","index":1,"direction":2}
```

Name-based (JavaScript helpers):
```javascript
skeyName("STP", true);    // → sends key 231 press
srotName("VEL", 2);       // → sends rotary 1 inc
```

Name maps: `keyNames` and `rotNames` objects in web_gui.html.

---

## Rotary Encoders

| Index | Name | OSC Address | Panel Location |
|-------|------|-------------|----------------|
| 0 | TPO | `/rotary/TPO` | Top right of circle |
| 1 | VEL | `/rotary/VEL` | Track 9 (top, left rotary) |
| 2 | PIT | `/rotary/PIT` | Track 8 |
| 3 | LEN | `/rotary/LEN` | Track 7 |
| 4 | STA | `/rotary/STA` | Track 6 |
| 5 | POS | `/rotary/POS` | Track 5 |
| 6 | DIR | `/rotary/DIR` | Track 4 |
| 7 | AMT | `/rotary/AMT` | Track 3 |
| 8 | GRV | `/rotary/GRV` | Track 2 |
| 9 | MCC | `/rotary/MCC` | Track 1 |
| 10 | MCH | `/rotary/MCH` | Track 0 (bottom, left rotary) |
| 11 | R9 | `/rotary/R9` | Track 9 (top, right rotary) |
| 12 | R8 | `/rotary/R8` | Track 8 |
| 13 | R7 | `/rotary/R7` | Track 7 |
| 14 | R6 | `/rotary/R6` | Track 6 |
| 15 | R5 | `/rotary/R5` | Track 5 |
| 16 | R4 | `/rotary/R4` | Track 4 |
| 17 | R3 | `/rotary/R3` | Track 3 |
| 18 | R2 | `/rotary/R2` | Track 2 |
| 19 | R1 | `/rotary/R1` | Track 1 |
| 20 | R0 | `/rotary/R0` | Track 0 (bottom, right rotary) |

---

## Attribute Buttons (Matrix Left Column)

| Key | Name | OSC Address | Track |
|-----|------|-------------|-------|
| 1 | VEL | `/key/VEL` | 9 (top) |
| 2 | PIT | `/key/PIT` | 8 |
| 3 | LEN | `/key/LEN` | 7 |
| 4 | STR | `/key/STR` | 6 |
| 5 | POS | `/key/POS` | 5 |
| 6 | DIR | `/key/DIR` | 4 |
| 7 | AMT | `/key/AMT` | 3 |
| 8 | GRV | `/key/GRV` | 2 |
| 9 | MCC | `/key/MCC` | 1 |
| 10 | MCH | `/key/MCH` | 0 (bottom) |

## Step Pads (Matrix Grid)

Key formula: `key = 11 + col * 11 + row` (row 0=bottom, row 9=top, col 0-15)

No named OSC addresses — use index: `/key <index> <press>`

## Mix Strip (Bottom Row)

| Key | Name | OSC Address | Function |
|-----|------|-------------|----------|
| 21 | MIX | `/key/MIX` | Mix target selector |
| 32 | SEL | `/key/SEL` | Select master |
| 43 | ATR | `/key/ATR` | Mix target: Attribute |
| 54 | VOL | `/key/VOL` | Mix target: Volume |
| 65 | PAN | `/key/PAN` | Mix target: Pan |
| 76 | MOD | `/key/MOD` | Mix target: Modulation |
| 87 | EXP | `/key/EXP` | Mix target: Expression |
| 98 | U0 | `/key/U0` | Mix target: User 0 |
| 109 | U1 | `/key/U1` | Mix target: User 1 |
| 120 | U2 | `/key/U2` | Mix target: User 2 |
| 131 | U3 | `/key/U3` | Mix target: User 3 |
| 142 | U4 | `/key/U4` | Mix target: User 4 |
| 153 | U5 | `/key/U5` | Mix target: User 5 |
| 164 | MUT | `/key/MUT` | Mute master |
| 175 | EDT | `/key/EDT` | Edit master |
| 186 | ESC | `/key/ESC` | Return / Escape |

## Mutator Buttons (Matrix Right Column)

| Key | Name | OSC Address | Function |
|-----|------|-------------|----------|
| 187 | TGGL | `/key/TGGL` | Toggle |
| 188 | SOLO | `/key/SOLO` | Solo |
| 189 | CLR | `/key/CLR` | Clear |
| 190 | RND | `/key/RND` | Randomize |
| 191 | FLT | `/key/FLT` | Filter |
| 192 | RMX | `/key/RMX` | Remix |
| 193 | EFF | `/key/EFF` | Effect |
| 194 | ZOOM | `/key/ZOOM` | Zoom mutator |
| 195 | CPY | `/key/CPY` | Copy |
| 196 | PST | `/key/PST` | Paste |

## Circle — Outer Ring (32 buttons)

Going clockwise from top (slot 0 = 0° = top):

| Slot | Key | Name | OSC Address | Section |
|------|-----|------|-------------|---------|
| 0 | 215 | BK9 | `/key/BK9` | Big Knob |
| 1 | 224 | BK100 | `/key/BK100` | Big Knob |
| 2 | 233 | BK200 | `/key/BK200` | Big Knob / Clock |
| 3 | 213 | CHN | `/key/CHN` | Channel |
| 4 | 214 | FLW | `/key/FLW` | Follow |
| 5 | 243 | MY | `/key/MY` | Scale |
| 6 | 244 | PEN | `/key/PEN` | Scale |
| 7 | 245 | WHL | `/key/WHL` | Scale |
| 8 | 246 | MAJ | `/key/MAJ` | Scale |
| 9 | 242 | PGM | `/key/PGM` | Program |
| 10 | 234 | TPO | `/key/TPO` | Tempo |
| 11 | 247 | MIN | `/key/MIN` | Scale |
| 12 | 248 | DIM | `/key/DIM` | Scale |
| 13 | 250 | P4 | `/key/P4` | Transport |
| 14 | 241 | P1 | `/key/P1` | Transport |
| 15 | 232 | PSE | `/key/PSE` | Transport |
| 16 | 231 | STP | `/key/STP` | Transport |
| 17 | 223 | REC | `/key/REC` | Transport |
| 18 | 240 | P2 | `/key/P2` | Transport |
| 19 | 249 | CHR | `/key/CHR` | Scale |
| 20 | 205 | CH1 | `/key/CH1` | Chain Mode |
| 21 | 204 | CH2 | `/key/CH2` | Chain Mode |
| 22 | 203 | CH3 | `/key/CH3` | Chain Mode |
| 23 | 202 | CH4 | `/key/CH4` | Chain Mode |
| 24 | 201 | BK1 | `/key/BK1` | Big Knob |
| 25 | 200 | BK2 | `/key/BK2` | Big Knob |
| 26 | 199 | BK3 | `/key/BK3` | Big Knob |
| 27 | 198 | BK4 | `/key/BK4` | Big Knob |
| 28 | 197 | BK5 | `/key/BK5` | Big Knob |
| 29 | 207 | BK6 | `/key/BK6` | Big Knob |
| 30 | 206 | BK7 | `/key/BK7` | Big Knob |
| 31 | 216 | BK8 | `/key/BK8` | Big Knob |

## Circle — Inner Ring (16 buttons)

| Key | Name | OSC Address | Note |
|-----|------|-------------|------|
| 225 | F# | `/key/F#` | F# |
| 226 | G | `/key/G` | G |
| 235 | G# | `/key/G#` | G# |
| 236 | A | `/key/A` | A |
| 237 | A# | `/key/A#` | A# |
| 238 | B | `/key/B` | B |
| 239 | CUP | `/key/CUP` | C (octave up) |
| 230 | CAD | `/key/CAD` | Scale CAD |
| 222 | SSEL | `/key/SSEL` | Scale SEL (≠ mix SEL=32) |
| 221 | SMOD | `/key/SMOD` | Scale MOD (≠ mix MOD=76) |
| 212 | C | `/key/C` | C |
| 211 | C# | `/key/C#` | C# |
| 210 | D | `/key/D` | D |
| 209 | D# | `/key/D#` | D# |
| 208 | E | `/key/E` | E |
| 217 | F | `/key/F` | F |

## Circle — Center Cluster

| Key | Name | OSC Address | Function |
|-----|------|-------------|----------|
| 218 | GRID | `/key/GRID` | Zoom: Grid |
| 219 | PAGE | `/key/PAGE` | Zoom: Page |
| 220 | TRK | `/key/TRK` | Zoom: Track |
| 227 | STEP | `/key/STEP` | Zoom: Step |
| 228 | MAP | `/key/MAP` | Zoom: Map |
| 229 | PLAY | `/key/PLAY` | Zoom: Play |

## Circle — Top Right (Side Bow)

| Key | Name | OSC Address | Function |
|-----|------|-------------|----------|
| 251 | ALN | `/key/ALN` | Align |
| 258 | CHORD0 | `/key/CHORD0` | Chord size 0 (root only) |
| 257 | CHORD1 | `/key/CHORD1` | Chord size 1 |
| 256 | CHORD2 | `/key/CHORD2` | Chord size 2 |
| 255 | CHORD3 | `/key/CHORD3` | Chord size 3 |
| 254 | CHORD4 | `/key/CHORD4` | Chord size 4 |
| 253 | CHORD5 | `/key/CHORD5` | Chord size 5 |
| 252 | CHORD6 | `/key/CHORD6` | Chord size 6 (max) |

## Name Collision Notes

| Name | Key 1 | Key 2 | Disambiguation |
|------|-------|-------|----------------|
| SEL | 32 (mix strip) | 222 (inner ring) | `SEL`=32, `SSEL`=222 |
| MOD | 76 (mix strip) | 221 (inner ring) | `MOD`=76, `SMOD`=221 |
| ZOOM | 194 (mutator) | — (center uses GRID/PAGE/etc.) | `ZOOM`=194 |
| TPO | 234 (key) | 0 (rotary) | `/key/TPO` vs `/rotary/TPO` |

## LED-Only (No Key)

| LED | Name |
|-----|------|
| 259 | EDIT_INDICATOR |
| 260 | MIX_INDICATOR |

## MIR LED Format

170 bytes = 2 sets × 17 rows × 5 bytes.

Each row has 5 bytes:
- Byte 0: Blink plane (bit per LED)
- Byte 1: Red plane
- Byte 2: Green plane
- Byte 3: Shine Green plane
- Byte 4: Shine Red plane

Access: `mirData[set * 85 + row * 5 + byte]`, bit `b` = LED position 0-7.

Blink is pre-applied by the firmware: when blinker is off, red and green bits are cleared where the blink bit is set. The browser receives the final state and just displays red/green/yellow/off.
