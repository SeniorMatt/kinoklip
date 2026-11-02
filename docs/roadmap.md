# Kinoklip roadmap

Owner's long-term plan (recorded 2026-11-02). v1 is done (0.1.0-alpha.1 - alpha.4); v2 is done
(0.2.0-alpha.1) except mask tracking; v2.5 has started (Cut page, see below); later versions are **planned, not started**. Code written today must keep these doors open (see plan.md 5.9 / 5.9b /
5.10 for the profile-ready and command-based rules).

Naming rule for all versions: pages and features get our own names. Generic words (Cut, Edit,
Color, Deliver, Media) are fine; Blackmagic product names (e.g. "Fusion", "Fairlight") are not used.

---

## Release naming
- Everything before the owner's decision is a **pre-release**: `0.x.y-alpha.N` while features are
  still landing and things break, `0.x.y-beta.N` once a version's features are complete and the
  focus is stability. Current: **0.2.0-alpha.1**.
- **1.0.0 is released only when the owner decides.** Until then no build is called "1.0".
- The "v1 / v2 / v2.5 / v3 / v4" names below are feature milestones, not version numbers.

## v1 - the basics (done)
Media / Edit / Deliver pages with the Resolve-like layout and behaviour: import (all common formats),
multi-track timeline editing, trims, keyframes, simple effects, transitions, titles, GPU playback and
export, real Breeze look, single-file Linux x86_64 binary. Color page visible but disabled.

## v2 - deeper editing (niche features) (done, except mask tracking)
- **Masks on the Edit page**: shape masks (rectangle, ellipse, polygon, bezier/freehand), feather,
  expand/contract, invert, opacity, multiple masks per clip with add/subtract/intersect, all keyframed;
  mask tracking (point/planar tracker) as a stretch goal.
- **Compound clips**: collapse a selection into one clip, open/edit it in its own timeline,
  decompose back in place, nested timelines (timeline used as a clip in another timeline).
- **Timeline tabs enabled by default**: several timelines open at once as tabs above the timeline
  (switch, reorder, close), each with its own playhead and zoom; stacked view as an option.
- Other niche editing tools (candidates, to confirm): adjustment clips, freeze frame, speed change and
  speed ramps with optical-flow / frame-blend retiming, reverse clip, multicam clips and the multicam
  viewer, sync by audio waveform / timecode, subtitles track (SRT import/export), proxies and
  optimised media, render cache, dynamic trim (JKL trimming), smart insert, replace edit, fit to fill,
  ripple overwrite, audio keyframe lines on the timeline, clip attributes (pixel aspect, field order,
  audio channel mapping), project bins/smart bins, power bins, clip colour labels and flags.

## v2.1 - compound clips and tracks (owner feedback on 0.2.0-alpha.1)
- Double-clicking a compound clip opens it in a timeline tab at the moment under the pointer.
- Dragging clips above the top video track or below the bottom audio track creates a new track,
  like Resolve.
- Released before any v2.5 work is merged.

## v2.2 - small UI animations (owner request, 0.2.2-alpha.1)
- Short tweened animations on the timeline so edits are easier to follow: clips slide into
  place after trims, ripple deletes, inserts, nudges and undo / redo; new clips grow in,
  removed clips fade out; a blade cut flashes and the two halves settle together; trimmed
  edges flash on release and the selected edit point pulses; the playhead line glides on
  jumps; the snap indicator fades. Display only: the timeline model and undo change at once.
- Preferences > UI Settings > "Animate timeline edits" (on by default) and "Animation speed";
  KDE's global animation speed applies too (instant = off).
- Ships together with the Cut page (v2.5 work merged before this release), docs/behavior-spec.md
  section 36.

## v2.5 - Cut and Audio pages (owner decision 2026-11-03)
- **Cut page** (done, basic level): dual timeline, source tape, smart edits, fast review.
- **Audio page** (in progress, basic level): mixer with buses, EQ / dynamics, automation, meters,
  loudness.
- The Color and Compositing pages moved to v4.

## v3 - polish basic video editing (current focus)
Make everyday editing on the Edit and Cut pages feel finished before adding more pages: the
remaining Edit page behaviours and quirks of Resolve, the deferred spec items (shuttle audio,
viewer edit overlay, two-up trim view, ...), smoothness and responsiveness, small animations
(v2.2), and fixes from owner testing. Driven by `docs/behavior-spec.md` and the parity tests.

## v4 - the other pages, and switching from Resolve
- **Color page** (colour grading): primaries wheels, curves, HSL qualifier, windows, nodes, LUTs,
  scopes, gallery stills. A started branch `colorpage` exists (paused 2026-11-03).
- **Compositing page** (own name, node-based compositing: merge, transform, masks, text, keyers,
  tracker).
- Media and Deliver pages extended (clone tool, render presets per platform).
- **Import of Resolve projects** so users can move their work over.
  - Primary path (safe): the interchange formats Resolve already exports — OpenTimelineIO (.otio),
    FCPXML, AAF, EDL — covering timelines, clips, transitions, retimes, markers, transforms, basic
    audio levels. Also export to these formats for round-tripping.
  - Direct `.drp` (project) / `.dra` (project archive) import: these formats are undocumented and
    reading them requires analysing files Resolve produces. This conflicts with the original
    "no reverse engineering" rule, so it only happens after a legal check of the interoperability
    exception in the relevant jurisdictions. Until then: interchange formats only.
- Migration helpers: map colour grades and effects where an equivalent exists, report what could
  not be converted.

## Project website (later)
A website presenting Kinoklip as a free replacement / alternative for DaVinci Resolve (in the spirit
of how ArtCraft positions itself against Adobe): features, screenshots, downloads, roadmap, docs.
Rules: refer to Resolve only by name for comparison ("an alternative to DaVinci Resolve"), never
use Blackmagic logos, screenshots of Resolve, or wording that suggests affiliation or endorsement;
all marketing claims must be true of the shipped build. Brand assets: our own logo (packaging icon).

## Interface profiles (any version after v1)
- Resolve profile (default), Plasma profile, Vim profile — see plan.md 5.9 / 5.9b.

## Distribution (any version after v1)
- Flatpak, AppImage, distro packages, Windows installer, macOS .dmg — see plan.md 8b.

## Later - finish every page, every detail
Complete feature set and all the small behaviours/quirks of each page so that a long-time Resolve
user can switch with no retraining: shortcut parity per page, same default behaviours, same panel
options, same edge-case handling. Driven by an expanded `docs/behavior-spec.md` per page and the
parity test suite.
