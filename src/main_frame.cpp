#include "main_frame.hpp"

#include "activity_panel.hpp"
#include "downloads_panel.hpp"
#include "elfeed_import.hpp"
#include "entry_detail.hpp"
#include "image_cache.hpp"
#include "util.hpp"
#include "entry_list.hpp"
#include "events.hpp"
#include "feeds_panel.hpp"
#include "log_panel.hpp"

#include <wx/aui/framemanager.h>
#include <wx/busyinfo.h>
#include <wx/clipbrd.h>
#include <wx/dialog.h>
#include <wx/display.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/hyperlink.h>
#include <wx/icon.h>
#include <wx/menu.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/textdlg.h>
#include <wx/msgdlg.h>
#include <wx/srchctrl.h>
#include <wx/sizer.h>
#include <wx/statusbr.h>
#include <wx/utils.h>

#include <algorithm>
#include <cstdio>

enum {
    ID_Fetch = wxID_HIGHEST + 1,
    ID_ImportClassic,
    ID_ReloadConfig,
    ID_ReclaimSpace,
    ID_DownloadURL,
    ID_ToggleFeeds,
    ID_TogglePreview,
    ID_ToggleLog,
    ID_ToggleDownloads,
    ID_ToggleActivity,
    ID_ResetLayout,
};

MainFrame::MainFrame(Elfeed *app)
    : wxFrame(nullptr, wxID_ANY, "Elfeed2",
              wxDefaultPosition, wxDefaultSize)
    , app_(app)
{
    app_->event_sink = this;

    // Restore window geometry from the previous run. Format in the DB:
    // "x y w h maximized" (five ints, space-separated). If anything
    // looks off — unparseable, too small, or no longer intersects any
    // connected display — fall back to the 1500x900 centered default.
    bool restored = false;
    std::string geom = db_load_ui_state(app_, "geometry");
    if (!geom.empty()) {
        int x = 0, y = 0, w = 0, h = 0, maxi = 0;
        if (sscanf(geom.c_str(), "%d %d %d %d %d",
                   &x, &y, &w, &h, &maxi) == 5 &&
            w >= 400 && h >= 300) {
            // Only apply the saved rect if some part of it still
            // lands on a connected display; otherwise we'd spawn the
            // window on a monitor the user no longer has.
            bool on_screen = false;
            wxRect saved(x, y, w, h);
            for (unsigned i = 0; i < wxDisplay::GetCount(); i++) {
                if (wxDisplay(i).GetGeometry().Intersects(saved)) {
                    on_screen = true;
                    break;
                }
            }
            if (on_screen) {
                SetSize(saved);
                if (maxi) Maximize(true);
                restored = true;
            }
        }
    }
    if (!restored) {
        SetClientSize(FromDIP(wxSize(1500, 900)));
        Centre();
    }
    normal_rect_ = wxRect(GetPosition(), GetSize());

    // Title-bar / taskbar icon. wxICON() expands to:
    //   Windows: wxIcon("elfeed2") which loads the named ICON
    //            resource embedded by elfeed2.rc.
    //   GTK:     wxIcon(elfeed2_xpm) — needs an XPM symbol; we
    //            don't ship one yet, so the macro yields nothing.
    //   macOS:   no-op; the bundle's CFBundleIconFile is the
    //            single source of truth for app iconography.
    // Wrapped in #ifdef so non-Windows builds don't error on the
    // missing XPM symbol.
#ifdef __WXMSW__
    SetIcon(wxICON(elfeed2));
#endif

    build_menus();
    build_widgets();
    bind_events();

    CreateStatusBar(1);

    requery();
    update_status();
    update_menu_checks();

    // Tick the status bar every 60s so "last fetch: Nm ago" advances
    // even when the user isn't interacting. Matches the minute-level
    // resolution of the display for the first hour; past that the
    // display changes rarely enough that the interval is invisible.
    status_timer_.SetOwner(this);
    Bind(wxEVT_TIMER,
         [this](wxTimerEvent &) { update_status(); },
         status_timer_.GetId());
    status_timer_.Start(60 * 1000);

    // One-shot revert timer for flash_status. Same handler — restoring
    // the regular status text — but a separate ID so it doesn't get
    // confused with the recurring tick above.
    flash_timer_.SetOwner(this);
    Bind(wxEVT_TIMER,
         [this](wxTimerEvent &) { update_status(); },
         flash_timer_.GetId());

    // Coalesce log persistence: flush the in-memory log to the DB
    // every 5 seconds rather than on every UI wake. on_close also
    // calls log_drain_to_db so a clean exit catches the last
    // unwritten window.
    log_drain_timer_.SetOwner(this);
    Bind(wxEVT_TIMER,
         [this](wxTimerEvent &) { log_drain_to_db(app_); },
         log_drain_timer_.GetId());
    log_drain_timer_.Start(5 * 1000);

    // One-shot debounce for filter-bar typing. on_filter_text starts
    // it; when the user pauses for ~180 ms, this fires the actual
    // requery with the viewport cap applied (live-typing mode).
    // Enter/Escape/focus-out paths stop the timer and call
    // commit_filter(/*capped=*/false) instead, so leaving the
    // filter bar always produces the full uncapped match set.
    filter_debounce_.SetOwner(this);
    Bind(wxEVT_TIMER,
         [this](wxTimerEvent &) { commit_filter(/*capped=*/true); },
         filter_debounce_.GetId());

    // Focus the entry list so vi-style keys work immediately.
    CallAfter([this] { list_->SetFocus(); });
}

MainFrame::~MainFrame()
{
    app_->event_sink = nullptr;
    // mgr_.UnInit() happens in on_close, but cover the path where
    // the frame is destroyed without a close event (rare).
    mgr_.UnInit();
}

// ---- Building ------------------------------------------------------

void MainFrame::build_menus()
{
    auto *mbar = new wxMenuBar;

    auto *m_elfeed = new wxMenu;
    // F5 is the universal "refresh" key on Windows/Linux, but on macOS
    // F5 is intercepted by the OS for spotlight / function-key remaps.
    // Use Cmd+R on macOS — that's what browsers and Finder use for
    // reload, and what users reach for when they want "update now".
    // (Capital G also works on all platforms; see on_list_key.)
#ifdef __WXOSX__
    m_elfeed->Append(ID_Fetch, "&Fetch All\tCtrl+R");
#else
    m_elfeed->Append(ID_Fetch, "&Fetch All\tF5");
#endif
    m_elfeed->Append(ID_ReloadConfig, "&Reload Config\tCtrl+Shift+R");
    // Download &URL... — `U` mnemonic so we don't collide with the
    // `D` that's already in "Reclaim &Disk Space…" below. The
    // menu accelerator is Ctrl+Shift+D; the bare `p` key also
    // triggers this when the entry list has focus
    // (see on_list_key).
    m_elfeed->Append(ID_DownloadURL,
                     wxT("Download &URL…\tCtrl+Shift+D"));
    m_elfeed->AppendSeparator();
    m_elfeed->Append(ID_ImportClassic, wxT("&Import Classic Elfeed Index…"));
    // Drops the image LRU and VACUUMs the DB. Named for what the
    // user gets rather than what SQLite calls it.
    m_elfeed->Append(ID_ReclaimSpace, wxT("Reclaim &Disk Space…"));
    m_elfeed->AppendSeparator();
    m_elfeed->Append(wxID_EXIT, "&Quit\tCtrl+Q");
    mbar->Append(m_elfeed, "&Elfeed");

    auto *m_view = new wxMenu;
    menu_feeds_id_ =
        m_view->AppendCheckItem(ID_ToggleFeeds,     "&Feeds")->GetId();
    menu_preview_id_ =
        m_view->AppendCheckItem(ID_TogglePreview,   "&Preview")->GetId();
    menu_log_id_ =
        m_view->AppendCheckItem(ID_ToggleLog,       "&Log")->GetId();
    menu_downloads_id_ =
        m_view->AppendCheckItem(ID_ToggleDownloads, "&Downloads")->GetId();
    menu_activity_id_ =
        m_view->AppendCheckItem(ID_ToggleActivity,  "&Activity")->GetId();
    m_view->AppendSeparator();
    m_view->Append(ID_ResetLayout, "&Reset Layout");
    mbar->Append(m_view, "&View");

    auto *m_help = new wxMenu;
    m_help->Append(wxID_ABOUT, "&About elfeed2");
    mbar->Append(m_help, "&Help");

    SetMenuBar(mbar);
}

