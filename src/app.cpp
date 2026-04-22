#include "app.hpp"
#include "data_uri_handler.hpp"
#include "events.hpp"
#include "main_frame.hpp"

#include <wx/cmdline.h>
#include <wx/event.h>
#include <wx/filesys.h>
#include <wx/image.h>
#include <wx/msgdlg.h>
#include <wx/utils.h>

#include "util.hpp"

#include <functional>

#include <cstdarg>
#include <cstdio>
#include <ctime>

wxDEFINE_EVENT(wxEVT_ELFEED_WAKE, wxThreadEvent);

wxIMPLEMENT_APP(ElfeedApp);

// ---- Logging -----------------------------------------------------

void elfeed_log(Elfeed *app, LogKind kind, const char *fmt, ...)
{
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    LogEntry entry;
    entry.kind = kind;
    entry.time = (double)time(nullptr);
    entry.message = buf;

    {
        std::lock_guard lock(app->log_mutex);
        app->log.push_back(std::move(entry));
    }
    app_wake_ui(app);
}

// ---- Thread-to-UI bridge -----------------------------------------

void app_wake_ui(Elfeed *app)
{
    if (!app->event_sink) return;
    wxQueueEvent(app->event_sink, new wxThreadEvent(wxEVT_ELFEED_WAKE));
}

// ---- Lifecycle ---------------------------------------------------

void elfeed_init(Elfeed *app)
{
    config_load(app);
    db_open(app);
    db_load_feeds(app);

    // Sync user-set titles into the DB. Each feed listed in config gets
    // its title_user written: a non-empty user_title sets the override,
    // an empty one clears it (the rule: "if the config lists a feed
    // without a title, forget the user-set title"). Feeds NOT in
    // config are left alone — their stored title_user persists so the
    // historical entry view still shows a friendly name.
    for (auto &f : app->feeds)
        db_set_user_title(app, f.url, f.user_title);

    db_load_feed_titles(app);

    // Restore "last fetch was N ago" counter across restarts. Stored
    // as an epoch-seconds string in ui_state; empty or unparseable
    // means "never fetched," which format_relative_time renders as
    // "never".
    std::string ts = db_load_ui_state(app, "last_fetch");
    if (!ts.empty()) {
        try { app->last_fetch = std::stod(ts); }
        catch (...) { app->last_fetch = 0; }
    }

    // Restore the log filter checkbox state. ui_state values are
    // "1" or "0"; missing keys default to true (the struct default).
    auto load_bool = [&](const char *key, bool &dst) {
        std::string v = db_load_ui_state(app, key);
        if (!v.empty()) dst = (v != "0");
    };
    load_bool("log.show_info",     app->log_show_info);
    load_bool("log.show_requests", app->log_show_requests);
    load_bool("log.show_success",  app->log_show_success);
    load_bool("log.show_errors",   app->log_show_errors);
    load_bool("log.auto_scroll",   app->log_auto_scroll);

    // Restore log history from the previous session, then purge
    // anything older than the retention window. Done in this order
    // so the user sees the in-memory log immediately even if the
    // purge takes a moment on a very large table.
    double now = (double)::time(nullptr);
    double cutoff = now - (double)app->log_retention_days * 86400.0;
    {
        std::lock_guard lock(app->log_mutex);
        db_log_load(app, cutoff, app->log);
        app->log_db_committed = app->log.size();
    }
    db_log_purge(app, cutoff);

    app->current_filter = filter_parse(app->default_filter);
}

void log_drain_to_db(Elfeed *app)
{
    // Copy out the new range under the mutex (cheap) so worker
    // threads aren't blocked on the DB write that follows. Tracks
    // log_db_committed as the high-water mark of "already persisted"
    // entries — touched only here on the UI thread.
    std::vector<LogEntry> batch;
    {
        std::lock_guard lock(app->log_mutex);
        if (app->log_db_committed >= app->log.size()) return;
        batch.assign(app->log.begin() + app->log_db_committed,
                     app->log.end());
        app->log_db_committed = app->log.size();
    }
    db_log_save(app, batch);
}

