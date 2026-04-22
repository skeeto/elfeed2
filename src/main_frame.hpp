#ifndef ELFEED_MAIN_FRAME_HPP
#define ELFEED_MAIN_FRAME_HPP

#include <wx/aui/framemanager.h>
#include <wx/frame.h>
#include <wx/timer.h>
#include "elfeed.hpp"

class EntryList;
class EntryDetail;
class FeedsPanel;
class LogPanel;
class DownloadsPanel;
class wxSearchCtrl;
class wxDataViewEvent;
class wxAuiManagerEvent;

class MainFrame : public wxFrame {
public:
    explicit MainFrame(Elfeed *app);
    ~MainFrame() override;

    // Called by FeedsPanel when the user double-clicks a feed.
    // Sets the filter to `=<feed_url>` (show only that feed).
    void set_filter_to_feed(const std::string &feed_url);

    // Show a transient confirmation message in the status bar (e.g.
    // "Copied to clipboard"). Restored to the normal status text
    // after a short delay so the feedback registers but doesn't
    // linger. Safe to call from any panel via dynamic_cast on
    // wxGetTopLevelParent.
    void flash_status(const wxString &msg);

private:
    // ---- Building ----
    void build_menus();
    void build_widgets();
    void bind_events();

    // ---- Data plumbing ----
    void requery();
    void update_status();
    void update_menu_checks();

    // Apply `text` to the filter bar, re-run the query, and persist
    // the filter to the DB. Used by preset keys, feed-row double-click,
    // and on-blur.
    void apply_filter(const std::string &text);

    // ---- Selection helpers ----
    void move_selection(int delta);
    void go_to(long row);
    void advance_from(long row);

    // ---- Pane helpers ----
    void toggle_pane(const char *name);
    bool pane_shown(const char *name) const;

    // ---- Events ----
    void on_wake(wxThreadEvent &);
    void on_frame_move_size(wxEvent &);
    void on_filter_text(wxCommandEvent &);
    void on_fetch_all(wxCommandEvent &);
    void on_reload_config(wxCommandEvent &);
    void on_import_classic(wxCommandEvent &);
    void do_import_classic();  // deferred via CallAfter
    void on_toggle_feeds(wxCommandEvent &);
    void on_toggle_preview(wxCommandEvent &);
    void on_toggle_log(wxCommandEvent &);
    void on_toggle_downloads(wxCommandEvent &);
    void on_reset_layout(wxCommandEvent &);
    void on_about(wxCommandEvent &);
    void on_quit(wxCommandEvent &);
    void on_close(wxCloseEvent &);
    void on_list_selected(wxDataViewEvent &);
    void on_list_activated(wxDataViewEvent &);
    void on_list_context_menu(wxDataViewEvent &);
    void on_list_key(wxKeyEvent &);
    void on_filter_key(wxKeyEvent &);
    void on_pane_close(wxAuiManagerEvent &);

    // ---- Actions ----
    void action_mark_read();
    void action_mark_unread();
    void action_open_in_browser();
    void action_copy_link();
    void action_download();

    Elfeed *app_;
    wxSearchCtrl *filter_ = nullptr;
    wxAuiManager mgr_;
    EntryList *list_ = nullptr;
    EntryDetail *detail_ = nullptr;
    FeedsPanel *feeds_ = nullptr;
    LogPanel *log_ = nullptr;
    DownloadsPanel *downloads_ = nullptr;

    int menu_feeds_id_ = 0;
    int menu_preview_id_ = 0;
    int menu_log_id_ = 0;
    int menu_downloads_id_ = 0;

    // Snapshot of the AUI perspective right after build_widgets has
    // applied the construction-time defaults but before any saved
    // perspective is loaded. Reset-layout restores this exact state.
    wxString default_perspective_;

    // Last known non-maximized / non-iconized window rect. Updated on
    // every move/resize; saved at close time so "quit while maximized,
    // relaunch, un-maximize" restores the previous floating window.
    wxRect normal_rect_;

    // Ticks the "last fetch: Nm ago" counter in the status bar when
    // nothing else is driving update_status. 60 seconds matches the
    // minute-level display resolution; past an hour the display
    // changes rarely enough that the interval is invisible.
    wxTimer status_timer_;

    // One-shot timer for flash_status: restores the regular status
    // text a couple seconds after a transient confirmation was shown.
    wxTimer flash_timer_;
};

#endif