void MainFrame::build_widgets()
{
    auto *outer = new wxPanel(this);
    auto *vsz = new wxBoxSizer(wxVERTICAL);

    // Filter bar stays outside AUI as a fixed top strip. The initial
    // text is the last filter the user had (persisted to the DB on
    // filter blur), falling back to the config's default_filter.
    std::string initial_filter = db_load_ui_state(app_, "filter");
    if (initial_filter.empty()) initial_filter = app_->default_filter;
    filter_ = new wxSearchCtrl(outer, wxID_ANY,
                               wxString::FromUTF8(initial_filter),
                               wxDefaultPosition, wxDefaultSize,
                               wxTE_PROCESS_ENTER);
    filter_->ShowSearchButton(false);
    filter_->ShowCancelButton(true);
    app_->current_filter = filter_parse(initial_filter);
    vsz->Add(filter_, 0, wxEXPAND | wxALL, FromDIP(4));

    // AUI manager lives on a host panel that fills the rest.
    auto *aui_host = new wxPanel(outer, wxID_ANY);
    vsz->Add(aui_host, 1, wxEXPAND);

    outer->SetSizer(vsz);

    auto *frame_sz = new wxBoxSizer(wxVERTICAL);
    frame_sz->Add(outer, 1, wxEXPAND);
    SetSizer(frame_sz);

    mgr_.SetManagedWindow(aui_host);

    list_      = new EntryList(aui_host, app_);
    detail_    = new EntryDetail(aui_host, app_);
    feeds_     = new FeedsPanel(aui_host, app_,
                                [this](const std::string &url) {
                                    set_filter_to_feed(url);
                                });
    log_       = new LogPanel(aui_host, app_);
    downloads_ = new DownloadsPanel(aui_host, app_);
    activity_  = new ActivityPanel(aui_host, app_);

    // Default layout at the 1500x900 default client size:
    //   Feeds   left   250 px (1/6 of width), full height
    //   Preview right  500 px (1/3 of width), full height
    //   Center: entry list on top + Downloads docked beneath, sharing
    //           the middle 750 px column. Downloads is 180 px (1/5
    //           of height); list takes the remaining 720 px (4/5).
    //
    // Layer matters: feeds and preview at Layer(1) become outer columns
    // that span the full window height, while downloads at Layer(0)
    // sits in the inner ring directly under the central pane and
    // doesn't extend under the side columns. wxAUI's "onion" docking:
    // higher layer = farther from center.
    //
    // wxAUI ignores wxAuiPaneInfo::BestSize for layered side docks;
    // it uses MinSize as the floor and the contained panel's
    // intrinsic best size otherwise. The only knob that reliably
    // pins the initial dock dimensions is MinSize itself, so we set
    // it here to the desired width/height. We then drop these floors
    // back to a tiny size after the first mgr_.Update() (below) so
    // the user can drag panes narrower than the construction
    // defaults — the dock_size is captured in the saved perspective
    // by then and doesn't need MinSize to survive.
    mgr_.AddPane(list_,
                 wxAuiPaneInfo()
                     .Name("entry_list")
                     .CenterPane()
                     .CaptionVisible(false));
    mgr_.AddPane(feeds_,
                 wxAuiPaneInfo()
                     .Name("feeds")
                     .Caption("Feeds")
                     .Left()
                     .Layer(1)
                     .MinSize(FromDIP(wxSize(250, -1)))
                     .Show());
    mgr_.AddPane(detail_,
                 wxAuiPaneInfo()
                     .Name("entry_detail")
                     .Caption("Preview")
                     .Right()
                     .Layer(1)
                     .MinSize(FromDIP(wxSize(500, -1)))
                     .Show());
    mgr_.AddPane(downloads_,
                 wxAuiPaneInfo()
                     .Name("downloads")
                     .Caption("Downloads")
                     .Bottom()
                     .Layer(0)
                     .MinSize(FromDIP(wxSize(-1, 180)))
                     .Show());
    mgr_.AddPane(log_,
                 wxAuiPaneInfo()
                     .Name("log")
                     .Caption("Log")
                     .Bottom()
                     .Layer(0)
                     .MinSize(FromDIP(wxSize(-1, 200)))
                     .Hide());
    mgr_.AddPane(activity_,
                 wxAuiPaneInfo()
                     .Name("activity")
                     .Caption("Activity")
                     .Bottom()
                     .Layer(0)
                     // One 53×7 heatmap grid plus month-label and
                     // day-label strips. ~130 px holds it cleanly
                     // at a reasonable cell size; resize to taste.
                     .MinSize(FromDIP(wxSize(-1, 150)))
                     .Hide());

    // Run an initial Update with just the construction-time defaults
    // so wxAUI computes dock_size entries from the MinSize hints
    // above. Snapshot the resulting perspective for Reset Layout
    // before any saved perspective is applied on top — the snapshot
    // includes the MinSize values, but loosen_pane_min_sizes() will
    // override them after every load (initial and Reset).
    mgr_.Update();
    default_perspective_ = mgr_.SavePerspective();

    std::string saved = db_load_ui_state(app_, "layout");
    if (!saved.empty())
        mgr_.LoadPerspective(wxString::FromUTF8(saved), false);

    // Drop the MinSize hints so the user can drag any pane narrower
    // than its construction default. Done AFTER LoadPerspective
    // because the perspective string carries MinSize and applying
    // it would put the floors back. Done in Reset Layout too via
    // the same helper.
    loosen_pane_min_sizes();
    mgr_.Update();
}

void MainFrame::loosen_pane_min_sizes()
{
    const wxSize floor = FromDIP(wxSize(40, 40));
    for (const char *name :
         {"feeds", "entry_detail", "downloads", "log", "activity"}) {
        wxAuiPaneInfo &pi = mgr_.GetPane(name);
        if (pi.IsOk()) pi.MinSize(floor);
    }
}

void MainFrame::bind_events()
{
    Bind(wxEVT_ELFEED_WAKE, &MainFrame::on_wake, this);
    Bind(wxEVT_MENU, &MainFrame::on_fetch_all,        this, ID_Fetch);
    Bind(wxEVT_MENU, &MainFrame::on_reload_config,    this, ID_ReloadConfig);
    Bind(wxEVT_MENU, &MainFrame::on_import_classic,   this, ID_ImportClassic);
    Bind(wxEVT_MENU, &MainFrame::on_reclaim_space,    this, ID_ReclaimSpace);
    Bind(wxEVT_MENU,
         [this](wxCommandEvent &) { action_download_url(); },
         ID_DownloadURL);
    Bind(wxEVT_MENU, &MainFrame::on_toggle_feeds,     this, ID_ToggleFeeds);
    Bind(wxEVT_MENU, &MainFrame::on_toggle_preview,   this, ID_TogglePreview);
    Bind(wxEVT_MENU, &MainFrame::on_toggle_log,       this, ID_ToggleLog);
    Bind(wxEVT_MENU, &MainFrame::on_toggle_downloads, this, ID_ToggleDownloads);
    Bind(wxEVT_MENU, &MainFrame::on_toggle_activity,  this, ID_ToggleActivity);
    Bind(wxEVT_MENU, &MainFrame::on_reset_layout,     this, ID_ResetLayout);
    Bind(wxEVT_MENU, &MainFrame::on_about, this, wxID_ABOUT);
    Bind(wxEVT_MENU, &MainFrame::on_quit,  this, wxID_EXIT);
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::on_close, this);
    // Bind on the manager, not on `this`: wxAuiManagerEvent inherits
    // from wxEvent (not wxCommandEvent) so it doesn't propagate up
    // to the frame. The previous frame-level bind silently never
    // fired, which is why the View menu's check marks lagged the
    // actual pane state after an X-button close.
    mgr_.Bind(wxEVT_AUI_PANE_CLOSE, &MainFrame::on_pane_close, this);
    Bind(wxEVT_MOVE, &MainFrame::on_frame_move_size, this);
    Bind(wxEVT_SIZE, &MainFrame::on_frame_move_size, this);

    filter_->Bind(wxEVT_TEXT, &MainFrame::on_filter_text, this);
    filter_->Bind(wxEVT_SEARCH_CANCEL, [this](wxCommandEvent &) {
        filter_->Clear();
    });
    filter_->Bind(wxEVT_CHAR_HOOK, &MainFrame::on_filter_key, this);
    // Persist the current filter text when the user leaves the filter
    // control (blur), so it's remembered across runs.
    filter_->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &e) {
        // Leaving the filter bar: drop the live-typing row cap
        // and show the full match set. Stop any pending debounce
        // first (its in-flight commit would have been capped);
        // then commit uncapped. Stop() is a no-op if the timer
        // isn't running, so just always call it.
        filter_debounce_.Stop();
        commit_filter(/*capped=*/false);
        db_save_ui_state(app_, "filter",
                         filter_->GetValue().utf8_string().c_str());
        e.Skip();
    });

    list_->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED,
                &MainFrame::on_list_selected, this);
    list_->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED,
                &MainFrame::on_list_activated, this);
    list_->Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU,
                &MainFrame::on_list_context_menu, this);
    list_->Bind(wxEVT_CHAR_HOOK, &MainFrame::on_list_key, this);
}

