# Transitions between clips

A transition changes one clip into the next. Kinoklip has them in the same folders as
[DaVinci Resolve](https://www.blackmagicdesign.com/products/davinciresolve): open the effects
library (**Toolbox > Video Transitions**) and drag one onto the cut between two clips. Pressing
**Ctrl+T** adds the standard one, a one-second **Cross Dissolve**. Double-clicking a transition
in the library puts it on the selected cut, or on the cut nearest the playhead.

Click a transition on the timeline to change it in the **Inspector** (its **Transition** tab):
the length, where it sits on the cut, and the settings of that transition. Drag its edges to
make it longer or shorter.

## The folders

- **Dissolve.** Cross Dissolve, Dip to Color, Additive Dissolve (the two pictures are added, so
  the middle gets brighter), Non-Additive Dissolve (keeps the brighter of the two), Blur
  Dissolve (blurs both pictures towards the middle) and Smooth Cut. **Start Ratio** and **End
  Ratio** say how much of the next clip is visible at the start and at the end.
- **Iris.** A shape grows from the centre and shows the next clip inside it: Arrow, Cross,
  Diamond, Eye, Hexagon, Oval, Pentagon, Square and Triangle.
- **Shape.** Box (from a corner or the middle of an edge), Heart, Star (4, 5 or 6 points),
  Triangle Left and Triangle Right.
- **Wipe.** Wipe (an edge that moves in a direction), Band, Center, Clock, Radial, Spiral,
  Venetian Blind and X.
- **Motion.** Push, Barn Door, Slide and Split. **Motion Blur** smears the moving pictures a
  little.

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

Transitions that Resolve groups under Fusion and Resolve FX are not in Kinoklip yet.
