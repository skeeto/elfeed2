#ifndef ELFEED_LOG_PANEL_HPP
#define ELFEED_LOG_PANEL_HPP

#include <wx/dataview.h>
#include <wx/panel.h>
#include "elfeed.hpp"

class wxCheckBox;
class LogListModel;

// Fetch/download log as a wxAUI-dockable panel. Four columns: Time,
// Type, URL, Result. Filter checkboxes at top hide message kinds.
// Backed by wxDataViewCtrl so the column-visibility menu and reorder
// drag come for free.
class LogPanel : public wxPanel {
public:
    LogPanel(wxWindow *parent, Elfeed *app);

    // Snapshot filtered log entries and re-size the list. UI-thread
    // only; takes app->log_mutex internally.
    void refresh();

private:
    void on_filter_changed(wxCommandEvent &);
    void on_clear(wxCommandEvent &);

    Elfeed *app_;
    wxCheckBox *cb_info_ = nullptr;
    wxCheckBox *cb_req_ = nullptr;
    wxCheckBox *cb_ok_ = nullptr;
    wxCheckBox *cb_err_ = nullptr;
    wxCheckBox *cb_autoscroll_ = nullptr;
    wxDataViewCtrl *list_ = nullptr;
    wxObjectDataPtr<LogListModel> model_;

    friend class LogListModel;
    std::vector<LogEntry> snapshot_;
};

#endif
