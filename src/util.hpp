#ifndef ELFEED_UTIL_HPP
#define ELFEED_UTIL_HPP

// Small utilities that hide platform differences: UTF-8-safe filesystem
// ops, local-time formatting, and feed-style date parsing. Callers use
// plain std::string / double so non-UI modules don't pull in wxWidgets
// headers in their own headers; the .cpp uses wxDateTime / wxFileName.

#include <string>

// ---- Filesystem ----

// Create `path` and any missing parent directories. UTF-8 input; safe
// on Windows (routes through wxFileName::Mkdir so non-ASCII paths work).
// Returns true on success or if the directory already exists.
bool make_directory(const std::string &path);

// ---- User directories (UTF-8) ----
// wxApp must have called SetAppName() first so the returned paths
// include the app-specific suffix.

// Per-user data directory for this app (DB and similar app-managed
// state). Platform-native via wxStandardPaths:
//   macOS:   ~/Library/Application Support/<app>
//   Windows: %APPDATA%\<app>
//   Linux:   $XDG_DATA_HOME/<app> (defaults to ~/.local/share/<app>)
std::string user_data_dir();

// Per-user config directory for this app (hand-edited text config).
// Follows XDG on Unix/macOS so users can reach it from a terminal
// without Finder gymnastics; falls back to the data dir on Windows.
//   macOS / Linux:  $XDG_CONFIG_HOME/<app> (default ~/.config/<app>)
//   Windows:        %APPDATA%\<app>
std::string user_config_dir();

// The user's home directory (e.g. for `~` expansion in config paths).
std::string user_home_dir();

// ---- Time formatting (local time) ----

// Format `epoch` (Unix seconds, UTC) as local-time strings. Returns an
// empty string for non-positive input.
std::string format_date(double epoch);          // "YYYY-MM-DD"
std::string format_datetime(double epoch);      // "YYYY-MM-DD HH:MM:SS"
std::string format_date_compact(double epoch);  // "YYYYMMDD"

// ---- Filename building ----

// Sanitize one path segment:
//   - 0x41-0x5A  (A-Z): lowercased
//   - 0x30-0x39, 0x61-0x7A  (0-9, a-z): kept as-is
//   - >= 0x80: passed through verbatim (UTF-8 bytes keep their shape)
//   - other 0-127: a run of such bytes becomes a single '_'
// Leading and trailing '_' are trimmed.
std::string sanitize_filename(const std::string &s);

// Map an HTTP Content-Type to a file extension (no leading dot). On
// miss, tries to pluck one from `url_fallback` (trailing .xxx, up to
// 5 ASCII alnum chars, before any '?' or '#'). On still-miss, "bin".
// Caller-provided type is lowercased before lookup; MIME parameters
// (";charset=...") are stripped.
std::string mime_to_extension(const std::string &mime_type,
                              const std::string &url_fallback = {});

// Given directory, base filename (no extension), and extension (no
// dot), return a full path that does not collide with an existing
// file. On collision, appends " (N)" before the dot, starting at 1,
// up to 999. The returned path may not yet exist; the caller opens
// it. Races between concurrent downloads are not handled — but our
// single-in-flight model means this isn't an issue in practice.
std::string disambiguate_path(const std::string &dir,
                              const std::string &base,
                              const std::string &ext);

// ---- Feed date parsing ----

// Parse an ISO 8601 date/datetime. Fields without a timezone are
// treated as UTC. Returns UTC epoch seconds, or 0 on parse failure.
double parse_iso8601(const std::string &s);

// Parse an RFC 822 / RFC 2822 date (used by RSS), honoring the
// timezone suffix. Returns UTC epoch seconds, or 0 on parse failure.
double parse_rfc822(const std::string &s);

#endif
