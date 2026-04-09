# Elfeed2

A standalone feed reader, successor to [Elfeed][elfeed]. Built with C++20,
Dear ImGui, SDL3, SQLite, and libcurl.

[elfeed]: https://github.com/skeeto/elfeed

## Building

Requires CMake 3.25+, a C++20 compiler, and libcurl. All other
dependencies (Dear ImGui, SDL3, SQLite, pugixml) are fetched
automatically.

    cmake -B build
    cmake --build build

Dependencies in `deps/` are used when present (bundled), otherwise
downloaded. Set `-DDEPS=LOCAL` to use only system-installed packages, or
`-DDEPS=DOWNLOAD` to always fetch.

## Configuration

The config file is `$XDG_CONFIG_HOME/elfeed2/config` (typically
`~/.config/elfeed2/config`). It is a simple line-oriented format.

### Feeds

Lines containing `://` are feed URLs. Optional space-separated autotags
follow the URL:

    https://example.com/feed.xml
    https://blog.example.com/rss comic webcomic

Autotags are automatically applied to new entries from that feed.

### Settings

Lines with `key=value` configure application behavior:

    # Directory for yt-dlp downloads (~ is expanded)
    download-dir = ~/Downloads

    # yt-dlp binary name or path
    ytdlp-program = yt-dlp

    # Extra yt-dlp arguments (one per line, repeatable)
    ytdlp-arg = --no-warnings
    ytdlp-arg = --embed-metadata

    # Default search filter shown on startup
    default-filter = @6-months-ago +unread

    # Maximum concurrent feed fetches
    max-connections = 16

    # Per-feed fetch timeout in seconds
    fetch-timeout = 30

Comments begin with `#`. Blank lines are ignored.

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
| `Enter`     | Open entry detail view                   |
| `b`         | Open entry link in browser               |
| `y`         | Copy entry link to clipboard             |
| `u`         | Mark unread (advances cursor)            |
| `r`         | Mark read (advances cursor)              |
| `R`         | Refresh filter                           |
| `v`         | Toggle visual selection                  |
| `s` / `/`   | Edit filter                              |
| `f`         | Fetch all feeds                          |
| `d`         | Download enclosure (via yt-dlp)          |
| `D`         | Toggle downloads panel                   |
| `Escape`    | Clear selection                          |

Visual selection (`v`) extends `u`, `r`, `b`, `y` to operate on a range.

### Filter editing keys

| Key         | Action                                   |
|-------------|------------------------------------------|
| `Enter`     | Submit filter                            |
| `Escape`    | Cancel and restore previous filter       |
| `Backspace` | Delete character                         |
| `Ctrl+Backspace` | Delete word                        |
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

The database is stored at `$XDG_DATA_HOME/elfeed2/elfeed.db` (typically
`~/.local/share/elfeed2/elfeed.db`). Window geometry and UI state are
persisted there as well.
