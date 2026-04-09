#ifndef ELFEED_HPP
#define ELFEED_HPP

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct ImFont;
struct sqlite3;
struct SDL_Window;

// --- Data model ---

struct Author {
    std::string name;
    std::string email;
    std::string uri;
};

struct Enclosure {
    std::string url;
    std::string type;
    int64_t length = 0;
};

struct Entry {
    std::string namespace_;
    std::string id;
    std::string feed_url;
    std::string title;
    std::string link;
    double date = 0;
    std::string content;
    std::string content_type;
    std::vector<Author> authors;
    std::vector<Enclosure> enclosures;
    std::vector<std::string> tags;
    bool selected = false;
};

struct Feed {
    std::string url;
    std::string title;
    std::string author;
    std::string etag;
    std::string last_modified;
    std::string canonical_url;
    int failures = 0;
    double last_update = 0;
    std::vector<std::string> autotags;
};

// --- Filter ---

struct Filter {
    std::vector<std::string> must_have;
    std::vector<std::string> must_not_have;
    double after = 0;
    double before = 0;
    std::vector<std::string> matches;
    std::vector<std::string> not_matches;
    std::vector<std::string> feeds;
    std::vector<std::string> not_feeds;
    int limit = 0;
};

// --- Download manager ---

struct DownloadItem {
    int id = 0;
    std::string url;
    std::string title;
    std::string directory;
    std::string destination;
    int failures = 0;
    int priority = 0;
    std::string progress;
    std::string total;
    std::vector<std::string> log;
    bool paused = false;
    bool slow = false;
    bool is_video = false;
};

// --- Fetch ---

struct FetchResult {
    std::string url;
    int status_code = 0;
    std::string body;
    std::string etag;
    std::string last_modified;
    std::string final_url;
    std::string error;
};

// --- Log ---

enum LogKind { LOG_INFO, LOG_REQUEST, LOG_SUCCESS, LOG_ERROR };

struct LogEntry {
    LogKind kind;
    double time;
    std::string message;
};

// --- Application state ---

struct Elfeed {
    // Database
    sqlite3 *db = nullptr;
    std::string db_path;
    std::string config_path;

    // Config
    std::vector<Feed> feeds;
    std::string download_dir;
    std::string ytdlp_program = "yt-dlp";
    std::vector<std::string> ytdlp_args;
    std::string default_filter = "@6-months-ago +unread";
    int max_connections = 16;
    int fetch_timeout = 30;

    // Current view
    std::vector<Entry> entries;
    Filter current_filter;
    char filter_buf[512] = {};
    char filter_buf_backup[512] = {};
    bool filter_dirty = true;
    bool filter_editing = false;

    // Selection
    int cursor = 0;
    int sel_anchor = -1;

    // Fetch
    std::mutex fetch_mutex;
    std::vector<FetchResult> fetch_inbox;
    std::atomic<int> fetches_active{0};
    std::atomic<int> fetches_total{0};
    std::thread fetch_thread;
    std::atomic<bool> fetch_running{false};

    // Downloads
    std::mutex download_mutex;
    std::vector<DownloadItem> downloads;
    int download_next_id = 1;
    int download_active_id = 0;
    std::atomic<int> download_child_pid{0};
    std::thread download_thread;
    std::atomic<bool> download_running{false};

    // Log
    std::mutex log_mutex;
    std::vector<LogEntry> log;

    // UI state
    std::string ini_path;
    SDL_Window *window = nullptr;
    uint32_t wake_event_type = 0;
    int win_x = -1, win_y = -1;
    int win_w = 1280, win_h = 800;
    bool win_maximized = false;
    bool want_quit = false;
    bool needs_redraw = true;
    bool title_dirty = true;
    bool show_downloads = false;
    bool show_log = true;
    bool log_show_info = true;
    bool log_show_requests = true;
    bool log_show_success = true;
    bool log_show_errors = true;
    bool log_auto_scroll = true;
    bool show_entry = false;
    int show_entry_idx = -1;
    ImFont *mono_font = nullptr;
    float dpi_scale = 1.0f;
};

// Shared logic
void elfeed_init(Elfeed *app);
void elfeed_frame(Elfeed *app);
void elfeed_shutdown(Elfeed *app);

// Database
void db_open(Elfeed *app);
void db_close(Elfeed *app);
void db_add_entries(Elfeed *app, std::vector<Entry> &entries);
void db_query_entries(Elfeed *app, const Filter &filter,
                      std::vector<Entry> &out);
void db_tag(Elfeed *app, const std::string &ns, const std::string &id,
            const std::string &tag);
void db_untag(Elfeed *app, const std::string &ns, const std::string &id,
              const std::string &tag);
void db_update_feed(Elfeed *app, const Feed &feed);
void db_load_feeds(Elfeed *app);
void db_save_ui_state(Elfeed *app, const char *key, const char *value);
std::string db_load_ui_state(Elfeed *app, const char *key);

// Config
void config_load(Elfeed *app);

// Feed parsing
struct FeedParseResult {
    std::string feed_title;
    std::string feed_author;
    std::vector<Entry> entries;
};
FeedParseResult parse_feed(const std::string &url, const std::string &xml_body);

// HTML
std::string html_strip(const std::string &html);
std::string elfeed_cleanup(const std::string &s);

// Filter
Filter filter_parse(const std::string &expr);

// Downloads
void download_enqueue(Elfeed *app, const std::string &url,
                      const std::string &title, bool is_video);
void download_tick(Elfeed *app);
void download_remove(Elfeed *app, int id);
void download_pause(Elfeed *app, int id);
void download_stop(Elfeed *app);

// Fetching
void fetch_all(Elfeed *app);
void fetch_stop(Elfeed *app);
void fetch_process_results(Elfeed *app);

// Logging
void elfeed_log(Elfeed *app, LogKind kind, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

// XDG paths
std::string xdg_data_home();
std::string xdg_config_home();

#endif
