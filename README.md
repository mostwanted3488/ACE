# ACE

One-knob saturation plugin (VST3 / AU) by **@BUYORBEALOSER** — a sniper you charge from 0 to 100 %.

- **Charge** (the AWP): drag up/down. Drive goes up to +9 / +12 / +15 dB depending on the ammo.
- **Ammo**: `SOFT` (round tanh, darker highs), `HARD` (asymmetric hard clip), `BIT` (bit and sample-rate reduction).
- **Suppressor**: dry/wet mix. **Safety**: bypass.
- **Scope** (click the scope or press `S`): live spectrum (input ghost / output red) with dancing stickmen.
  Shooting a dancer stutters his frequency band for 0.4 s.
- Skins: Stock, Black Chrome, Red Tiger, Glitch.

## Download (macOS)

Every push to `main` builds a universal (Apple Silicon + Intel) zip:
**Releases → `latest` → `ACE-mac.zip`**. Install steps: [`INSTALL-MAC.txt`](INSTALL-MAC.txt).

## Project layout

| Path | What |
|---|---|
| `Source/AceDsp.h` | Saturation engine (3 cross-faded shapers, DC blocker, mix, dancer stutter filters) |
| `Source/PluginProcessor.*` | Parameters (`charge`, `mode`, `mix`, `safety`), state, meters and spectrum taps |
| `Source/PluginEditor.*` | Native WebView showing `ui/index.html`, JS ↔ C++ bridge, FFT for the scope |
| `ui/src/template.html` | The interface (same as the validated preview) |
| `ui/src/assets`, `ui/src/fonts` | Images and fonts (Barlow Condensed, JetBrains Mono — SIL OFL) |
| `tools/build_ui.py` | Inlines assets/fonts → `ui/index.html` (run after editing the UI) |

## Build locally (Mac)

```sh
python3 tools/build_ui.py          # only if you changed ui/src
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"
cmake --build build --config Release --target ACE_VST3 ACE_AU
```

JUCE 8.0.9 is fetched automatically.
