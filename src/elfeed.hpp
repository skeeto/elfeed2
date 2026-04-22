#ifndef ELFEED_HPP
#define ELFEED_HPP

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct sqlite3;
class wxEvtHandler;
class wxProcess;

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
};

struct Feed {
    std::string url;
    std::string title;        // self-declared by the feed
    std::string user_title;   // override from config; empty if none
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

enum class DownloadKind {
    Subprocess,   // yt-dlp (is_video) or curl (default) via wxProcess
    HttpDirect,   // our own http_download() into output_path
};

struct DownloadItem {
    int id = 0;
    std::string url;
    std::string title;
    std::string directory;
    std::string destination;
    // Full absolute path for HttpDirect kind, chosen at enqueue time.
    // Unused by subprocess downloads (they write wherever yt-dlp picks).
    std::string output_path;
    int failures = 0;
    int priority = 0;
    std::string progress;
    std::string total;
    std::vector<std::string> log;
    DownloadKind kind = DownloadKind::Subprocess;
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
    // Number of consecutive failures the download scheduler will
    // tolerate before giving up on an item. Items at the cap stay
    // in the queue (visible in the Downloads pane as "failed") so
    // the user can right-click → Retry to reset and try again.
    int max_download_failures = 5;
    // Days of log history to keep in the DB. Older rows are
    // purged on startup. Default 90; configurable via the
    // `log-retention-days` directive.
    int log_retention_days = 90;
    // Single-letter filter presets (from the `preset` config directive).
    // Key is the ASCII letter the user pressed; value is the filter
    // string to apply. Populated by config_load.
    std::unordered_map<char, std::string> presets;

    // Per-tag entry-list foreground colors (from the `color`
    // directive). Stored in config order so first-match-wins is
    // explicit: walked in order, the first tag we recognize on an
    // entry sets that entry's row colour. Color is packed 0xRRGGBB
    // — wxColour built at render time so this header stays free of
    // wxWidgets dependencies.
    std::vector<std::pair<std::string, uint32_t>> tag_colors;

    // Current view (populated by db_query_entries after filter changes)
    std::vector<Entry> entries;
    Filter current_filter;

    // URL → display title for every feed the DB knows about (subscribed
    // or not). Display code reads this; the source of truth is the DB.
    // COALESCE(title_user, title) at load time. Refreshed by
    // db_load_feed_titles() after config sync, fetch, or import.
    std::unordered_map<std::string, std::string> feed_titles;

    // Fetch
    std::mutex fetch_mutex;
    std::vector<FetchResult> fetch_inbox;
    std::atomic<int> fetches_active{0};
    std::atomic<int> fetches_total{0};
    std::vector<std::thread> fetch_workers;
    std::atomic<bool> fetch_running{false};

    // Epoch seconds when the last F5 / Fetch-All batch completed.
    // 0 means "never fetched". Persisted to ui_state so the "X
    // minutes ago" display survives restarts. The UI-thread-only
    // flag fetch_ran_since_status tracks whether a running batch
    // has transitioned to done since we last observed it, so
    // fetch_process_results can stamp last_fetch exactly once per
    // completed batch.
    double last_fetch = 0;
    bool   last_fetch_seen_running = false;

    // Downloads (all state is UI-thread-only; no mutex needed).
    // download_process is non-null while a child is running; wxProcess
    // self-deletes after OnTerminate fires so we just nil the pointer.
    std::vector<DownloadItem> downloads;
    int download_next_id = 1;
    int download_active_id = 0;
    wxProcess *download_process = nullptr;

    // Inline image cache (preview pane). Worker threads fetch images
    // referenced by entry content and push results onto image_inbox;
    // the UI thread drains the inbox on wake, writes rows to
    // image_cache in the DB, then re-renders the active entry so the
    // newly-cached images appear as data: URIs.
    struct ImageInboxItem {
        std::string url;
        std::string mime;   // empty on fetch failure
        std::string bytes;
    };
    std::mutex image_mutex;
    std::vector<ImageInboxItem> image_inbox;
    // URLs with an in-flight worker; dedupes concurrent fetch requests
    // so opening two entries that share an image doesn't double-fetch.
    std::unordered_set<std::string> image_in_flight;

