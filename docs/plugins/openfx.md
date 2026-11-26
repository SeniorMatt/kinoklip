# OpenFX plugins

Kinoklip is an [OpenFX](https://openfx.readthedocs.io) host for video effects. Free OpenFX packs such
as [openfx-misc](https://github.com/NatronGitHub/openfx-misc) (the plugins of Natron) show up in the
Effects library, folder **OpenFX**, and work like built-in effects: Inspector, keyframes, undo,
project files, proxies, export, the Compositing page. (Native Kinoklip plugins are a separate thing:
[installing-plugins.md](installing-plugins.md).)

## Install a pack

An OpenFX plugin is a folder ending in `.ofx.bundle` with a binary at
`Contents/Linux-x86-64/<name>.ofx` (`Contents/Linux-arm-64/` on ARM64). Kinoklip looks for bundles in:

| Folder | Used for |
|---|---|
| `~/.local/share/Kinoklip/ofx` | your plugins (Preferences > Plugins > Open OpenFX Folder) |
| `/usr/OFX/Plugins` | the standard system folder (what a distribution package of a pack uses) |
| `~/.OFX/Plugins` | another common per-user folder |
| every folder in the environment variable `OFX_PLUGIN_PATH` (separated by `:`) | anything else |
| `share/kinoklip/ofx` next to the program | bundles shipped with Kinoklip: the dev AppImage carries openfx-misc (built by `scripts/build_openfx_misc.sh`); other builds none |

Copy the bundle folder into one of them and press **Reload** in Preferences > Plugins. Distributions
package some packs; on Arch Linux and Debian-based systems look for `openfx-misc`. Kinoklip has no
download button for packs and never downloads plugins on its own.

**Flatpak**: the sandbox does not see `/usr/OFX/Plugins` of your system. Copy the bundle into
`~/.var/app/org.kinoklip.Kinoklip/data/Kinoklip/ofx` instead. A plugin that needs libraries the
Flatpak runtime does not have will not load (it goes on the bad-plugin list with the reason).
**AppImage**: it carries its own system libraries; a plugin built against a newer system may not
load there. Both cases show up as a bad plugin, never as a crash.

Folders one level down are scanned too, so a pack unpacked into a folder of its own works.

## Which free packs work

| Pack | Licence | Status in Kinoklip |
|---|---|---|
| [openfx-misc](https://github.com/NatronGitHub/openfx-misc) (filters, keyers, transforms, colour tools) | GPL-2.0-or-later (the OpenFX headers inside it are BSD-3-Clause) | works. Checked 2026-11-25 with the build of commit a047cbd (Misc.ofx.bundle, CImg and Shadertoy parts not built): 185 plugin entries in the bundle, 113 effects offered (the rest are older versions of the same plugins, generators, transitions, several-input merges, retimers and plugins that need parametric parameters), 112 of them render with their default values (the keyer PIK needs its optional inputs), Multiply and Saturation give the expected pixels. Size: 31 MB binary (4.6 MB compressed) and 1.3 MB of icons |
| [openfx-gmic](https://github.com/NatronGitHub/openfx-gmic) (G'MIC filters, 1200 plugins in one bundle) | dual: CeCILL-C or CeCILL v2.0 (the CeCILL v2.0 option is GPL-compatible); some community filters inside carry other licences | loads like any bundle; a Filter-context plugin works. Not tested here (its build takes G'MIC's source and a long compile) |
| openfx-arena (text, particles, ...) | GPL-2.0-or-later | not tested; plugins that need more than one input or the OpenGL suite do not run |
| Natron's other free packs, commercial plugins you own | their own | any plugin that follows the rules below can work; Kinoklip does not check licences of what you install |

Only the **Filter** context (one input, one output) and the **General** context where the only
required input is "Source" are used. Generators, transitions, retimers, paint plugins, plugins with
several required inputs, plugins that need OpenGL rendering or on-screen overlays are listed in
Preferences > Plugins with the reason ("not used: ...") and are not offered as effects.

## What you see

- **Effects library > OpenFX**: every usable plugin of every bundle.
- **Inspector**: the plugin's parameters, with keyframes. Numbers, integers, checkboxes, choices,
  colours (RGB / RGBA), points and sizes as X / Y numbers, text. A point or size is stored as a
  fraction of the clip size (0.5 / 0.5 is the centre) and given to the plugin in pixels. Groups,
  pages, buttons and custom parameters are not shown.
- **Preferences > Plugins**: each bundle with its plugins, versions and state. A bundle that is a
  **bad plugin** is listed with the reason and a **Retry** button. **Get Free Plugin Packs** opens
  this page.
- A project that uses a plugin which is not installed on this computer keeps the effect with all
  its settings and keyframes, the Inspector says "Missing plugin", and the picture is drawn
  without it. Install the plugin and the effect works again (the effect id is `ofx:<plugin
  identifier>`, e.g. `ofx:net.sf.openfx.GainPlugin`).

## Bad plugins

A plugin is a program that runs inside Kinoklip, so a broken one could crash the editor. Kinoklip
protects you in two steps:

1. **At scan time** every bundle is loaded and described by a separate program,
   `kinoklip-ofx-probe`, in its own process. If that process crashes, hangs (30 s limit) or fails,
   the bundle goes on the bad-plugin list with the reason and Kinoklip never loads it itself. The
   result of every scan is cached (`~/.cache/Kinoklip/ofx-scan.json`, keyed by the binary's size and
   date), so later starts are quick and only new or changed bundles are probed again. New or changed
   bundles are probed in the background, up to three at a time, so the first window never waits for
   a probe: the effects library fills when the answers come (spec 159.8).
2. **At render time** Kinoklip writes a mark before it loads a plugin for the first time and removes
   it after the plugin's first frame. If Kinoklip died in between, the next start lists that bundle
   as bad ("Kinoklip crashed the last time this plugin was loaded").

Press **Retry** after updating a plugin (or **Reload** to look at new bundles only).

## How effects are rendered

The picture of the clip (premultiplied, with the effects above in the stack applied) is read back
from the GPU as float RGBA, given to the plugin with OpenFX's bottom-left origin and premultiplied
alpha, rendered on the CPU, and uploaded again. Plugins that cannot do float pixels get 8 bit.

- Time is the clip-local frame; the frame rate is the timeline's. Keyframed parameters are read at
  the time the plugin asks for.
- **Render scale**: previews and proxies render smaller; Kinoklip passes the scale (OpenFX
  `renderScale`), the plugin scales its pixel values. A plugin that does not support
  multi-resolution may look different in a small preview than in an export.
- **Threads**: the plugin's own thread-safety flag is obeyed (unsafe plugins render one at a time,
  instance-safe ones one frame per instance, fully safe ones in parallel) and the OpenFX
  multi-thread suite splits the work over the CPU cores.
- Rendering happens on the render thread, never on the interface thread.
- A frame that fails is shown without the effect (bypass); the reason is in Preferences > Plugins.

## Transitions and effects that grow the picture

- **Transition context**: a plugin that describes the Transition context (clips `SourceFrom`,
  `SourceTo` and `Output`, a double parameter `Transition`) and no Filter or General context is a
  transition in Effects library > Transitions > OpenFX. The host sets `Transition` to the progress
  (0 to 1, after the transition's curve); the other parameters are in the Inspector. Both pictures
  are read back from the GPU like an effect's layer.
- **Region of definition**: before each frame Kinoklip asks the plugin for its region of definition
  for a source of the clip's size (`getRegionOfDefinition`) and gives the layer a transparent
  margin as wide as it reaches past the clip (the largest side, at most 1000 pixels), as the
  built-in Glow and Blur do. A blur or glow therefore no longer stops at the clip's edge.

## G'MIC-OFX

`scripts/build_gmic_ofx.sh <dir>` builds [openfx-gmic](https://github.com/NatronGitHub/openfx-gmic)
(G'MIC filters; CeCILL v2.0 or CeCILL-C, GPL-compatible) from a pinned commit; the pack test uses it
when `KK_OFX_GMIC_DIR` points at the output folder. It is not bundled: the standard library of the
GPL build contains community filters under other licences. Copy `GMIC.ofx.bundle` into
`~/.local/share/Kinoklip/ofx/` to use it.

## Not supported (yet)

- Generator, retimer and paint contexts; several required inputs; mask inputs.
- On-screen controls (overlays) in the viewer; keyframe editing by the plugin; custom and
  parametric parameters; string-choice parameters.
- OpenGL rendering and OpenCL / CUDA / Metal suites (everything runs on the CPU).
- Tiled rendering; effects that read other frames (temporal clip access) get the current frame.
- Windows and macOS binaries: the host loads Linux bundles (`Linux-x86-64`, `Linux-arm-64`).

## Build openfx-misc yourself

```
git clone --recurse-submodules https://github.com/NatronGitHub/openfx-misc.git
cd openfx-misc
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_CIMG_PLUGINS=OFF -DBUILD_SHADERTOY=OFF
cmake --build build -j4
cp -r build/Plugins/Misc.ofx.bundle ~/.local/share/Kinoklip/ofx/
```

## For plugin developers

The host is `src/ofx/OfxHost.*` (OpenFX 1.4 / 1.5 headers, `third_party/openfx`, BSD-3-Clause);
`tests/ofx/kk_test_ofx.cpp` is a complete small plugin (gain, invert, choice, point, colour, integer,
text) written against the C API only, a good starting point to test a plugin against Kinoklip.
Messages a plugin posts through the OpenFX message suite go to the Kinoklip log (category
`kk.ofx`; `QT_LOGGING_RULES="kk.ofx*=true"`). With `KK_OFX_TRACE=1` the host also prints every
property a plugin asked for and did not get, every suite it did not find and every action that
failed (run `kinoklip-ofx-probe <bundle> out.json` to see what the scan sees).
