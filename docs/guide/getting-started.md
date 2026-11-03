# Getting started

This page walks you through your first video: you make a project, bring in your clips, put
them on the timeline, cut them, add a title and a transition, and export the result. It takes
about fifteen minutes.

**Words you will meet:**
- **Clip**: one piece of video, sound or a picture.
- **Timeline**: the long strip at the bottom where your clips sit one after the other. Time runs
  from left to right.
- **Track**: one row of the timeline. Video tracks are called V1, V2, ... and audio tracks A1,
  A2, ... A clip on a higher video track covers the tracks below it.
- **Playhead**: the red line on the timeline that shows which moment you are looking at.

## The pages

Kinoklip is split into pages, one for each stage of the work. The buttons for the pages are
along the bottom of the window.

![The page buttons along the bottom of the window](images/getting-started/1-pages.png)

1. **Media**: look through your files and bring them into the project.
2. **Cut**: a quick, simple editor for fast rough cuts.
3. **Edit**: the main editor, where you will spend most of your time. This guide uses it.
4. **Compositing**: not finished yet.
5. **Color**: not finished yet.
6. **Audio**: mix the sound: track levels, meters, equaliser and dynamics.
7. **Deliver**: export the finished video to a file.

You can also switch pages with the keyboard: **Shift+2** (Media) up to **Shift+8** (Deliver);
**Shift+4** is the Edit page.

## Make a project

A project holds your timeline and the list of clips you use. When Kinoklip starts, it shows the
**Project Manager** with all your projects. You can open it again any time with
**File > Project Manager** (**Shift+1**).

![The Project Manager with the New Project button](images/getting-started/2-project-manager.png)

1. Click **New Project**.
2. Type a name.
3. Choose the kind of project:

   ![The New Project dialog with the two kinds](images/getting-started/3-new-project.png)

   1. **Single file**: everything, your clips included, is copied into one project file. Easy to
      move or back up, but it takes more disk space.
   2. **Linked**: the project only points to your clips where they already are. Smaller, but do
      not move or rename the clips afterwards.
4. Click **Create**. The project opens on the Edit page.

Save your work often with **File > Save Project** (**Ctrl+S**).

## The Edit page

![The Edit page with its four main areas](images/getting-started/4-edit-page.png)

1. **Media pool**: the clips of your project.
2. **Viewer**: shows the picture under the playhead.
3. **Inspector**: the settings of the selected clip (size, position, volume, effects, ...).
4. **Timeline**: your edit.

The buttons at the top of the window show or hide panels: **Media Pool**, **Effects**,
**Inspector** and more.

## Import your clips

Do one of these:
- Choose **File > Import Media** (**Ctrl+I**) and pick your video, audio and picture files.
- Or drag the files from your file manager into the media pool.

Your clips now appear in the media pool. Kinoklip reads common camera and phone formats (MP4,
MOV, MKV, H.264, H.265, ...), audio files and pictures.

## Put clips on the timeline

The easiest way: **drag a clip from the media pool onto the timeline**. It lands where you drop
it, with its picture on a video track and its sound on an audio track below. Hold **Ctrl**
while you drop to push the other clips aside instead of covering them.

To use only part of a clip:
1. Double-click the clip in the media pool. It opens in the source viewer.
2. Play to where the part should start and press **I** (mark in).
3. Play to where it should end and press **O** (mark out).
4. Press one of the edit buttons above the timeline (4 in the picture):

   ![The timeline tool bar](images/getting-started/5-timeline-tools.png)

   - **Insert** (**F9**): puts the part at the playhead and moves the later clips to the right.
   - **Overwrite** (**F10**): puts the part at the playhead, covering what is there.
   - **Place on Top** (**F12**): puts the part on the first free track above.
   - **Replace** (**F11**): swaps the clip under the playhead for this one.

   Or press **Shift+F12** (**Edit > Append at End**) to add it after the last clip.

## Play your edit

Click in the timeline, then:
- **Space**: play and stop.
- **L**: play forward; press it again to play faster.
- **J**: play backwards; press it again to go faster.
- **K**: stop.
- **Left** / **Right** arrow: one frame back or forward.
- **Up** / **Down** arrow: jump to the previous or next cut.
- **Home** / **End**: jump to the start or the end.

You can also click or drag in the ruler above the tracks to move the playhead.

## Cut, trim and delete

The tools on the left of the timeline tool bar (see the picture above):

- **Selection** (1, **A**): click clips to select them, drag them to move them.
- **Trim** (2, **T**): drag the edge of a clip to make it shorter or longer; the clips after it
  move along, so no gap appears.
- **Blade** (3, **B**): click a clip to cut it in two at that spot. Press **A** to go back to
  the selection tool.
- **Split Clip at Playhead** (5, **Ctrl+\\**): cuts the clips under the playhead in two (only
  the selected ones, if you selected any). Also in the **Timeline** menu.

**To trim** with the selection tool, drag the left or right edge of a clip. This leaves a gap
where the clip got shorter. **Shift+[** and **Shift+]** trim the start or the end of the clip
under the playhead to the playhead.

**To delete**, select a clip and press:
- **Backspace**: removes the clip and leaves a gap.
- **Delete**: removes the clip and closes the gap (a "ripple delete": everything after it moves
  left).

To remove a gap, click the empty space between two clips and press **Delete**.

Made a mistake? **Ctrl+Z** undoes, **Ctrl+Shift+Z** redoes.

## Add a title

1. Click **Effects** at the top left to open the effects library.
2. In **Toolbox > Titles**, find **Text**.
3. Drag **Text** onto an empty video track above your clips, where the title should appear. It
   becomes a five-second title clip.

   ![Dragging the Text title onto the timeline](images/getting-started/6-title.png)

4. With the title clip selected, open the **Title** tab of the Inspector and type your text.
   There you can also change the font, size, colour, outline and background. To move the title,
   use **Position X** and **Position Y** on the **Video** tab.

Drag the edges of the title clip to make it show for a shorter or longer time.

## Add a transition

A transition blends one clip into the next instead of a hard cut.

1. Click the cut between two clips on the timeline (the line where they touch).
2. Press **Ctrl+T** (**Timeline > Add Default Transition**). A one-second **Cross Dissolve**
   appears on the cut.

   ![A cross dissolve on the cut between two clips](images/getting-started/7-transition.png)

3. To make it longer or shorter, drag its left or right edge.

Other transitions (**Dip to Color**, **Wipe**, **Push**) are in the effects library under
**Toolbox > Video Transitions**: drag one onto a cut. To remove a transition, click it and press
**Delete**.

## Export your video

1. Click **Deliver** at the bottom of the window (**Shift+8**).

   ![The Deliver page](images/getting-started/8-deliver.png)

2. Click a preset at the top left, for example **YouTube 1080p** (1).
3. Enter a **Filename** and choose a **Location** (the folder) for the file.
4. Next to **Render** (2), choose **Entire Timeline**, or **In/Out Range** to export only the
   part between the in and out marks you set on the timeline with **I** and **O**.
5. Click **Add to Render Queue** (3). The job appears in the render queue on the right.
6. Click **Render All**.

   ![The render queue with Render All](images/getting-started/9-render-queue.png)

The bar on the job shows how far it is. When it is done, the folder button on the job opens the
folder with your new video. You can keep editing while it renders.

## Next steps

- [Export a video for YouTube](export-for-youtube.md)
- [Remove a green screen](remove-a-green-screen.md)
- [Speed up or slow down a clip](change-clip-speed.md)
- [Fade audio in and out, and add music](fade-audio-and-add-music.md)
