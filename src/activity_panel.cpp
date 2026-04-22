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
    // Buffered painting: the grid paints hundreds of rectangles
    // per year block, and unbuffered on-screen compositing
    // produces visible tearing on resize.
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
    tm.tm_isdst = -1;  // let mktime renormalize across DST flips
    return (int64_t)mktime(&tm);
}

// Local midnight of the Monday on or before Jan 1 of `year`. This
// anchors column 0 of the year's grid: every day sits at
// (days_since_anchor / 7, days_since_anchor % 7) with Mon=row 0.
static int64_t first_monday_of_year(int year)
{
    struct tm tm{};
    tm.tm_year  = year - 1900;
    tm.tm_mon   = 0;
    tm.tm_mday  = 1;
    tm.tm_isdst = -1;
    mktime(&tm);                          // populates tm_wday
    int dow_mon0 = (tm.tm_wday + 6) % 7;  // Mon=0 … Sun=6
    tm.tm_mday -= dow_mon0;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

static int year_of(int64_t epoch)
{
    time_t t = (time_t)epoch;
    struct tm tm{};
    localtime_r(&t, &tm);
    return tm.tm_year + 1900;
}

static int64_t jan1_of(int year)
{
    struct tm tm{};
    tm.tm_year  = year - 1900;
    tm.tm_mon   = 0;
    tm.tm_mday  = 1;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

static int64_t dec31_of(int year)
{
    struct tm tm{};
    tm.tm_year  = year - 1900;
    tm.tm_mon   = 11;
    tm.tm_mday  = 31;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

// Advance one calendar day via tm arithmetic so DST transitions
// don't drift the result by ±1 hour (would misbucket those days).
static int64_t next_day(int64_t day_start)
{
    time_t t = (time_t)day_start;
    struct tm tm{};
    localtime_r(&t, &tm);
    tm.tm_mday += 1;
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

void ActivityPanel::refresh()
{
    counts_.clear();
    max_count_ = 0;

    if (!app_->entries.empty()) {
        for (auto &e : app_->entries) {
            if (e.date <= 0) continue;
            counts_[day_start_of(e.date)]++;
        }
        for (auto &[day, c] : counts_)
            max_count_ = std::max(max_count_, c);
    }
    Refresh();
}

void ActivityPanel::on_size(wxSizeEvent &e)
{
    Refresh();
    e.Skip();
}

// Geom choices. Dimensions are computed on every paint so the
// widget scales cleanly when the AUI pane is resized.
namespace {
struct Geom {
    int cell;     // cell side (square) in pixels
    int gap;      // gap between cells
    int label_h;  // year-label strip height
    int grid_x;   // x offset to column 0
    int block_h;  // full per-year block height
};
}

static Geom compute_layout(wxDC &dc, const wxSize &sz)
{
    Geom l;
    l.gap     = 1;
    // Fit 53 columns in the available width. Small floor/ceiling
    // keep the grid legible on both narrow docked panes and
    // stretched-wide window layouts.
    int cell = (sz.x - 4 - 52 * l.gap) / 53;
    if (cell < 3)  cell = 3;
    if (cell > 14) cell = 14;
    l.cell    = cell;
    l.label_h = dc.GetCharHeight() + 2;
    l.grid_x  = 2;
    l.block_h = l.label_h + 7 * (l.cell + l.gap);
    return l;
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

    if (counts_.empty() || max_count_ == 0) {
        wxString msg = "(no entries match the current filter)";
        wxSize tsz = dc.GetTextExtent(msg);
        dc.SetTextForeground(faint);
        dc.DrawText(msg, (sz.x - tsz.x) / 2, (sz.y - tsz.y) / 2);
        return;
    }

    Geom l = compute_layout(dc, sz);

    int year_now = year_of((int64_t)time(nullptr));
    int year_min = year_of(counts_.begin()->first);

    // Empty cells take a faint tint (~7% of the way from the window
    // background to the highlight color) so the grid structure
    // remains visible even on days with no entries. Non-empty cells
    // blend further toward the highlight color on a log scale —
    // log compression keeps single-entry days distinguishable from
    // very-busy days without squashing the low end, matching the
    // visual density GitHub uses for the same chart.
    wxColour empty_c = lerp_colour(bg, fg, 0.07);

    int y_off = 0;
    for (int year = year_now; year >= year_min && y_off < sz.y; year--) {
        dc.SetTextForeground(faint);
        dc.DrawText(wxString::Format("%d", year), l.grid_x, y_off);

        int64_t anchor = first_monday_of_year(year);
        int64_t start  = jan1_of(year);
        int64_t end_cap = (year == year_now)
                              ? day_start_of((double)time(nullptr))
                              : dec31_of(year);

        for (int64_t d = start; d <= end_cap; d = next_day(d)) {
            int64_t days = (d - anchor) / 86400;
            int col = (int)(days / 7);
            int row = (int)(days % 7);
            if (col < 0 || col > 52 || row < 0 || row > 6) continue;

            auto it = counts_.find(d);
            int count = it != counts_.end() ? it->second : 0;

            wxColour c;
            if (count == 0) {
                c = empty_c;
            } else {
                double t = std::log(1.0 + (double)count) /
                           std::log(1.0 + (double)max_count_);
                c = lerp_colour(empty_c, fg, 0.25 + 0.75 * t);
            }
            dc.SetBrush(c);
            dc.DrawRectangle(
                l.grid_x + col * (l.cell + l.gap),
                y_off + l.label_h + row * (l.cell + l.gap),
                l.cell, l.cell);
        }

        y_off += l.block_h;
    }
}

void ActivityPanel::on_motion(wxMouseEvent &e)
{
    if (counts_.empty()) { UnsetToolTip(); return; }

    wxClientDC dc(this);
    Geom l = compute_layout(dc, GetClientSize());

    int year_now = year_of((int64_t)time(nullptr));
    int year_min = year_of(counts_.begin()->first);

    // Invert the paint layout: figure out which year block the
    // cursor is in, then which cell within that block, then
    // which actual date that cell represents.
    int mx = e.GetX();
    int my = e.GetY();

    if (my < 0 || l.block_h <= 0) { UnsetToolTip(); return; }
    int block = my / l.block_h;
    int year  = year_now - block;
    if (year < year_min) { UnsetToolTip(); return; }

    int in_y = my - block * l.block_h - l.label_h;
    int in_x = mx - l.grid_x;
    if (in_y < 0 || in_x < 0) { UnsetToolTip(); return; }

    int col = in_x / (l.cell + l.gap);
    int row = in_y / (l.cell + l.gap);
    if (col < 0 || col > 52 || row < 0 || row > 6) {
        UnsetToolTip(); return;
    }

    // Resolve (year, col, row) → actual day via tm math (DST-safe).
    int64_t anchor = first_monday_of_year(year);
    time_t t = (time_t)anchor;
    struct tm tm{};
    localtime_r(&t, &tm);
    tm.tm_mday += col * 7 + row;
    tm.tm_isdst = -1;
    int64_t day = (int64_t)mktime(&tm);

    // Out-of-year cells (the blanks padded around Jan 1 / Dec 31)
    // don't represent a real day within this year's block.
    if (day < jan1_of(year) || day > dec31_of(year)) {
        UnsetToolTip(); return;
    }

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);

    auto it = counts_.find(day);
    int count = it != counts_.end() ? it->second : 0;
    SetToolTip(wxString::Format("%s: %d %s",
                                buf, count,
                                count == 1 ? "entry" : "entries"));
}
