#ifndef ELFEED_DOWNLOADS_PANEL_HPP
#define ELFEED_DOWNLOADS_PANEL_HPP

#include <wx/panel.h>
#include "elfeed.hpp"

class wxListCtrl;
class wxButton;

// Download queue as a wxAUI-dockable panel. Columns: %, Size, Name,
// Failures. Pause/Resume/Remove buttons act on the selected row(s).
class DownloadsPanel : public wxPanel {
public:
    DownloadsPanel(wxWindow *parent, Elfeed *app);

    // Re-snapshot app_->downloads under the lock and refresh the list.
    void refresh();

private:
    void on_pause(wxCommandEvent &);
    void on_remove(wxCommandEvent &);

    Elfeed *app_;
    wxListCtrl *list_ = nullptr;
    wxButton *btn_pause_ = nullptr;
    wxButton *btn_remove_ = nullptr;

    struct Row {
        int id;
        bool paused;
        bool active;
        std::string progress;
        std::string total;
        std::string name;
        int failures;
    };
    std::vector<Row> snapshot_;
};

#endif
