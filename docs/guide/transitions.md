# Transitions between clips

A transition changes one clip into the next. Kinoklip has them in the same folders as
DaVinci Resolve: open the effects
library (**Toolbox > Video Transitions**) and drag one onto the cut between two clips. Pressing
**Ctrl+T** adds the standard one, a **Cross Dissolve** unless you changed it (see below).
Double-clicking a transition in the library puts it on the selected cut, or on the cut nearest
the playhead. With several clips selected, double-clicking a transition (or dragging one onto
the edge of a selected clip) puts it on every selected clip of the same kind; effects work the
same way (Inspector **Add Effect**, a double-click or a drag from the library).

## Choose the standard transition

The standard transition belongs to the project. Right-click a transition in the effects library
and choose **Set as Standard Transition**; the standard video transition and the standard audio
transition are shown in bold. You can also pick them, and the standard length in frames, in
**File > Project Settings > General Options > Standard Transitions**. Ctrl+T, the Cut page's
transition button and the Inspector's **Transition In** and **Transition Out** buttons all use
them. A new project starts with the length from **Preferences > Editing > Standard transition
duration**; projects made before keep a Cross Dissolve of that length. Changing the standard
transition can be undone.

Click a transition on the timeline to change it in the **Inspector** (its **Transition** tab):
the length, where it sits on the cut, and the settings of that transition. Drag its edges to
make it longer or shorter.

To copy a transition with everything set on it, hold **Alt** and drag it onto another cut or
clip edge (Alt can be pressed after you start dragging). Or click it, press **Ctrl+C**, select
a cut or some clips and press **Ctrl+V**: the transition is pasted on the selected cut, or on
both ends of each selected clip, in one step.

## The folders

- **Dissolve.** Cross Dissolve, Dip to Color, Additive Dissolve (the two pictures are added, so
  the middle gets brighter), Non-Additive Dissolve (keeps the brighter of the two), Blur
  Dissolve (blurs both pictures towards the middle) and Smooth Cut. **Start Ratio** and **End
  Ratio** say how much of the next clip is visible at the start and at the end, and **Reverse**
  plays that curve from the other end (Cross Dissolve, Dip to Color and the three dissolves
  above have all three).
- **Iris.** A shape grows from the centre and shows the next clip inside it: Arrow, Cross,
  Diamond, Eye, Hexagon, Oval, Pentagon, Square and Triangle.
- **Shape.** Box (from a corner or the middle of an edge), Heart, Star (4, 5 or 6 points),
  Triangle Left and Triangle Right.
- **Wipe.** Wipe (an edge that moves in a direction), Band, Center, Clock, Radial, Spiral,
  Venetian Blind and X.
- **Fusion Transitions.** Brightness Flash, Zoom In, Zoom In and Out, Crash Zoom, Rotate,
  Rotate 90, Noise Dissolve, Glitch, Camera Shake, Circles, Flip 3D, Slice Push, Warp, Drop Warp,
  Tunnel of Light, Fall and Bounce (the next clip falls in and bounces to rest; **Fall Angle**
  is the way it falls), Film Strip (the picture shrinks to a frame of a moving film strip and
  grows again on the next one), Foreground Wipe (a magnifying column sweeps across; **Invert
  Wipe**, **Shadow Softness**, **Border Width**), Paint On (brush strokes paint the next clip
  in; **Strokes**, **Shadow Blend**), Round and Down (the first clip swings down round a pivot
  while it dissolves), and the eight Slide Left / Right / Up / Down and Pan Left / Right / Up /
  Down. The slides and pans have a **Curve** (Linear, Easing or Custom, with **Custom Curve**
  for how steep the custom one is); the pans also have **Motion Blur**.
- **Resolve FX Transitions.** Burn Away: the first clip burns away from one side (set the
  direction with **Angle**) and shows the next one.
- **Motion.** Push, Barn Door, Slide and Split. **Motion Blur** smears the moving pictures a
  little, and **Border Color**, **Border** and **Feather** draw a line along the moving edge.

The iris, shape and wipe transitions share four settings. **Border Color** and **Border** draw a
coloured line along the moving edge, **Feather** softens the edge, and **Reverse** runs it the
other way round (an iris that closes instead of opening). **Offset X / Y** moves the centre of a
shape, **Rotation** turns it, and **Aspect Ratio** makes it wider or narrower.

## Hide a jump cut with Smooth Cut

A jump cut is a cut inside one shot, for example where you removed a stumble from an interview:
the person seems to jump a little. **Smooth Cut** (in the Dissolve folder) hides it by morphing
the last picture before the cut into the first picture after it, following how the things in
the picture moved, instead of fading between two pictures that do not line up.

1. Put the transition on the cut. Press **Ctrl+T** and then pick Smooth Cut in the
   effects library, or drag Smooth Cut onto the cut.
2. Make it short, two to six frames. Longer than that the morph shows.
3. Play it. It works best when the person sits in about the same place on both sides of the cut.

It needs the clips to have a few frames of spare picture on both sides of the cut, as every
transition does. Smooth Cut needs the graphics card (OpenGL) to morph; without it the picture
simply dissolves. A big jump, such as a person who moves across the whole picture, is too much
for it and the morph then looks like a dissolve.

## Taking transitions to another editor

When you export the timeline as OpenTimelineIO (**File > Export**, see
[Move a timeline to another editor](move-a-timeline-to-another-editor.md)), every transition is
written as a transition with its name and settings. The same Kinoklip reads it back as it was.
Another program that does not know it shows a custom transition, and a transition from another
program that Kinoklip does not know becomes a cross dissolve.
