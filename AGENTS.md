# Notes for AI agents working on Elfeed2

A standalone C++20 / wxWidgets feed reader, successor to Emacs Elfeed.
Single-binary GUI app, no plugins. Scope is intentionally a bit
broader than classic Elfeed — built-in podcast / yt-dlp downloads
manager, inline image cache, etc.

The maintainer is Chris Wellons (skeeto, author of classic Elfeed and
dcmake). Concrete prose > corporate prose. He notices when a comment
explains the *what* instead of the *why*.

## Build & run

```sh
cmake -B build
cmake --build build --target elfeed2
build/elfeed2.app/Contents/MacOS/elfeed2          # macOS
build/elfeed2.exe                                 # Windows
build/elfeed2                                     # Linux
```

CLI options: `--db PATH` and `--config PATH` (see README). Use them
when stress-testing so the production database stays untouched. The
single-instance lock is per-DB so test instances run alongside the
real one.

`-DDEPS=LOCAL` switches from FetchContent-pinned versions to system
libraries (for distro packagers). `cmake/Toolchain-Mingw64.cmake`
cross-builds a self-contained Windows .exe.

`tests/fakefeeds.py` serves N synthetic Atom feeds on localhost with
configurable latency and failure injection. Use this for any
"generate lots of fetch / log / download activity" testing — the
maintainer reads real feeds with this app and isn't going to slam
real publishers for testing.

## Source layout

- **`src/app.cpp` / `src/app.hpp`** — wxApp entry, OnInit, single-
  instance check, CLI parse, elfeed_init / shutdown lifecycle.
- **`src/elfeed.hpp`** — central Elfeed state struct + all public
  function declarations. Deliberately does NOT include wx headers
  (only forward declares wx pointer types) so non-UI modules don't
  pull in the wx world. Anything wx-flavored stored here uses
  primitives (uint32_t color, std::string for paths, etc).
- **`src/main_frame.{hpp,cpp}`** — the wxFrame + wxAUI orchestrator.
  Owns all the panels.
- **`src/entry_list.{hpp,cpp}`** — virtual-list wxDataViewCtrl for
  entries. Includes the TabularTextRenderer for the date column.
- **`src/entry_detail.{hpp,cpp}`** — wxHtmlWindow preview pane.
- **`src/feeds_panel.{hpp,cpp}`** — feeds list with right-click
  context menu and `↳` indicator for canonical-URL redirects.
- **`src/log_panel.{hpp,cpp}`** — log table with filter checkboxes,
  Copy/Export/Clear context menu.
- **`src/downloads_panel.{hpp,cpp}`** — download queue with
  wxDataViewProgressRenderer per row.
- **`src/db.cpp`** — all SQLite. Schema lives in `schema_sql` at top.
- **`src/feed.cpp`** — pugixml parser for Atom/RSS/RDF.
- **`src/filter.cpp`** — filter DSL (`+tag -tag @age =feed ~feed
  !title #limit bareTitle`). `=` and `~` are case-insensitive
  substring (not regex — explicit choice).
- **`src/fetch.cpp`** — fetch worker pool, posts results to inbox.
- **`src/download.cpp`** — download manager. Subprocess (yt-dlp /
  curl via wxProcess) AND HTTP-direct (cpp-httplib via std::thread)
  paths.
- **`src/image_cache.{hpp,cpp}`** — preview pane image cache. SQLite
  blob storage + LRU eviction; URLs in entry HTML rewritten to
  `data:` URIs.
- **`src/data_uri_handler.{hpp,cpp}`** — wxFileSystemHandler for
  `data:` URIs (wx 3.2 has none built in).
- **`src/http.hpp`** + `http_posix.cpp` / `http_win.cpp` — HTTP
  client. cpp-httplib + mbedTLS (or OpenSSL with `-DDEPS=LOCAL`)
  on POSIX; WinHTTP on Windows.
- **`src/util.{hpp,cpp}`** — paths, time formatting, filename
  sanitization, MIME helpers, wxDataView column persistence + sort
  helpers.
- **`src/config.cpp`** — line-oriented `ssh_config`-style config
  parser.
- **`src/elfeed_import.cpp`** — one-way importer for classic
  Elfeed's `index` s-expression DB.

## Architecture notes

### Threading model

