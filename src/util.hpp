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
std::string format_date(double epoch);      // "YYYY-MM-DD"
std::string format_datetime(double epoch);  // "YYYY-MM-DD HH:MM:SS"

// ---- Feed date parsing ----

// Parse an ISO 8601 date/datetime. Fields without a timezone are
// treated as UTC. Returns UTC epoch seconds, or 0 on parse failure.
double parse_iso8601(const std::string &s);

// Parse an RFC 822 / RFC 2822 date (used by RSS), honoring the
// timezone suffix. Returns UTC epoch seconds, or 0 on parse failure.
double parse_rfc822(const std::string &s);

#endif
