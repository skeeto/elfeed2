#ifndef ELFEED_ACTIVITY_PANEL_HPP
#define ELFEED_ACTIVITY_PANEL_HPP

#include <wx/panel.h>

#include <cstdint>
#include <vector>

#include "elfeed.hpp"

// Bottom-dockable AUI pane that renders entry frequency over time as
// a bar chart, weekly bins. Driven by app->entries (the same entries
// the entry list shows, so it respects whatever filter is active).
class ActivityPanel : public wxPanel {
public:
    ActivityPanel(wxWindow *parent, Elfeed *app);

    // Re-bin from app->entries and repaint. Cheap (linear in
    // entry count); MainFrame calls it after every requery.
    void refresh();

private:
    void on_paint(wxPaintEvent &);
    void on_motion(wxMouseEvent &);
    void on_size(wxSizeEvent &);

    // Map a unix epoch to the unix epoch of the Monday 00:00 local
    // that starts the containing week. Local TZ so week boundaries
    // match how the user perceives reading cadence.
    static int64_t week_start_of(double epoch);

    Elfeed *app_;

    // One bin per week with at least one entry; sorted by week_start.
    struct Bin {
        int64_t week_start;
        int     count;
    };
    std::vector<Bin> bins_;
    int max_count_ = 0;
};

#endif
