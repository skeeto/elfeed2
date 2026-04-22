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

// Discrete 0–4 level for a count, with 0 reserved for empty days.
// Log-compressed against the actual max so one really-busy day
// genuinely pushes everything else toward the cooler end of the
// ramp — that shape (one outlier day against background hum) is
// interesting, not something to normalize away. Floor at 1 so
// any nonzero count stays distinct from 0; cap at 4.
static int level_for(int count, int max_c)
{
    if (count <= 0) return 0;
    if (max_c <= 1) return 4;
    double t = std::log(1.0 + (double)count) /
               std::log(1.0 + (double)max_c);
    int level = (int)std::lround(t * 4.0);
    if (level < 1) level = 1;
    if (level > 4) level = 4;
    return level;
}

// Fixed five-stop palettes, one per theme. Hardcoded rather than
// derived from wxSYS_COLOUR_HIGHLIGHT because the system highlight
// is typically a midtone blue — lerping bg→highlight tops out
// before we've used half the available dynamic range, which is
// why adjacent levels read as "the same medium blue." These go
// all the way to deep navy (light) / saturated cyan-blue (dark)
// so all five levels are visually distinct at small cell sizes.
static const wxColour light_palette[5] = {
    wxColour(235, 237, 240),  // 0 — near-bg
    wxColour(168, 201, 236),  // 1 — pale blue
    wxColour( 88, 146, 210),  // 2 — medium
    wxColour( 30,  92, 172),  // 3 — strong
    wxColour(  8,  40, 102),  // 4 — deep navy
};

static const wxColour dark_palette[5] = {
    wxColour( 22,  27,  34),  // 0 — near-bg
    wxColour( 16,  56, 102),  // 1
    wxColour( 38, 109, 180),  // 2
    wxColour( 74, 163, 232),  // 3
    wxColour(140, 208, 255),  // 4 — bright saturated
};

static wxColour colour_for_level(wxColour bg, int level)
{
    // ITU-R BT.709 luma approximation on the window background.
    // Less than half of full luminance → dark theme.
    double luma = (0.2126 * bg.Red() +
                   0.7152 * bg.Green() +
                   0.0722 * bg.Blue()) / 255.0;
    const wxColour *pal = luma < 0.5 ? dark_palette : light_palette;
    if (level < 0) level = 0;
    if (level > 4) level = 4;
    return pal[level];
}

void ActivityPanel::refresh()
{
    counts_.clear();
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
    for (auto &[_, c] : counts_)
        max_count_ = std::max(max_count_, c);

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
        int level = level_for(count, max_count_);
        dc.SetBrush(colour_for_level(bg, level));
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
