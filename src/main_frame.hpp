#ifndef ELFEED_MAIN_FRAME_HPP
#define ELFEED_MAIN_FRAME_HPP

#include <wx/aui/framemanager.h>
#include <wx/frame.h>
#include "elfeed.hpp"

class EntryList;
class EntryDetail;
class FeedsPanel;
class LogPanel;
class DownloadsPanel;
class wxSearchCtrl;
class wxListEvent;
class wxAuiManagerEvent;

class MainFrame : public wxFrame {
public:
    explicit MainFrame(Elfeed *app);
    ~MainFrame() override;

    // Called by FeedsPanel when the user double-clicks a feed.
    // Sets the filter to `=<feed_url>` (show only that feed).
    void set_filter_to_feed(const std::string &feed_url);

private:
    // ---- Building ----
    void build_menus();
    void build_widgets();
    void bind_events();

    // ---- Data plumbing ----
    void requery();
    void update_status();
    void update_menu_checks();

    // ---- Selection helpers ----
    void move_selection(int delta);
    void go_to(long row);
    void advance_from(long row);

    // ---- Pane helpers ----
    void toggle_pane(const char *name);
    bool pane_shown(const char *name) const;

    // ---- Events ----
    void on_wake(wxThreadEvent &);
    void on_filter_text(wxCommandEvent &);
    void on_fetch_all(wxCommandEvent &);
    void on_toggle_feeds(wxCommandEvent &);
    void on_toggle_preview(wxCommandEvent &);
    void on_toggle_log(wxCommandEvent &);
    void on_toggle_downloads(wxCommandEvent &);
    void on_about(wxCommandEvent &);
    void on_quit(wxCommandEvent &);
    void on_close(wxCloseEvent &);
    void on_list_selected(wxListEvent &);
    void on_list_activated(wxListEvent &);
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
};

#endif
