#ifndef ELFEED_MAIN_FRAME_HPP
#define ELFEED_MAIN_FRAME_HPP

#include <wx/frame.h>
#include "elfeed.hpp"

class EntryList;
class EntryDetail;
class LogFrame;
class DownloadsFrame;
class wxSearchCtrl;
class wxSplitterWindow;
class wxTimer;
class wxListEvent;
class wxTimerEvent;

class MainFrame : public wxFrame {
public:
    explicit MainFrame(Elfeed *app);
    ~MainFrame() override;

private:
    // ---- Building ----
    void build_menus();
    void build_widgets();
    void bind_events();

    // ---- Data plumbing ----
    void requery();          // re-run db_query_entries and refresh list
    void update_status();    // update status bar text
    void update_menu_checks();

    // ---- Selection helpers ----
    void move_selection(int delta); // single-select navigation (j/k)
    void go_to(long row);           // absolute jump (g/G)
    void advance_from(long row);    // after u/r/d: move down one

    // ---- Events ----
    void on_wake(wxThreadEvent &);
    void on_filter_text(wxCommandEvent &);
    void on_fetch_all(wxCommandEvent &);
    void on_toggle_log(wxCommandEvent &);
    void on_toggle_downloads(wxCommandEvent &);
    void on_about(wxCommandEvent &);
    void on_quit(wxCommandEvent &);
    void on_close(wxCloseEvent &);
    void on_list_selected(wxListEvent &);
    void on_list_activated(wxListEvent &);   // double-click / Enter
    void on_list_key(wxKeyEvent &);
    void on_filter_key(wxKeyEvent &);

    // ---- Actions on selection ----
    void action_mark_read();
    void action_mark_unread();
    void action_open_in_browser();
    void action_copy_link();
    void action_download();

    Elfeed *app_;
    wxSearchCtrl *filter_ = nullptr;
    wxSplitterWindow *splitter_ = nullptr;
    EntryList *list_ = nullptr;
    EntryDetail *detail_ = nullptr;
    LogFrame *log_frame_ = nullptr;
    DownloadsFrame *downloads_frame_ = nullptr;

    int menu_log_id_ = 0;
    int menu_downloads_id_ = 0;
};

#endif
