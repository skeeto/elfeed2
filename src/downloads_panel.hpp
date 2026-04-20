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

    // Re-snapshot app_->downloads and update the list. Updates are
    // differential so selection and scroll position survive the
    // frequent progress-driven refreshes.
    void refresh();

private:
    struct Row {
        int id;
        bool paused;
        bool active;
        std::string progress;
        std::string total;
        std::string name;
        int failures;
    };

    void on_pause(wxCommandEvent &);
    void on_remove(wxCommandEvent &);
    // Update columns in row `row` to match `r`, SetItem-ing only the
    // columns whose text actually changed (so wx doesn't repaint the
    // whole row on every tick).
    void update_row(long row, const Row &r);

    Elfeed *app_;
    wxListCtrl *list_ = nullptr;
    wxButton *btn_pause_ = nullptr;
    wxButton *btn_remove_ = nullptr;
    std::vector<Row> snapshot_;
};

#endif
