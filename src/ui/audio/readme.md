# src/ui/audio — Audio page widgets (mixer, meters, EQ and dynamics editors)

The widgets of the Audio page (docs/behavior-spec.md section 37) and of the Edit page's Mixer
panel. The page itself (layout, wiring, page-only actions) is `src/app/pages/AudioPage`; the mixer
model and its edits are `core/MixerData.h` / `core/MixerOps.h`; the DSP is in `src/render`
(`AudioMixer`, `ChannelProcessor`, `Equalizer`, `Dynamics`, `LoudnessMeter`); the meter readings
come from `src/playback` (`AudioFeeder` -> `PlaybackEngine::meters()`). Everything painted here
takes its colours from `ui/common/SchemeColors.h` (meter zones: positive / neutral / negative).

## Key classes

| File | What |
|---|---|
| `MixerController.h` | the only way the widgets edit the mix: `edit()` (one undo step), `beginGesture()` / `updateGesture()` / `endGesture()` (a drag = one undo step), `pressControl()` / `moveControl()` / `releaseControl()` for faders and pan knobs, which also record automation in Touch / Write mode while the transport plays |
| `MixerPanel.h` | the mixer: a `MixerStrip` per audio track, a separator, a strip per bus; rebuilt when tracks / buses change, refreshed on every edit and playhead move |
| `MixerStrip.h` | one channel strip: input, EQ / Dyn buttons (open the editors), bus assignment (tracks: checkable bus menu; buses: Output), pan, automation mode, R (voice-over arm; right-click: input menu; level bar while armed) / M / S, fader + meter, value, name + channel format (right-click: Mono / Stereo, bus Rename / Delete) |
| `VoiceOverController.h` | voice-over (spec 75): arm one audio track (opens and meters its input), count-in, record to a WAV file while the timeline plays, then import the take (bin "Voice Over") and overwrite it onto the armed track at the start frame with the input latency skipped at its head, as ONE undo step; the input is an `AudioInput` made by a factory, so tests feed a generated signal; registers itself as `AppContext::voiceOver` |
| `VoiceOverMenu.h` | the input menu of the arm button (timeline header and mixer strip) and its tooltip (the problem when there is no microphone) |
| `FaderWidget.h` | `FaderWidget` (console fader with dB scale) and `PanKnob`; they report press / move / release, the owner applies them |
| `LevelMeter.h` | vertical peak meter (one or two bars, peak hold, optional dB scale) reading a `MeterModel` |
| `MeterModel.h` | meter ballistics (instant rise, 24 dB/s fall, 1.5 s peak hold), fed by `PlaybackEngine::metersUpdated()` or `setFrame()` |
| `LoudnessPanel.h` | the monitoring panel: output meter, a meter per bus, `LoudnessGauge` (M / S bars against the -23 LUFS target), readouts (momentary, short-term, integrated LUFS, true peak max dBTP), Reset |
| `EqWindow.h` / `EqCurveView.h` | the EQ editor: response curve from the mixer's own biquads with draggable band handles (wheel = Q) and per-band type / on / frequency / gain / Q fields |
| `DynamicsWindow.h` | the compressor / limiter editor with the transfer curve and the gain reduction readout |
| `AudioScales.h` | pure mappings: fader travel, meter fill, log frequency axis (unit-tested) |

## Data flow

```
 widget gesture ──► MixerController ──beginEdit / mutableData / endEdit──► Project (one undo step)
                                                       │ Timeline::changed()
 PlaybackEngine ◄── snapshot (sound-only edits do not re-render pictures) ──┘
   └─ AudioFeeder thread: AudioMixer (MixLevels) + LoudnessMeter ─► MeterFrame ring
        └─ engine tick (~30 Hz, the frame being heard) ─► metersUpdated() ─► MeterModel ─► meters
```

## How to extend

- **A new strip control** (e.g. a send level): add the field to `ChannelStrip` / `TrackMix`
  (`core/MixerData.h`, with JSON in `core/MixerJson.cpp` and a clamp in `mixerops::clampStrip()`),
  process it in `render/ChannelProcessor` or `render/AudioMixerGraph.cpp`, then add the widget to
  `MixerStrip` and apply it through `MixerController::edit()` (button) or the gesture calls (drag).
- **A new processor window** (e.g. a de-esser): write the DSP class in `src/render` like
  `Dynamics`, its settings struct in `core/MixerData.h`, and a window like `DynamicsWindow` that
  edits through `MixerController::edit()`; open it from a strip button.
- **A new meter** (e.g. a phase correlation meter): compute it in `AudioFeeder::run()` next to the
  loudness meter, add the value to `MeterFrame` (`render/MixLevels.h`) and paint it from
  `MeterModel::lastFrame()`.
- **A new Audio page command**: add it to the "audio" category in `ui/common/StandardActions.cpp`,
  a row in `scripts/keymap-resolve-windows.py` (then run it), and connect it in
  `AudioPage::connectActions()` (page-only commands go in `kPageActions`).

## Timeline animations

The Audio page's timeline is an ordinary `TimelineWidget`, so the display-only animator of v2.2
(`ui/timeline/TimelineAnimator`, switched by `ui/common/UiAnimation`; spec 36) runs there too.
Mixer edits move no clips, so they animate nothing; fader and EQ drags are open edits, which only
mark the animator stale until the gesture ends. Animations are off by default in offscreen tests.
