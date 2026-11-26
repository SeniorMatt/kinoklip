# Plugin system: design

The base of Kinoklip's plugin system (plugin API 1). How to write and install plugins:
[writing-plugins.md](writing-plugins.md), [installing-plugins.md](installing-plugins.md), and the
short prompt for an AI assistant, [llms.txt](llms.txt).

## Goals

1. **Easy to use.** A plugin effect is in the Effects library, the Inspector, the keyframes, the
   undo history and the project file exactly like a built-in one.
2. **Easy to install.** Preferences > Plugins, a `.kkplugin` file dropped on the window, a folder
   copied into `~/.local/share/Kinoklip/plugins`, or `kinoklip plugin install`.
3. **Easy to write and to change.** A plugin is a folder of text files: `plugin.json` and a GLSL
   file. Built-in effects that are plain shaders have a plugin form to copy and change
   (`plugins/examples`, `plugin.copy_builtin`); Reload shows an edited shader at once.
4. **Doable by an AI.** Everything an assistant needs is in [writing-plugins.md](writing-plugins.md)
   and the commands `plugin.create`, `plugin.validate` (errors with file and line, for OpenGL 3.3
   and OpenGL ES 3.0) and `plugin.render_preview` (a picture of the result). Presets and scripts
   call the same command layer the CLI and MCP use.
5. **Only open source is bundled** (GPL-compatible licences); users install what they like.

## Decisions

| Question | Decision | Why |
|---|---|---|
| Plugin format | folder with `plugin.json` (or the same folder zipped as `.kkplugin`) | text only, diffable, an AI writes it without tools; the zip reader and writer are 400 lines in `src/plugins/ZipArchive.cpp` (no new dependency) |
| Where | user: `<AppData>/plugins` (`~/.local/share/Kinoklip/plugins`, `KK_PLUGIN_USER_DIR` overrides); bundled: `share/kinoklip/plugins` next to the program (`KK_PLUGIN_BUNDLED_DIR`); examples: `share/kinoklip/plugin-examples` | XDG; the AppImage and Flatpak install the same share folder |
| Ids | `plugin:<plugin id>/<item id>` is the descriptor id stored in projects | cannot clash with built-in ids; the prefix tells the renderer to ask the plugin registry |
| Effects and transitions | GLSL fragment shader; the host adds the version line, a prelude (standard uniforms, one `u_<key>` uniform per parameter) and `main()`; the author writes `effect(uv)`, `apply(c)` (colour) or `transition(uv)` | one convention for desktop GL 3.3 and OpenGL ES 3.0, same as the built-ins (spec 155); gl-transitions (MIT) shaders work with their own convention |
| Parameters | number, integer, boolean, color, choice (string for presets); numbers and integers are keyframable | maps one to one to the built-in `ParamDescriptor`, so the Inspector, keyframes and the Compositing node need no plugin code |
| Registration | the plugin registry adds `EffectDescriptor`s (category video effect or transition) and a Compositing node per effect; the renderer builds the shader when first used and drops it when the registry generation changes (reload) | a plugin is a descriptor plus a shader: nothing else knows the difference |
| Missing plugin | the effect stays on the clip with its parameters and keyframes, the Inspector says "Missing plugin", it renders as bypass (a transition as a plain dissolve) | opening a project on another computer never loses work |
| Presets | a recipe: a list of command-layer calls with `$clip`, `$start`, `$end`, `$duration`, `$playhead` and `$last_effect` placeholders, run as ONE undo step | reuses the command layer instead of a second format; covers effect stacks, animations (keyframes) and titles; browsable in Effects library > Toolbox > Presets, dragged onto a clip or double-clicked |
| Scripts | external `python3` in isolated mode, JSON lines over stdin/stdout to the command layer (`resources/plugin-sdk/kinoklip.py`) | nothing to embed (QJSEngine would add Qt Qml); Python is what assistants write best; the bridge enforces `project.read` / `project.edit` |
| Safety | shaders cannot touch files; presets only call commands; a plugin with scripts is installed **switched off**, enabling it shows what it may do, an assistant on the live connection cannot enable it (`needs_consent`), nothing downloaded runs by itself, archives are checked (names, sizes, checksums) | scripts are programs: they run with the user's rights, the permissions are declared (and the project ones enforced), not a sandbox |
| Validation | `plugin.json` rules in code (every problem with its path) and a JSON Schema file (`plugin.schema.json`) for editors and AIs; the shader lint and a compile on a throw-away GL context | errors with line numbers an AI can act on |
| Hot reload | Reload (Preferences > Plugins, `plugin.reload`) rescans, re-registers, bumps a generation that makes every renderer rebuild plugin shaders and changes the render cache key | the edit-run loop of a shader author takes a second |