    // Log
    std::mutex log_mutex;
    std::vector<LogEntry> log;
    // Count of log entries already written to the DB. The next
    // log_drain_to_db pass picks up at this index. Touched only
    // from the UI thread (drain happens there).
    size_t log_db_committed = 0;

    // Persistent UI toggles (saved to DB). Pane visibility lives in
    // the wxAUI perspective string under DB key "layout".
    bool log_show_info = true;
    bool log_show_requests = true;
    bool log_show_success = true;
    bool log_show_errors = true;
    bool log_auto_scroll = true;

    // Worker-thread event sink (usually the MainFrame). Workers post events
    // here via wxQueueEvent to wake the UI thread without polling.
    wxEvtHandler *event_sink = nullptr;
};

// Lifecycle
void elfeed_init(Elfeed *app);
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
// Set or clear the user-supplied title override for `url`. An empty
// title clears it (NULL in the DB), so the display falls back to the
// self-declared title from the feed XML.
void db_set_user_title(Elfeed *app, const std::string &url,
                       const std::string &title);
// Refresh app->feed_titles from the DB.
void db_load_feed_titles(Elfeed *app);
void db_save_ui_state(Elfeed *app, const char *key, const char *value);
std::string db_load_ui_state(Elfeed *app, const char *key);

// Log persistence. db_log_load reads entries with time >=
// since_epoch into `out`, ordered chronologically. db_log_save
// appends a batch. db_log_purge deletes entries older than
// older_than_epoch. db_log_clear empties the table.
void db_log_load(Elfeed *app, double since_epoch,
                 std::vector<LogEntry> &out);
void db_log_save(Elfeed *app, const std::vector<LogEntry> &entries);
void db_log_purge(Elfeed *app, double older_than_epoch);
void db_log_clear(Elfeed *app);

// Drain new in-memory log entries (since app->log_db_committed)
// to the DB. Holds log_mutex briefly to copy out the new range,
// then writes outside the lock. Safe to call frequently —
// returns immediately if nothing's new.
void log_drain_to_db(Elfeed *app);

// Config
void config_load(Elfeed *app);

// Clear config-derived state (feeds subscription list, presets,
// ytdlp args, globals) and re-run config_load. Re-hydrates feed
// metadata from the DB and refreshes feed_titles. The caller is
// responsible for telling the UI to re-render.
void config_reload(Elfeed *app);

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
// Enqueue an HTTP-direct download (used for RSS enclosures like
// podcasts). `output_path` is the exact destination, already
// disambiguated by the caller.
void download_enqueue_http(Elfeed *app, const std::string &url,
                           const std::string &title,
                           const std::string &output_path);
void download_tick(Elfeed *app);
void download_remove(Elfeed *app, int id);
void download_pause(Elfeed *app, int id);
// Reset the failures counter on `id` and unpause it so the next
// download_tick will reconsider it. No-op if `id` doesn't match an
// item in the queue. Used by the Downloads pane's Retry action.
void download_retry(Elfeed *app, int id);
void download_stop(Elfeed *app);

// Fetching
void fetch_all(Elfeed *app);
void fetch_stop(Elfeed *app);
// Drain the worker->UI inbox. Returns true if at least one result was
// processed (so the caller knows whether to requery the entry list).
bool fetch_process_results(Elfeed *app);

// Logging.
// `gnu_printf` (rather than `printf`) so the format-string check
// uses C99/GNU rules across all targets. On mingw, the `printf`
// archetype maps to `ms_printf`, which doesn't recognise %zu / %ll
// and warns on every elfeed_log("%zu …", size_t). gnu_printf is
// the same as printf on GCC/Clang elsewhere.
void elfeed_log(Elfeed *app, LogKind kind, const char *fmt, ...)
    __attribute__((format(gnu_printf, 3, 4)));

// Post a "UI needs update" event to the main frame. Worker-thread safe.
// Implemented in app.cpp where wxWidgets headers are in scope.
void app_wake_ui(Elfeed *app);

#endif
