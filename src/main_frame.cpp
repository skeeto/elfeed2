#include "main_frame.hpp"

#include "downloads_frame.hpp"
#include "entry_detail.hpp"
#include "entry_list.hpp"
#include "events.hpp"
#include "log_frame.hpp"

#include <wx/accel.h>
#include <wx/clipbrd.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/srchctrl.h>
#include <wx/sizer.h>
#include <wx/splitter.h>
#include <wx/statusbr.h>
#include <wx/timer.h>
#include <wx/utils.h>

#include <algorithm>

enum {
    ID_Fetch = wxID_HIGHEST + 1,
    ID_ToggleLog,
    ID_ToggleDownloads,
    ID_MarkRead,
    ID_MarkUnread,
    ID_OpenBrowser,
    ID_CopyLink,
    ID_Download,
    ID_FocusFilter,
    ID_DownloadTimer,
};

MainFrame::MainFrame(Elfeed *app)
    : wxFrame(nullptr, wxID_ANY, "elfeed2",
              wxDefaultPosition, wxDefaultSize)
    , app_(app)
{
    app_->event_sink = this;

    SetClientSize(FromDIP(wxSize(1200, 800)));
    Centre();

    build_menus();
    build_widgets();
    bind_events();
    install_accelerators();

    CreateStatusBar(1);

    // Initial query + UI state
    requery();
    update_status();
    update_menu_checks();

    // Log / Downloads frames — created on demand, but if persistent UI
    // state says they were open, show them.
    if (app_->show_log) {
        log_frame_ = new LogFrame(this, app_);
        log_frame_->Show();
    }
    if (app_->show_downloads) {
        downloads_frame_ = new DownloadsFrame(this, app_);
        downloads_frame_->Show();
    }

    // Poll the download queue so download_tick() runs (starts the next
    // queued download once the active one exits). The actual download
    // progress is pushed via app_wake_ui from the download worker.
    download_timer_ = new wxTimer(this, ID_DownloadTimer);
    download_timer_->Start(250);
}

MainFrame::~MainFrame()
{
    if (download_timer_) {
        download_timer_->Stop();
        delete download_timer_;
    }
    app_->event_sink = nullptr;
}

// ---- Building ------------------------------------------------------

void MainFrame::build_menus()
{
    auto *mbar = new wxMenuBar;

    auto *m_elfeed = new wxMenu;
    m_elfeed->Append(ID_Fetch, "&Fetch All\tF5");
    m_elfeed->AppendSeparator();
    m_elfeed->Append(wxID_EXIT, "&Quit\tCtrl+Q");
    mbar->Append(m_elfeed, "&Elfeed");

    auto *m_view = new wxMenu;
    menu_log_id_ = m_view->AppendCheckItem(ID_ToggleLog, "&Log")->GetId();
    menu_downloads_id_ =
        m_view->AppendCheckItem(ID_ToggleDownloads, "&Downloads")->GetId();
    mbar->Append(m_view, "&View");

    auto *m_help = new wxMenu;
    m_help->Append(wxID_ABOUT, "&About elfeed2");
    mbar->Append(m_help, "&Help");

    SetMenuBar(mbar);
}

void MainFrame::build_widgets()
{
    auto *panel = new wxPanel(this);
    auto *vsz = new wxBoxSizer(wxVERTICAL);

    filter_ = new wxSearchCtrl(panel, wxID_ANY,
                               wxString::FromUTF8(app_->default_filter),
                               wxDefaultPosition, wxDefaultSize,
                               wxTE_PROCESS_ENTER);
    filter_->ShowSearchButton(false);
    filter_->ShowCancelButton(true);
    vsz->Add(filter_, 0, wxEXPAND | wxALL, FromDIP(4));

    splitter_ = new wxSplitterWindow(panel, wxID_ANY,
                                     wxDefaultPosition, wxDefaultSize,
                                     wxSP_LIVE_UPDATE | wxSP_THIN_SASH);
    splitter_->SetMinimumPaneSize(FromDIP(100));

    list_ = new EntryList(splitter_, app_);
    detail_ = new EntryDetail(splitter_, app_);
    splitter_->SplitHorizontally(list_, detail_, FromDIP(350));
    splitter_->SetSashGravity(0.55);
    vsz->Add(splitter_, 1, wxEXPAND);

    panel->SetSizer(vsz);

    auto *outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(panel, 1, wxEXPAND);
    SetSizer(outer);
}

