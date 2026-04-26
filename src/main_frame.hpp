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
class ActivityPanel;
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

    // Try to dispatch `e` as a single-letter filter preset (from
    // the `preset` config directive). Returns true if matched and
    // applied — caller should not call e.Skip(). Returns false if
    // the key isn't a preset trigger; caller should fall through.
    // Used by panels other than the entry list (FeedsPanel,
    // EntryDetail) so preset keys work even when their focus is
    // elsewhere in the app.
    bool try_preset_key(wxKeyEvent &e);

    // EntryDetail's wxEVT_CHAR_HOOK forwards to this. Handles the
    // reader-mode bindings (q/Escape return to list; n/p step
    // between entries; b/y/d/u act on the selection like in the
    // listing). Unhandled keys fall through to try_preset_key and
    // then e.Skip() so wxHtmlWindow's own navigation (arrow keys
    // for scrolling, etc.) still works.
    void on_detail_key(wxKeyEvent &e);

private:
    // ---- Building ----
    void build_menus();
    void build_widgets();
    void bind_events();

    // ---- Data plumbing ----
    // `default_limit > 0` caps result rows; used by the debounced
    // typing path to bound live-filter work to one viewport. Other
    // paths (import, Ctrl+L, fetch-arrival, blur) pass 0 for a
    // full-fidelity requery.
    void requery(int default_limit = 0);
    void update_status();
    void update_menu_checks();

    // Apply `text` to the filter bar, re-run the query, and persist
    // the filter to the DB. Used by preset keys, feed-row double-click,
    // and on-blur.
    void apply_filter(const std::string &text);

    // Re-parse whatever's currently in the filter bar and requery.
    // `capped=true` (the debounce-timer path) applies the viewport
    // row cap for live-typing responsiveness; `capped=false` (blur
    // path) runs a full-fidelity query so leaving the filter bar
    // always shows the complete match set.
    void commit_filter(bool capped = false);

    // ---- Selection helpers ----
    void move_selection(int delta);
    void go_to(long row);
    void advance_from(long row);
    // Detail-pane n/p equivalent: move the list's primary row by
    // `delta` and explicitly repaint the detail pane. Used from
    // on_detail_key where focus is on the preview (so the normal
    // selection-changed chain that j/k relies on isn't guaranteed).
    void step_entry(int delta);

    // ---- Pane helpers ----
    void toggle_pane(const char *name);
    bool pane_shown(const char *name) const;
    // Reset the four side/bottom panes' MinSize to a small floor so
    // the user can drag any pane narrower than its construction
    // default. Must be called after every LoadPerspective (initial
    // load and Reset Layout), since the perspective string carries
    // MinSize and re-applying it would put the floors back.
    void loosen_pane_min_sizes();

    // ---- Events ----
    void on_wake(wxThreadEvent &);
    void on_frame_move_size(wxEvent &);
    void on_filter_text(wxCommandEvent &);
    void on_fetch_all(wxCommandEvent &);
    void on_reload_config(wxCommandEvent &);
    void on_import_classic(wxCommandEvent &);
    void do_import_classic();  // deferred via CallAfter
    void on_reclaim_space(wxCommandEvent &);
    void on_toggle_feeds(wxCommandEvent &);
    void on_toggle_preview(wxCommandEvent &);
    void on_toggle_log(wxCommandEvent &);
    void on_toggle_downloads(wxCommandEvent &);
    void on_toggle_activity(wxCommandEvent &);
    void on_reset_layout(wxCommandEvent &);
    void on_about(wxCommandEvent &);
    void on_quit(wxCommandEvent &);
    void on_close(wxCloseEvent &);
    void on_list_selected(wxDataViewEvent &);
    void on_list_activated(wxDataViewEvent &);
    // Drive a preview-pane refresh from whatever's currently the
    // primary row. Bound to wxEVT_DATAVIEW_SELECTION_CHANGED via
    // on_list_selected, but also called explicitly from
    // move_selection / go_to because programmatic Select() doesn't
    // fire that event on Windows or Linux (only on macOS).
    void sync_preview();
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
    // Prompt for a URL (pre-filled from clipboard when URL-shaped)
    // and enqueue it in the Downloads panel. Routes URLs with a
    // known media/document extension to the HTTP-direct path with a
    // generated filename; otherwise hands off to yt-dlp just like
    // the no-enclosure entry-download path.
    void action_download_url();

    Elfeed *app_;
    wxSearchCtrl *filter_ = nullptr;
    wxAuiManager mgr_;
    EntryList *list_ = nullptr;
    EntryDetail *detail_ = nullptr;
    FeedsPanel *feeds_ = nullptr;
    LogPanel *log_ = nullptr;
    DownloadsPanel *downloads_ = nullptr;
    ActivityPanel *activity_ = nullptr;

    int menu_feeds_id_ = 0;
    int menu_preview_id_ = 0;
    int menu_log_id_ = 0;
    int menu_downloads_id_ = 0;
    int menu_activity_id_ = 0;

    // -1 when not in visual-selection mode. Otherwise the row that
    // was focused when the user pressed `v`: subsequent j/k/g/G
    // keep extending the list selection between this anchor and the
    // new cursor position. Cleared by Escape, by any row-acting
    // action (mark read/unread/open/copy/download), and by requery
    // (which invalidates row indices).
    long visual_anchor_ = -1;

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

    // Coalesces log_drain_to_db calls. Without it, every UI wake
    // (and there are many during a fetch storm) does its own
    // fsync-on-commit transaction. Firing every 5s keeps disk I/O
    // bounded; final drain happens at on_close so we don't lose
    // recent entries on a clean exit.
    wxTimer log_drain_timer_;

    // Debounces the filter bar's text-changed event so requery's
    // DB scan (potentially thousands of rows × 3 sub-queries for
    // tag/author/enclosure on a large imported DB) doesn't fire
    // per keystroke. Starts on every edit; the real commit runs
    // only when the user pauses typing for ~180 ms. Enter / Escape
    // / focus-out flush the pending commit immediately.
    wxTimer filter_debounce_;
};

#endif