UI thread owns the database, all wxWidgets state, and `app->log` /
`app->feeds` / `app->entries` / `app->downloads` / etc. Workers
push to **inbox vectors** under per-resource mutexes, then call
`app_wake_ui(app)` which posts a wxThreadEvent. `MainFrame::on_wake`
drains inboxes (fetch results, image cache results) and triggers
appropriate refreshes.

The pattern: workers don't touch DB or UI directly. They produce
data, the UI thread consumes it.

Notable exception: image-cache writes were per-wake (one BEGIN/INSERT
/COMMIT per cell update), now coalesced via timer. Log persistence
is similarly **timer-coalesced (5s) to avoid one-fsync-per-wake**;
the same pattern is worth applying anywhere you find yourself
reaching for synchronous DB writes from a high-frequency callback.

### Database

- Tables: `feed`, `entry`, `entry_tag`, `entry_author`,
  `entry_enclosure`, `ui_state`, `image_cache`, `log_entry`.
- Author / enclosure are **relational** (not JSON blobs). Tags too.
- Tag preservation across refetch: the upsert path checks
  `EXISTS(...)` first; if the entry already exists, no tag inserts
  happen. Matches classic Elfeed's merge semantics. Without this
  precheck every refetch silently restores `unread`.
- **Date is NOT updated on refetch.** The parser falls back to
  `time(now)` for entries without a published date; if we updated
  on every refetch, undated entries would perpetually bubble to the
  top. First-sight wins.
- `ui_state` is the kitchen-sink key/value table for everything
  remembered across runs: window geometry, AUI perspective,
  per-panel column widths/visibility, per-panel sort, filter bar
  text, last_fetch timestamp, log filter checkboxes. New "do you
  want to remember this?" features should usually go here.
- `image_cache` is LRU-evicted to 256 MiB.
- `log_entry` is purged on startup to the `log_retention_days`
  window (default 90).

### Config file

