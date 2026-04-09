Rewrite of Elfeed in C++20

This document references external project sources. You do not need to read
this sources in full, but use them as references as needed.

See elfeed-3.4.2-6-gbbb3cac.tar.zst for original's sources.

* CMake build
  * FetchContent dependencies
    * Prefer fetch over HTTP/HTTPS with SHA256 instead of Git
  * Multi-platform support
    * Windows, Linux, macOS
    * Others might work, but untested
  * See dcmake-v1.2.0-11-g48049b6c.tar.zst
* Dear ImGui interface, docking branch
  * SDL3 + OpenGL 3
  * Use SDL3 as much OS interfacing as possible
  * Passive rendering possible?
    * Typical usage leaves it up for a long time
    * Blazing at 60 FPS, or worse, is wasteful
* Retain similar UI to original Elfeed
  * Time-oriented listing
  * Entry tags
  * Filter expressions
    * Bookmarkable (maybe belongs in user config?)
  * vi-like keys (yank entry URLs, etc.)
* Fetch with curl, schannel on Windows, ??? elsewhere
  * Track etags and such for maximum conditional fetch
    * Store these in the database
* Parse feed XML with pugixml
  * Support Atom and several RSS variants (like Elfeed)
  * Parse different feed types into a common Atom-oriented format
    * See original source for hard-learned parsing tricks
* SQLite for database
  * Primary display is time-ordered, newest on top
    * Index at least by time
    * Other indexes for fast search
    * Full text search index on entry content?
    * Store UI state persistence in database
  * $XDG_DATA_HOME
* User-provided config in plain text file, outside database
  * URLs listing feeds as plain text
    * Automatic tagging features (regex? domain?)
  * What format to use? Must support comments
    * Something simple to parse
  * $XDG_CONFIG_HOME
  * Pick download location
* Ability to open multiple entries in the browser at a time
  * Generally filter to narrow, then open range
* Built-in download manager
  * Videos via yt-dlp subprocess
    * Capture output to display progress in UI
  * See youtube-dl-emacs-1.0-11-gd8c3e11.tar.zst
  * Podcast attachemnts
    * Support for automatic naming from date+title.
* Very simple HTML parsing to strip HTML from titles and such
* Source releases that bundle all dependencies
* Longer term goals:
  * Basic HTML parsing for basic rendering/reading in app
    * Inline image support
    * Store fetched content in database
    * No JavaScript
