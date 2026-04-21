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
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == '#' &&
            (i == 0 || std::isspace((unsigned char)line[i - 1]))) {
            return line.substr(0, i);
        }
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

// Return the substring of `line` starting just past the first token's
// trailing whitespace. `line` is assumed trimmed. Preserves internal
// whitespace; strips trailing whitespace.
std::string value_after_directive(const std::string &line)
{
    size_t i = 0;
    while (i < line.size() && !std::isspace((unsigned char)line[i])) ++i;
    while (i < line.size() && std::isspace((unsigned char)line[i])) ++i;
    std::string v = line.substr(i);
    while (!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
    return v;
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

} // namespace

void config_load(Elfeed *app)
{
    std::string dir = user_config_dir();
    make_directory(dir);
    app->config_path = dir + "/config";

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
