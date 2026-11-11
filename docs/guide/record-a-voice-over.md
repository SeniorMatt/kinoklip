# Record a voice-over

Speak your narration while the video plays, and Kinoklip puts the recording on an audio track,
lined up with the picture. You need a microphone (or any audio input) connected to the computer.
Use headphones for the timeline's sound: the microphone is never played back through the speakers,
but the speakers' sound would reach the microphone.

## Record

1. Put the playhead where the narration starts. The timeline plays from there, so it must be
   before the end of the timeline.
2. Click the **R** button in the header of the audio track you want to record onto (on the
   **Edit** page or the **Audio** page; the mixer strip on the **Audio** page has one too). The
   button turns red and a thin level bar appears along the foot of the header: speak and watch
   it move. Keep the bar out of the red part; a red mark at its end shows that the sound was
   too loud and distorted for a moment.
3. To record from another microphone, right-click the **R** button and choose an input. The
   first entry, **Default**, follows the setting in **Preferences > System > Audio I/O**.
4. Click the red **Record** button in the viewer's transport bar, or press **Ctrl+Alt+R**
   (**Playback > Record Voice-Over**). Numbers count down over the picture (3, 2, 1). When they
   end, the timeline plays and **REC** shows in the corner of the picture. Speak.
5. Press **Space** (or **Stop**) to end the take.

Only one track can be armed at a time: arming another track moves the red button. Click the red
**R** again to disarm. Arming is not saved with the project.

The recording is added to the track at the place where you pressed Record, over whatever was
there, as one step: **Ctrl+Z** takes the whole take away again and restores the clips it
covered.

## Where the takes go

Every take is a WAV file called after the track and a number, for example **A1 take 1.wav**:

- In a normal project it is saved in a folder named **Voice Over** next to the project file.
- In a single-file project it is copied into the project file, like any imported file.

Each take is also added to the Media Pool, in a bin called **Voice Over**. Earlier takes stay
there, so you can try again and still use the first one: drag it from the pool onto the timeline.

## Settings

Open **Preferences > System > Audio I/O**:

- **Input device**: the microphone used by tracks that you have not given their own.
- **Voice-over count-in**: 0 to 10 seconds. 0 starts at once.
- **Voice-over extra latency**: see below.
- **Voice-over WAV format**: 24-bit (default) or 16-bit.

## If the voice comes in late

A microphone and the computer need a moment to deliver the sound, so a recording is always a
little behind what you said. Kinoklip subtracts the delay that the microphone reports, but many
do not report it. If your voice is still late against the picture, raise **Voice-over extra
latency** by 20 to 50 ms, record a short test and look again. The file keeps the whole
recording, so you can also move the sound with **Slip**.

## If something goes wrong

- **The R button is grey, or a message says there is no microphone.** Connect a microphone and
  try again. The tooltip of the **R** button names the problem. If the message says the system
  does not let Kinoklip use the microphone, allow it in the system's privacy settings.
- **Record says there is nothing to play.** The playhead is at the end of the timeline. Move it
  earlier: the recording stops where playback stops.
- **Nothing was recorded.** The microphone delivered no sound. Check the chosen input and the
  system's input volume.
- **The track is locked.** Unlock it first.