## What is where

| Path | What |
|---|---|
| `src/plugins/` | manifest parser and validator, registry (scan, install, enable, hot reload), `.kkplugin` zip reader and writer, preset recipes, script runner |
| `src/render/GlPluginEffect.*`, `PluginShader.*` | the GPU effect and transition made of a plugin shader, the prelude, lint and compile-log parser |
| `src/render/PluginPreview.*`, `PluginValidate.*` | draw an effect on a picture without a timeline; check a plugin folder completely |
| `src/app/commands/PluginCommands.cpp` | the `plugin.*` commands (CLI and MCP) |
| `src/app/PluginHost.*`, `src/ui/dialogs/PluginsPage.*` | the window's side: presets, scripts menu, drops; the Plugins page |
| `plugins/examples/` | template plugins (ports of Invert, Posterize and Dip to Color, an animation pack, a script) |

## Limits of API 1 (what the next API versions add)

- One input picture for effects (no second layer, no mask or matte inputs), one pass, no state
  between frames, no on-screen controls: a plugin with points to drag in the viewer (a puppet pin
  warp, a tracker) needs a `point` parameter type with a viewer handle first. Until then such
  effects take their points as number parameters.
- Effects run on the GPU only: the CPU fallback renderer shows a plugin effect as bypass.
- A preset is applied to clips, at the playhead or at an edit point (`edit_point`, a transition
  preset: `$left`, `$right`, `$edit`; spec 160).
- Plugin text is not translated: labels are what the author wrote.
- Audio plugins are LV2 and CLAP, hosted as in the next section (spec 159).

## Ecosystems of other programs (step 6: plan)

The native format is the base. Plugins of the free ecosystems come next, as hosts inside Kinoklip.
Nothing below is implemented yet; the order is by value over work.

### 1. gl-transitions and shader packs (done: the base and the bundled pack, spec 160)

The pack (`plugins/bundled/dev.kinoklip.gl-transitions` and `-2`: a plugin holds at most 200 transitions) has 394 of the 401 transitions of the
collection (the others: BSD or unnamed licence, a link to Shadertoy, texture inputs), made by
`scripts/convert_gl_transitions.py` and installed in `share/kinoklip/plugins`; it shows in
Effects library > Transitions > GL Transitions.

