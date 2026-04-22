# Elfeed2 : the Elfeed feed reader experience without Emacs

Standalone feed reader, successor to [Elfeed][elfeed]. The goal: replicate
the Elfeed experience without Emacs. Built with C++20, wxWidgets, SQLite3,
pugixml, cpp-httplib, and mbedTLS.

![](assets/screenshot.png)

**This is an early work in progress.** Expect breakage. Config format and
database schema are not locked down. You may need to delete your database
between updates. Use the import feature (File menu) to load your classic
Elfeed database (one-way conversion) so you have something meaningful to
test.

[Input and ideas are welcome][discussion] as the UI takes shape. The scope
is somewhat broader, and Elfeed2 has a built-in video (via user-supplied
[yt-dlp][]) and podcast download manager.

## Building

Requires CMake 3.25+ and a C++20 compiler. By default all dependencies
are fetched automatically (pinned versions, hermetic build):

    $ cmake -B build
    $ cmake --build build

Distribution packagers can switch to system-installed dependencies
(wxWidgets ≥ 3.2, SQLite3, pugixml, OpenSSL, cpp-httplib) via
`-DDEPS=LOCAL`:

    $ cmake -B build -DDEPS=LOCAL
    $ cmake --build build

`DEPS=LOCAL` builds use OpenSSL for TLS (universally available
wherever cpp-httplib is packaged); the bundled `DEPS=FETCH` build
uses pinned mbedTLS 3.6.2 to keep the self-contained binary smaller
and avoid an OpenSSL runtime dependency.

To cross-compile a self-contained Windows binary from Linux/macOS using
mingw-w64 (apt: `gcc-mingw-w64-x86-64 g++-mingw-w64-x86-64`, brew:
`mingw-w64`):

    $ cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Mingw64.cmake
    $ cmake --build build-win

The resulting `elfeed2.exe` statically links libstdc++, libgcc, and
winpthread, so it doesn't drag any mingw runtime DLLs along with it.

## Command line

    elfeed2 [-h] [-d|--db PATH] [-c|--config PATH]

By default, the database lives at the platform's user data directory
(`~/Library/Application Support/elfeed2/elfeed.db` on macOS,
`%APPDATA%\elfeed2\elfeed.db` on Windows, `$XDG_DATA_HOME/elfeed2/elfeed.db`
on Linux), and the config at `$XDG_CONFIG_HOME/elfeed2/config`.

Override either with `--db` and `--config`. Useful for running a test
or scratch instance alongside your real one — the single-instance
guard is per-DB, so multiple instances against different databases
don't conflict.

For stress testing the fetch + log + download paths without bothering
real servers, `tests/fakefeeds.py` serves N synthetic Atom feeds on
localhost with optional latency and failure injection:

    # Terminal 1: serve 100 fake feeds with realistic latency
    python3 tests/fakefeeds.py --feeds 100 --latency-max 2.0 --fail-rate 0.05

    # Terminal 2: generate a matching config and launch a test instance
    python3 tests/fakefeeds.py --feeds 100 --print-config > /tmp/test.conf
    elfeed2 --db /tmp/test.db --config /tmp/test.conf

Press `f` inside the test instance to trigger a parallel fetch and
watch the Log + Downloads panels under load.

## Configuration

The configuration file is at `$XDG_CONFIG_HOME/elfeed2/config` (typically
`~/.config/elfeed2/config`) on all platforms. It is a line-oriented
format inspired by `ssh_config`: directives are `keyword value`, blank
lines are cosmetic, and comments start with `#` at the start of a line
or preceded by whitespace and followed by whitespace (so `#`-prefixed
values like `#f9f` aren't mistaken for comments).

### Global settings

    download-dir          ~/Downloads      # ~ expands to your home
    yt-dlp-program        yt-dlp
    yt-dlp-arg            --no-warnings    # repeatable
    yt-dlp-arg            --embed-metadata
    default-filter        @6-months-ago +unread
    max-connections       16
    fetch-timeout         30               # per-feed, seconds
    max-download-failures 5                # then mark "failed" until Retry
    log-retention-days    90               # DB log history kept across runs

### Feeds

A line whose first token contains `://` opens a new feed stanza.
Lines after it apply to that stanza until the next URL line. `title`
overrides the feed's self-declared title; `tag` adds autotags (one or
more per line, repeatable).

    https://acoup.blog/feed/
      title A Collection of Unmitigated Pedantry
      tag   blog history

    https://example.com/comic/feed/
      tag comic webcomic

Indentation is cosmetic — the parser doesn't require it.

### Aliases (macros)

