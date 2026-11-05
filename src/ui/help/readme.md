# src/ui/help — in-app help

Links the app to the user guide (`docs/guide/`, rendered by GitHub). Depends on `ui/common`,
`platform` and `core` (logging); used by `app/MainWindow`, `ui/inspector` and `ui/effects`.

| File | What |
|---|---|
| `GuideIndex` | the guide's index (`docs/guide/index.json`, format of the guide contract): defensive parsing, `resolve(contexts)` (pure), `search(text)`, article / start URLs |
| `Guide.h` | `guide::index()` (the bundled `:/guide/index.json`, loaded once), `guide::openArticle()` / `openUrl()` through `platform::openUrl`, widget help contexts (`setHelpContext()`, `contextsAt()`), the "?" `HelpButton`, `createHowToAction()`; test hooks `setIndexForTesting()`, `setUrlOpenerForTesting()` |
| `HelpController` | the main window's wiring: F1 (`help.guide_context`), Help > Guide… (`help.guide`), Help > Getting Started (`help.getting_started`), the "?" buttons in panel title bars |
| (app) | Help > Report a Bug or Inconsistency lives in `src/app/BugReportDialog` (it reads app-level settings); it opens GitHub through `guide::openUrl` too, so the same test hook applies |
| `GuideDialog` | Help > Guide…: search field + article list ("Start page" on top); Enter or double-click opens |
| `CopyLinkDialog` | shown by `guide::openUrl()` when no browser could be opened: the link in a selectable read-only field, Copy Link, Close; callers that close on success (Guide…, the bug report) stay open |

## The index

`resources/guide.qrc` bundles `docs/guide/index.json` as `:/guide/index.json`, so the app
always carries the index of its own version. The guide writer owns that file; its format:

```json
{ "formatVersion": 1, "base": "https://.../docs/guide/", "start": "readme.md",
  "articles": [ { "id": "...", "title": "...", "file": "....md", "summary": "...",
                  "keywords": ["..."], "contexts": ["effect:chroma_key", "page:edit"] } ] }
```

An unknown `formatVersion` or bad JSON gives an invalid index: no "?" buttons and no
"How to use" entries, and F1 opens the start page under the built-in base URL
(`GuideIndex::kDefaultBase`). Articles without id / title or with a file that is not a plain
relative path are skipped.

## Context keys

| Key | Who reports it |
|---|---|
| `effect:<EffectRegistry id>` | the Effects library's tree (its current entry) and every Inspector section showing an effect, transition, title or generator (`InspectorPanel::attachHelp()`) |
| `action:<ActionRegistry id>` | F1 pressed while a menu is open, for the highlighted command |
| `panel:<key>` | `PanelFrame::setPanelKey()`: media_pool, effects, edit_index, inspector, keyframes, mixer, metadata, timeline, viewer, render_settings, render_queue |
| `page:<id>` | `Page::pageId()` of the page a widget is on, and the visible page |

A new widget reports a context with `guide::setHelpContext(widget, key)`; a new panel calls
`setPanelKey()` in its constructor. F1 collects the keys from the widget under the pointer up
to the window, then from the focus widget, then the visible page, and
`GuideIndex::resolve()` picks the most specific kind that has an article
(effect > action > panel > page); nothing matching opens the start page.

## "?" buttons and menus

Shown only where the index has an article: at the right of a panel's title bar
(`HelpController::decoratePanels()`, once the window is built) and in an Inspector section
header after its title. Right-click on an Effects library entry or an Inspector section header
offers "How to use <name>…". All of them open the article's URL (`base + file`) in the
browser.

## Tests

`tests/app/test_inapphelp.cpp`: parsing, bad JSON, resolution order, search, F1 on the
Inspector and the Deliver page, the "?" buttons, the library's context menu, the Guide dialog.
The URL opener is replaced, so no browser opens.