The `transition(uv)` convention is the one of [gl-transitions](https://gl-transitions.com) (MIT).
A gl-transitions file without parameters works as it is, with a `plugin.json` around it. One with
`uniform` parameters needs its declarations deleted and the names changed to `u_<name>` (the host
declares them from the manifest; the defaults in its comments become the manifest defaults): a
mechanical edit an assistant can make. A pack converted this way can be bundled (MIT is
GPL-compatible, keep its licence file). Not bundled: Shadertoy shaders (CC BY-NC-SA by default: not GPL-compatible).

### 2. OpenFX (video effects): done for video effects (spec 158, [openfx.md](openfx.md))

What was built: `src/ofx/` (the host: property sets, parameter / image effect / memory / message /
multi-thread / progress / timeline suites, Describe and DescribeInContext for the Filter and General
contexts, CreateInstance, IsIdentity, Render on the CPU), `kinoklip-ofx-probe` (loads one bundle in
its own process), `src/plugins/OfxRegistry.*` (scan folders, probe, cache, bad-plugin list, one
`EffectDescriptor` per plugin with the id `ofx:<identifier>`, group OpenFX) and
`src/render/GlOfxEffect.*` (GPU layer to float RGBA, plugin, back to the GPU). The decisions:
the probe is a separate small program (not the editor with a flag) so tests and packaging need no
GUI; the description of every bundle is cached by binary size and date; a plugin is loaded into the
editor only after its probe worked, and a mark file catches a crash in its first frame; pixels are
premultiplied float RGBA (8 bit only for plugins without float); points and sizes are stored as
fractions of the clip size. Spec 160 added the transition context and regions of definition larger
than the clip. Still to do: several inputs and masks, overlays, OpenGL rendering. The plan below is
kept for what is not done.

- Headers: [OpenFX](https://openfx.readthedocs.io) (BSD-3-Clause, Academy Software Foundation).
  Reference hosts to study: Natron (GPL-2.0, the complete free host), and the openfx-misc test host.
- Plugins to support first: [openfx-misc](https://github.com/NatronGitHub/openfx-misc)
  (GPL-2.0-or-later: ~100 filters, keyers, transforms), openfx-arena (GPL-2.0-or-later), G'MIC-OFX
  (CeCILL, GPL-compatible, a thousand filters) and the open-source commercial-free packs of
  Natron. Licences must be checked per pack before bundling; installing them is the user's choice.
- Host work, in order: (1) scan `/usr/OFX/Plugins`, `~/.OFX/Plugins` and `OFX_PLUGIN_PATH` for
  `*.ofx.bundle`, load with `dlopen`, `OfxGetPlugin`; (2) property, parameter, memory, thread,
  multi-thread and message suites; (3) image effect suite for the `filter` and `general` contexts
  with float RGBA buffers: the GPU layer is read back, the plugin renders on a worker thread, the
  result is uploaded (cached per frame); (4) descriptors from the plugin's parameters, so
  the Inspector and keyframes work (host evaluates `kOfxParamPropAnimates` parameters per frame);
  (5) the transition context. A bad plugin must not take the editor down: run in a helper process.
- Id scheme: `ofx:<plugin identifier>`; the missing-plugin behaviour of the base already applies.

### 3. LV2 and CLAP (audio effects): done (spec 159, [audio-plugins.md](audio-plugins.md))

What was built: `src/audioplug/` (the hosts: `Lv2Host` through lilv with urid / options / buf-size
features, `ClapHost` on the vendored MIT headers, a `ChannelAdapter` that fits mono, stereo and
wider plugins to the mixer's interleaved stereo, `kinoklip-audio-probe` that loads one bundle in its own
process), `src/plugins/AudioPluginRegistry.*` (scan folders, probe up to three at once in the
background, cache, bad-plugin list, one `EffectDescriptor` per plugin with the id
`audio_lv2:<URI>` / `audio_clap:<id>`) and `dsp::PluginClipEffect` in `render/AudioClipEffects.cpp`
(a `ClipEffect` like the built-in ones, so clip effects, track effects, automation, latency
compensation, tails, export and undo all work unchanged). The decisions: the effect id starts with
`audio_` because the mixer recognises audio effects by it; the plugin is looked up lazily by
registry generation so a missing plugin is a bypass that comes back by itself; LV2 `lilv` is an
optional build dependency (without it LV2 is reported as unavailable) while CLAP needs only headers.
Not done: plugin windows, MIDI and instruments, state, side-chain, surround.

- Packs: LSP plugins (LGPL-3.0, LV2 + CLAP), Calf (LGPL-2.1), x42-plugins (GPL-2.0-or-later),
  ZAM (GPL-2.0-or-later), Dragonfly reverb (GPL-3.0), Airwindows (MIT). A bundled pack needs its
  licence file in the bundle (`scripts/package_linux.sh`, `KK_LV2_BUNDLE_DIR` / `KK_CLAP_BUNDLE_DIR`).

### 4. Resolve Fusion macros and fuses

Fusion `.setting` macros and Lua fuses are not read (their format is Blackmagic's). The way over is
writing the effect as a native plugin: a Fusion fuse such as PuppetPin (MIT) is a GLSL shader plus
parameters here; with point parameters (API 2) its viewer handles come with it.