An `alias NAME TEMPLATE` directive defines a shortcut. Using the
alias name in place of a URL expands `{}` in the template with the
rest of the line:

    alias youtube https://www.youtube.com/feeds/videos.xml?channel_id={}
    alias reddit  https://www.reddit.com/r/{}/.rss

    youtube UCbtwi4wK1YXd9AyV_4UcE6g
      title Adrian's Digital Basement
      tag   retrocomputing

    reddit cpp
      tag programming

### Per-tag colors

A `color TAG #RRGGBB` (or `#RGB` shorthand) directive tints entries
in the listing whose tag list includes `TAG`. The first directive
whose tag matches a given entry wins, so order them by priority.

    color youtube #ff99ff
    color news    #88c0d0
    color hn      #d08770

Foreground only (not background), applied on top of the default styling
including bold for unread entries. Stored only in config — not
persisted to the database.

### Filter presets

A `preset KEY FILTER` directive binds a single key to a filter string.
While the entry list has focus, pressing that key jumps the filter
bar (and the listing) to that filter. Built-in navigation keys
(`j`/`k`/`g`/`G`/`u`/`r`/`b`/`y`/`d`/`f`/`s`/`/`) take precedence.

    preset h @1-month +unread
    preset v @1-month +youtube
    preset n @1-month -youtube

The current filter is also persisted to the database when the filter
bar loses focus, so it's remembered across restarts.

## Usage

### Filter syntax

The filter bar controls which entries are displayed. Press **s** or
**/** to edit the filter, **Enter** to submit, **Escape** to cancel.
Tokens are space-separated:

| Prefix | Meaning                       | Example               |
|--------|-------------------------------|-----------------------|
| `+`    | Must have tag                 | `+unread`             |
| `-`    | Must not have tag             | `-junk`               |
| `@`    | Age limit                     | `@6-months-ago`       |
| `@`    | Age range                     | `@1-year-ago--1-week` |
| `#`    | Limit result count            | `#50`                 |
| `=`    | Feed URL must contain         | `=example.com`        |
| `~`    | Feed URL must not contain     | `~spam.example`       |
| `!`    | Title must not match          | `!sponsor`            |
| (bare) | Title must match              | `linux`               |

Age units: `year`/`y`, `month`/`M`, `week`/`w`, `day`/`d`, `hour`/`h`,
`min`, `sec`/`s`. Suffixes like `-ago` and `-old` are ignored.

### Listing keys

| Key         | Action                                   |
|-------------|------------------------------------------|
| `j` / `k`   | Move cursor down / up                   |
| `g` / `G`   | Jump to first / last entry               |
| `Enter`     | Focus the preview pane (reader mode)     |
| `b`         | Open entry link in browser               |
| `y`         | Copy entry link to clipboard             |
| `u`         | Mark unread (advances cursor)            |
| `r`         | Mark read (advances cursor)              |
| `Ctrl+L`    | Re-apply filter (re-query)               |
| `v`         | Toggle visual selection                  |
| `Ctrl+A`    | Select all entries                       |
| `s` / `/`   | Edit filter                              |
| `f`         | Fetch all feeds                          |
| `d`         | Download enclosure                       |
| `D`         | Toggle Downloads panel                   |
| `l`         | Toggle Log panel                         |
| `Escape`    | Clear selection                          |

Visual selection (`v`) anchors at the focused row; subsequent `j`, `k`,
`g`, `G` extend the selection from the anchor to the new cursor. Any
row-acting key (`u`, `r`, `b`, `y`, `d`) runs against the whole range
and then exits visual mode; `Escape` or `v` again also exits.

### Filter editing keys

| Key         | Action                                   |
|-------------|------------------------------------------|
| `Enter`     | Submit filter                            |
| `Escape`    | Cancel and restore previous filter       |
| `Backspace` | Delete character                         |
| `Ctrl+W`    | Delete word                              |

### Entry detail keys

| Key     | Action                          |
|---------|---------------------------------|
| `q` / `Escape` | Return to listing        |
| `n` / `p`      | Next / previous entry    |
| `b`            | Open link in browser     |
| `y`            | Copy link to clipboard   |
| `d`            | Download enclosure       |
| `u`            | Toggle unread            |

## Data

Window geometry and UI state are persisted in the database as well as feed
and entry data. The database is stored at:

* Windows: `%APPDATA%\elfeed2`
* macOS: `~/Library/Application Support/elfeed2`
* Linux: `$XDG_DATA_HOME/elfeed2`


[discussion]: https://github.com/skeeto/elfeed2/discussions
[elfeed]: https://github.com/skeeto/elfeed
[yt-dlp]: https://github.com/yt-dlp/yt-dlp
