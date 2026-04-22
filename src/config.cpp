#include "elfeed.hpp"
#include "util.hpp"

#include <wx/textfile.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

// Config grammar, ssh_config-style:
//
//   # comment to end of line (also trailing: "...value # comment")
//   download-dir ~/Downloads
//   ytdlp-arg    --no-warnings           # ytdlp-arg accumulates
//
//   alias youtube https://www.youtube.com/feeds/videos.xml?channel_id={}
//
//   preset h @1-month +unread            # press 'h' in the list to
//   preset v @1-month +youtube           # jump the filter to this
//
//   https://acoup.blog/feed/             # URL line opens a stanza
//     title A Collection of Unmitigated Pedantry
//     tag   blog history
//
//   youtube UCbtwi4wK1YXd9AyV_4UcE6g     # alias line opens a stanza
//     title Adrian's Digital Basement
//     tag   retrocomputing
//
// Lines following a URL/alias line apply to that "current" stanza
// until the next URL/alias line. Blank lines are cosmetic. Comments
// are `#` at line start or preceded by whitespace; `#` embedded in a
// token (e.g. inside a URL fragment) is kept.

namespace {

std::string trim(const std::string &s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::string strip_comment(const std::string &line)
{
    // A `#` starts a comment iff it's at line start OR preceded by
    // whitespace AND followed by whitespace or end-of-line. That
    // catches the common `key value  # description` pattern but
    // leaves `#`-prefixed values alone — notably hex colors like
    // `#f9f` in the `color` directive, which are values, not
    // comments. Token-internal `#` (e.g. inside a URL fragment) is
    // already excluded by the preceding-whitespace requirement.
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] != '#') continue;
        if (i > 0 && !std::isspace((unsigned char)line[i - 1])) continue;
        if (i + 1 < line.size() &&
            !std::isspace((unsigned char)line[i + 1])) continue;
        return line.substr(0, i);
    }
    return line;
}

std::vector<std::string> tokenize(const std::string &line)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
        if (i >= line.size()) break;
        size_t start = i;
        while (i < line.size() && !std::isspace((unsigned char)line[i])) ++i;
        out.push_back(line.substr(start, i - start));
    }
    return out;
}

// Return the substring of `line` starting just past the Nth token's
// trailing whitespace. `line` is assumed trimmed. Preserves internal
// whitespace; strips trailing whitespace.
std::string value_after_tokens(const std::string &line, size_t n)
{
    size_t i = 0;
    for (size_t t = 0; t < n; t++) {
        while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
        while (i < line.size() && !std::isspace((unsigned char)line[i])) ++i;
    }
    while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
    std::string v = line.substr(i);
    while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
    return v;
}

std::string value_after_directive(const std::string &line)
{
    return value_after_tokens(line, 1);
}

std::string expand_tilde(const std::string &val)
{
    if (!val.empty() && val[0] == '~')
        return user_home_dir() + val.substr(1);
    return val;
}

bool is_url(const std::string &s)
{
    return s.find("://") != std::string::npos;
}

// Parse "#RRGGBB" or "#RGB" into packed 0x00RRGGBB. Returns -1 on
// malformed input (caller emits a warning). The shorthand expands
// each digit by duplication: "#abc" -> 0xAABBCC, matching the CSS
// rule so users don't have to think about it.
int64_t parse_hex_color(const std::string &s)
{
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (s.empty() || s[0] != '#') return -1;
    if (s.size() == 7) {
        uint32_t r = 0;
        for (int i = 1; i < 7; i++) {
            int d = hex(s[i]);
            if (d < 0) return -1;
            r = (r << 4) | (uint32_t)d;
        }
        return (int64_t)r;
    }
    if (s.size() == 4) {
        uint32_t r = 0;
        for (int i = 1; i < 4; i++) {
            int d = hex(s[i]);
            if (d < 0) return -1;
            r = (r << 8) | (uint32_t)((d << 4) | d);
        }
        return (int64_t)r;
    }
    return -1;
}

} // namespace