// ---- Data plumbing -------------------------------------------------

void MainFrame::requery(int default_limit)
{
    // Row indices are about to change; the old anchor no longer
    // points to the same entry. Exit visual mode silently — the
    // user's selection will be re-homed by the ns/id lookup below.
    visual_anchor_ = -1;

    std::string sel_ns, sel_id;
    long primary = list_->primary();
    if (primary >= 0 && (size_t)primary < app_->entries.size()) {
        sel_ns = app_->entries[(size_t)primary].namespace_;
        sel_id = app_->entries[(size_t)primary].id;
    }

    // Caller decides whether to apply a default row cap. Live-
    // typing passes the viewport size; everything else (blur,
    // Ctrl+L, import, fetch arrival, preset keys) passes 0 for
    // the full match set.
    db_query_entries(app_, app_->current_filter, app_->entries,
                     default_limit);
    list_->refresh_items();
    // Note: activity_ is filter-independent, driven by the DB
    // directly, so a requery (which runs on every filter keystroke)
    // should not trigger it. Activity refresh happens on_wake when
    // new_entries lands, on pane-show, and after import.

    long new_primary = -1;
    if (!sel_ns.empty()) {
        for (size_t i = 0; i < app_->entries.size(); i++) {
            auto &e = app_->entries[i];
            if (e.namespace_ == sel_ns && e.id == sel_id) {
                new_primary = (long)i;
                break;
            }
        }
    }
    if (new_primary < 0 && !app_->entries.empty()) new_primary = 0;
    if (new_primary >= 0) {
        list_->select_only(new_primary);
        list_->ensure_visible_row(new_primary);
    }

    // Rendering the preview is only useful when the pane is
    // visible — otherwise db_entry_load_details + HTML build +
    // image-cache inline all run for nothing. When the pane is
    // re-opened, toggle_pane calls show_entry(current primary)
    // so the pane catches up with whatever's selected.
    if (pane_shown("entry_detail")) {
        if (new_primary >= 0 && (size_t)new_primary < app_->entries.size())
            detail_->show_entry(&app_->entries[(size_t)new_primary]);
        else
            detail_->show_entry(nullptr);
    }
}

void MainFrame::flash_status(const wxString &msg)
{
    // Restart the timer on each call so consecutive flashes (e.g.
    // user copies several entries in quick succession) each get
    // their full visibility window before reverting.
    SetStatusText(msg);
    flash_timer_.StartOnce(2000);
}

void MainFrame::update_status()
{
    int unread = 0;
    for (auto &e : app_->entries) {
        if (std::find(e.tags.begin(), e.tags.end(), "unread")
            != e.tags.end())
            unread++;
    }
    int active = app_->fetches_active.load();
    int total = app_->fetches_total.load();

    // Build the status line by concatenation rather than as a single
    // wxString::Format call. Embedding non-ASCII glyphs (… and ·)
    // inside the format literal routes through wx's format-string
    // machinery, which does a narrow/wide conversion dance inside
    // Format and has hit a null-format-pointer crash on Linux at
    // wxFormatConverterBase<char>::Convert. Keeping every format
    // string ASCII-only (so only %d conversions run through Format)
    // and splicing the glyphs in via wxString += wxT(...) sidesteps
    // it — the wide literals never touch the narrow-format path.
    // \u2026 = …, \u00b7 = ·.
    wxString msg;
    if (active > 0) {
        msg = wxString::Format("Fetching %d/%d", active, total);
        msg += wxT(" \u2026  ");
        msg += wxString::Format("%d unread of %d",
                                unread, (int)app_->entries.size());
        msg += wxT("  \u00b7  ");
        msg += wxString::Format("%d feeds", (int)app_->feeds.size());
    } else {
        // Append "last fetch" so the user can tell at a glance how
        // stale the view is. We don't poll automatically — this is
        // the nudge that tells them when it's time to fetch.
        wxString last = wxString::FromUTF8(
            format_relative_time(app_->last_fetch));
        msg = wxString::Format("%d unread of %d",
                               unread, (int)app_->entries.size());
        msg += wxT("  \u00b7  ");
        msg += wxString::Format("%d feeds", (int)app_->feeds.size());
        msg += wxT("  \u00b7  last fetch: ");
        msg += last;
    }
    SetStatusText(msg);
}

bool MainFrame::pane_shown(const char *name) const
{
    // GetPane is logically const here, but the wxAUI API isn't const-correct.
    auto &info = const_cast<wxAuiManager &>(mgr_).GetPane(name);
    return info.IsOk() && info.IsShown();
}

void MainFrame::update_menu_checks()
{
    auto *mbar = GetMenuBar();
    if (!mbar) return;
    mbar->Check(menu_feeds_id_,     pane_shown("feeds"));
    mbar->Check(menu_preview_id_,   pane_shown("entry_detail"));
    mbar->Check(menu_log_id_,       pane_shown("log"));
    mbar->Check(menu_downloads_id_, pane_shown("downloads"));
    mbar->Check(menu_activity_id_,  pane_shown("activity"));
}

void MainFrame::toggle_pane(const char *name)
{
    auto &info = mgr_.GetPane(name);
    if (!info.IsOk()) return;
    info.Show(!info.IsShown());
    mgr_.Update();
    update_menu_checks();
    if (info.IsShown()) {
        if (std::string(name) == "log" && log_) log_->refresh();
        else if (std::string(name) == "downloads" && downloads_)
            downloads_->refresh();
        else if (std::string(name) == "feeds" && feeds_)
            feeds_->refresh();
        else if (std::string(name) == "activity" && activity_)
            activity_->refresh();
        else if (std::string(name) == "entry_detail" && detail_) {
            // Preview-pane render sites elsewhere are gated on
            // pane_shown so they're no-ops while hidden; the pane
            // reopening is the trigger to catch up with whatever's
            // currently selected.
            long p = list_ ? list_->primary() : -1;
            if (p >= 0 && (size_t)p < app_->entries.size())
                detail_->show_entry(&app_->entries[(size_t)p]);
            else
                detail_->show_entry(nullptr);
        }
    }
}

void MainFrame::apply_filter(const std::string &text)
{
    filter_->ChangeValue(wxString::FromUTF8(text));
    app_->current_filter = filter_parse(text);
    requery();
    update_status();
    // The filter didn't get focus (we're driving it programmatically),
    // so the KILL_FOCUS handler won't fire — persist explicitly here.
    db_save_ui_state(app_, "filter", text.c_str());
}

void MainFrame::set_filter_to_feed(const std::string &feed_url)
{
    // Drop the "scheme://" prefix so the filter reads =example.com/...
    // rather than =https://example.com/... — shorter, friendlier, and
    // in the rare case a user has otherwise-identical http and https
    // feeds they can just retype the scheme manually.
    std::string tail = feed_url;
    size_t sep = tail.find("://");
    if (sep != std::string::npos) tail.erase(0, sep + 3);
    apply_filter("=" + tail);
}

