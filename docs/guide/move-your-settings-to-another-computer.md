# Move your settings to another computer

You will save all of Kinoklip's settings in one file, and load them on another computer (or on
the same one after a fresh install). Your keys, your layout, your preferences and your presets
come with you in one click each way.

## What is in the file

- Everything in **Preferences**: colours and style, autosave, playback and editing options.
- Your **keyboard**: the profile you chose (Resolve, Vim, [Premiere Pro](https://www.adobe.com/products/premiere.html), ...) and every key you
  changed, and your own keyboard profiles.
- The **layout**: window size, which panels are open and how big, the page you were on.
- **View options**: the Timeline View Options, Media Pool view, Edit Index columns, viewer
  guides, and the choices you made with "do not ask again".
- Your **presets**: guide presets, power bins and your own render presets.

## What is not in the file

- Projects and media. Copy those yourself (or use a single-file project).
- Caches and generated files: thumbnails, waveforms, proxies, the render cache, autosaves.
- Things that belong to one computer: the cache, proxy, project library and LUT folders, the
  audio output device, and the folders you last browsed.
- The list of recent projects, unless you tick a box (its paths only help if your projects are
  in the same places on the other computer).
- Nothing secret: Kinoklip stores no passwords or keys.
- **Local Control** (Preferences > System > Local Control) is never switched on by a file. If
  you use it, switch it on again by hand: it lets other programs drive the editor, so only you
  should turn it on.

## Save your settings

1. Choose **Kinoklip > Preferences** and open **Import and Export** (under User).
2. Click **Export Settings...**, choose where to save, and click **Export**. The file ends in
   `.kinoklip-settings`.

## Load them on another computer

1. Copy the file over, open **Preferences > Import and Export** and click **Import Settings...**.
2. Choose the file. Kinoklip lists what it contains. Importing **replaces your current
   settings** with the values in the file; settings the file does not mention stay as they are.
3. Click **Replace My Settings**. Before anything changes, Kinoklip saves your current settings
   to a backup file and tells you where (a `settings-backups` folder next to its settings file).
4. **Restart Kinoklip.** The layout, the keys and most options are read when it starts, so
   they apply after the restart.

To go back, import the backup file the same way.

## From the command line

```
kinoklip settings export --file my-settings.kinoklip-settings
kinoklip settings import --file my-settings.kinoklip-settings
```

Both work without a project and follow the same rules. Add `--recent-projects` to include the
recent-projects list.

## If the file is refused

A file made by a newer Kinoklip says so and asks you to update. A file that is damaged, too big
or not a settings file is refused, and entries Kinoklip does not understand are skipped with a
note. A settings file never writes anywhere except Kinoklip's own settings and data folders.