void config_reload(Elfeed *app)
{
    // Reset every field config_load writes to. The literal defaults
    // here must match the member initializers in struct Elfeed — they
    // represent "no config at all". Without this reset, values like
    // ytdlp_args (which accumulates) would grow on every reload.
    app->feeds.clear();
    app->presets.clear();
    app->tag_colors.clear();
    app->download_dir.clear();
    app->ytdlp_program = "yt-dlp";
    app->ytdlp_args.clear();
    app->default_filter = "@6-months-ago +unread";
    app->max_connections = 16;
    app->fetch_timeout = 30;
    app->max_download_failures = 5;
    app->log_retention_days = 90;

    config_load(app);

    // Re-decorate the fresh subscription list with DB-side metadata
    // (etag, title, last_update, etc.) so the next fetch still sends
    // conditional headers and the feeds pane shows the right titles.
    db_load_feeds(app);

    // Same user-title sync rule as elfeed_init: a config stanza with
    // `title X` writes title_user=X; a stanza with no title clears
    // any prior override. Feeds that disappeared from config are
    // left alone so historical entries still show a friendly name.
    for (auto &f : app->feeds)
        db_set_user_title(app, f.url, f.user_title);

    db_load_feed_titles(app);

    // Note: current_filter is deliberately NOT reset. The user's
    // active filter bar text is more recent than the config's
    // default_filter and shouldn't be clobbered by a reload.
}

void elfeed_shutdown(Elfeed *app)
{
    fetch_stop(app);
    download_stop(app);
    db_close(app);
}

// ---- wxApp -------------------------------------------------------

bool ElfeedApp::OnInit()
{
    // Set the global "current" narrow-string conversion to UTF-8.
    // Note: this does NOT affect wxString(const char*) constructors —
    // those bind to wxConvLibc at compile time. It only helps the wx
    // APIs that explicitly consult wxConvCurrent (some legacy printf
    // paths, env-var helpers). For UI strings containing non-ASCII
    // literals, use wxT(...) / L"..." so wx never sees narrow bytes.
    wxConvCurrent = &wxConvUTF8;

    SetAppName("elfeed2");

    // Command-line options. --db / --config let users (and the
    // mock-feed test rig) run against an isolated database +
    // config without disturbing the production install.
    {
        wxCmdLineParser parser(argc, argv);
        parser.AddSwitch("h", "help",
            "Show this help message and exit",
            wxCMD_LINE_OPTION_HELP);
        parser.AddOption("d", "db",
            "Use the named SQLite database instead of the default",
            wxCMD_LINE_VAL_STRING);
        parser.AddOption("c", "config",
            "Use the named config file instead of the default",
            wxCMD_LINE_VAL_STRING);
        // Don't choke on macOS's auto-injected -psn_ process serial
        // number when the .app launches from Finder.
        parser.SetSwitchChars("-");
        int rc = parser.Parse();
        if (rc != 0) return false;  // -1 = help shown, >0 = error
        wxString val;
        if (parser.Found("db",     &val))
            state_.db_path     = val.utf8_string();
        if (parser.Found("config", &val))
            state_.config_path = val.utf8_string();
    }

    // Resolve the DB path now (defaulting if --db wasn't given) so
    // the single-instance check below can scope its lock per-DB:
    // multiple instances against different DBs aren't a conflict
    // and shouldn't block each other. The check happens before
    // elfeed_init so we don't open the DB twice across processes.
    if (state_.db_path.empty()) {
        std::string dir = user_data_dir();
        make_directory(dir);
        state_.db_path = dir + "/elfeed.db";
    }

    // Install decoders for every image format wxWidgets can handle
    // (PNG, JPEG, GIF, WebP, TIFF, BMP, PCX, etc). Without this call
    // wxImage only knows BMP, so wxHtmlWindow pops up "Unknown image
    // data format" dialogs for any real-world <img> it tries to
    // render. Must run before any image decode happens, so place it
    // here near the top of OnInit.
    wxInitAllImageHandlers();

    // Register the data: URI handler with wxFileSystem so wxHtmlWindow
    // renders inline images written as data URIs. wxFileSystem owns
    // the handler after AddHandler; we just hand it off. wx 3.2 has
    // no built-in data: handler, so without this step <img src="data
    // :..."> silently drops.
    wxFileSystem::AddHandler(new DataURIHandler);

    // Single-instance guard. Two copies running against the same
    // SQLite database would race on writes and the AUI/geometry
    // state would clobber each other at close time. Scope the lock
    // per (user, DB) so a --db override (e.g. a test instance
    // against /tmp/test.db) doesn't conflict with the production
    // instance against the default DB.
    auto db_token = std::hash<std::string>{}(state_.db_path);
    instance_checker_ = std::make_unique<wxSingleInstanceChecker>(
        wxString::Format("elfeed2-%s-%lx",
                         wxGetUserId(),
                         (unsigned long)db_token));
    if (instance_checker_->IsAnotherRunning()) {
        wxMessageBox("Another Elfeed2 instance is already running "
                     "against this database.",
                     "Elfeed2", wxOK | wxICON_INFORMATION);
        instance_checker_.reset();
        return false;
    }

    elfeed_init(&state_);

    main_frame_ = new MainFrame(&state_);
    main_frame_->Show(true);
    return true;
}

int ElfeedApp::OnExit()
{
    elfeed_shutdown(&state_);
    return 0;
}
