#ifndef ELFEED_ACTIVITY_PANEL_HPP
#define ELFEED_ACTIVITY_PANEL_HPP

#include <wx/panel.h>

#include <cstdint>
#include <map>

#include "elfeed.hpp"

// Bottom-dockable AUI pane rendering entry cadence as a
// GitHub-contributions-style heatmap: one 53-column × 7-row grid
// per calendar year, one cell per day, color intensity scaled
// from the entry count. Years stack top (most recent, partial
// through today) to bottom (oldest). Driven by app->entries, so
// the view respects whatever filter is active.
class ActivityPanel : public wxPanel {
public:
    ActivityPanel(wxWindow *parent, Elfeed *app);

    // Re-bin from app->entries and repaint. Linear in entry count.
    // Called after every requery and on wake ticks while the pane
    // is visible.
    void refresh();

private:
    void on_paint(wxPaintEvent &);
    void on_motion(wxMouseEvent &);
    void on_size(wxSizeEvent &);

    // Unix epoch of local midnight for the given timestamp. Local
    // TZ so cells align to how the user perceives days.
    static int64_t day_start_of(double epoch);

    Elfeed *app_;

    // Local-midnight epoch → entry count. Only days with ≥1 entry
    // are present; the renderer synthesizes empty cells.
    std::map<int64_t, int> counts_;
    int max_count_ = 0;
};

#endif
