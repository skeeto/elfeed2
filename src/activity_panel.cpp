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

// Jet-like ramp: seven stops spanning the full hue wheel from
// cool to warm. Unlike a single-hue lightness ramp (which tops
// out in perceived dynamic range because we only vary one channel),
// jet crosses through multiple hues — dark blue, blue, cyan,
// green, yellow, red, dark red — so every part of the distribution
// lands on a visibly distinct color. Well-known downsides (not
// perceptually uniform in luminance, tricky for some kinds of
// colorblindness) are acceptable here because the task is "spot
// the pattern at a glance," not precise quantitative reading.
namespace {
struct Stop { double t; wxColour c; };
}
static const Stop jet_stops[] = {
    {0.00, wxColour(  0,   0, 143)},
    {0.11, wxColour(  0,   0, 255)},
    {0.36, wxColour(  0, 255, 255)},
    {0.52, wxColour(  0, 255,   0)},
    {0.68, wxColour(255, 255,   0)},
    {0.87, wxColour(255,   0,   0)},
    {1.00, wxColour(143,   0,   0)},
};

static wxColour jet_at(double t)
{
    if (t <= 0) return jet_stops[0].c;
    constexpr size_t n = sizeof(jet_stops) / sizeof(jet_stops[0]);
    for (size_t i = 1; i < n; i++) {
        if (t <= jet_stops[i].t) {
            double span = jet_stops[i].t - jet_stops[i - 1].t;
            double lt   = span > 0
                              ? (t - jet_stops[i - 1].t) / span
                              : 0.0;
            return lerp_colour(jet_stops[i - 1].c, jet_stops[i].c, lt);
        }
    }
    return jet_stops[n - 1].c;
}

// Continuous color for a given (count, min_c, max_c). Empty cells
// track the theme background (faint tint); non-empty cells map
// their log-compressed position within the [min_c, max_c] range
// through the jet ramp. Normalizing against the actual range of
// observed counts (rather than 0-to-max) guarantees the full ramp
// is always used — the quietest nonzero day lands at the cool end,
// the busiest at the warm end, even when everything is clustered.
static wxColour colour_for_count(wxColour bg,
                                 int count, int min_c, int max_c)
{
    double luma = (0.2126 * bg.Red() +
                   0.7152 * bg.Green() +
                   0.0722 * bg.Blue()) / 255.0;
    bool dark = luma < 0.5;
    wxColour empty_c = dark ? wxColour(22, 27, 34)
                            : wxColour(235, 237, 240);
    constexpr size_t last = sizeof(jet_stops) /
                            sizeof(jet_stops[0]) - 1;

    if (count <= 0) return empty_c;
    if (max_c <= min_c) return jet_stops[last].c;  // one-value case

    double log_min = std::log(1.0 + (double)min_c);
    double log_max = std::log(1.0 + (double)max_c);
    double log_c   = std::log(1.0 + (double)count);
    double t = (log_c - log_min) / (log_max - log_min);
    return jet_at(t);
}

void ActivityPanel::refresh()
{
    counts_.clear();
    min_count_ = 0;
    max_count_ = 0;

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
    for (auto &[_, c] : counts_) {
        if (c <= 0) continue;
        max_count_ = std::max(max_count_, c);
        if (min_count_ == 0 || c < min_count_) min_count_ = c;
    }

    // Pre-compute day_at / month for every grid cell once here,
    // instead of running 400+ mktime calls on every paint. The
    // anchor changes only when the wall-clock week rolls over,
    // so refreshing these at the same cadence as the data is
    // plenty current in practice.
    int64_t anchor = heatmap_anchor();
    for (int col = 0; col < 53; col++) {
        for (int row = 0; row < 7; row++)
            cell_day_[col][row] = day_at(anchor, col, row);
        time_t t = (time_t)cell_day_[col][0];
        struct tm tm{};
        localtime_r(&t, &tm);
        col_month_[col] = tm.tm_mon;
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
    wxColour faint = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);

    wxSize sz = GetClientSize();
    dc.SetBrush(bg);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, sz.x, sz.y);

    Geom g = compute_geom(dc, sz);

    // Today's day-start caps the visible range; future cells
    // (the trailing end of the current week) stay blank.
    int64_t today_start = day_start_of((double)time(nullptr));

    // Day-of-week labels (Mon, Wed, Fri) at rows 1, 3, 5. With Sun
    // at row 0 and Sat at row 6, this labels three alternating
    // weekdays, matching GitHub's two-of-three convention.
    static const char *day_names[7] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    dc.SetTextForeground(faint);
    for (int r : {1, 3, 5}) {
        int y = g.grid_y + r * (g.cell + g.gap) +
                (g.cell - dc.GetCharHeight()) / 2;
        dc.DrawText(day_names[r], 2, y);
    }

    // Month labels across the top, one per column where the
    // column's Sunday falls in a new month. The col_month_ array
    // is pre-computed in refresh() so paint avoids tzdata lookups.
    static const char *month_names[12] = {
        "Jan","Feb","Mar","Apr","May","Jun",
        "Jul","Aug","Sep","Oct","Nov","Dec"
    };
    int prev_month = -1;
    for (int col = 0; col < 53; col++) {
        int month = col_month_[col];
        if (month != prev_month) {
            int x = g.grid_x + col * (g.cell + g.gap);
            dc.DrawText(month_names[month], x, 0);
            prev_month = month;
        }
    }

    // Cells, using the pre-computed day_start array.
    for (int i = 0; i < 53 * 7; i++) {
        int col = i / 7;
        int row = i % 7;
        int64_t d = cell_day_[col][row];
        if (d > today_start) continue;
        auto it = counts_.find(d);
        int count = it != counts_.end() ? it->second : 0;
        dc.SetBrush(colour_for_count(bg, count,
                                     min_count_, max_count_));
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

    int64_t today_start = day_start_of((double)time(nullptr));
    int64_t d           = cell_day_[col][row];
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