// ---- Event handlers ------------------------------------------------

void MainFrame::on_wake(wxThreadEvent &)
{
    bool new_entries = fetch_process_results(app_);
    if (new_entries) {
        requery();
        if (feeds_) feeds_->refresh();  // titles may have been filled in
        // Activity's DB query is the only one of the bottom panes
        // that touches real storage, so gate it on new entries
        // actually landing (rather than refreshing on every wake).
        // When hidden, the refresh is skipped entirely; toggle_pane
        // freshens the data on next show.
        if (pane_shown("activity") && activity_) activity_->refresh();
    }
    // Drain the inline-image inbox: worker threads push downloaded
    // bytes, we write them to the cache table, and if anything
    // landed, re-render the preview pane so the new data: URIs
    // replace the broken <img> boxes. Skip the re-render when
    // the pane is hidden; reopening it does a fresh render from
    // scratch (toggle_pane path) that naturally picks up
    // whatever's in the cache by then.
    if (image_cache_process_results(app_)
        && detail_ && pane_shown("entry_detail"))
        detail_->relayout();
    // Note: log persistence is intentionally NOT drained here.
    // on_wake fires very often (per fetch result, per image
    // download, per download tick) and a synchronous SQLite
    // commit per wake would freeze the UI on a fetch storm. A
    // dedicated 5-second timer handles drain instead; on_close
    // does a final drain so a clean exit doesn't lose entries.
    update_status();
    download_tick(app_);
    if (pane_shown("log")       && log_)       log_->refresh();
    if (pane_shown("downloads") && downloads_) downloads_->refresh();
}

void MainFrame::on_filter_text(wxCommandEvent &)
{
    // Restart the debounce window on every edit. See commit_filter.
    filter_debounce_.StartOnce(180);
}

void MainFrame::commit_filter(bool capped)
{
    std::string text = filter_->GetValue().utf8_string();
    app_->current_filter = filter_parse(text);
    requery(capped ? list_->desired_row_count() : 0);
    update_status();
}

void MainFrame::on_fetch_all(wxCommandEvent &)
{
    fetch_all(app_);
    update_status();
}

void MainFrame::on_reload_config(wxCommandEvent &)
{
    // Re-parse the config file and rebuild subscription-derived state
    // in place — same effect as quit+relaunch, minus the UI state
    // (window geometry, AUI layout, current filter) which stays put.
    // Feed metadata (etag, last_modified, last_update) survives
    // because config_reload re-hydrates it from the DB.
    elfeed_log(app_, LOG_INFO, "reloading config from %s",
               app_->config_path.c_str());
    config_reload(app_);

    // The subscription list, feed titles, and autotags may all have
    // changed. Refresh the feeds panel, re-run the query so the entry
    // list picks up new `feed_title` values (used by = / ~ filters),
    // and update the status bar entry count.
    if (feeds_) feeds_->refresh();
    requery();
    update_status();
}

void MainFrame::on_import_classic(wxCommandEvent &)
{
    // Defer the dialog until the menu event has fully unwound. On
    // macOS, opening a modal file dialog directly from inside a menu
    // command handler sometimes fails to show because the menu bar
    // dropdown is still closing; CallAfter runs the dialog at the
    // next idle tick, after the menu is fully dismissed.
    CallAfter([this] { do_import_classic(); });
}

void MainFrame::do_import_classic()
{
    // Only suggest ~/.elfeed as the default dir when it actually
    // exists — macOS's NSOpenPanel ignores (or errors on) a missing
    // default path. Fall back to the user's home directory otherwise.
    wxString default_dir = wxString::FromUTF8(user_home_dir() + "/.elfeed");
    if (!wxDirExists(default_dir))
        default_dir = wxString::FromUTF8(user_home_dir());

    wxFileDialog dlg(this,
                     "Select Classic Elfeed index file",
                     default_dir,
                     wxEmptyString,
                     "All files (*)|*",
                     wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() != wxID_OK) return;

    std::string path = dlg.GetPath().utf8_string();
    ImportStats stats;
    {
        wxBusyInfo busy(wxT("Importing Classic Elfeed index…"), this);
        wxYield();  // give wxBusyInfo a chance to paint
        stats = import_classic_elfeed(app_, path);
    }

    if (!stats.error.empty()) {
        wxMessageBox(wxString::FromUTF8("Import failed: " + stats.error),
                     "Import Error", wxOK | wxICON_ERROR, this);
        return;
    }

    // Refresh the UI to show imported content. Reload the title map
    // first so imported feeds (which won't be in app->feeds) still
    // resolve to nice titles in the entry list / detail panel.
    db_load_feed_titles(app_);
    requery();
    if (feeds_) feeds_->refresh();
    // Import just dumped a year (or more) of entries into the DB —
    // the heatmap shape changes completely, so refresh if visible.
    if (pane_shown("activity") && activity_) activity_->refresh();
    update_status();

    wxMessageBox(
        wxString::Format(
            "Imported %d feeds and %d entries.%s",
            stats.feeds_imported, stats.entries_imported,
            stats.entries_skipped > 0
                ? wxString::Format("\n(Skipped %d malformed entries.)",
                                   stats.entries_skipped)
                : wxString()),
        "Import Complete", wxOK | wxICON_INFORMATION, this);
}

void MainFrame::on_reclaim_space(wxCommandEvent &)
{
    // Flush any coalesced-but-not-yet-written log entries first so
    // VACUUM's write lock doesn't fight the periodic drain timer.
    log_drain_to_db(app_);

    int64_t saved;
    {
        // VACUUM takes seconds on a mid-sized DB — show a modal
        // busy indicator so the user knows we're working, not
        // hung. Scope limits the indicator to the call.
        wxBusyInfo busy(wxT("Reclaiming disk space…"), this);
        saved = db_reclaim_space(app_);
    }

    if (saved > 0) {
        flash_status(wxString::Format(
            "Freed %s of disk space",
            wxFileName::GetHumanReadableSize(wxULongLong(saved))));
    } else {
        flash_status(wxT("Database already compact"));
    }
}

void MainFrame::on_toggle_feeds(wxCommandEvent &)     { toggle_pane("feeds"); }
void MainFrame::on_toggle_preview(wxCommandEvent &)   { toggle_pane("entry_detail"); }
void MainFrame::on_toggle_log(wxCommandEvent &)       { toggle_pane("log"); }
void MainFrame::on_toggle_downloads(wxCommandEvent &) { toggle_pane("downloads"); }
void MainFrame::on_toggle_activity(wxCommandEvent &)  { toggle_pane("activity"); }

void MainFrame::on_reset_layout(wxCommandEvent &)
{
    // Reset every "layout" concern: AUI perspective, per-panel
    // column widths/visibility, per-panel sort state. Current
    // working state (filter, selection, window geometry) is
    // deliberately preserved — those aren't layout per se.
    if (!default_perspective_.empty()) {
        mgr_.LoadPerspective(default_perspective_, false);
        // The snapshotted perspective carries the original MinSize
        // values; loosen them again so resizing stays free after a
        // reset. (Same reason loosen_pane_min_sizes runs after
        // every LoadPerspective on startup.)
        loosen_pane_min_sizes();
        mgr_.Update();
        db_save_ui_state(app_, "layout",
                         default_perspective_.utf8_string().c_str());
    }
    if (feeds_)     feeds_->reset_layout();
    if (log_)       log_->reset_layout();
    if (downloads_) downloads_->reset_layout();
    if (list_) {
        list_->reset_layout();
        // Re-run the query so entries come back in SQL order (date
        // DESC) rather than whatever the previous sort produced.
        requery();
    }
    update_menu_checks();
}

