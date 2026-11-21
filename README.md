# Pianola

**Pianola** turns the M-VAVE FM-1 into a modern pocket player grand piano you perform with your fingertips. Pick any of **87 classic piano rolls** — Debussy, Satie, Chopin, Beethoven, Bach, Mozart, Schubert, Schumann, Brahms, Grieg, and Scott Joplin — and **tap any key** to play every chord, melody, and arpeggio with **your own timing, rubato, and finger velocity**.

Switch **Pianola** off with a single tap of `ARP` whenever you want to break out into a **Basic 27-key Grand Piano**, or jump into one-finger **Chord Mode** (`LFO`) and a **4-slot Roll Recorder** (`REC`). Every single note is synthesised in real time across both CPU cores at 396 MHz — zero audio samples, zero ROM wavetables.

![Pianola Tap-Play Stage view](docs/card.png) ![Harp String Resonance view](app/docs/img/scene_grand.png) ![Chord Mode view](app/docs/img/deck_keys_chord.png)

- **Install on The Firmware Hub** (one-click Web MIDI): <https://thefirmwarehub.com/firmware/clairpiano/>
- **Read the manual**: [app/docs/MANUAL.md](app/docs/MANUAL.md)
- **Download signed release packages**: [GitHub Releases](https://github.com/tokumine/fm1-claire-piano/releases)

> **Hardware notes:** Supports both FM-1 flash revisions (`0x856014` and newer `0xB36014` `UC25HQ80`). Runs PLL1 at `cpu_clock = 396` MHz with `cpu2_fallback = true` so all 14 polyphonic voices (224 stiff-string partials + 28 longitudinal phantom partials + 16-mode bridge resonator + sympathetic strings + 4-line Hadamard FDN hall) render at ~60% CPU with zero xruns and clean headroom on headphones.
>
> **Storage compatibility:** Pianola keeps its settings and 4 user piano rolls in its own flash layout and does not read projects or samples created by other firmwares (back up other firmwares' work before switching). Leaving Pianola back to stock V15 or any other custom firmware works normally via Web MIDI or M-Flasher.

## Play the Pianola — Right Out of the Box

1. **Tap-Play Pianola (Default ON, Stopped):** Turn on the FM-1, pick any of the 87 rolls with `PRESET` (or a composer shelf with `ALGO`), and **tap any key on the keyboard**. Each strike plays the next phrase or chord in the score at **your exact rhythm and touch sensitivity** (soft taps whisper `pp`, firm strikes bloom `ff`) while automatic harmonic pedalling breathes underneath. Hold a key to sustain; release to damp.
2. **Live Punch-In FX & Pianist Flourishes (During `PLAY`):** Hit `PLAY` to run the roll hands-free — while playing in Pianola mode, the 27 keys transform into a **Pocket-Operator-style live performance deck**:
   - **Left 12 keys (`F3..E4`, Time & Dynamics FX):** `HALF 0.5X` (slow half-time), `BRAKE` (gradual mechanical rubato slowdown), `FAST 1.5X` (double-time push), `FREEZE` (hold pedal & pause score at exact splice point), `RPT 1/2` / `RPT 1/4` / `RPT 1/8` (beat-synced roll loop that snaps cleanly back to the splice point on release), `STACCATO` (dry crisp articulation), `UNA FELT` (soft felt hush), `SWELL FF` (dramatic crescendo), `OCT -1` (deep bass octave), `OCT +1` (sparkling treble melody).
   - **Right 15 keys (`F4..G5`, In-Key & In-Tempo Pianist Flourishes):** `BASS OCT`, `PEDAL 5TH`, `ARP UP`, `ARP DN`, `HARP RUN`, `ROLL CHD`, held `TRILL`, `TURN`, `MORDENT`, held `TREMOLO`, `SHIMMER`, `GRACE 9`, `OCT MEL`, `CADENZA`, and `GLISS` — all locked to the roll's live detected harmony and tempo so you can ornament any bar and drop right back into the score when released.
3. **Break Out to Basic Piano (`ARP`):** Tap `ARP` once to disengage Pianola mode and play a pure, velocity- and pressure-sensitive **27-key Concert Grand Piano** (`OCT-` / `OCT+` spans all 88 keys). Tap `ARP` again to snap right back into Pianola mode.
4. **One-Finger Chord Mode (`LFO`):** Press `LFO` twice to play concert-voiced piano chords (`CLOSE`, `OPEN`, or `ROLL` arpeggios) with left-hand harmony modifiers (`5`, `sus2`, `6`, `sus4`, `7`, `dim`, `9`).
5. **Record Your Own Piano Rolls (`REC` + `SAVE`):** Record up to 4 custom rolls in `USER SLOTS`, save to flash, and tap-play or remix your own compositions just like the built-in library.

## Quick Controls

| Control | Function |
|---|---|
| **Keys (27)** | **Stopped:** Tap-Play Pianola (tap any key to advance score at your timing & touch) · **Playing:** Live Punch-In FX (`F3..E4`) & In-Key Pianist Flourishes (`F4..G5`) |
| **`ARP`** | Toggle **Pianola** ↔ **Basic Piano** (hold 0.45s to latch/release Sustain Pedal) |
| **`PLAY` · `SEQ`** | Start/pause roll (enables Live Punch-In FX on keys) · rewind roll to start & silence strings |
| **`PRESET` / `ALGO` / `SELECT`** | Browse 87 rolls + 4 user rolls / filter composer shelf / scrub bar backward & forward |
| **`HOME` · `SEL` · `ENV` · `LFO`** | Decks: **ROLL** (`HOME`/`FX`), **BODY** (`SEL`), **HALL** (`ENV`), **KEYS** (`LFO`; tap twice for **Chord Mode**) |
| **`EDIT`** | Cycle pixel-art views: **STAGE** (moonlit concert grand), **HARP** (88 strings + dampers), **TAPE** (dual-reel roll) |
| **`REC` · `SAVE`** | Arm/record a custom roll into User Slots 1–4 · save settings & user rolls to flash |
| **`GLO`** | Toggle Dark / OP-Paper theme (hold 1s to toggle Bluetooth MIDI; hold `REC` 1s to scan/pair BLE MIDI keyboards) |

## Building and Testing

Pianola is built on [fm1-base](https://github.com/charlesvestal/fm1-base) (`base/`, a git submodule). Clone with `--recursive`:

```sh
git clone --recursive https://github.com/tokumine/fm1-claire-piano.git
cd fm1-claire-piano
./build.sh                                                  # app/build/pianola.fwsc
sh tests/run_tests.sh                                       # OTA check, update loader, host simulator scenario
./build.sh --web                                            # browser WebAssembly emulator in app/build/emu/
python3 base/tools/fm1_install.py app/build/pianola.fwsc
```

Releases are built and signed with Sigstore build provenance on GitHub Actions via [`zvenson/fm1-build@v1`](https://github.com/zvenson/fm1-build).

## Credits & Licence

Free software under **Mozilla Public License 2.0** (`MPL-2.0`); see [LICENSE](LICENSE) and [LICENSING.md](LICENSING.md). Built on [fm1-base](https://github.com/charlesvestal/fm1-base) by Charles Vestal and [felucca-bsp](https://github.com/hugelton/felucca-bsp) from [Felucca](https://github.com/hugelton/Felucca) by Leo Kuroshita (Hügelton Instruments). Built-in classical piano rolls are strictly public-domain editions from the [Mutopia Project](https://www.mutopiaproject.org) and Craig Stuart Sapp's Humdrum encodings of 1915 public-domain editions. M-VAVE and FM-1 are trademarks of their respective owners; Pianola is independent firmware and not affiliated with M-VAVE.
