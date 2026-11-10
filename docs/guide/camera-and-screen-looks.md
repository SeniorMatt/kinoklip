# Camera and screen looks: CCTV, glitch, drone, DSLR, video call and more

Seven ready-made looks make a picture look like it was seen through another device, or put it in
a frame. Each one comes two ways:

- as a clip in **Toolbox > Effects** (CCTV, Digital Glitch, Drone Overlay, DSLR, DVE, Video Call,
  Video Camera). Drag it onto a track above your footage; it is an adjustment clip, so it acts on
  every track below it. Its effect is already on it.
- as a video effect in the **Stylize** folder of the effects library, to put on a single clip.

Everything the looks show is drawn by Kinoklip itself. All settings are in the **Inspector**, and
numbers can be keyframed (the diamond next to a setting), so a drone's altitude can climb or a
video-call ring can light up while someone speaks.

## CCTV

A security-camera picture: washed-out colors, noise that changes every frame, scanlines and dark
corners. Text is drawn over it: the **Camera Name** at the top left, a blinking red dot with REC at
the top right, and the date and time at the bottom left.

- **Start Date and Time** is the time shown on the first frame (`yyyy-MM-dd HH:mm:ss`). The clock
  then runs with the clip.
- **Time Format** says how the time is written. `dd/MM/yyyy hh:mm:ss AP` gives
  `04/03/2026 03:30:00 PM`; `HH:mm` shows only hours and minutes.
- **Desaturation**, **Noise**, **Scanlines** and **Vignette** set the picture; **Text Color**,
  **Font**, **Text Size** and **Overlay Opacity** set the writing.

## Digital Glitch

Digital damage on the picture: bands slide sideways, blocks jump to other places, the red and blue
parts of the picture pull apart and bright speckles flicker. **Amount** is how much is wrong, and
at 0 the picture is untouched.

- **Band Shift** is the widest sideways slide, **Block Size** the size of the jumping blocks,
  **Color Split** how far red and blue part, **Digital Noise** the speckles.
- **Glitches per Second** is how often a new pattern appears; **Seed** picks another set of
  patterns. The same frame always gives the same glitch, so exports repeat exactly.

## Drone Overlay

The display of a drone camera: corner framing marks, a compass bar at the top, a horizon line that
moves with the camera tilt around a small cross, the recording time, and readouts of **Altitude**,
**Speed**, **Distance to Home**, **Battery**, **Satellites** and the camera's **Gimbal Pitch**.

The readouts are plain numbers that you type or keyframe, so a flight can be written in by hand.
**Heading** turns the compass bar. **Units** switches between meters and feet. **Framing Marks**
and **Compass Bar** can be switched off.

## DSLR

A photo camera's viewfinder: a grid (**Rule of Thirds**, **Fine Grid** or off), frame lines, an
array of focus points with one active **Focus Point** (X and Y move it), and an information bar
with **Shutter Speed** (text, for example `1/250`), **Aperture**, **ISO**, **Shots Remaining**,
the focus mode and an exposure scale whose marker follows **Exposure Compensation**. The active
focus point is white until **Focus Locked** turns it green.

## DVE

A picture-in-picture box. The picture shrinks to **Size** percent of the frame and sits at
**Position X / Y** (pixels from the middle). **Rotation** turns it in the picture plane; **Tilt X**
and **Tilt Y** swing it away from you with perspective. It has a **Border** (width and color),
rounded corners and a soft drop shadow (opacity, blur and offset). Around the box the picture is
transparent: put the effect on a clip that lies above other footage and that footage shows there. As
an adjustment clip it shrinks everything below it into the box.

Keyframe Position and Size to slide the box in. For a plain move, scale and turn without the
border and shadow, use the **Transform** effect instead.

## Video Call

The picture in a rounded tile on a dark background, like one person in a video call. A pill at the
lower left carries a microphone mark and the **Name** (a red crossed-out microphone when **Muted**
is on), the top right shows connection bars (**Connection Bars**, 0 to 4) and the top left a call
timer. **Speaking** lights a green ring around the tile.

**Margin** is the space around the tile, **Corner Radius** its rounding. Set **Background
Opacity** to 0 to see the tracks below around the tile, for example to build a grid of several
callers.

## Video Camera

A camcorder's viewfinder: a red dot that blinks next to REC (or STBY when **Recording** is off),
the **Format Label** and **Card Remaining** texts, a battery icon with its **Battery** level, a
timecode at the bottom left that runs from **Start Timecode**, a **Zoom** bar from W to T, a focus
frame in the middle and marks in the corners. **Blink Recording Dot**, **Focus Frame** and
**Corner Marks** can be switched off.

## Text, fonts and sizes

Every text can be edited in the Inspector. **Font** takes the name of a font family; empty uses the
default font of your system (a fixed-width one for the camera displays). **Text Size** scales all
text and marks of an overlay at once, and everything is drawn in proportion to the picture height,
so a look has the same layout at any resolution.
