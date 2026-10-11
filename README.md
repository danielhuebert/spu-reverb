# PSX SPU Reverb — `psx rvb`

A faithful port of the **PlayStation 1 SPU reverb** hardware block to the
**Korg minilogue xd** (logue SDK v1, `revfx`).

It recreates the exact signal chain from the SPU reverb hardware as documented
in the Nocash PSX specification:

```
input gain → same/different-side IIR reflections → 4-tap comb (early echo)
           → 2 all-pass filters (late reverb) → output
```

All reflection delays, comb taps and all-pass stages share one circular buffer
with a single advancing write pointer, exactly like the SPU's reverb work area.

## The "PSX grit"

On real hardware the reverb does **not** run at the full 44.1 kHz system rate —
the SPU downsamples to **22050 Hz**, runs the reverb there, and upsamples back.
This band-limiting (everything above ~11 kHz is gone) plus the internal 16-bit
saturation is what gives PS1 reverb its unmistakable dark, slightly grainy tail.

This unit reproduces that: the reverb core runs at a true **22050 Hz**, with the
original (unscaled) register offsets, wrapped in a 48 kHz ↔ 22050 Hz resampler
(boxcar decimation in, linear interpolation out) plus 16-bit output saturation.
The dry path stays at full 48 kHz bandwidth — only the wet signal is "gritty".

## Controls

| Control       | Function                                                        |
|---------------|-----------------------------------------------------------------|
| **TIME**      | Selects the reverb preset (small room → large hall → space echo)|
| **DEPTH**     | Wet/dry mix                                                     |
| **SHIFT+DEPTH** | Decay / tail length (scales `vWALL`; maximum = authentic)     |

### Presets (on TIME, ascending by length)

Room · Studio Small · Studio Medium · Studio Large · Hall · Half Echo · Space Echo

These are Sony's original register sets from the Nocash spec. The SPU reverb is
fully parametric — the presets are hand-tuned, stable points in the parameter
space, not a fixed list baked into the hardware.

## Install

Grab `spu_reverb.mnlgxdunit` from the [Releases](../../releases) page, drop it
onto a free *User Reverb* slot in the **minilogue xd Sound Librarian**, and
transfer it to the instrument. Firmware ≥ 2.00 is required.

On the device: enable the REVERB section, then turn TIME to pick a room.

## Build

Requires the [logue SDK](https://github.com/korginc/logue-sdk) with submodules
and the Docker build image. Clone this repo into the platform folder:

```sh
git clone <this-repo> logue-sdk/platform/minilogue-xd/spu-reverb
cd logue-sdk
docker/run_cmd.sh build minilogue-xd/spu-reverb
```

The resulting `spu_reverb.mnlgxdunit` is written into the project folder.

## Credits

- Reverb algorithm & register values: Martin Korth's Nocash PSX specification
  and the [psx-spx](https://github.com/psx-spx/psx-spx.github.io) project.
- Built with the Korg logue SDK (BSD 3-Clause).