void MainFrame::on_about(wxCommandEvent &)
{
    // Custom dialog instead of wxAboutBox. The generic wxAboutDialog
    // wraps the license in a wxCollapsiblePane whose expansion path
    // does top->SetClientSize(sizer->ComputeFittingClientSize(top)),
    // and the static text inside that pane is created with a Wrap()
    // hint of wxGetDisplaySize().x / 3. On a wide macOS display
    // that's ~1700px and the dialog blows up to that size on expand.
    // Owning the layout here sidesteps both issues.
    wxDialog dlg(this, wxID_ANY, "About Elfeed2",
                 wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);

    auto *outer = new wxBoxSizer(wxVERTICAL);
    int pad = dlg.FromDIP(10);

    // Heading: Elfeed2 (large, bold) + version (subdued).
    auto *name = new wxStaticText(&dlg, wxID_ANY, "Elfeed2");
    {
        wxFont f = name->GetFont();
        f.SetPointSize(f.GetPointSize() + 6);
        f.MakeBold();
        name->SetFont(f);
    }
    outer->Add(name, 0, wxALIGN_CENTRE | wxTOP, pad);

    auto *ver = new wxStaticText(&dlg, wxID_ANY,
                                 wxString("Version " ELFEED_VERSION));
    outer->Add(ver, 0, wxALIGN_CENTRE | wxTOP, dlg.FromDIP(2));

    auto *desc = new wxStaticText(&dlg, wxID_ANY,
        wxT("A standalone feed reader, successor to Emacs Elfeed."));
    outer->Add(desc, 0, wxALIGN_CENTRE | wxTOP, pad);

    // Clickable project URL.
    auto *link = new wxHyperlinkCtrl(&dlg, wxID_ANY,
        "github.com/skeeto/elfeed2",
        "https://github.com/skeeto/elfeed2");
    outer->Add(link, 0, wxALIGN_CENTRE | wxTOP, pad);

    auto *copyright = new wxStaticText(&dlg, wxID_ANY,
        wxT("Public Domain (Unlicense)"));
    outer->Add(copyright, 0, wxALIGN_CENTRE | wxTOP, dlg.FromDIP(4));

    auto *deps_label = new wxStaticText(&dlg, wxID_ANY,
                                        wxT("Bundled components:"));
    outer->Add(deps_label, 0, wxLEFT | wxRIGHT | wxTOP, pad);

    // wxTextCtrl multi-line so the deps list scrolls inside the
    // dialog instead of pushing the dialog's outer size to fit.
    // Read-only; users can still select / copy. Versions track
    // what FetchContent pulls in CMakeLists — keep them in sync
    // when bumping deps.
    auto *deps = new wxTextCtrl(&dlg, wxID_ANY, wxT(
        "  wxWidgets 3.2.10        wxWindows Library Licence\n"
        "  cpp-httplib 0.43.0      MIT\n"
        "  mbedTLS 3.6.2           Apache License 2.0\n"
        "  pugixml 1.14            MIT\n"
        "  SQLite 3.49.1           Public Domain\n"),
        wxDefaultPosition, dlg.FromDIP(wxSize(420, 120)),
        wxTE_MULTILINE | wxTE_READONLY | wxTE_DONTWRAP);
    // Monospace face so the columns of name + license align.
    {
        wxFont mono(wxFontInfo(deps->GetFont().GetPointSize())
                        .Family(wxFONTFAMILY_TELETYPE));
        deps->SetFont(mono);
    }
    outer->Add(deps, 1, wxEXPAND | wxLEFT | wxRIGHT, pad);

    auto *btns = dlg.CreateButtonSizer(wxOK);
    outer->Add(btns, 0, wxEXPAND | wxALL, pad);

    dlg.SetSizerAndFit(outer);
    dlg.SetMinSize(dlg.GetSize());
    dlg.CentreOnParent();
    dlg.ShowModal();
}

void MainFrame::on_quit(wxCommandEvent &)
{
    Close(false);
}

void MainFrame::on_frame_move_size(wxEvent &e)
{
    // Track the last non-maximized / non-iconized rect so we can
    // persist it at close time. Also allow wx to process the event
    // normally (Skip()) — this is just a sniffer.
    if (!IsMaximized() && !IsIconized())
        normal_rect_ = wxRect(GetPosition(), GetSize());
    e.Skip();
}

void MainFrame::on_close(wxCloseEvent &)
{
    // Persist per-panel column widths/visibility before the panels die.
    if (list_)      list_->save_columns();
    if (feeds_)     feeds_->save_columns();
    if (log_)       log_->save_columns();
    if (downloads_) downloads_->save_columns();

    // Filter text: the KILL_FOCUS handler covers typed edits, but if
    // the filter still has focus at quit-time the blur doesn't fire.
    if (filter_) {
        db_save_ui_state(app_, "filter",
                         filter_->GetValue().utf8_string().c_str());
    }

    // Save the AUI perspective (covers pane positions).
    wxString persp = mgr_.SavePerspective();
    db_save_ui_state(app_, "layout", persp.utf8_string().c_str());

    // Save the outer-frame geometry. When maximized we still record
    // the last non-maximized rect (tracked by on_frame_move_size) so
    // un-maximize next run gives the user the same window they had.
    bool maxi = IsMaximized();
    char buf[64];
    snprintf(buf, sizeof(buf), "%d %d %d %d %d",
             normal_rect_.x, normal_rect_.y,
             normal_rect_.width, normal_rect_.height,
             maxi ? 1 : 0);
    db_save_ui_state(app_, "geometry", buf);

    // Final log drain so any unsaved entries from this session
    // make it to the DB. The 5-second drain timer can lag the
    // last few seconds of activity by definition.
    log_drain_to_db(app_);

    mgr_.UnInit();
    Destroy();
}

void MainFrame::on_pane_close(wxAuiManagerEvent &e)
{
    // wxAUI is closing a pane via its X button. The pane's IsShown()
    // flag doesn't reliably flip until after this event returns AND
    // wxAUI's internal Update() runs, so polling it (even via
    // CallAfter) can read stale state. Instead, read GetPane() to
    // learn which pane is closing and uncheck its menu directly —
    // we know the state unconditionally at this point.
    wxAuiPaneInfo *pane = e.GetPane();
    auto *mbar = GetMenuBar();
    if (pane && mbar) {
        wxString name = pane->name;
        if      (name == "feeds")        mbar->Check(menu_feeds_id_,     false);
        else if (name == "entry_detail") mbar->Check(menu_preview_id_,   false);
        else if (name == "log")          mbar->Check(menu_log_id_,       false);
        else if (name == "downloads")    mbar->Check(menu_downloads_id_, false);
        else if (name == "activity")     mbar->Check(menu_activity_id_,  false);
    }
    e.Skip();
}

void MainFrame::on_list_selected(wxDataViewEvent &)
{
    // Only render when the preview pane is actually visible.
    // Navigating entries with the pane hidden (the "headline
    // scan" pattern) otherwise ran a full DB detail-load + HTML
    // build + image-cache pass on every j/k, none of which the
    // user could see. When the pane opens, toggle_pane does a
    // one-shot show_entry on the current selection.
    if (!pane_shown("entry_detail")) return;
    long p = list_->primary();
    if (p >= 0 && (size_t)p < app_->entries.size())
        detail_->show_entry(&app_->entries[(size_t)p]);
}

void MainFrame::on_list_activated(wxDataViewEvent &)
{
    // wxEVT_DATAVIEW_ITEM_ACTIVATED fires on Enter or double-click.
    // Both gestures mean "commit to reading this one": move keyboard
    // focus into the preview pane so the reader-mode bindings
    // (q/Escape, n/p, b/y/d/u) take over. The preview was already
    // showing the entry as soon as selection landed on it, so this
    // is strictly a focus transfer — no content change.
    if (detail_) detail_->focus_body();
}

