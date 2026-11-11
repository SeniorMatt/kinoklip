# Track a moving thing and warp the picture

You will make a graphic or a clip follow something that moves in the footage, pin a graphic to a
bending surface, and bend a picture by hand. The effects are in the **Effects** library, folders
**Transform** (Match Move, Surface Tracker) and **Warp** (Warper), and work like the effects with
the same names in DaVinci Resolve.

## Make a graphic follow a moving thing (Match Move)

1. Put the footage on a video track and the graphic (a title, a picture, a solid) on the track
   above it. Place the graphic where it should sit at one frame of your choice.
2. Move the playhead to that frame and add **Match Move** to the graphic (Effects tab of the
   Inspector, **Add Effect**, **Transform**).
3. Set **Track** to **Footage Below**. Put the **Region** on the thing to follow: **Region X** and
   **Region Y** are its centre in percent of the picture (50 and 50 is the middle), **Region Size**
   its width. Choose a corner or an edge with some detail, not a flat colour.
4. Click **Track Forward** to follow it from the playhead to the end of the clip, and **Track
   Backward** to follow it to the start. A progress bar shows; you can keep editing, and
   **Cancel** stops with the clip unchanged.
5. Play the clip. The graphic stays where you put it at the playhead frame and moves with the
   thing from there.

Other settings:

- **Tracking**: **Position** follows one point. **Position, Scale and Rotation** follows a square
  region and also turns and enlarges the graphic when the thing does.
- **Direction**: **Follow the Track** moves the clip with the thing. **Hold the Track in Place**
  does the opposite, so the thing stays still and the picture moves around it (set **Track** to
  **This Clip** to steady a clip on a feature).
- **Strength** is how much of the movement is used. **Offset X** and **Offset Y** shift the clip
  away from the thing, and can be animated.

If you change **Track**, **Tracking** or the region after tracking, the old track is not used and
the effect says **Track again**. The other settings change the picture at once. **Clear Track**
throws the track away. The track is saved in the project with the clip and follows it when you
trim, split or change its speed, but if you move the graphic against the footage, track again.

## Pin a graphic to a bending surface, or bend the picture (Surface Tracker)

Use **Surface Tracker** when the thing moves and also bends or turns, like a flag, a sheet of
paper or a screen filmed at an angle.

1. Add **Surface Tracker** (**Transform** folder) to the clip, set **Track** (**Footage Below** for
   a graphic above the footage), move the playhead to the frame where the graphic should sit.
2. Cover the surface with the region (**Region X**, **Y**, **Width**, **Height**). **Grid Density**
   is how many points follow the surface on each side: more points follow a stronger bend.
3. Click **Track Forward** and **Track Backward**, as with Match Move.
4. **Apply** chooses the result: **Pin to the Surface** moves the graphic with the surface, and
   **Warp with the Surface** bends the clip's own picture the way the surface bends. **Smoothing**
   calms jumpy points.

Like Match Move, the track is stored with the clip and a change of region, grid or source needs a
new track.

## Bend a picture by hand (Warper)

Add **Warper** (**Warp** folder). It lays a grid of points over the picture: **Columns** and
**Rows** set how many (3 by 3 to start, up to 4 by 4). Each point has **Point row, column X** and
**Y**, how many pixels it moves right and down (row 1 is the top row). The picture bends smoothly
with the points, and the edge of the grid holds the picture there. Every point can be animated
with keyframes, so a mouth, a flag or a logo can be made to ripple and sway.
