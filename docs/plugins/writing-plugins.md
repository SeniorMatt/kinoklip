# Writing Kinoklip plugins

Everything needed to write a working plugin for Kinoklip (plugin API 1) without reading the
source: a person can follow it, and so can an AI assistant (point it at this file, or at the
shorter [llms.txt](llms.txt)). Design background: [design.md](design.md). Installing:
[installing-plugins.md](installing-plugins.md).

- [Quick start](#quick-start)
- [What a plugin is](#what-a-plugin-is)
- [The manifest, plugin.json](#the-manifest-pluginjson)
- [Effects](#effects)
- [Transitions](#transitions)
- [Presets](#presets)
- [Scripts](#scripts)
- [The loop for an AI assistant: validate and preview](#the-loop-for-an-ai-assistant-validate-and-preview)
- [Changing a built-in effect](#changing-a-built-in-effect)
- [Common errors](#common-errors)
- [Sharing a plugin](#sharing-a-plugin)

## Quick start

```sh
kinoklip plugin create --id com.example.warm --name "Warm Look" --kind effect --entry color \
    --param "amount:number:0:100:50:Amount"
# edit ~/.local/share/Kinoklip/plugins/com.example.warm/warm_look.glsl, then:
kinoklip plugin reload
kinoklip plugin validate --target com.example.warm
kinoklip plugin render_preview --plugin com.example.warm --set amount=80 --file /tmp/preview.png
```

`plugin create` writes a working plugin into your plugin folder and installs it. The effect
appears in the Effects library (Video Effects > the folder you named, or Plugins) at once. The same
commands are MCP tools (`plugin_create`, `plugin_validate`, `plugin_render_preview`, ...), see
[the loop for an AI assistant](#the-loop-for-an-ai-assistant-validate-and-preview).

## What a plugin is

A plugin is a folder. Packed as a zip file with the suffix `.kkplugin` it is one file you can send
to someone. The only required file is `plugin.json`; it names the other files (relative paths
inside the folder, no `..`).

```
com.example.warm/
  plugin.json          the manifest
  warm_look.glsl       a shader (effects and transitions)
  presets/fade.json    a recipe (presets)
  tools/mark.py        a script (scripts)
```

One plugin can hold any mix of: **effects** (a GLSL shader, applied to a clip, with parameters),
**transitions** (a GLSL shader that mixes two clips), **presets** (a recipe of commands applied to a
clip or at the playhead: effect stacks, animations, titles), **scripts** (a Python 3 program that
calls Kinoklip's commands).

Where plugins live:

| Where | What |
|---|---|
| `~/.local/share/Kinoklip/plugins/<plugin id>/` | your plugins (`KK_PLUGIN_USER_DIR` overrides the folder) |
| `<program>/../share/kinoklip/plugins/` | plugins bundled with Kinoklip (`KK_PLUGIN_BUNDLED_DIR` overrides) |
| `<program>/../share/kinoklip/plugin-examples/` | the example plugins of this repository's `plugins/examples/` |

Identifiers. A plugin has an **id** (`com.example.warm`: 2 to 64 characters, lower case letters,
digits, `.`, `-`, `_`). Each effect, transition, preset or script in it has an **item id** (lower
case letters, digits, `_`). Together they form the id stored in projects:
`plugin:com.example.warm/warm_look`. Never change an id after you gave the plugin to anyone:
projects refer to it. Effect parameters have a **key** (a letter, then letters and digits, up to 32:
`amount`, `tint2`), also never changed.

A project that uses a plugin which is not installed or not enabled keeps the effect with all its
settings and keyframes; the Inspector says "Missing plugin", the effect is skipped when rendering
(a transition renders as a plain dissolve). Installing the plugin again brings it back.

## The manifest, plugin.json

JSON, UTF-8. [plugin.schema.json](plugin.schema.json) is the JSON Schema of this file; Kinoklip
itself reports every problem with its path (`effects[0].params[1].min`) when it loads or installs
a plugin and in `plugin validate`.

Top level:

| Field | Type | Required | Meaning |
|---|---|---|---|
| `id` | string | yes | plugin id, see above |
| `name` | string | yes | name shown in the Plugins page |
| `version` | string | yes | `1.0`, `1.2.3`, `1.0.0-beta1` |
| `author` | string | yes | who wrote it |
| `license` | string | yes | SPDX id of the licence, e.g. `MIT`, `GPL-3.0-or-later` |
| `api` | integer | yes | plugin API version the plugin is written for: `1`. A higher number than the running Kinoklip knows is refused with "update Kinoklip" |
| `description` | string | no | one or two sentences |
| `homepage` | string | no | URL (shown, never fetched) |
| `permissions` | string list | no | what the plugin may do, shown before a plugin with scripts is enabled: `project.read`, `project.edit`, `files.read`, `files.write`, `network`, `processes`. Only the first two are enforced by Kinoklip (for scripts); the others are the author's honest declaration |
| `effects` | list | one of the four lists must be there | see [Effects](#effects) |
| `transitions` | list | | see [Transitions](#transitions) |
| `presets` | list | | see [Presets](#presets) |
| `scripts` | list | | see [Scripts](#scripts) |

Every item of the four lists has `id`, `name` and `description` (optional) and a `file`.

Effect item:

| Field | Type | Required | Meaning |
|---|---|---|---|
| `id` | string | yes | item id |
| `name` | string | yes | name in the Effects library and the Inspector |
| `description` | string | no | tooltip |
| `group` | string | no | Effects library folder: `blur`, `color`, `distort`, `key`, `light`, `stylize`, `time`, `temporal`, `texture`, `revival`, `sharpen`, `refine`, `transform`, or any other text for a folder of your own. Default: Plugins |
| `file` | string | yes | the GLSL file (`.glsl` or `.frag`) |
| `entry` | string | no | `main` (default): you write `vec4 effect(vec2 uv)`; `color`: you write `vec3 apply(vec3 c)` |
| `padding_px` | number | no | transparent margin (timeline pixels, 0 to 2000) the effect needs around the picture, for effects that draw outside it (shadows, glows) |
| `ported_from` | string | no | id of the built-in this is a port of (templates; `plugin copy_builtin` uses it) |
| `params` | list | no | up to 32 parameters |

Transition item: the same without `entry` and `padding_px`; `group` is one of `dissolve`, `iris`,
`motion`, `shape`, `wipe` or your own text.

Preset item: `target` (`clip`, default, `playhead` or `edit_point`), `file` (`.json`), `group` (a folder name
in the Presets list; default: the plugin's name).

Script item: `file` (`.py`), `interpreter` (`python3`, the only one), `permissions` (list, as above).

Parameter (`params` of an effect or transition):

| Field | Meaning |
|---|---|
| `key` | identifier; the shader uniform is `u_<key>` |
| `label` | name in the Inspector (default: the key) |
| `tooltip` | help text |
| `type` | `number`, `integer`, `boolean`, `color`, `choice` (`string` only for presets, a shader cannot read it) |
| `min`, `max`, `default` | required for number and integer; `min < max`, default inside; integers need whole numbers |
| `step` | slider / spin box step (default 0.01, integers 1) |
| `unit` | text after the value: `%`, `px`, `deg`, ... (display only) |
| `keyframable` | number and integer only, default `true`: the parameter can be animated with keyframes |
| `default` (boolean) | `true` or `false` |
| `default` (color) | `"#rrggbb"` or `"#aarrggbb"` (alpha first) |
| `choices` | choice only: a list of strings, or of `{"value": "x", "label": "Shown"}` |
| `default` (choice) | one of the choice values |

How a parameter reaches the shader (the host declares these uniforms; you do not):

| type | uniform | notes |
|---|---|---|
| number | `uniform float u_key;` | clamped to `min..max`, at the current frame (keyframes evaluated by Kinoklip) |
| integer | `uniform int u_key;` | |
| boolean | `uniform bool u_key;` | |
| choice | `uniform int u_key;` | the index in `choices`, 0 for the first |
| color | `uniform vec4 u_key;` | straight (not premultiplied) RGBA, 0..1, in the same encoding as the picture |

Limits: 32 parameters per effect, 200 items per list, shader files up to 256 KiB, a plugin up to
1000 files and 32 MiB.

## Effects

An effect is one fragment shader that is run for every pixel of the clip's picture. Kinoklip adds
the version line, a prelude with the standard uniforms and your parameter uniforms, and `main()`;
you write the part in the middle. The first line of your file is line 1 in error messages.

### The two entries

`entry: "main"` (default). Write:

```glsl
vec4 effect(vec2 uv)
{
    vec4 c = texture(uTex, uv);   // premultiplied RGBA of the picture at uv
    return c;                     // return premultiplied RGBA
}
```

`entry: "color"`. For effects that change each pixel's colour from itself. Write:

```glsl
vec3 apply(vec3 c)               // c: straight (not premultiplied) colour, 0..1
{
    return 1.0 - c;               // return the new straight colour
}
```

Kinoklip does what the built-in colour effects do around it: transparent pixels stay transparent,
the colour is divided by alpha before and multiplied after, and the result is clamped to 0..1.
Prefer `color` when you can: it cannot get the alpha handling wrong.

### What the shader can use

| Name | Type | Meaning |
|---|---|---|
| `uTex` | `sampler2D` | the clip's picture (after the effects above this one), premultiplied RGBA |
| `uResolution` | `vec2` | size of the picture in pixels (preview quality changes it) |
| `uScale` | `float` | picture pixels per timeline pixel: multiply a length given in timeline pixels by it |
| `uTime` | `float` | seconds since the start of the clip |
| `uFrame` | `float` | frames since the start of the clip |
| `uFps` | `float` | the timeline's frame rate |
| `uContent` | `vec4` | the picture rectangle in uv: `x0, y0, x1, y1` (it is `0,0,1,1` unless `padding_px` added a margin) |
| `u_<key>` | per type | your parameters |
| `vUv` | `vec2` | the uv of this pixel, already passed to `effect(uv)` |
| `luma(vec3)` | function | BT.709 luma of a colour |
| `straight(vec4)` | function | the straight colour of a premultiplied pixel (black where transparent) |
| `kLuma` | `vec3` | the luma weights |
| `hash01(uint)`, `hash2(ivec2, uint)`, `hashU(uint)`, `maxAbs(vec3)` | functions | a deterministic integer hash (same noise for the same pixel and seed: exports repeat exactly), and the largest absolute component |

Do not declare any of these yourself, do not use these names for your own functions or variables,
and do not define `main()`. `fragColor`, `vQuad` and `edgeCoverage()` also exist (the host's own).

### Coordinates, colour, alpha

- **uv** is the texture coordinate: `(0,0)` is the top left corner, `(1,1)` the bottom right, y
  goes down. One pixel is `1.0 / uResolution`. `texture(uTex, uv)` is bilinear and clamps at the
  edge.
- **Colour space.** The picture is gamma-encoded, display-referred (what you see), as everywhere
  in Kinoklip's compositor. Nothing is linearised for you. Work on the values as they are, or
  convert yourself: `pow(c, vec3(2.4))` to linear light and `pow(c, vec3(1.0/2.4))` back (the
  Grade nodes use gamma 2.4). Colour parameters are in the same encoding.
- **Alpha is premultiplied** in `uTex` and in what `effect()` returns: `rgb` already multiplied by
  `a`. To change colour, take `straight(t)`, change it, then `vec4(rgb * a, a)`. Returning
  `rgb > a` is out of range. The `color` entry does this for you.
- Values are 16-bit float on the way: do not rely on exact 8-bit values.
- Do not use random numbers that change per run: use the hash helpers with `uFrame` for animated
  noise so exports repeat.

### OpenGL ES 3 compatibility (required)

Kinoklip runs on desktop OpenGL 3.3 core and on OpenGL ES 3.0 (Raspberry Pi 5, ARM boards). Your
shader must compile on both. The same text is compiled with `#version 330 core` or
`#version 300 es` with `precision highp float; precision highp int; precision highp sampler2D;`.
`plugin validate` compiles for both where the machine has them and lints the rest.

- Write `1.0`, never `1` where a float is meant (`float x = 1;` and `mix(a, b, 1)` fail on ES).
  `float(u_count)` converts an int.
- No `f` suffix (`1.0f`), no `double`, no `#version`, no `#extension`, no `layout(...)`.
- Old names are gone: `texture2D`, `gl_FragColor`, `varying`, `attribute` (use `texture`, return
  the colour from `effect()`).
- **Reserved words may not be names** of variables, functions or parameters: `patch`, `sample`,
  `subroutine`, `common`, `partition`, `active`, `asm`, `class`, `union`, `enum`, `typedef`,
  `template`, `this`, `resource`, `goto`, `inline`, `noinline`, `public`, `static`, `extern`,
  `external`, `interface`, `long`, `short`, `double`, `half`, `fixed`, `unsigned`, `superp`,
  `input`, `output`, `filter`, `sizeof`, `cast`, `namespace`, `using`. `patch` and `sample` are
  the usual accidents.
- Keep loops small and bounded (`for (int i = 0; i < 16; ++i)`); array sizes are constants.
- A global variable cannot be initialised from a uniform; use a function.
- `bool` uniforms and `int` uniforms are fine; `uint` needs the `u` suffix (`1u`).

### Time and animation

Parameters animate with keyframes like any built-in (Inspector diamond, Keyframes panel, curve
editor): a keyframable number or integer arrives in the shader already evaluated for the current
frame. For looks that move by themselves use `uTime` (seconds) or `uFrame`.

### Performance

The shader runs for every pixel of every frame. Keep loops short, sample `uTex` as few times as
you can, prefer `mix`/`step`/`smoothstep` to branches. A blur wider than a few pixels is better
done by an existing effect above yours in the stack (`Gaussian Blur`) than inside a plugin.
Effects see one picture and have no memory between frames.

### Complete worked example

`plugins/examples/dev.kinoklip.duotone` (this is the whole plugin):

```json
{
  "id": "dev.kinoklip.duotone",
  "name": "Duotone (example plugin)",
  "version": "1.0.0",
  "author": "The Kinoklip contributors",
  "license": "GPL-3.0-or-later",
  "api": 1,
  "effects": [
    {
      "id": "duotone",
      "name": "Two-Colour Look",
      "description": "Maps the brightness of the picture to a gradient between two colours",
      "group": "color",
      "entry": "color",
      "file": "duotone.glsl",
      "params": [
        { "key": "shadows", "label": "Shadows", "type": "color", "default": "#1b1f3b" },
        { "key": "highlights", "label": "Highlights", "type": "color", "default": "#ffcf8b" },
        { "key": "amount", "label": "Amount", "type": "number", "min": 0, "max": 100, "default": 100, "step": 1, "unit": "%" }
      ]
    }
  ]
}
```

```glsl
vec3 apply(vec3 c)
{
    float y = luma(c);
    vec3 duo = mix(u_shadows.rgb, u_highlights.rgb, y);
    return mix(c, duo, clamp(u_amount / 100.0, 0.0, 1.0));
}
```

An effect with the `main` entry and an integer parameter: `plugins/examples/dev.kinoklip.posterize`.

## Transitions

A transition mixes the outgoing clip (A) and the incoming clip (B) while `progress` runs from 0
to 1. It follows the convention of [gl-transitions](https://gl-transitions.com) (MIT), so shaders
from that collection work with little change. Write:

```glsl
vec4 transition(vec2 uv)
{
    return mix(getFromColor(uv), getToColor(uv), progress);
}
```

| Name | Meaning |
|---|---|
| `getFromColor(uv)`, `getToColor(uv)` | premultiplied RGBA of the outgoing and the incoming picture |
| `progress` | 0 (all outgoing) to 1 (all incoming), after the transition's curve |
| `ratio` | width / height of the picture |
| `uResolution` | picture size in pixels |
| `u_<key>` | your parameters |

**Here uv has its origin at the bottom left** (the gl-transitions convention): `uv.y = 1.0` is the
top of the picture. Return premultiplied RGBA. The same rules for colour, alpha and ES 3 apply.
Transitions have no `uTime` or `uFrame`. The result is composited over the tracks below like a
built-in transition. Worked example: `plugins/examples/dev.kinoklip.circle-reveal`; a port with a
colour parameter: `dev.kinoklip.dip-to-color`.

Kinoklip bundles the MIT-licensed gl-transitions as `plugins/bundled/dev.kinoklip.gl-transitions`
(made by `scripts/convert_gl_transitions.py`, which also shows the conversion rules for your own
files: `uniform` lines removed, each use of `x` becomes `u_x`, a `vec2` / `vec3` / `ivec2` becomes one
number parameter per component `x_x`, `x_y`, a colour `vec3` / `vec4` becomes a colour parameter).

To use a gl-transitions shader: put its code in the `.glsl` file, delete its `uniform`
declarations and rename each uniform `x` to `u_x`, declare it in `params` with the default from
the comment (`// = 0.5`), keep the licence notice.

## Presets

A preset is a recipe: a list of calls of Kinoklip's command layer, with placeholders for the clip
it is applied to. They show up in Effects library > Toolbox > Presets; drag one onto a clip, or
select clips and double-click. Everything a preset does is **one undo step**, and if one command
fails nothing is changed.

```json
{
  "commands": [
    { "command": "effect.add", "args": { "clip": "$clip", "type": "gaussian_blur", "set": ["radius=2"] } },
    { "command": "keyframe.set", "args": { "clip": "$clip", "param": "opacity", "at": "$start", "value": "0" } },
    { "command": "keyframe.set", "args": { "clip": "$clip", "param": "opacity", "at": "$start+24", "value": "100", "interp": "easeOut" } }
  ]
}
```

- `command` is any command name of `kinoklip commands --json` (`clip.set`, `effect.add`,
  `effect.set`, `keyframe.set`, `title.add`, `marker.add`, `transition.add`, ...); `args` are its
  parameters by name (the command line's `--source-in` is `"source-in"`). Numbers may be numbers or
  strings. Times are timeline frames or timecode strings.
- Placeholders, in any string of `args`:

| Placeholder | Replaced by |
|---|---|
| `$clip` | the id of the clip (a number) |
| `$start` | the clip's first timeline frame |
| `$end` | the clip's last timeline frame (`start + duration - 1`) |
| `$duration` | the clip's length in frames |
| `$playhead` | the playhead's timeline frame |
| `$last_effect` | the id of the effect the most recent `effect.add` of this preset made |
| `$left`, `$right`, `$edit` | `edit_point` presets: the clip that ends at the cut, the clip that starts at it, the cut's timeline frame (`$clip`, `$start`, `$end`, `$duration` are those of the right clip, else the left) |

  A string that is exactly a placeholder becomes a number, and may carry an offset: `"$end-24"`,
  `"$start+12"`. Inside longer text the number is written in: `"radius=$duration"`.
- `target`: `clip` runs the recipe once for every selected (or dropped-on) clip; `playhead` runs it
  once with `$playhead` (a title or generator added at the playhead); `edit_point` runs it at a
  cut between two clips (a transition preset: `{"command": "transition.add", "args": {"clip":
  "$right", "edge": "head", "type": "cross_dissolve", "duration": 12}}`). Double-click applies it
  at the edge of each selected clip that is nearest the playhead; dropping it on a clip uses its
  head or tail, whichever half is under the pointer. A recipe that uses `$left` or `$right` where
  the clip is missing changes nothing and says so. Example: `plugins/bundled/dev.kinoklip.transition-presets`.
- A preset that changes a plugin effect: `"type": "plugin:com.example.warm/warm_look"`.
- Animation packs (what panels like Animation Composer do) are presets that put keyframes on
  `opacity`, `posX`, `posY`, `zoomX`, `zoomY`, `rotation` with `keyframe.set`; effect stacks are
  `effect.add` lines. Example pack: `plugins/examples/dev.kinoklip.animation-pack`.

Find the parameter names of a command with `kinoklip help clip set`, and the keys of an effect
with `kinoklip effect list --id <effect id>`.

## Scripts

A script is a Python 3 program that Kinoklip starts when the person chooses it in Workspace >
Scripts. It talks to Kinoklip through the module `kinoklip` (provided by Kinoklip, nothing to
install):

```python
import kinoklip

clips = kinoklip.call("clip.list")["clips"]
for clip in clips:
    kinoklip.call("marker.add", at=clip["start"], name=clip["name"] or "Clip")
kinoklip.log("done", len(clips))
```

- `kinoklip.call(command, **arguments)` runs one command and returns its result dictionary. A
  failing command raises `kinoklip.CommandError` (`.code`, `.message`). Arguments with a dash
  need the dictionary form: `kinoklip.call("clip.trim", {"clip": 12, "source-in": "00:00:01:00"})`.
- `kinoklip.log(...)` and `print(...)` write to the output window.
- `kinoklip.commands()` lists the available command names.
- Every call is one undo step in the window. Commands act on the project that is open.
- **Permissions.** A call that only reads the project needs `project.read` in the script's
  `permissions`; one that changes it needs `project.edit`. Without them the call raises
  `CommandError("permission_denied")`.
- **Safety.** Python starts with `-I` (no `PYTHON*` variables; helper modules next to the
  script are found after the standard library, so a file there cannot replace it), the working folder is the plugin folder, a run ends after 15 minutes, and a plugin
  with scripts is installed **switched off**: the person switches it on in Preferences > Plugins
  after reading what it asks for. A script is still a program: it runs with the person's rights and
  can read files and use the network. `files.read`, `files.write`, `network` and `processes` are
  declarations shown to the person, not a sandbox. An assistant connected through MCP cannot
  enable a plugin with scripts.

Example: `plugins/examples/dev.kinoklip.script-example`.

## The loop for an AI assistant: validate and preview

The loop that gets a working shader without a person looking at error text:

1. **Create.** `plugin_create` with `id`, `name`, `kind` (`effect`, `transition`, `preset`,
   `script`), `entry`, `group` and one `param` per parameter:
   `key:number:min:max:default[:label]`, `key:integer:min:max:default`, `key:boolean:true`,
   `key:color:#rrggbb`, `key:choice:a|b|c:default`. The answer lists the files written.
2. **Write the shader** into the `.glsl` file named in the answer (an ordinary file edit).
3. **Reload.** `plugin_reload` (a created plugin is already loaded; after editing a file reload).
4. **Validate.** `plugin_validate` with `target` the plugin id. The answer has `valid` and a list
   of `problems`, each with `where` (the file or the manifest path), `line` and `message`, and per
   item `opengl33` / `opengles30` = `ok` / `error` / `skipped`. Fix every problem, reload,
   validate again until `valid` is true. `skipped` means this machine has no context of that API;
   the lint still checks the ES 3 rules.
5. **Look.** `plugin_render_preview` with `plugin`, optional `item`, `set` (`["amount=80"]`),
   `width`, `at` (clip frame, for animated looks) or `progress` (transitions). The answer carries
   a picture of the result on a test chart (colour bars, ramps, skin tones, a circle, a grid) as
   an image. Compare with what you intended; change the shader; reload; preview again. Pass
   `source` to use a picture of your own.
6. **Use.** `effect_add` with `type` `plugin:<plugin id>/<item id>` puts the effect on a clip;
   `effect_set` and `keyframe_set` work as for built-ins; `transition_add` takes a plugin
   transition id the same way. `effect_list --id <id>` shows the parameters.
7. **Share.** `plugin_pack` writes the `.kkplugin` file.

Other commands: `plugin_list`, `plugin_info` (manifest, items, problems, shader compile errors from
the renderer), `plugin_install`, `plugin_uninstall`, `plugin_enable`, `plugin_disable`,
`plugin_copy_builtin`. On the command line they are `kinoklip plugin list`, `kinoklip plugin
create ...` and so on; `kinoklip help plugin create` shows the options.

`plugin_validate` and `plugin_render_preview` need OpenGL. In a window session they use it. In a
headless session (no display) the command line runs them again under `xvfb-run` if that program is
installed; with Local Control switched on they run inside the open window.

## Changing a built-in effect

Built-ins that are plain shaders have a plugin form:

```sh
kinoklip plugin copy_builtin --builtin invert --id me.my-invert --name "My Invert"
```

This copies `plugins/examples/dev.kinoklip.invert` (the port of Invert) into a plugin of your own;
edit its `invert.glsl` and `plugin.json`, reload and preview. The ported effects draw exactly what
the built-in draws (a test compares them byte for byte), so your change starts from the real thing.
Ports available: Invert, Posterize, Dip to Color. For a built-in without a plugin form the answer
says so; write the effect with `plugin create` (the shader of the built-in is not reusable as it
is: its uniforms are set by C++ code, a plugin gets them from `plugin.json`).

To make a port of another built-in: write the plugin, check it against the built-in with
`plugin render_preview` and the same settings, and send it to the project to be added to
`plugins/examples/` with a `ported_from` field.

## Common errors

| Message | Cause and fix |
|---|---|
| `id: "X" is not a valid plugin id` | upper case letters, spaces or too short: `com.example.tint` |
| `effects[0].file: the file "x.glsl" does not exist` | the manifest names a file that is not in the folder (names are case sensitive, paths relative) |
| `... must end in .glsl .frag` | shader file suffix |
| `params[0].key: "my_key" is not a valid key` | no underscore, start with a letter: `myKey` |
| `params[0].default: 12 is outside min..max` | the default must be inside the range |
| `"patch" is a reserved word` | rename the variable (also `sample`, `input`, `output`, `filter`, ...) |
| `OpenGL ES does not convert an int to a float` | `float x = 1;` -> `1.0` |
| `texture2D is old GLSL` | use `texture(uTex, uv)` |
| `do not define main()` | define `effect(uv)`, `apply(c)` or `transition(uv)` instead |
| `do not declare the uniform u_amount` | the host declares parameter uniforms; delete your `uniform` line |
| `define the entry function: vec4 effect(vec2 uv)` | the function is missing or has another signature (`entry` in plugin.json must match: `main` -> `effect`, `color` -> `apply`) |
| `'u_foo' : undeclared identifier` | the parameter `foo` is not in `params`, or the key is spelled differently |
| `... no matching overloaded function found` / a `vec3` where `vec4` is wanted | `effect` returns `vec4`; `apply` returns `vec3` |
| the effect does nothing visible | the parameter is at its neutral value; premultiplied alpha mistakes (`rgb * a` twice); a returned alpha of 0 |
| the picture is too dark or has fringes at soft edges | un-premultiply with `straight()` before changing colour, premultiply after, or use `entry: color` |
| plugin shows "(problem)" in Preferences > Plugins | select it: the details list the problems with line numbers |
| `Missing plugin: ...` in the Inspector | the plugin is not installed or is switched off; the effect is bypassed and its settings are kept |
| a plugin with scripts is "switched off" after install | by design: switch it on in Preferences > Plugins after reading what it asks for |

## Sharing a plugin

`kinoklip plugin pack --id com.example.warm --file warm.kkplugin` writes a zip of the plugin
folder (a `.kkplugin` file is an ordinary zip: you can also zip the folder yourself, with
`plugin.json` at the top level or inside one folder). Give the plugin a licence (the `license`
field is required; put the licence text in a file in the folder). Only GPL-compatible open source
plugins are bundled with Kinoklip; people can install any plugin they like. Send an installable
plugin that you want bundled to the Kinoklip repository as a pull request in `plugins/`.