void MainFrame::on_list_context_menu(wxDataViewEvent &event)
{
    // Right-click on empty space (no row): nothing to act on.
    wxDataViewItem item = event.GetItem();
    if (!item.IsOk()) return;

    // If the user right-clicks a row outside the current selection,
    // make that the selection — same pattern FeedsPanel uses.
    // Otherwise the menu would dispatch against whatever the user
    // had selected before, which is rarely what they meant. Also
    // refresh the preview pane: programmatic Select doesn't fire
    // wxEVT_DATAVIEW_SELECTION_CHANGED, so on_list_selected won't
    // run for us.
    if (!list_->IsSelected(item)) {
        list_->UnselectAll();
        list_->Select(item);
        list_->SetCurrentItem(item);
        long p = list_->primary();
        if (p >= 0 && (size_t)p < app_->entries.size()
            && pane_shown("entry_detail"))
            detail_->show_entry(&app_->entries[(size_t)p]);
    }

    auto sel = list_->selection();
    // Enclosure data is deferred during the listing query, so we
    // need to pull it for the selected rows before we can gate the
    // "Download Enclosure" menu item on any_enclosure.
    bool any_enclosure = false;
    for (long i : sel) {
        if (i >= 0 && (size_t)i < app_->entries.size()) {
            Entry &e = app_->entries[(size_t)i];
            db_entry_load_details(app_, e);
            if (!e.enclosures.empty()) { any_enclosure = true; break; }
        }
    }

    enum {
        ID_Browser = wxID_HIGHEST + 1,
        ID_CopyLink,
        ID_MarkRead,
        ID_MarkUnread,
        ID_Download,
        ID_FilterFeed,
    };

    // \tKey makes wxWidgets right-align the shortcut hint native-
    // style — same convention the main menu uses for "Quit\tCtrl+Q"
    // and the feeds-panel context menu uses for the copy actions.
    // Bare letters (b, y, r, u, d) are display-only here; the
    // actual keystrokes go through on_list_key.
    wxMenu menu;
    menu.Append(ID_Browser,    "Open in &Browser\tb");
    menu.Append(ID_CopyLink,   "&Copy Link\ty");
    menu.AppendSeparator();
    menu.Append(ID_MarkRead,   "Mark as &Read\tr");
    menu.Append(ID_MarkUnread, "Mark as &Unread\tu");
    if (any_enclosure)
        menu.Append(ID_Download, "&Download Enclosure\td");
    menu.AppendSeparator();
    menu.Append(ID_FilterFeed, "&Filter to This Feed");

    int choice = list_->GetPopupMenuSelectionFromUser(menu);
    switch (choice) {
    case ID_Browser:    action_open_in_browser(); break;
    case ID_CopyLink:   action_copy_link();       break;
    case ID_MarkRead:   action_mark_read();       break;
    case ID_MarkUnread: action_mark_unread();     break;
    case ID_Download:   action_download();        break;
    case ID_FilterFeed: {
        // Use the primary (focused) entry's feed, even if multiple
        // rows are selected — the menu was launched from one of
        // those rows, so its source feed is the natural pick.
        long p = list_->primary();
        if (p >= 0 && (size_t)p < app_->entries.size()) {
            set_filter_to_feed(app_->entries[(size_t)p].feed_url);
        }
        break;
    }
    }
}

void MainFrame::on_list_key(wxKeyEvent &e)
{
    int code = e.GetKeyCode();
    bool plain = !e.HasAnyModifiers();
    // Ctrl+A / Cmd+A: select every entry. ControlDown maps to Cmd
    // on macOS (the OS-native shortcut for Select All), while
    // RawControlDown is the physical Ctrl key on every platform —
    // bind both so cross-platform muscle memory works.
    if (code == 'A' && (e.RawControlDown() || e.ControlDown()) &&
        !e.AltDown()) {
        list_->SelectAll();
        return;
    }
    // Ctrl+L: re-apply the current filter. Same mental model as
    // vi/terminal Ctrl+L (redraw screen) — handy after toggling
    // the `unread` tag on a row under a `+unread` filter, since
    // the row stays visible until the next requery. Physical Ctrl
    // only (RawControlDown); on macOS we deliberately don't bind
    // Cmd+L, which browsers use for address-bar focus and users
    // may have elsewhere-muscle-memory for.
    if (code == 'L' && e.RawControlDown() &&
        !e.AltDown() && !e.MetaDown()) {
        requery();
        return;
    }
    // Escape: two-stage reset. If visual-selection mode is on, the
    // first press exits it (leaves the range selected, in case the
    // user still wants to act on it). The next press, with no
    // anchor active, collapses the multi-selection back to just
    // the focused row — the "Clear selection" affordance.
    if (code == WXK_ESCAPE && plain) {
        if (visual_anchor_ >= 0) {
            visual_anchor_ = -1;
            flash_status("Visual selection off");
        } else {
            long p = list_->primary();
            if (p >= 0) list_->select_only(p);
        }
        return;
    }
    switch (code) {
    case 'J': if (plain) { move_selection(+1); return; } break;
    case 'K': if (plain) { move_selection(-1); return; } break;
    case 'G':
        if (e.ShiftDown())
            go_to((long)app_->entries.size() - 1);
        else if (plain)
            go_to(0);
        else break;
        return;
    case 'U': if (plain) { action_mark_unread();      return; } break;
    case 'R': if (plain) { action_mark_read();        return; } break;
    case 'B': if (plain) { action_open_in_browser();  return; } break;
    case 'Y': if (plain) { action_copy_link();        return; } break;
    case 'P': if (plain) { action_download_url();     return; } break;
    case 'D':
        // Lowercase d: download the selected entries (per-entry
        // path chosen in action_download). Capital D (Shift+D):
        // toggle the Downloads panel so the user can watch
        // progress. Pairs with 'd' mnemonically.
        if (e.ShiftDown()) { toggle_pane("downloads"); return; }
        if (plain)         { action_download();        return; }
        break;
    case 'L':
        // Lowercase l: toggle the Log panel. Vim's `l` (move one
        // character right) has no analog in our row-oriented list,
        // so the key is free. Capital L is reserved for future use.
        if (plain) { toggle_pane("log"); return; }
        break;
    case 'F': if (plain) { fetch_all(app_); update_status(); return; } break;
    case 'V':
        // Visual-selection mode. Entering anchors at the focused
        // row so subsequent j/k/g/G extend the selection as a
        // contiguous range; exiting (v again, Escape, or any row
        // action) clears the anchor. Starts fresh on entry — any
        // prior multi-selection collapses to just the anchor row.
        if (plain) {
            if (visual_anchor_ >= 0) {
                visual_anchor_ = -1;
                flash_status("Visual selection off");
            } else {
                long p = list_->primary();
                if (p >= 0) {
                    visual_anchor_ = p;
                    list_->select_only(p);
                    flash_status("Visual selection");
                }
            }
            return;
        }
        break;
    case 'S':
    case '/':
        if (plain) {
            filter_->SetFocus();
            filter_->SetInsertionPointEnd();
            return;
        }
        break;
    }

    if (try_preset_key(e)) return;

    e.Skip();
}

bool MainFrame::try_preset_key(wxKeyEvent &e)
{
    // User-defined filter presets bound to single letters via the
    // `preset` config directive. Skip when modifiers are held —
    // those are reserved for native shortcuts (Ctrl+W, Cmd+Q, …).
    // Shift counts as "case modifier" for letters: `preset H` and
    // `preset h` are independently bindable.
    if (e.ControlDown() || e.AltDown() || e.MetaDown()) return false;
    int code = e.GetKeyCode();
    if (code < 0x20 || code >= 0x7F) return false;
    char c = (char)code;
    if (!e.ShiftDown() && c >= 'A' && c <= 'Z')
        c = (char)(c - 'A' + 'a');
    auto it = app_->presets.find(c);
    if (it == app_->presets.end()) return false;
    apply_filter(it->second);
    return true;
}

void MainFrame::on_detail_key(wxKeyEvent &e)
{
    // Reader-mode bindings, active while the preview pane has focus.
    // Plain single letters only — wxHtmlWindow's built-in scrolling
    // (arrow keys, Page Up/Down, Home/End) still works because we
    // Skip() for anything we don't recognize.
    int code = e.GetKeyCode();
    bool plain = !e.HasAnyModifiers();
    if (plain) {
        switch (code) {
        case WXK_ESCAPE:
        case 'Q':
            // Return focus to the listing. The detail pane stays
            // visible; it's just no longer the keyboard target.
            list_->SetFocus();
            return;
        case 'N': step_entry(+1);           return;
        case 'P': step_entry(-1);           return;
        case 'B': action_open_in_browser(); return;
        case 'Y': action_copy_link();       return;
        case 'D': action_download();        return;
        case 'U': action_mark_unread();     return;
        case 'R': action_mark_read();       return;
        case 'J':
            // j/k scroll the body by a line. n/p are the "navigate
            // between entries" keys in reader mode, so j/k are free
            // to mean what they do in any vi-ish pager: move the
            // viewport, not the entry.
            if (detail_) detail_->scroll_lines(+1);
            return;
        case 'K':
            if (detail_) detail_->scroll_lines(-1);
            return;
        }
    }
    if (try_preset_key(e)) return;
    e.Skip();
}

