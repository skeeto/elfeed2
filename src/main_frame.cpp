#include "main_frame.hpp"

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
#include <wx/display.h>
#include <wx/filedlg.h>
#include <wx/menu.h>
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
    ID_ToggleFeeds,
    ID_TogglePreview,
    ID_ToggleLog,
    ID_ToggleDownloads,
    ID_ResetLayout,
};

MainFrame::MainFrame(Elfeed *app)
    : wxFrame(nullptr, wxID_ANY, "elfeed2",
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

    build_menus();
    build_widgets();
    bind_events();

    CreateStatusBar(1);

    requery();
    update_status();
    update_menu_checks();

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
    m_elfeed->Append(ID_Fetch, "&Fetch All\tF5");
    m_elfeed->Append(ID_ReloadConfig, "&Reload Config\tCtrl+Shift+R");
    m_elfeed->AppendSeparator();
    m_elfeed->Append(ID_ImportClassic, wxT("&Import Classic Elfeed Index…"));
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
    // wxAUI ignores wxAuiPaneInfo::BestSize for layered side docks; it
    // uses MinSize as the floor and the contained panel's intrinsic
    // best size otherwise. The only knob that reliably pins the
    // initial dock dimensions is MinSize itself, so we set it to the
    // desired width/height. This means users can't drag a pane
    // narrower than the default — the workaround is to hide the pane
    // entirely via the View menu or the column-header context menu.
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

    // Run an initial Update with just the construction-time defaults so
    // wxAUI computes the dock_size entries we want. Snapshot the
    // resulting perspective for "Reset Layout" before any saved
    // perspective is applied on top.
    mgr_.Update();
    default_perspective_ = mgr_.SavePerspective();

    std::string saved = db_load_ui_state(app_, "layout");
    if (!saved.empty()) {
        mgr_.LoadPerspective(wxString::FromUTF8(saved), false);
        mgr_.Update();
    }
}

void MainFrame::bind_events()
{
    Bind(wxEVT_ELFEED_WAKE, &MainFrame::on_wake, this);
    Bind(wxEVT_MENU, &MainFrame::on_fetch_all,        this, ID_Fetch);
    Bind(wxEVT_MENU, &MainFrame::on_reload_config,    this, ID_ReloadConfig);
    Bind(wxEVT_MENU, &MainFrame::on_import_classic,   this, ID_ImportClassic);
    Bind(wxEVT_MENU, &MainFrame::on_toggle_feeds,     this, ID_ToggleFeeds);
    Bind(wxEVT_MENU, &MainFrame::on_toggle_preview,   this, ID_TogglePreview);
    Bind(wxEVT_MENU, &MainFrame::on_toggle_log,       this, ID_ToggleLog);
    Bind(wxEVT_MENU, &MainFrame::on_toggle_downloads, this, ID_ToggleDownloads);
    Bind(wxEVT_MENU, &MainFrame::on_reset_layout,     this, ID_ResetLayout);
    Bind(wxEVT_MENU, &MainFrame::on_about, this, wxID_ABOUT);
    Bind(wxEVT_MENU, &MainFrame::on_quit,  this, wxID_EXIT);
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::on_close, this);
    Bind(wxEVT_AUI_PANE_CLOSE, &MainFrame::on_pane_close, this);
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
        db_save_ui_state(app_, "filter",
                         filter_->GetValue().utf8_string().c_str());
        e.Skip();
    });

    list_->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED,
                &MainFrame::on_list_selected, this);
    list_->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED,
                &MainFrame::on_list_activated, this);
    list_->Bind(wxEVT_CHAR_HOOK, &MainFrame::on_list_key, this);
}

// ---- Data plumbing -------------------------------------------------

void MainFrame::requery()
{
    std::string sel_ns, sel_id;
    long primary = list_->primary();
    if (primary >= 0 && (size_t)primary < app_->entries.size()) {
        sel_ns = app_->entries[(size_t)primary].namespace_;
        sel_id = app_->entries[(size_t)primary].id;
    }

    db_query_entries(app_, app_->current_filter, app_->entries);
    list_->refresh_items();

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

    if (new_primary >= 0 && (size_t)new_primary < app_->entries.size())
        detail_->show_entry(&app_->entries[(size_t)new_primary]);
    else
        detail_->show_entry(nullptr);
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

    // wxT() for format strings containing non-ASCII (… and ·): narrow
    // literals route through wxConvLibc, which on Windows is CP1252
    // and mangles UTF-8 bytes. Wide literals skip that path entirely.
    wxString msg;
    if (active > 0)
        msg = wxString::Format(
            wxT("Fetching %d/%d …  %d unread of %d  ·  %d feeds"),
            active, total, unread,
            (int)app_->entries.size(),
            (int)app_->feeds.size());
    else
        msg = wxString::Format(
            wxT("%d unread of %d  ·  %d feeds"),
            unread, (int)app_->entries.size(),
            (int)app_->feeds.size());
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
    }
    // Drain the inline-image inbox: worker threads push downloaded
    // bytes, we write them to the cache table, and if anything
    // landed, re-render the preview pane so the new data: URIs
    // replace the broken <img> boxes.
    if (image_cache_process_results(app_) && detail_)
        detail_->relayout();
    update_status();
    download_tick(app_);
    if (pane_shown("log")       && log_)       log_->refresh();
    if (pane_shown("downloads") && downloads_) downloads_->refresh();
}

