# Add layers to a title

You will build a title from several parts, for example a name over a coloured bar with a thin line
under it, and keep it all as one clip on the timeline. A title is a stack of **layers**: text
layers and shape layers. The layers are drawn from the bottom of the list to the top, in the
viewer, during playback and in the exported video.

## Add a layer

1. Click a title on the timeline (or add one from **Effects > Titles > Text**) and open the
   **Title** tab of the Inspector.
2. At the top of the tab is the **Layers** list. A title you made earlier has one layer, the text
   it always had. The top layer is the first in the list.
3. Click **Text** to add another text layer, or **Shape** and pick **Rectangle**, **Rounded
   Rectangle**, **Ellipse** or **Line**. The new layer lands on top and is selected.
4. The controls below the list now belong to the selected layer. A text layer has everything a
   title has: font, size, colours, tracking, line spacing, alignment, outline, shadow, background
   box, position, anchor, rotation and opacity. A shape layer has its width and height, corner
   radius (rounded rectangles), position, rotation, fill colour and opacity, stroke colour and
   width, and opacity.

Positions work as in a title: Position X runs from 0 (left) to 1 (right) and Position Y from 0
(bottom) to 1 (top) of the picture. A **Line** is a bar as long as the shape's width and as thick as
its height, with round ends.

## Order, names and visibility

- **Up** and **Down** (the arrows under the list) move the selected layer. A layer higher in the
  list is drawn over the ones below it, so a bar goes under the text by moving it down.
- Tick or untick the box in front of a layer to show or hide it. A hidden layer is not drawn and
  not exported.
- Double-click a layer's name to rename it.
- **Duplicate** copies the selected layer and puts the copy directly above it.
- **Delete** removes the selected layer. The first layer, the one the title was made with, cannot be
  deleted, but it can be hidden, renamed and moved like the others.

Each of these is one step for **Undo**.

## Animate a layer

Every layer has its own keyframe diamonds. Select the layer, move the playhead and click the
diamond next to a value, as in [Animate with keyframes](keyframe-animation.md). A bar that slides in
from the left while the text stays put is a keyframed Position X on the bar alone. The layer's
keyframes also show in the Keyframes panel, under the layer's name.

## From the command line

The same steps work through the [command line](control-kinoklip-from-the-command-line.md) and
the [AI assistant interface](control-kinoklip-from-an-ai-assistant.md):

```
kinoklip title layer list --project p.kkproj --clip 12
kinoklip title layer add --project p.kkproj --clip 12 --kind shape --shape rounded --set shapeWidth=500
kinoklip title layer move --project p.kkproj --clip 12 --layer 1 --by -1
kinoklip title layer set --project p.kkproj --clip 12 --layer 1 --visible false
kinoklip title layer remove --project p.kkproj --clip 12 --layer 1
```

`keyframe set` takes `--layer` to key a layer's parameter.

## If something goes wrong

- **The controls show the wrong layer.** Click the layer in the list; the controls below always
  belong to the highlighted layer.
- **A layer does not show.** Check its box in the list, its **Opacity**, and that its position is
  inside the picture.
