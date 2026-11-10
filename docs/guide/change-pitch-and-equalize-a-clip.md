# Change the pitch of a clip and equalize it

You will make a voice or a piece of music higher or lower without changing how long it plays,
and shape its sound with the clip's own four-band equalizer. Both live in the Inspector.

## Change the pitch

1. Select the clip with the sound. Open the **Inspector** and click the **Audio** tab.
2. Set **Pitch** in semitones, from -24 to 24. +12 is one octave up, -12 one octave down. Use
   **Pitch Fine** for hundredths of a semitone (cents, -100 to 100).
3. Play the clip. It keeps its length and its speed; only the pitch changes.

Notes: the pitch is one value for the whole clip (it has no keyframe button). It works on clips
at normal speed; a clip with a speed change or a retime curve keeps its own pitch handling (see
[Change the speed of a clip](change-clip-speed.md)). Large changes, more than a few semitones,
sound less natural, especially on speech. The sound is delayed by nothing: the clip starts and
ends where it did.

## Equalize the clip

1. In the same **Audio** tab, scroll to **Equalizer** and tick it.
2. Each of the four bands has a **Frequency** (Hz), a **Gain** (dB, -24 to 24) and a **Q**.
   Band 1 is a low shelf (lifts or lowers everything below its frequency), bands 2 and 3 are
   bells (a peak or dip around their frequency; a higher Q makes it narrower) and band 4 is a
   high shelf.
3. Untick **Equalizer** to hear the clip without it; the settings stay.

The clip equalizer works on this clip only. For the whole track use the equalizer of the
track's strip on the Audio page.

## From the command line

```
kinoklip clip set --project my.kkproj --clip 12 --pitch 3 --eq on --eq2-freq 800 --eq2-gain 5
```
