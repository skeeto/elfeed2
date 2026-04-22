#include "activity_panel.hpp"

#include <wx/dcbuffer.h>
#include <wx/settings.h>
#include <wx/tooltip.h>

#include <algorithm>
#include <ctime>
#include <map>

ActivityPanel::ActivityPanel(wxWindow *parent, Elfeed *app)
    : wxPanel(parent, wxID_ANY)
    , app_(app)
{
    // Buffered painting eliminates flicker when the panel is
    // resized or refreshed mid-fetch — the bars are dense enough
    // that unbuffered paints noticeably blink.
    SetBackgroundStyle(wxBG_STYLE_PAINT);

    Bind(wxEVT_PAINT,  &ActivityPanel::on_paint,  this);
    Bind(wxEVT_MOTION, &ActivityPanel::on_motion, this);
    Bind(wxEVT_SIZE,   &ActivityPanel::on_size,   this);

    refresh();
}

int64_t ActivityPanel::week_start_of(double epoch)
{
    time_t t = (time_t)epoch;
    struct tm tm{};
    localtime_r(&t, &tm);
    // tm_wday is 0=Sunday … 6=Saturday. Convert to Mon=0..Sun=6
    // so we can roll back to the Monday that starts the week.
    int dow = (tm.tm_wday + 6) % 7;
    tm.tm_mday -= dow;
    tm.tm_hour  = 0;
    tm.tm_min   = 0;
    tm.tm_sec   = 0;
    tm.tm_isdst = -1;  // let mktime renormalize across DST flips
    return (int64_t)mktime(&tm);
}

void ActivityPanel::refresh()
{
    bins_.clear();
    max_count_ = 0;

    if (!app_->entries.empty()) {
        std::map<int64_t, int> by_week;
        for (auto &e : app_->entries) {
            if (e.date <= 0) continue;
            by_week[week_start_of(e.date)]++;
        }
        bins_.reserve(by_week.size());
        for (auto &[w, c] : by_week) bins_.push_back({w, c});
        for (auto &b : bins_)
            max_count_ = std::max(max_count_, b.count);
    }
    Refresh();
}

void ActivityPanel::on_size(wxSizeEvent &e)
{
    Refresh();
    e.Skip();
}

void ActivityPanel::on_paint(wxPaintEvent &)
{
    wxAutoBufferedPaintDC dc(this);

    wxColour bg     = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    wxColour fg     = wxSystemSettings::GetColour(wxSYS_COLOUR_HIGHLIGHT);
    wxColour faint  = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);

    wxSize sz = GetClientSize();
    dc.SetBrush(bg);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.DrawRectangle(0, 0, sz.x, sz.y);

    if (bins_.empty() || max_count_ == 0) {
        wxString msg = "(no entries match the current filter)";
        wxSize tsz = dc.GetTextExtent(msg);
        dc.SetTextForeground(faint);
        dc.DrawText(msg, (sz.x - tsz.x) / 2, (sz.y - tsz.y) / 2);
        return;
    }

    // Layout. Bars span the full width with a small top/bottom
    // margin: 4px above so the y-axis label has room, 4px below
    // for breathing room at the baseline.
    const int top_margin = 18;
    const int bot_margin = 4;
    int avail_h = sz.y - top_margin - bot_margin;
    if (avail_h < 8) avail_h = 8;

    int n = (int)bins_.size();
    double bar_w = (double)sz.x / (double)n;
    int bar_inner = std::max(1, (int)bar_w - 1);

    dc.SetBrush(fg);
    for (int i = 0; i < n; i++) {
        double frac = (double)bins_[i].count / (double)max_count_;
        int h = std::max(1, (int)(avail_h * frac));
        int x = (int)(i * bar_w);
        int y = top_margin + (avail_h - h);
        dc.DrawRectangle(x, y, bar_inner, h);
    }

    // Y-axis label and weekly span — minimal chrome that lets the
    // viewer read absolute scale at a glance.
    dc.SetTextForeground(faint);
    dc.DrawText(wxString::Format("max %d entries / week", max_count_),
                4, 2);
    dc.DrawText(wxString::Format("%d weeks", n),
                sz.x - dc.GetTextExtent(
                    wxString::Format("%d weeks", n)).x - 4,
                2);
}

void ActivityPanel::on_motion(wxMouseEvent &e)
{
    if (bins_.empty()) {
        UnsetToolTip();
        return;
    }
    int n = (int)bins_.size();
    int idx = (int)((double)e.GetX() / (double)GetClientSize().x * n);
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;

    time_t t = (time_t)bins_[idx].week_start;
    struct tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    SetToolTip(wxString::Format(
        "Week of %s: %d entries", buf, bins_[idx].count));
}
