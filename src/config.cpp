#include "elfeed.hpp"

#include <cstdio>
#include <cstring>
#include <sys/stat.h>

static std::string trim(const std::string &s)
{
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

void config_load(Elfeed *app)
{
    std::string dir = xdg_config_home() + "/elfeed2";
#ifdef _WIN32
    _mkdir(dir.c_str());
#else
    mkdir(dir.c_str(), 0755);
#endif
    app->config_path = dir + "/config";

    FILE *f = fopen(app->config_path.c_str(), "r");
    if (!f) return;

    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        // Strip trailing newline
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;

        // Check for key=value
        size_t eq = s.find('=');
        if (eq != std::string::npos && s.find("://") == std::string::npos) {
            std::string key = trim(s.substr(0, eq));
            std::string val = trim(s.substr(eq + 1));

            if (key == "download-dir") {
                // Expand ~ at start
                if (!val.empty() && val[0] == '~') {
                    const char *home = getenv("HOME");
                    if (home) val = home + val.substr(1);
                }
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
            // bookmark = name = expression (skip for now)
            continue;
        }

        // Otherwise it's a feed URL, possibly with autotags
        // Format: URL [tag1 tag2 ...]
        Feed feed;
        size_t space = s.find(' ');
        if (space != std::string::npos) {
            feed.url = s.substr(0, space);
            std::string rest = s.substr(space + 1);
            // Split on whitespace for tags
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

    fclose(f);
}