void MainFrame::bind_events()
{
    Bind(wxEVT_ELFEED_WAKE, &MainFrame::on_wake, this);
    Bind(wxEVT_MENU, &MainFrame::on_fetch_all, this, ID_Fetch);
    Bind(wxEVT_MENU, &MainFrame::on_toggle_log, this, ID_ToggleLog);
    Bind(wxEVT_MENU, &MainFrame::on_toggle_downloads, this,
         ID_ToggleDownloads);
    Bind(wxEVT_MENU, &MainFrame::on_about, this, wxID_ABOUT);
    Bind(wxEVT_MENU, &MainFrame::on_quit, this, wxID_EXIT);
    Bind(wxEVT_CLOSE_WINDOW, &MainFrame::on_close, this);

    Bind(wxEVT_MENU, [this](wxCommandEvent &) { action_mark_read(); },
         ID_MarkRead);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { action_mark_unread(); },
         ID_MarkUnread);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { action_open_in_browser(); },
         ID_OpenBrowser);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { action_copy_link(); },
         ID_CopyLink);
    Bind(wxEVT_MENU, [this](wxCommandEvent &) { action_download(); },
         ID_Download);
    Bind(wxEVT_MENU,
         [this](wxCommandEvent &) { filter_->SetFocus(); filter_->SelectAll(); },
         ID_FocusFilter);

    filter_->Bind(wxEVT_TEXT, &MainFrame::on_filter_text, this);
    filter_->Bind(wxEVT_SEARCH_CANCEL, [this](wxCommandEvent &) {
        filter_->Clear();
    });
    filter_->Bind(wxEVT_CHAR_HOOK, &MainFrame::on_filter_key, this);

    list_->Bind(wxEVT_LIST_ITEM_SELECTED,
                &MainFrame::on_list_selected, this);
    list_->Bind(wxEVT_LIST_ITEM_ACTIVATED,
                &MainFrame::on_list_activated, this);
    list_->Bind(wxEVT_CHAR_HOOK, &MainFrame::on_list_key, this);

    Bind(wxEVT_TIMER, &MainFrame::on_download_tick, this, ID_DownloadTimer);
}

void MainFrame::install_accelerators()
{
    // Menu shortcuts for vi-like keys. wxEVT_CHAR_HOOK on the list
    // handles 'j'/'k'/etc. that collide with incremental list search,
    // but we still wire them into the menu so they show in menus.
    wxAcceleratorEntry entries[] = {
        { wxACCEL_NORMAL, (int)'U', ID_MarkUnread },
        { wxACCEL_NORMAL, (int)'R', ID_MarkRead },
        { wxACCEL_NORMAL, (int)'B', ID_OpenBrowser },
        { wxACCEL_NORMAL, (int)'Y', ID_CopyLink },
        { wxACCEL_NORMAL, (int)'D', ID_Download },
        { wxACCEL_NORMAL, (int)'F', ID_Fetch },
        { wxACCEL_NORMAL, (int)'S', ID_FocusFilter },
        { wxACCEL_NORMAL, (int)'/', ID_FocusFilter },
    };
    SetAcceleratorTable(
        wxAcceleratorTable(sizeof(entries)/sizeof(entries[0]), entries));
}

// ---- Data plumbing -------------------------------------------------

void MainFrame::requery()
{
    // Preserve selection across re-query: save the (namespace,id) of the
    // focused item, then search for it in the new entries.
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
        list_->SetItemState(new_primary,
                            wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                            wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
        list_->EnsureVisible(new_primary);
    }

    // Refresh detail pane from the new primary
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

    wxString msg;
    if (active > 0)
        msg = wxString::Format("Fetching %d/%d …  %d unread of %d  ·  %d feeds",
                               active, total, unread,
                               (int)app_->entries.size(),
                               (int)app_->feeds.size());
    else
        msg = wxString::Format("%d unread of %d  ·  %d feeds",
                               unread, (int)app_->entries.size(),
                               (int)app_->feeds.size());
    SetStatusText(msg);
}

void MainFrame::update_menu_checks()
{
    auto *mbar = GetMenuBar();
    if (!mbar) return;
    mbar->Check(menu_log_id_, app_->show_log);
    mbar->Check(menu_downloads_id_, app_->show_downloads);
}

// ---- Event handlers ------------------------------------------------

