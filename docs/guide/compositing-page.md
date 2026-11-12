# Put text over a clip on the Compositing page

The **Compositing** page (new in Kinoklip 1.0.0 Beta) builds the picture of one clip from
**nodes**: small boxes that each do one thing (show the clip, write a text, blur, key out green,
combine two pictures) and that are joined by lines. It is the place for effects that need
several layers inside one clip. As a first step, you will put a text over a clip.

For a simple title over several clips, the Text title on the Edit page is quicker; see
[Getting started](getting-started.md#add-a-title).

## Open the clip on the Compositing page

1. On the Edit page, click the clip on the timeline.
2. Click **Compositing** in the page bar at the bottom of the window (**Shift+5**). You can
   also right-click the clip and choose **Open in Compositing Page**.

The page has:
- two **viewers** at the top; the right one shows the finished picture;
- the **node editor** at the bottom (2 in the picture below), with a row of buttons above it;
- the **Inspector** on the right, with the settings of the selected node.

At first the node editor shows two nodes joined by a line: **MediaIn1** (the clip's own picture)
and **MediaOut1** (what comes out of the clip). Nothing has changed yet.

## Add the text

1. Click **MediaOut1** in the node editor to select it.
2. Click **Text** in the row of buttons above the node editor (1). Kinoklip adds two nodes:
   **Text1**, which draws the text, and **Merge1**, which puts it over the clip's picture.

   ![The Compositing page with a Text node merged over the clip](images/compositing-page/1-compositing-page.png)

3. **Text1** is selected. In the Inspector, type your text into **Text** (3).
4. Change **Font**, **Size**, **Color**, **Outline Width** and **Drop Shadow** to taste.
5. To move the text, drag it in the right viewer (4), or change **Center X** and **Center Y**
   in the Inspector (0.5 and 0.5 is the middle of the picture).

Go back to the **Edit** page (**Shift+4**): the clip now shows the text there too, during
playback and in the export. A small node mark on the clip in the timeline shows that it has a
composition.

## Find your way around the node editor

- **Move** a node by dragging it. **Pan** with the middle mouse button (or hold **Space** and
  drag); **zoom** with the mouse wheel; **Ctrl+F** fits all nodes into view.
- **Show a node in a viewer:** select it and press **1** (left viewer) or **2** (right viewer).
  Handy to look at the text alone, or at the clip before the text.
- **Add other nodes:** the buttons above the node editor add the most common ones after the
  selected node (Background, Merge, Transform, masks, Blur, Color Corrector, Chroma Key).
  **Shift+Space** opens a list of every node with a search field.
- **Turn a node off** for a moment: select it and press **Ctrl+P**; press it again to turn it
  back on.
- **Delete** a node: select it and press **Delete**. **Ctrl+Z** undoes every step.

## Spline and Keyframes panels

Click **Spline** (or **Keyframes**) in the bar at the top of the page, next to **Nodes**. The
panel opens beside the node editor and lists the animated parameters of the selected nodes,
node by node, with a check box for each curve. **Sel** switches between the selected nodes
and every node; **All** and **None** show or hide all curves; **Show All** also lists the
parameters you have not animated yet.

Select keyframes with a click or a rubber band and use the tools above the curves:
**Smooth** (**S**), **Linear** (**L**), **Step In** (**I**), **Step Out** (the Static button,
**O**), **Ease In and Out** (**T**), **Flat**, **Reverse** (**V**) and **Time Stretch**. **F** fits all curves to
the panel. The **T** and **V** boxes type an exact frame and value for the selected keyframe.

To repeat an animation, click **Loop**: after the last keyframe the value repeats the keyed
range (a dashed curve shows the repeats). Its arrow offers **Set Ping-Pong**, **Set
Relative** (each repeat continues from where the last ended, for a turning angle),
**Linear Extrapolation**, **Remove Loop** and the **Pre-Loop** versions for before the first
keyframe. The panel is small on purpose: widen it by dragging the line between it and the
node editor, and use the **»** button when the tool row does not fit.

## Viewer tools

Each viewer has a row of buttons under its title (the ones that do not fit are in the **»**
menu). The keys below work only while that viewer has the focus (click into it first), so they
never get in the way of keys like **I** and **O** elsewhere.

- **A, B and A|B** (keys **,** **.** and **/**): the viewer has two buffers. Buffer A shows the
  node you sent with **1** / **2**. Press **.** to switch to buffer B (it starts as a copy of
  A), then send another node with **1** / **2** or drag one in: it goes into B and A keeps its
  node. **/** shows both at once with a split line; drag the dot to move the line, drag the
  line itself to turn it (hold **Shift** for steps of 45 degrees).
- **Color, Red, Green, Blue, Alpha** (keys **C**, **R**, **G**, **B**, **A**): show one channel
  of the picture as grey. **Checker** shows a checker pattern under the picture so you can see
  which parts are transparent. These need a saved composition (make one edit first).
- **Controls** (**Ctrl+K**): hide or show the handles over the picture.
- **Lock** (**Ctrl+L**): the viewer keeps its node when you press **1** / **2** for another.
- **Clear** (**~**): empties both viewers.
- **100%** (**Ctrl+1**): shows the picture at its real size.
- **Loop** (next to Play): click to change what happens at the end of the clip: **Loop**,
  **Ping-pong** (plays forward and backward) or **Once** (stops on the last frame).

## Tips

- **Fade the text in:** select **Merge1** and keyframe its **Blend** (0 at the start, 1 a
  second later) with the diamond in the Inspector; see [Animate with keyframes](keyframe-animation.md).
- **Green screen in one clip:** select **MediaIn1** and click **Chroma Key**, then click
  **Background**. The new Merge puts the background in front of the keyed picture; select the
  Merge and press **Ctrl+T** to swap its inputs, so the background is behind. This does what
  [Remove a green screen](remove-a-green-screen.md) does with two tracks, inside one clip.
- **A logo picture:** import the logo into the media pool first. Click **Media Pool** in the
  bar at the top of the page and drag the logo onto the node editor: a new **MediaIn2** node
  reads it. Select **MediaOut1** and click **Merge**, then drag from the output of
  **MediaIn2** to the foreground input of **Merge1** (foreground links are green). A shortcut: drag from the output of **MediaIn2** and drop it on the output of **MediaIn1**; a Merge appears with **MediaIn1** as the background and **MediaIn2** as the foreground, and whatever read **MediaIn1** now reads the Merge. Select
  **Merge1** and lower its **Size** and change **Center X** / **Center Y** to put the logo in a
  corner. To read another picture into a MediaIn node, select it and choose the item under
  **Source** in the Inspector (**This clip** goes back to the clip's own picture).
- **Start over:** on the Edit page, select the clip and choose **Clip > Reset Composition**.

## If something goes wrong

- **The page says "Select a clip on the Edit page".** Go back to the Edit page, click a clip on
  a video track (or put the playhead over one) and switch to the Compositing page again.
- **The text does not show in the right viewer.** The right viewer must show **MediaOut1**
  (its name is in the viewer's title). Select **MediaOut1** and press **2**.
- **Keys like Delete or Ctrl+C do nothing.** Click into the node editor first; its keys only
  work while it has the focus.