void MainFrame::on_filter_key(wxKeyEvent &e)
{
    int code = e.GetKeyCode();
    if (code == WXK_ESCAPE || code == WXK_RETURN || code == WXK_NUMPAD_ENTER) {
        // Stop any pending debounced edit; the KILL_FOCUS handler
        // fires as we set focus away and does the final uncapped
        // commit, so we don't do one here. (Doing it here would
        // be a capped commit immediately followed by an uncapped
        // one from KILL_FOCUS — wasteful.)
        filter_debounce_.Stop();
        list_->SetFocus();
        return;
    }

    // Ctrl+W: delete previous whitespace-delimited word. Shell/readline
    // style — treats `+unread` and `@6-months-ago` as single words,
    // unlike the platform-native Alt+Backspace which breaks at non-alnum
    // punctuation. RawControlDown() is the physical Ctrl key; on macOS
    // ControlDown() is Cmd, which we must not shadow (Cmd+W = close).
    if (code == 'W' && e.RawControlDown() &&
        !e.AltDown() && !e.MetaDown()) {
        long from = 0, to = 0;
        filter_->GetSelection(&from, &to);
        if (from != to) {
            filter_->Remove(from, to);
            filter_->SetInsertionPoint(from);
            return;
        }
        wxString text = filter_->GetValue();
        long pos = filter_->GetInsertionPoint();
        long left = pos;
        while (left > 0 &&
               (text[left - 1] == ' ' || text[left - 1] == '\t'))
            left--;
        while (left > 0 &&
               text[left - 1] != ' ' && text[left - 1] != '\t')
            left--;
        if (left < pos) {
            filter_->Remove(left, pos);
            filter_->SetInsertionPoint(left);
        }
        return;
    }

    e.Skip();
}

// ---- Selection helpers ---------------------------------------------

void MainFrame::move_selection(int delta)
{
    long n = (long)app_->entries.size();
    if (n == 0) return;
    long p = list_->primary();
    long target;
    if (p < 0) {
        // No row is focused yet (just finished a requery, or the
        // list never had focus). j/k should "wake up" the list by
        // landing on the top row — that's the convention every
        // other list widget follows, and it keeps the keys useful
        // after a filter change that cleared the selection.
        target = 0;
    } else {
        target = p + delta;
        if (target < 0 || target >= n) return;
    }
    if (visual_anchor_ >= 0) {
        long lo = std::min(visual_anchor_, target);
        long hi = std::max(visual_anchor_, target);
        list_->select_range(lo, hi, target);
    } else {
        list_->select_only(target);
    }
    list_->ensure_visible_row(target);
}

void MainFrame::go_to(long row)
{
    long n = (long)app_->entries.size();
    if (row < 0 || row >= n) return;
    if (visual_anchor_ >= 0) {
        long lo = std::min(visual_anchor_, row);
        long hi = std::max(visual_anchor_, row);
        list_->select_range(lo, hi, row);
    } else {
        list_->select_only(row);
    }
    list_->ensure_visible_row(row);
}

void MainFrame::advance_from(long row)
{
    long n = (long)app_->entries.size();
    long next = row + 1;
    if (next >= n) next = n - 1;
    if (next < 0) return;
    list_->select_only(next);
    list_->ensure_visible_row(next);
    if (pane_shown("entry_detail"))
        detail_->show_entry(&app_->entries[(size_t)next]);
}

void MainFrame::step_entry(int delta)
{
    long p = list_->primary();
    long n = (long)app_->entries.size();
    if (p < 0) return;
    long target = p + delta;
    if (target < 0 || target >= n) return;
    // Use select_only (not select_range) — stepping in the detail
    // pane shouldn't silently extend a visual range; if the user
    // wants that workflow they can do it from the list.
    visual_anchor_ = -1;
    list_->select_only(target);
    list_->ensure_visible_row(target);
    detail_->show_entry(&app_->entries[(size_t)target]);
}

// ---- Actions -------------------------------------------------------

static void strip_unread(Entry &e, Elfeed *app)
{
    auto it = std::find(e.tags.begin(), e.tags.end(), "unread");
    if (it != e.tags.end()) {
        e.tags.erase(it);
        db_untag(app, e.namespace_, e.id, "unread");
    }
}

static void add_unread(Entry &e, Elfeed *app)
{
    if (std::find(e.tags.begin(), e.tags.end(), "unread") == e.tags.end()) {
        e.tags.push_back("unread");
        std::sort(e.tags.begin(), e.tags.end());
        db_tag(app, e.namespace_, e.id, "unread");
    }
}

void MainFrame::action_mark_read()
{
    auto sel = list_->selection();
    if (sel.empty()) return;
    for (long i : sel) {
        if (i < 0 || (size_t)i >= app_->entries.size()) continue;
        strip_unread(app_->entries[(size_t)i], app_);
        list_->refresh_row(i);
    }
    if (sel.size() == 1) advance_from(sel[0]);
    visual_anchor_ = -1;
    update_status();
}

void MainFrame::action_mark_unread()
{
    auto sel = list_->selection();
    if (sel.empty()) return;
    for (long i : sel) {
        if (i < 0 || (size_t)i >= app_->entries.size()) continue;
        add_unread(app_->entries[(size_t)i], app_);
        list_->refresh_row(i);
    }
    if (sel.size() == 1) advance_from(sel[0]);
    visual_anchor_ = -1;
    update_status();
}

void MainFrame::action_open_in_browser()
{
    auto sel = list_->selection();
    if (sel.empty()) return;
    for (long i : sel) {
        if (i < 0 || (size_t)i >= app_->entries.size()) continue;
        Entry &e = app_->entries[(size_t)i];
        if (!e.link.empty())
            wxLaunchDefaultBrowser(wxString::FromUTF8(e.link));
        // Opening in the browser counts as reading it — match 'r'.
        strip_unread(e, app_);
        list_->refresh_row(i);
    }
    if (sel.size() == 1) advance_from(sel[0]);
    visual_anchor_ = -1;
    update_status();
}

void MainFrame::action_copy_link()
{
    auto sel = list_->selection();
    std::vector<std::string> urls;
    for (long i : sel) {
        if (i < 0 || (size_t)i >= app_->entries.size()) continue;
        const std::string &link = app_->entries[(size_t)i].link;
        if (!link.empty()) urls.push_back(link);
    }
    if (urls.empty()) return;

    std::string joined;
    for (auto &u : urls) {
        if (!joined.empty()) joined += '\n';
        joined += u;
    }
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(
            new wxTextDataObject(wxString::FromUTF8(joined)));
        wxTheClipboard->Close();
    }
    // Confirm the copy in the status bar — silent yank is hard to
    // tell apart from a misfire. For a single URL show the URL
    // itself so the user can sanity-check it; for many show a count.
    if (urls.size() == 1)
        flash_status(wxString::FromUTF8("Copied: " + urls[0]));
    else
        flash_status(wxString::Format("Copied %zu URLs to clipboard",
                                      urls.size()));
    visual_anchor_ = -1;
}