`ssh_config`-style. Keyword value, blank lines cosmetic, comments
are `#` at line start OR whitespace-`#`-whitespace (so hex colors
like `#f9f` aren't eaten). URL lines open a stanza; subsequent
lines apply until the next URL/alias line. `alias NAME TEMPLATE`
defines macros with `{}` substitution.

Reload-config (Ctrl+Shift+R) rebuilds subscription state in place;
all config-derived fields are reset to struct defaults first to
avoid accumulator bugs (notably `ytdlp_args`).

## Conventions

- **UTF-8 everywhere.** Use `wxT(...)` for any non-ASCII string
  literal so MSVC doesn't mangle it through wxConvLibc / CP1252.
  Pass `/utf-8` to MSVC (already set in CMakeLists).
- **Display vs identifier strings.** `Elfeed2` (capitalized) for
  user-facing display strings — wxFrame title, About dialog,
  CFBundleName, Windows ProductName / FileDescription. `elfeed2`
  (lowercase) for paths, identifiers, target names, lock tokens,
  CFBundleExecutable, manifest assemblyIdentity.
- **Comments explain the why.** Prefer load-bearing comments over
  restating what code does. The maintainer's an experienced systems
  programmer; a comment like "increments x" wastes his time.
- **No emoji** in code or commits unless explicitly requested.
- **Don't proactively create new docs** (`.md`) without being asked.
  This `AGENTS.md` was a request.
- **wxDataViewVirtualListModel sort: override `Compare` to return
  0.** Otherwise the default `pos1-pos2` reverses descending sorts
  on macOS's native NSOutlineView, on top of whatever apply_sort
  already produced.
- **wxAuiPaneInfo `MinSize` is a hard floor**, including from
  serialized perspectives. Set it to your desired initial dock size,
  then loosen via `loosen_pane_min_sizes()` AFTER every
  LoadPerspective call. The dock_size is preserved in the
  perspective string regardless.
- **Panels reach MainFrame via `wxGetTopLevelParent(this)` +
  `dynamic_cast<MainFrame *>`.** No back-pointer members. See
  `flash_status`, `try_preset_key`, `copy_to_clipboard`.

## wxWidgets gotchas we've hit

- **`wxInitAllImageHandlers()` in OnInit** or every `<img>` triggers
  "Unknown image data format" pop-ups. wx defaults to BMP-only.
- **`wxFileSystem` has no built-in `data:` handler** — register
  `DataURIHandler` in OnInit. Without it, our base64-inlined
  preview-pane images silently drop.
- **`wxHtmlWindow` is severely CSS-limited** but honors legacy
  `<body bgcolor= text= link= vlink=>` attributes — that's how the
  preview pane adapts to dark mode (`render()` wraps body using
  `wxSystemSettings::GetColour`).
- **`wxHtmlWindow` link clicks** default to in-pane navigation via
  wxFileSystem (which fails for http/s). Bind
  `wxEVT_HTML_LINK_CLICKED` and route to `wxLaunchDefaultBrowser`.
- **`wxDataViewProgressRenderer` wants a `long` model column.** For
  text columns rendered as bars, change `GetColumnType(col)` to
  return `"long"` for that column index.
- **`wxDataViewCtrl::ClearColumns()` + re-Append** is the only way
  to programmatically reorder columns; there's no SetColumnsOrder.
  See `build_columns()` in each panel.
- **`wxDATAVIEW_COL_HIDDEN` is a STATE flag**, not a permission.
  Don't put it in the construction-time flags or columns hide on
  Windows. Use `SetHidden(true)` post-construction.
- **`wxFontFamily::wxFONTFAMILY_TELETYPE`** picks the platform's
  native monospace font (Menlo / Consolas / Monospace).
- **macOS tabular figures** via Core Text feature settings
  (`kNumberSpacingType` + `kMonospacedNumbersSelector`) construct
  a CTFont, wrap with `wxFont(CTFontRef)`. Used by
  `TabularTextRenderer` in entry_list.cpp for the Date column.
- **`wxDataViewCustomRenderer::GetSize()` and `Render()` must use
  the same font weight.** `GetSize` is what reserves cell width;
  if it measures regular and `Render` paints bold (for unread), the
  framework truncates with `…`.
- **`wxAboutBox` opens a generic dialog when License or WebSite is
  set**, but its License pane is a wxCollapsiblePane that on
  expansion resizes the dialog to `wxGetDisplaySize().x / 3`. On a
  wide display it blows up to 1700 px. Use a custom wxDialog
  instead. See `MainFrame::on_about`.
- **Single-letter shortcut hints** in popup menus via `\tKey` —
  display-only; the actual keystroke handling lives in
  `wxEVT_CHAR_HOOK` handlers. Bare letters work in popups; in the
  main menu they'd grab the global accelerator.

## Platform-specific notes

### macOS

- Bundle is built via `MACOSX_BUNDLE` target prop. `Info.plist.in`
  is hand-rolled (CMake's bundled template isn't always found by
  Ninja).
- `CFBundleIdentifier = com.nullprogram.elfeed2` is required for
  NSOpenPanel / NSSavePanel to work — absent it, save dialogs
  silently fail with "domain name cannot be nil or empty".
- `open -R <path>` reveals a file in Finder; used by Downloads
  panel's "Show in Finder".
- `wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW)` follows the
  OS appearance setting at runtime; subscribe to
  `wxEVT_SYS_COLOUR_CHANGED` for live theme switches.

### Windows

- DPI awareness via hand-rolled `src/elfeed2.manifest` (PerMonitorV2).
  Without this Windows bitmap-scales the window on hidpi displays.
- VERSIONINFO + ICON resources via `src/elfeed2.rc.in` configured
  with `PROJECT_VERSION_*`.
- WinHTTP for HTTP — no cpp-httplib involvement.
- mingw cross builds are self-contained (libgcc / libstdc++ /
  winpthread statically linked). See `cmake/Toolchain-Mingw64.cmake`.

### Linux

- DEPS=LOCAL is the typical packager path. Debian's
  `libcpp-httplib.so` is the split-mode library (must be linked,
  not just headers).
- mbedTLS support in older distro `cpp-httplib` packages (≤ 0.18) is
  incomplete, so DEPS=LOCAL uses OpenSSL instead.
- No standard "reveal in folder" verb — fall back to opening the
  parent dir via wxLaunchDefaultApplication.

## Commits

Maintainer's commit style — match it:
- Short summary line, no period.
- Blank line, then longer prose explaining the why (not the what).
- Concrete; no fluff. References to specific files / functions OK.
- Trailer: `Co-Authored-By: Claude Opus 4.7 (1M context)
  <noreply@anthropic.com>` (the version may bump over time; check
  the existing `git log` for the current spelling).
- Don't push without being asked.
- Don't include unrelated changes.

When in doubt about a design choice, propose options with their
trade-offs rather than committing immediately. The maintainer wants
to make the call; you implement it.
