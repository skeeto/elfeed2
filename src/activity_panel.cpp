#include "activity_panel.hpp"

#include <wx/dcbuffer.h>
#include <wx/settings.h>
#include <wx/tooltip.h>

#include <algorithm>
#include <cmath>
#include <ctime>

ActivityPanel::ActivityPanel(wxWindow *parent, Elfeed *app)
    : wxPanel(parent, wxID_ANY)
    , app_(app)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);

    Bind(wxEVT_PAINT,  &ActivityPanel::on_paint,  this);
    Bind(wxEVT_MOTION, &ActivityPanel::on_motion, this);
    Bind(wxEVT_SIZE,   &ActivityPanel::on_size,   this);

    refresh();
}

int64_t ActivityPanel::day_start_of(double epoch)
{
    time_t t = (time_t)epoch;
    struct tm tm{};
    localtime_r(&t, &tm);
    tm.tm_hour  = 0;
    tm.tm_min   = 0;
    tm.tm_sec   = 0;
    tm.tm_isdst = -1;  // renormalize across DST flips
    return (int64_t)mktime(&tm);
}

// Sunday-of-this-week minus 52 weeks, at local midnight. This is
// the anchor for column 0 of the heatmap; cell (col,row) represents
// `anchor + col*7 + row` days (row 0 = Sun, 6 = Sat). Putting Sun
// on top bookends the weekend days at the edges of the grid, which
// makes the weekly-cadence pattern easier to pick out visually.
static int64_t heatmap_anchor()
{
    time_t now = time(nullptr);
    struct tm tm{};
    localtime_r(&now, &tm);
    int dow_sun0 = tm.tm_wday;  // 0=Sun … 6=Sat
    tm.tm_mday -= dow_sun0 + 52 * 7;
    tm.tm_hour  = 0;
    tm.tm_min   = 0;
    tm.tm_sec   = 0;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

// Resolve cell (col, row) to the local-midnight epoch of the day it
// represents, given a heatmap anchor. Uses tm arithmetic so DST
// transitions don't drift the result.
static int64_t day_at(int64_t anchor, int col, int row)
{
    time_t t = (time_t)anchor;
    struct tm tm{};
    localtime_r(&t, &tm);
    tm.tm_mday += col * 7 + row;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

static wxColour lerp_colour(wxColour a, wxColour b, double t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return wxColour(
        (unsigned char)(a.Red()   + (b.Red()   - a.Red())   * t),
        (unsigned char)(a.Green() + (b.Green() - a.Green()) * t),
        (unsigned char)(a.Blue()  + (b.Blue()  - a.Blue())  * t));
}

// Discrete 0–4 level for a count, with 0 reserved for empty days.
// Scales against a robust roof (scale_max) rather than the absolute
// max: a single very-busy day shouldn't flatten the rest of the
// dataset into level 1 just because it outpaces everything else
// by 10×. Log compression inside the scaled range keeps level-1
// days visible from empty ones and gives some spread to the quiet
// half of the distribution. Floor at 1 so any nonzero count stays
// distinct from 0; cap at 4 so outliers above the scale top out
// rather than overflowing.
static int level_for(int count, int scale_max)
{
    if (count <= 0) return 0;
    if (scale_max <= 1) return 4;
    double c = (double)std::min(count, scale_max);
    double t = std::log(1.0 + c) /
               std::log(1.0 + (double)scale_max);
    int level = (int)std::lround(t * 4.0);
    if (level < 1) level = 1;
    if (level > 4) level = 4;
    return level;
}

// Five shades along bg→fg; tuned for visible contrast between
// adjacent levels even when `fg` is a muted system accent color.
static wxColour colour_for_level(wxColour bg, wxColour fg, int level)
{
    static const double ramp[5] = {0.08, 0.30, 0.55, 0.80, 1.00};
    return lerp_colour(bg, fg, ramp[level]);
}

void ActivityPanel::refresh()
{
    counts_.clear();
    scale_max_ = 0;

    // Pull *every* entry's date, unfiltered — the heatmap shows
    // reading cadence as a whole, independent of whatever filter
    // the user has active in the listing. Bound the query to the
    // last ~58 weeks (the heatmap's 53-week window rounded up so
    // we include the full Sunday-anchored leading week) to avoid
    // shipping the full history across for large DBs.
    double cutoff = (double)time(nullptr) - (int64_t)58 * 7 * 86400;
    std::vector<double> dates;
    db_entry_dates_since(app_, cutoff, dates);

    for (double d : dates) {
        if (d <= 0) continue;
        counts_[day_start_of(d)]++;
    }

    if (!counts_.empty()) {
        // Robust scale: 3× median of nonzero counts, clipped to the
        // actual max. Using the median (rather than the max itself)
        // as the basis keeps the gradient from being distorted by a
        // single high-count day. Floor at 2 so we always have at
        // least two useful levels to work with when data is uniform.
        std::vector<int> nz;
        nz.reserve(counts_.size());
        for (auto &[_, c] : counts_) nz.push_back(c);
        std::sort(nz.begin(), nz.end());
        int median = nz[nz.size() / 2];
        int max_c  = nz.back();
        scale_max_ = std::max(3 * median, 2);
        if (scale_max_ > max_c) scale_max_ = max_c;
    }

    Refresh();
}

void ActivityPanel::on_size(wxSizeEvent &e)
{
    Refresh();
    e.Skip();
}

// Layout shared between paint and hit-test.
namespace {
struct Geom {
    int cell;      // square cell size in px
    int gap;       // gap between cells
    int left_w;    // day-of-week label column width
    int top_h;     // month label strip height
    int grid_x;    // x of column 0
    int grid_y;    // y of row 0
};
}

static Geom compute_geom(wxDC &dc, const wxSize &sz)
{
    Geom g;
    g.gap    = 2;
    g.left_w = dc.GetTextExtent("Mon").x + 6;
    g.top_h  = dc.GetCharHeight() + 3;
    g.grid_x = g.left_w;
    g.grid_y = g.top_h;
    // Fit 53 columns in what's left of the width.
    int avail_w = sz.x - g.grid_x - 4;
    int cell = (avail_w - 52 * g.gap) / 53;
    if (cell < 4)  cell = 4;
    if (cell > 18) cell = 18;
    g.cell = cell;
    return g;
}

void ActivityPanel::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(this);

    wxColour bg    = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    wxColour fg    = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
    wxColour faint = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);

    wxSize sz = GetClientSize();
    dc.SetBrush(bg);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, sz.x, sz.y);

    Geom g = compute_geom(dc, sz);

    // Anchor and today's day-start; cells past today stay blank.
    int64_t anchor     = heatmap_anchor();
    int64_t today_start = day_start_of((double)time(nullptr));

    // Draw day-of-week labels (Mon, Wed, Fri) at rows 1, 3, 5.
    // With Sun at row 0 and Sat at row 6, this labels three
    // alternating weekdays, matching GitHub's two-of-three
    // convention that orients the reader without crowding rows.
    static const char *day_names[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    dc.SetTextForeground(faint);
    for (int r : {1, 3, 5}) {
        int y = g.grid_y + r * (g.cell + g.gap) +
                (g.cell - dc.GetCharHeight()) / 2;
        dc.DrawText(day_names[r], 2, y);
    }

    // Draw month labels across the top, one per column where that
    // column's Sunday (row 0) falls in a new month.
    static const char *month_names[12] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    int prev_month = -1;
    for (int col = 0; col < 53; col++) {
        int64_t col_sunday = day_at(anchor, col, 0);
        time_t tt = (time_t)col_sunday;
        struct tm tm{};
        localtime_r(&tt, &tm);
        int month = tm.tm_mon;
        if (month != prev_month) {
            int x = g.grid_x + col * (g.cell + g.gap);
            dc.DrawText(month_names[month], x, 0);
            prev_month = month;
        }
    }

    // Cells.
    for (int i = 0; i < 53 * 7; i++) {
        int col = i / 7;
        int row = i % 7;
        int64_t d = day_at(anchor, col, row);
        if (d > today_start) continue;  // future cells stay blank
        auto it = counts_.find(d);
        int count = it != counts_.end() ? it->second : 0;
        int level = level_for(count, scale_max_);
        dc.SetBrush(colour_for_level(bg, fg, level));
        dc.DrawRectangle(
            g.grid_x + col * (g.cell + g.gap),
            g.grid_y + row * (g.cell + g.gap),
            g.cell, g.cell);
    }
}

void ActivityPanel::on_motion(wxMouseEvent &e)
{
    wxClientDC dc(this);
    Geom g = compute_geom(dc, GetClientSize());

    int mx = e.GetX() - g.grid_x;
    int my = e.GetY() - g.grid_y;
    if (mx < 0 || my < 0) { UnsetToolTip(); return; }

    int col = mx / (g.cell + g.gap);
    int row = my / (g.cell + g.gap);
    if (col < 0 || col > 52 || row < 0 || row > 6) {
        UnsetToolTip(); return;
    }

    int64_t anchor      = heatmap_anchor();
    int64_t today_start = day_start_of((double)time(nullptr));
    int64_t d           = day_at(anchor, col, row);
    if (d > today_start) { UnsetToolTip(); return; }

    time_t t = (time_t)d;
    struct tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);

    auto it = counts_.find(d);
    int count = it != counts_.end() ? it->second : 0;
    SetToolTip(wxString::Format("%s: %d %s",
                                buf, count,
                                count == 1 ? "entry" : "entries"));
}
