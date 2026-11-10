# Emoji in titles and captions

You will put emoji in a title or in a subtitle, in colour, and see them in the viewer and in the
exported video.

## Type an emoji

1. Select a title on the timeline and open the **Title** tab of the Inspector (for a subtitle,
   select the caption and open the **Captions** tab).
2. Click in the **Text** box and type or paste the emoji. Most desktops have an emoji picker:
   **Ctrl+.** or **Ctrl+;** in many apps, or copy an emoji from a web page.
3. The viewer shows the emoji in colour, next to the other text. A flag, a family, a hand with a
   skin tone and a digit in a box are each one emoji, as on your phone.

Size, colour, tracking and alignment of the title work as before; the emoji take the size of the
text around them.

## What the title's looks do to emoji

- **Outline:** the outline is drawn around the letters. Emoji keep their own colours and get no
  outline.
- **Drop shadow:** an emoji gets a shadow in the shadow colour, like the letters.
- **Background:** the box is drawn behind the emoji too.
- **Layers:** every text layer of a [title with layers](add-layers-to-a-title.md) can hold emoji,
  and a rotated layer turns its emoji.
- **Exports and burnt-in subtitles:** the exported video shows the same emoji as the viewer.

## Which font draws the emoji

Kinoklip uses the colour emoji font of your system when there is one ([Noto Color Emoji](https://github.com/googlefonts/noto-emoji), Twemoji,
or the font of your desktop). The AppImage also carries Noto Color Emoji and uses it when your
system has none, so emoji look the same on every computer. Subtitle files exported as text
(`.srt`, `.vtt`) keep the emoji as characters; the player draws them.

## If something goes wrong

- **The emoji show as empty boxes.** The text uses a font without them and no colour emoji font
  was found. Install one (for example the package **Noto Color Emoji**,
  `fonts-noto-color-emoji` on Debian and Ubuntu, `noto-fonts-emoji` on Arch) and start Kinoklip
  again.
- **An emoji is black and white.** Some symbols are text by default; add the **emoji selector** by
  picking the symbol from the emoji picker instead of typing the plain symbol.
