#include "app.hpp"
#include "data_uri_handler.hpp"
#include "events.hpp"
#include "main_frame.hpp"

#include <wx/event.h>
#include <wx/filesys.h>
#include <wx/image.h>
#include <wx/msgdlg.h>
#include <wx/utils.h>

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

    app->current_filter = filter_parse(app->default_filter);
}

void config_reload(Elfeed *app)
{
    // Reset every field config_load writes to. The literal defaults
    // here must match the member initializers in struct Elfeed — they
    // represent "no config at all". Without this reset, values like
    // ytdlp_args (which accumulates) would grow on every reload.
    app->feeds.clear();
    app->presets.clear();
    app->download_dir.clear();
    app->ytdlp_program = "yt-dlp";
    app->ytdlp_args.clear();
    app->default_filter = "@6-months-ago +unread";
    app->max_connections = 16;
    app->fetch_timeout = 30;

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
    // SQLite database would race on writes and the AUI/geometry state
    // would clobber each other at close time. Per-user scope via
    // wxGetUserId so different users on the same machine aren't
    // locked out. On Windows this is a named mutex; on POSIX it's a
    // lock file under the user's runtime/temp dir. On macOS the
    // bundle already single-instances when launched from Dock/Finder,
    // but running the binary directly from a terminal bypasses that
    // — this covers it.
    instance_checker_ = std::make_unique<wxSingleInstanceChecker>(
        "elfeed2-" + wxGetUserId());
    if (instance_checker_->IsAnotherRunning()) {
        wxMessageBox("Another elfeed2 instance is already running.",
                     "elfeed2", wxOK | wxICON_INFORMATION);
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
