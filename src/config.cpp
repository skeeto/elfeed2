#include "elfeed.hpp"
#include "util.hpp"

#include <wx/textfile.h>

#include <cstdlib>
#include <cstring>

static std::string trim(const std::string &s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

void config_load(Elfeed *app)
{
    std::string dir = user_config_dir();
    make_directory(dir);
    app->config_path = dir + "/config";

    // wxTextFile opens the file via wxFileName (UTF-8-safe on Windows),
    // auto-detects line endings (LF / CR / CRLF), and strips BOMs.
    wxTextFile tf;
    if (!tf.Open(wxString::FromUTF8(app->config_path))) return;

    for (size_t i = 0; i < tf.GetLineCount(); i++) {
        std::string s = trim(tf[i].utf8_string());
        if (s.empty() || s[0] == '#') continue;

        // key=value settings
        size_t eq = s.find('=');
        if (eq != std::string::npos && s.find("://") == std::string::npos) {
            std::string key = trim(s.substr(0, eq));
            std::string val = trim(s.substr(eq + 1));

            if (key == "download-dir") {
                // Expand leading ~ to the user's home directory.
                if (!val.empty() && val[0] == '~')
                    val = user_home_dir() + val.substr(1);
                app->download_dir = val;
            } else if (key == "ytdlp-program") {
                app->ytdlp_program = val;
            } else if (key == "ytdlp-arg") {
                app->ytdlp_args.push_back(val);
            } else if (key == "default-filter") {
                app->default_filter = val;
            } else if (key == "max-connections") {
                app->max_connections = atoi(val.c_str());
            } else if (key == "fetch-timeout") {
                app->fetch_timeout = atoi(val.c_str());
            }
            continue;
        }

        // Otherwise it's a feed URL, possibly with autotags:
        //   URL [tag1 tag2 ...]
        Feed feed;
        size_t space = s.find(' ');
        if (space != std::string::npos) {
            feed.url = s.substr(0, space);
            std::string rest = s.substr(space + 1);
            size_t pos = 0;
            while (pos < rest.size()) {
                size_t next = rest.find(' ', pos);
                if (next == std::string::npos) next = rest.size();
                std::string tag = trim(rest.substr(pos, next - pos));
                if (!tag.empty()) feed.autotags.push_back(tag);
                pos = next + 1;
            }
        } else {
            feed.url = s;
        }

        if (!feed.url.empty())
            app->feeds.push_back(std::move(feed));
    }
}
