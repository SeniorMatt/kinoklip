# Remove a background with the AI tool

You will cut a person out of a clip without a green screen, so they appear over another picture.
This uses **Background Removal**, a tool that runs an AI model. It is optional: Kinoklip works
without it, and the model is only downloaded when you ask.

## What you need

- A clip with a person in it. Plain, well-lit footage works best; the person should not be tiny.
- The model, downloaded once (see step 3). It is about 14 MB.
- A background: another video or a picture. Put it on track **V1** and the clip with the person on
  **V2**, right above it.

## Is it private?

Yes. The model is downloaded once and then runs entirely on your computer. Nothing is uploaded, no
account is needed, and Kinoklip sends no usage information. You can delete the model again at any
time in **Preferences > System > AI Models**.

## Steps

1. Click **Effects** at the top left to open the effects library.
2. Open **Video Effects > Key**, find **Background Removal** and drag it onto the clip with the
   person. You can also type "background" in the search field.
3. Click the clip, then open the **Effects** tab of the **Inspector**. At the top of the settings
   you see the **AI** label and a status line. If the model is missing it says so. Press
   **Download Model...**. A window shows the name of the model, its size, its licence and where it
   comes from. Nothing is downloaded until you press **Download** in that window; **Cancel** closes it.
4. When the download is finished, the person is cut out. The first frames of a clip show the
   normal picture for a moment: the cutting out happens in the background, so playing never
   stops. Each finished frame is kept on disk, so the clip is only cut out once.
5. To have the whole clip ready before you play it, press **Process Whole Clip**. A progress bar
   shows how far it is. **Cancel** stops it.
6. Check the edges. Set **Output** to **Matte Only** to see what stays (white) and what is removed
   (black), then set it back to **Foreground**.

## Settings

| Setting | What it does |
|---|---|
| **Output** | **Foreground** keeps the person. **Matte Only** shows the cut-out as black and white. **Background Only** keeps everything except the person. |
| **Threshold** | Where the edge is drawn. Higher removes more of the soft edge area. |
| **Edge Softness** | How gradual the step from kept to removed is. 0 is a hard edge. |
| **Shrink / Grow** | Moves the edge inwards (negative) or outwards (positive), in pixels. Shrink a little to lose a light fringe. |
| **Feather** | Blurs the edge, in pixels. |
| **Quality** | **Preview (fast)** looks at a small picture and keeps up with playing. **Full (slow)** looks at a larger picture and finds finer edges, such as hair; use it for the final video. Each quality has its own cached cut-outs. |

## Good to know

- **Playing and exporting.** While you play, a frame that is not cut out yet shows the normal
  picture. When you export (and when Kinoklip builds its render cache), it waits for every
  frame, so the finished video never has an uncut frame.
- **Speed.** A computer without a graphics card for AI work still manages this: it runs on the
  processor. Expect a fraction of a second per frame at the preview quality, and longer at full
  quality. Process the clip once and it is ready from then on.
- **When it is greyed out or says "not available".** The status line gives the reason: the
  model is not downloaded yet, or this copy of Kinoklip was built without the AI runtime.
- **Clean up.** **Preferences > System > AI Models** shows the model, how much room the cut-outs
  use, and has **Remove Model** and **Clear Matte Cache**.
- The cut-out is made from the clip as it is in the media file. Other effects above it in the
  list, and cropping, do not change the cut-out.