void MainFrame::action_download()
{
    auto sel = list_->selection();
    for (long i : sel) {
        if (i < 0 || (size_t)i >= app_->entries.size()) continue;
        Entry &e = app_->entries[(size_t)i];
        // Listing query defers enclosure loading; pull it now so
        // the enclosure-vs-yt-dlp branch below makes the right
        // call for this entry.
        db_entry_load_details(app_, e);

        if (!e.enclosures.empty()) {
            // Enclosure present (e.g. podcast): download the first one
            // directly via our HTTP stack with a filename we control.
            const Enclosure &enc = e.enclosures.front();
            std::string feed_title;
            for (auto &f : app_->feeds) {
                if (f.url == e.feed_url) { feed_title = f.title; break; }
            }
            std::string base = format_date_compact(e.date);
            std::string ft = sanitize_filename(feed_title);
            std::string et = sanitize_filename(e.title);
            if (!ft.empty()) base += "-" + ft;
            if (!et.empty()) base += "-" + et;
            std::string ext = mime_to_extension(enc.type, enc.url);
            std::string dir = app_->download_dir.empty()
                                  ? user_home_dir() + "/Downloads"
                                  : app_->download_dir;
            make_directory(dir);
            std::string path = disambiguate_path(dir, base, ext);
            download_enqueue_http(app_, enc.url, e.title, path);
        } else {
            // No enclosure — hand off to yt-dlp as before.
            download_enqueue(app_, e.link, e.title, /*is_video=*/true);
        }
        strip_unread(e, app_);
        list_->refresh_row(i);
    }
    download_tick(app_);
    if (pane_shown("downloads") && downloads_) downloads_->refresh();
    if (sel.size() == 1) advance_from(sel[0]);
    visual_anchor_ = -1;
    update_status();
}

namespace {

// Does the URL's path end in a media / document extension we can
// download directly via HTTP, bypassing yt-dlp? `mime_to_extension`
// with an empty content-type falls back to a URL-trailing-alnum
// extractor, which returns "bin" when nothing recognizable is
// there — that's our "not a direct download" signal. Everything
// else (plain http(s), query strings, fragment tails) is fine —
// `mime_to_extension` already peels them off before looking at
// the dot.
bool url_is_direct_download(const std::string &url)
{
    static const char *known[] = {
        "mp3", "m4a", "aac", "ogg", "opus", "flac", "wav", "webm",
        "mp4", "mpeg", "mov", "mkv",
        "pdf", "zip", "epub",
        "jpg", "jpeg", "png", "gif", "webp",
        nullptr,
    };
    std::string ext = mime_to_extension("", url);
    for (const char **p = known; *p; p++)
        if (ext == *p) return true;
    return false;
}

// Extract (host, basename-without-extension) from a URL for the
// HTTP-direct filename rule. No entry title or feed title is
// available for an ad-hoc download, so we fall back to the URL's
// own hostname + trailing path segment — a best-effort label
// that's still stable across disambiguate_path collisions.
void url_host_and_basename(const std::string &url,
                           std::string &host, std::string &base)
{
    host.clear();
    base.clear();
    size_t scheme_end = url.find("://");
    size_t host_start = (scheme_end == std::string::npos)
                            ? 0
                            : scheme_end + 3;
    size_t path_start = url.find('/', host_start);
    host = (path_start == std::string::npos)
               ? url.substr(host_start)
               : url.substr(host_start, path_start - host_start);
    // Strip userinfo@ and :port
    if (auto at = host.find('@'); at != std::string::npos)
        host = host.substr(at + 1);
    if (auto col = host.find(':'); col != std::string::npos)
        host = host.substr(0, col);

    if (path_start != std::string::npos) {
        size_t path_end = url.find_first_of("?#", path_start);
        if (path_end == std::string::npos) path_end = url.size();
        std::string path = url.substr(path_start, path_end - path_start);
        size_t last_slash = path.rfind('/');
        std::string fname = (last_slash == std::string::npos)
                                ? path
                                : path.substr(last_slash + 1);
        size_t dot = fname.rfind('.');
        base = (dot == std::string::npos) ? fname
                                          : fname.substr(0, dot);
    }
}

} // namespace

void MainFrame::action_download_url()
{
    // Pre-fill the dialog from the clipboard when it looks URL-ish.
    // The heuristic is deliberately loose — any scheme (`foo://`)
    // or a `www.` prefix counts — because the cost of a bad guess
    // is a one-keystroke deletion in the dialog, and the win is
    // the common copy-from-browser flow reduces to two keystrokes
    // total (`p`, Enter).
    // Try CLIPBOARD first (the Ctrl+C / Cmd+C buffer — what
    // browsers populate on copy, universally). If that doesn't
    // surface a URL, fall back to PRIMARY (the X11 / Wayland
    // highlight-to-select selection) so `select URL in terminal,
    // switch, p` works without a separate Ctrl+C step. wx's
    // UsePrimarySelection toggle is a no-op on macOS and
    // Windows, so those platforms simply evaluate the CLIPBOARD
    // branch and exit — no redundant second read.
    wxString initial;
    auto read_clipboard_url = [](wxString &out) {
        if (!wxTheClipboard->IsSupported(wxDF_UNICODETEXT) &&
            !wxTheClipboard->IsSupported(wxDF_TEXT)) {
            return;
        }
        // Check both text formats: browsers on macOS and modern
        // Windows put URLs on the clipboard as wxDF_UNICODETEXT
        // (UTF-16 on Windows, UTF-8 on Unix-ish); wxDF_TEXT is
        // the legacy ANSI format. Either is fine for
        // wxTextDataObject, which normalizes both into a wxString.
        wxTextDataObject data;
        if (!wxTheClipboard->GetData(data)) return;
        wxString cb = data.GetText();
        cb.Trim(true).Trim(false);
        if (cb.Contains(wxT("://")) || cb.StartsWith(wxT("www.")))
            out = cb;
    };
    if (wxTheClipboard->Open()) {
        read_clipboard_url(initial);
        if (initial.empty()) {
            // Round-trip through PRIMARY. UsePrimarySelection is
            // sticky on the global wxTheClipboard, so the final
            // call restores the Ctrl+C/V default for anything
            // else in the app (e.g. action_copy_link) that runs
            // later.
            wxTheClipboard->UsePrimarySelection(true);
            read_clipboard_url(initial);
            wxTheClipboard->UsePrimarySelection(false);
        }
        wxTheClipboard->Close();
    }

    wxTextEntryDialog dlg(
        this,
        wxT("URL to download. Direct media URLs (.mp3, .mp4, .pdf, …) "
            "are fetched via HTTP; anything else goes through yt-dlp."),
        wxT("Download URL"),
        initial,
        wxOK | wxCANCEL);
    // Default dialog is too narrow for real-world URLs; size-hint
    // a wider field. The text control is the dialog's own child;
    // resizing the dialog lets it grow too.
    dlg.SetSize(FromDIP(wxSize(560, -1)));
    dlg.CentreOnParent();
    if (dlg.ShowModal() != wxID_OK) return;

    std::string url = dlg.GetValue().utf8_string();
    while (!url.empty() && std::isspace((unsigned char)url.front()))
        url.erase(0, 1);
    while (!url.empty() && std::isspace((unsigned char)url.back()))
        url.pop_back();
    if (url.empty()) return;

    if (url_is_direct_download(url)) {
        std::string host, basename;
        url_host_and_basename(url, host, basename);
        // Filename: YYYYMMDD-host-basename.ext. Mirrors the
        // enclosure-download rule (date first for stable chrono
        // sort in the user's Downloads folder), with host + URL
        // basename in place of feed/entry titles we don't have.
        std::string base = format_date_compact((double)::time(nullptr));
        if (!host.empty())     base += "-" + sanitize_filename(host);
        if (!basename.empty()) base += "-" + sanitize_filename(basename);
        std::string ext = mime_to_extension("", url);
        std::string dir = app_->download_dir.empty()
                              ? user_home_dir() + "/Downloads"
                              : app_->download_dir;
        make_directory(dir);
        std::string path = disambiguate_path(dir, base, ext);
        // Title seeded with the URL; there's no better label to
        // start with, and the Downloads panel's Name column is
        // driven by the output filename once the download begins.
        download_enqueue_http(app_, url, url, path);
    } else {
        // Title = URL for the same reason; yt-dlp will later emit
        // a real filename we surface in the Name column as soon
        // as its progress output begins.
        download_enqueue(app_, url, url, /*is_video=*/true);
    }

    download_tick(app_);
    if (pane_shown("downloads") && downloads_) downloads_->refresh();
    flash_status("Download queued");
    update_status();
}
