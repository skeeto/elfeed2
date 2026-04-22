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

    // Persist current column widths/visibility into the DB.
    void save_columns();

    // Restore construction-time columns + clear sort, then refresh.
    void reset_layout();

private:
    void on_filter_changed(wxCommandEvent &);
    void on_clear(wxCommandEvent &);
    void on_sort(wxDataViewEvent &);
    void on_context_menu(wxDataViewEvent &);

    // Sort snapshot_ in place per the current column header state.
    // Default (no sort key) is insertion order, i.e. chronological.
    void apply_sort();

    Elfeed *app_;
    wxCheckBox *cb_info_ = nullptr;
    wxCheckBox *cb_req_ = nullptr;
    wxCheckBox *cb_ok_ = nullptr;
    wxCheckBox *cb_err_ = nullptr;
    wxCheckBox *cb_autoscroll_ = nullptr;
    wxDataViewCtrl *list_ = nullptr;
    wxObjectDataPtr<LogListModel> model_;

    void append_column(const wxString &title);
    void build_columns(const std::vector<std::string> &order);

    friend class LogListModel;
    std::vector<LogEntry> snapshot_;
    std::vector<std::string> default_order_;
};

#endif
