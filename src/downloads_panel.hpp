#ifndef ELFEED_DOWNLOADS_PANEL_HPP
#define ELFEED_DOWNLOADS_PANEL_HPP

#include <wx/dataview.h>
#include <wx/panel.h>
#include "elfeed.hpp"

class wxButton;
class DownloadsPanelModel;

// Download queue as a wxAUI-dockable panel. Columns: %, Size, Name,
// Failures. Pause/Resume/Remove buttons act on the selected row(s).
// Backed by wxDataViewCtrl: column show/hide via right-click header,
// reorder by drag.
class DownloadsPanel : public wxPanel {
public:
    DownloadsPanel(wxWindow *parent, Elfeed *app);

    // Re-snapshot app->downloads and update the list.
    void refresh();

    // Persist current column widths/visibility into the DB.
    void save_columns();

    // Restore construction-time columns + clear sort, then refresh.
    void reset_layout();

    // The currently-displayed snapshot, used by the model.
    struct Row {
        int id;
        bool paused;
        bool active;
        std::string progress;
        std::string total;
        std::string name;
        int failures;
    };

private:
    void on_pause(wxCommandEvent &);
    void on_remove(wxCommandEvent &);
    void on_sort(wxDataViewEvent &);

    // Sort snapshot_ in place per the current column header state.
    // Called from refresh() when a non-default sort is active.
    void apply_sort();

    Elfeed *app_;
    wxDataViewCtrl *list_ = nullptr;
    wxObjectDataPtr<DownloadsPanelModel> model_;
    wxButton *btn_pause_ = nullptr;
    wxButton *btn_remove_ = nullptr;

    void append_column(const wxString &title);
    void build_columns(const std::vector<std::string> &order);

    friend class DownloadsPanelModel;
    std::vector<Row> snapshot_;
    std::vector<std::string> default_order_;
};

#endif