void MainFrame::on_wake(wxThreadEvent &)
{
    fetch_process_results(app_);
    // Some results may have arrived — requery keeps the list current.
    // Only do a full requery if fetch_inbox had meaningful content;
    // cheap heuristic: always requery (db_query_entries is fast).
    requery();
    update_status();
    if (log_frame_ && log_frame_->IsShown()) log_frame_->refresh();
    if (downloads_frame_ && downloads_frame_->IsShown())
        downloads_frame_->refresh();
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

void MainFrame::on_toggle_log(wxCommandEvent &)
{
    if (!log_frame_) log_frame_ = new LogFrame(this, app_);
    if (log_frame_->IsShown()) {
        log_frame_->Hide();
        app_->show_log = false;
    } else {
        log_frame_->Show();
        log_frame_->refresh();
        app_->show_log = true;
    }
    update_menu_checks();
}

void MainFrame::on_toggle_downloads(wxCommandEvent &)
{
    if (!downloads_frame_)
        downloads_frame_ = new DownloadsFrame(this, app_);
    if (downloads_frame_->IsShown()) {
        downloads_frame_->Hide();
        app_->show_downloads = false;
    } else {
        downloads_frame_->Show();
        downloads_frame_->refresh();
        app_->show_downloads = true;
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

void MainFrame::on_close(wxCloseEvent &)
{
    if (log_frame_) log_frame_->Destroy();
    if (downloads_frame_) downloads_frame_->Destroy();
    Destroy();
}

void MainFrame::on_list_selected(wxListEvent &e)
{
    long idx = e.GetIndex();
    if (idx >= 0 && (size_t)idx < app_->entries.size())
        detail_->show_entry(&app_->entries[(size_t)idx]);
}

void MainFrame::on_list_activated(wxListEvent &)
{
    // Double-click or Enter: mark read + advance (same as 'r')
    action_mark_read();
}

void MainFrame::on_list_key(wxKeyEvent &e)
{
    int code = e.GetKeyCode();
    // Only handle unmodified presses; otherwise let accelerators /
    // menus handle it.
    if (e.HasAnyModifiers()) { e.Skip(); return; }

    switch (code) {
    case 'J': {
        long p = list_->primary();
        long n = (long)app_->entries.size();
        if (p + 1 < n) {
            list_->SetItemState(p, 0,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            list_->SetItemState(p + 1,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            list_->EnsureVisible(p + 1);
        }
        return;
    }
    case 'K': {
        long p = list_->primary();
        if (p > 0) {
            list_->SetItemState(p, 0,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            list_->SetItemState(p - 1,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            list_->EnsureVisible(p - 1);
        }
        return;
    }
    case 'G':
        if (e.ShiftDown()) {
            long n = (long)app_->entries.size();
            if (n > 0) {
                list_->SetItemState(n - 1,
                                    wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                    wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
                list_->EnsureVisible(n - 1);
            }
        } else {
            list_->SetItemState(0,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            list_->EnsureVisible(0);
        }
        return;
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
    e.Skip();
}

void MainFrame::on_download_tick(wxTimerEvent &)
{
    download_tick(app_);
    if (downloads_frame_ && downloads_frame_->IsShown())
        downloads_frame_->refresh();
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
    }
    list_->refresh_items();
    // Advance if single-selection
    if (sel.size() == 1) {
        long next = sel[0] + 1;
        long n = (long)app_->entries.size();
        if (next >= n) next = n - 1;
        if (next >= 0) {
            list_->SetItemState(sel[0], 0,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            list_->SetItemState(next,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            list_->EnsureVisible(next);
            detail_->show_entry(&app_->entries[(size_t)next]);
        }
    }
    update_status();
}

void MainFrame::action_mark_unread()
{
    auto sel = list_->selection();
    if (sel.empty()) return;
    for (long i : sel) {
        if (i < 0 || (size_t)i >= app_->entries.size()) continue;
        add_unread(app_->entries[(size_t)i], app_);
    }
    list_->refresh_items();
    if (sel.size() == 1) {
        long next = sel[0] + 1;
        long n = (long)app_->entries.size();
        if (next >= n) next = n - 1;
        if (next >= 0) {
            list_->SetItemState(sel[0], 0,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            list_->SetItemState(next,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            list_->EnsureVisible(next);
            detail_->show_entry(&app_->entries[(size_t)next]);
        }
    }
    update_status();
}

void MainFrame::action_open_in_browser()
{
    auto sel = list_->selection();
    for (long i : sel) {
        if (i < 0 || (size_t)i >= app_->entries.size()) continue;
        const std::string &link = app_->entries[(size_t)i].link;
        if (!link.empty())
            wxLaunchDefaultBrowser(wxString::FromUTF8(link));
    }
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
        for (auto &enc : e.enclosures) {
            bool video = enc.type.find("video") != std::string::npos ||
                enc.url.find("youtube") != std::string::npos ||
                enc.url.find("youtu.be") != std::string::npos;
            download_enqueue(app_, enc.url, e.title, video);
        }
        if (e.enclosures.empty())
            download_enqueue(app_, e.link, e.title, true);
        strip_unread(e, app_);
    }
    list_->refresh_items();
    if (downloads_frame_ && downloads_frame_->IsShown())
        downloads_frame_->refresh();
    // Advance like mark_read
    if (sel.size() == 1) {
        long next = sel[0] + 1;
        long n = (long)app_->entries.size();
        if (next >= n) next = n - 1;
        if (next >= 0) {
            list_->SetItemState(sel[0], 0,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            list_->SetItemState(next,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                                wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
            list_->EnsureVisible(next);
            detail_->show_entry(&app_->entries[(size_t)next]);
        }
    }
    update_status();
}
