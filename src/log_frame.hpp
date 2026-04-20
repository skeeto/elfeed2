#ifndef ELFEED_LOG_FRAME_HPP
#define ELFEED_LOG_FRAME_HPP

#include <wx/frame.h>
#include "elfeed.hpp"

class wxCheckBox;
class LogList;  // defined in log_frame.cpp

// Toplevel window showing the fetch/download log. 4 columns: Time,
// Type, URL, Result. Filter checkboxes at top hide message kinds.
class LogFrame : public wxFrame {
public:
    LogFrame(wxWindow *parent, Elfeed *app);

    // Snapshot filtered log entries and re-size the list. Safe on UI
    // thread; the log mutex is taken internally.
    void refresh();

private:
    void on_filter_changed(wxCommandEvent &);
    void on_clear(wxCommandEvent &);
    void on_close(wxCloseEvent &);

    Elfeed *app_;
    wxCheckBox *cb_info_ = nullptr;
    wxCheckBox *cb_req_ = nullptr;
    wxCheckBox *cb_ok_ = nullptr;
    wxCheckBox *cb_err_ = nullptr;
    wxCheckBox *cb_autoscroll_ = nullptr;
    LogList *list_ = nullptr;

    friend class LogList;
    // Filtered snapshot that the virtual LogList reads.
    std::vector<LogEntry> snapshot_;
};

#endif