void config_load(Elfeed *app)
{
    // Honor a pre-set config_path (--config CLI option set it).
    // Otherwise use the platform default and ensure the dir
    // exists so the user can edit-and-save without a missing
    // parent stopping them.
    if (app->config_path.empty()) {
        std::string dir = user_config_dir();
        make_directory(dir);
        app->config_path = dir + "/config";
    }

    wxTextFile tf;
    if (!tf.Open(wxString::FromUTF8(app->config_path))) return;

    // Aliases defined so far. Value is the raw template string (may
    // include "{}" for arg substitution). Lookups at invocation time
    // do a single pass — no chained alias-of-alias.
    std::unordered_map<std::string, std::string> aliases;

    // Current feed stanza, tracked by index since app->feeds may
    // reallocate as we push new stanzas.
    int current = -1;

    auto warn = [&](const std::string &msg, size_t line_no) {
        elfeed_log(app, LOG_INFO, "config:%zu: %s",
                   line_no + 1, msg.c_str());
    };

    for (size_t ln = 0; ln < tf.GetLineCount(); ln++) {
        std::string line = trim(strip_comment(tf[ln].utf8_string()));
        if (line.empty()) continue;

        std::vector<std::string> tokens = tokenize(line);
        if (tokens.empty()) continue;
        const std::string &dir0 = tokens[0];

        // --- alias definition --------------------------------------
        if (dir0 == "alias") {
            if (tokens.size() < 3) {
                warn("alias needs a name and a template", ln);
                continue;
            }
            aliases[tokens[1]] = tokens[2];
            continue;
        }

        // --- per-tag entry-list color ------------------------------
        if (dir0 == "color") {
            if (tokens.size() < 3) {
                warn("color needs a tag and a #RRGGBB or #RGB hex", ln);
                continue;
            }
            int64_t rgb = parse_hex_color(tokens[2]);
            if (rgb < 0) {
                warn("color must be #RRGGBB or #RGB: " + tokens[2], ln);
                continue;
            }
            // Append rather than upsert: first-match-wins is the
            // documented rule, and duplicate entries for the same
            // tag are harmless (later ones never get reached).
            app->tag_colors.push_back({tokens[1], (uint32_t)rgb});
            continue;
        }

        // --- filter preset bound to a single key -------------------
        if (dir0 == "preset") {
            if (tokens.size() < 3) {
                warn("preset needs a letter and a filter string", ln);
                continue;
            }
            if (tokens[1].size() != 1) {
                warn("preset key must be a single character", ln);
                continue;
            }
            app->presets[tokens[1][0]] = value_after_tokens(line, 2);
            continue;
        }

        // --- global settings ---------------------------------------
        if (dir0 == "download-dir") {
            app->download_dir = expand_tilde(value_after_directive(line));
            continue;
        }
        if (dir0 == "ytdlp-program") {
            app->ytdlp_program = value_after_directive(line);
            continue;
        }
        if (dir0 == "ytdlp-arg") {
            app->ytdlp_args.push_back(value_after_directive(line));
            continue;
        }
        if (dir0 == "default-filter") {
            app->default_filter = value_after_directive(line);
            continue;
        }
        if (dir0 == "max-connections") {
            app->max_connections = atoi(value_after_directive(line).c_str());
            continue;
        }
        if (dir0 == "fetch-timeout") {
            app->fetch_timeout = atoi(value_after_directive(line).c_str());
            continue;
        }
        if (dir0 == "max-download-failures") {
            int n = atoi(value_after_directive(line).c_str());
            if (n < 1) n = 1;
            app->max_download_failures = n;
            continue;
        }
        if (dir0 == "log-retention-days") {
            int n = atoi(value_after_directive(line).c_str());
            if (n < 1) n = 1;
            app->log_retention_days = n;
            continue;
        }

        // --- stanza body (applies to current feed) -----------------
        if (dir0 == "title") {
            if (current < 0) {
                warn("title line with no preceding feed URL", ln);
                continue;
            }
            app->feeds[(size_t)current].user_title = value_after_directive(line);
            continue;
        }
        if (dir0 == "tag") {
            if (current < 0) {
                warn("tag line with no preceding feed URL", ln);
                continue;
            }
            for (size_t i = 1; i < tokens.size(); i++)
                app->feeds[(size_t)current].autotags.push_back(tokens[i]);
            continue;
        }

        // --- stanza head: URL, or alias invocation ------------------
        std::string url;
        if (is_url(dir0)) {
            // Direct URL. The line shouldn't have trailing tokens but
            // we forgive that (they'd only appear if the user mixes
            // old-style syntax); we just use the URL.
            url = dir0;
        } else {
            auto it = aliases.find(dir0);
            if (it == aliases.end()) {
                warn("unknown directive or alias: " + dir0, ln);
                continue;
            }
            std::string arg = (tokens.size() >= 2)
                                  ? value_after_directive(line)
                                  : std::string();
            url = it->second;
            size_t slot = url.find("{}");
            if (slot != std::string::npos) url.replace(slot, 2, arg);
        }

        Feed feed;
        feed.url = std::move(url);
        app->feeds.push_back(std::move(feed));
        current = (int)app->feeds.size() - 1;
    }
}
