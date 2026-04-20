#include "entry_list.hpp"

#include <wx/font.h>
#include <wx/settings.h>

#include <algorithm>
#include <ctime>

static wxString fmt_date(double epoch)
{
    time_t t = (time_t)epoch;
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return wxString::FromUTF8(buf);
}

static bool has_tag(const Entry &e, const char *tag)
{
    return std::find(e.tags.begin(), e.tags.end(), tag) != e.tags.end();
}

EntryList::EntryList(wxWindow *parent, Elfeed *app)
    : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                 wxLC_REPORT | wxLC_VIRTUAL)
    , app_(app)
{
    AppendColumn("Date", wxLIST_FORMAT_LEFT, FromDIP(90));
    AppendColumn("Title", wxLIST_FORMAT_LEFT, FromDIP(400));
    AppendColumn("Feed", wxLIST_FORMAT_LEFT, FromDIP(150));
    AppendColumn("Tags", wxLIST_FORMAT_LEFT, FromDIP(120));

    wxFont base = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    wxFont bold = base.Bold();
    attr_unread_.SetFont(bold);
    attr_read_.SetTextColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));

    SetItemCount(0);
}

void EntryList::refresh_items()
{
    SetItemCount((long)app_->entries.size());
    Refresh();
}

long EntryList::primary() const
{
    return GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_FOCUSED);
}

std::vector<long> EntryList::selection() const
{
    std::vector<long> out;
    long idx = -1;
    while ((idx = GetNextItem(idx, wxLIST_NEXT_ALL,
                              wxLIST_STATE_SELECTED)) != -1) {
        out.push_back(idx);
    }
    return out;
}

wxString EntryList::OnGetItemText(long item, long column) const
{
    if (item < 0 || (size_t)item >= app_->entries.size()) return {};
    const Entry &e = app_->entries[(size_t)item];

    switch (column) {
    case 0:
        return fmt_date(e.date);
    case 1:
        return wxString::FromUTF8(html_strip(e.title));
    case 2: {
        for (auto &f : app_->feeds) {
            if (f.url == e.feed_url)
                return wxString::FromUTF8(f.title);
        }
        return wxString::FromUTF8(e.feed_url);
    }
    case 3: {
        std::string tags;
        for (auto &t : e.tags) {
            if (!tags.empty()) tags += ",";
            tags += t;
        }
        return wxString::FromUTF8(tags);
    }
    }
    return {};
}

wxListItemAttr *EntryList::OnGetItemAttr(long item) const
{
    if (item < 0 || (size_t)item >= app_->entries.size()) return nullptr;
    const Entry &e = app_->entries[(size_t)item];
    return has_tag(e, "unread") ? &attr_unread_ : &attr_read_;
}
