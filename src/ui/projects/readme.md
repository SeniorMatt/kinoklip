# src/ui/projects — the Project Manager window

The Project Manager (spec section 38): File > Project Manager (Shift+1), shown at start-up.
The library logic is in `src/projects`; `app/ProjectController` creates the window, gives it the
"save changes?" guard and opens what it asks for.

## Key classes

| File | What |
|---|---|
| `ProjectManagerWindow.h` | the window (QDialog, shown window-modal over the editor with `show()`): places (Project Library, Recent Projects), folder path buttons (drop targets), search, sort, cards / list, card size slider, bottom bar (Import, Export, New Folder, New Project, Open), context menu, keys (Enter, Delete, F2, Backspace), settings `projectManager/*` |
| `ProjectLibraryModel.h` | one folder (or the Recent list) as a table: columns Name, Date Modified, Duration, Resolution, Frame Rate, Kind, Location; roles for the cards; lazy metadata / posters; rename by `setData()`; drag and drop (move inside the library, import from outside). `ProjectSortProxy`: folders first, name search |
| `ProjectCardDelegate.h` | a card: poster (letterboxed) or folder / placeholder glyph, badges (Open, Media offline, the kind), name, date, "duration · resolution · fps"; error cards |
| `NewProjectDialog.h` | name + kind (Single file default, Linked); remembers the choice (`projectManager/newProjectKind`); a kind the build cannot create is disabled with a tooltip |

## Behaviour notes
- Metadata is asked for in `data()`, so only painted cards (and the sort column, when it needs
  metadata) cause reads; sorting by name or date never opens a project.
- Opening: `openPath()` enters folders; for projects it calls the switch guard and emits
  `openRequested(path)`; the controller loads and hides the window (an error dialog keeps it).
- Export offers both kinds; another kind than the project's converts it on a worker thread with
  progress (`ProjectLibrary::exportProject()`: Pack / Unpack); a linked project's context menu
  also has Export as Single File.
- Colours come from `kk::schemeColors()` at paint time; icons from `kk::icon()`.

## How to extend
**A new action:** add a slot (and a dialog-free variant for tests) to `ProjectManagerWindow`,
put it in `showContextMenu()` and, if it needs a button, the bottom bar; refuse it for the
open project through `ProjectLibrary::isOpenOrContainsOpen()` when it changes the file.
**A new column:** add it to `ProjectLibraryModel::Column`, `headerData()` and `data()` (a
`SortRole` value of one type per column).
