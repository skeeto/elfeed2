#include "app.hpp"
#include "events.hpp"
#include "main_frame.hpp"

#include <wx/event.h>

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
    app->current_filter = filter_parse(app->default_filter);

    // Restore persistent UI toggles
    std::string s;
    s = db_load_ui_state(app, "show_log");
    if (!s.empty()) app->show_log = (s == "1");
    s = db_load_ui_state(app, "show_downloads");
    if (!s.empty()) app->show_downloads = (s == "1");
}

void elfeed_shutdown(Elfeed *app)
{
    fetch_stop(app);
    download_stop(app);
    db_save_ui_state(app, "show_log", app->show_log ? "1" : "0");
    db_save_ui_state(app, "show_downloads",
                     app->show_downloads ? "1" : "0");
    db_close(app);
}

// ---- wxApp -------------------------------------------------------

bool ElfeedApp::OnInit()
{
    SetAppName("elfeed2");

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