void MainFrame::on_filter_text(wxCommandEvent &)
{
    std::string text = filter_->GetValue().utf8_string();
    app_->current_filter = filter_parse(text);
    requery();
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

void MainFrame::on_toggle_feeds(wxCommandEvent &)     { toggle_pane("feeds"); }
void MainFrame::on_toggle_preview(wxCommandEvent &)   { toggle_pane("entry_detail"); }
void MainFrame::on_toggle_log(wxCommandEvent &)       { toggle_pane("log"); }
void MainFrame::on_toggle_downloads(wxCommandEvent &) { toggle_pane("downloads"); }

void MainFrame::on_reset_layout(wxCommandEvent &)
{
    // Reset every "layout" concern: AUI perspective, per-panel
    // column widths/visibility, per-panel sort state. Current
    // working state (filter, selection, window geometry) is
    // deliberately preserved — those aren't layout per se.
    if (!default_perspective_.empty()) {
        mgr_.LoadPerspective(default_perspective_, false);
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
    wxMessageBox("elfeed2 " ELFEED_VERSION "\n\nA feed reader.",
                 "About elfeed2", wxOK | wxICON_INFORMATION, this);
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

    mgr_.UnInit();
    Destroy();
}

void MainFrame::on_pane_close(wxAuiManagerEvent &e)
{
    // wxAUI just closed a pane via its X button. Our menu check marks
    // still reflect the pre-close state; refresh them on the next idle
    // tick (after the pane info has actually flipped).
    (void)e;
    CallAfter([this] { update_menu_checks(); });
}

void MainFrame::on_list_selected(wxDataViewEvent &)
{
    long p = list_->primary();
    if (p >= 0 && (size_t)p < app_->entries.size())
        detail_->show_entry(&app_->entries[(size_t)p]);
}

void MainFrame::on_list_activated(wxDataViewEvent &)
{
    action_mark_read();
}

void MainFrame::on_list_key(wxKeyEvent &e)
{
    int code = e.GetKeyCode();
    bool plain = !e.HasAnyModifiers();
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
    case 'D': if (plain) { action_download();         return; } break;
    case 'F': if (plain) { fetch_all(app_); update_status(); return; } break;
    case 'S':
    case '/':
        if (plain) {
            filter_->SetFocus();
            filter_->SetInsertionPointEnd();
            return;
        }
        break;
    }

    // User-defined filter presets bound to single keys via the `preset`
    // config directive. Built-in keys above take precedence, so users
    // can't override j/k/u/r/etc. — but any other printable ASCII
    // (letters, digits, punctuation) is fair game. Shift turns e.g.
    // 'h' into 'H', so both case variants can be bound independently.
    if (!e.ControlDown() && !e.AltDown() && !e.MetaDown() &&
        code >= 0x20 && code < 0x7F) {
        char c = (char)code;
        // wxKeyEvent reports letter codes in uppercase; map to lowercase
        // when Shift isn't held so `preset h` matches a plain `h` press.
        if (!e.ShiftDown() && c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        auto it = app_->presets.find(c);
        if (it != app_->presets.end()) {
            apply_filter(it->second);
            return;
        }
    }

    e.Skip();
}

void MainFrame::on_filter_key(wxKeyEvent &e)
{
    int code = e.GetKeyCode();
    if (code == WXK_ESCAPE || code == WXK_RETURN || code == WXK_NUMPAD_ENTER) {
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
    long p = list_->primary();
    long n = (long)app_->entries.size();
    if (p < 0) return;
    long target = p + delta;
    if (target < 0 || target >= n) return;
    list_->select_only(target);
    list_->ensure_visible_row(target);
}

void MainFrame::go_to(long row)
{
    long n = (long)app_->entries.size();
    if (row < 0 || row >= n) return;
    list_->select_only(row);
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
    detail_->show_entry(&app_->entries[(size_t)next]);
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
    update_status();
}

void MainFrame::action_copy_link()
{
    auto sel = list_->selection();
    std::string urls;
    for (long i : sel) {
        if (i < 0 || (size_t)i >= app_->entries.size()) continue;
        if (!urls.empty()) urls += '\n';
        urls += app_->entries[(size_t)i].link;
    }
    if (urls.empty()) return;
    if (wxTheClipboard->Open()) {
        wxTheClipboard->SetData(
            new wxTextDataObject(wxString::FromUTF8(urls)));
        wxTheClipboard->Close();
    }
}

void MainFrame::action_download()
{
    auto sel = list_->selection();
    for (long i : sel) {
        if (i < 0 || (size_t)i >= app_->entries.size()) continue;
        Entry &e = app_->entries[(size_t)i];

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
    update_status();
}
