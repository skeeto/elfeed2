#include "entry_list.hpp"
#include "util.hpp"

#include <wx/font.h>
#include <wx/settings.h>

#include <algorithm>

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
        return wxString::FromUTF8(format_date(e.date));
    case 1:
        return wxString::FromUTF8(html_strip(e.title));
    case 2: {
        // Title comes from the DB-backed map (which has every feed
        // ever fetched, not just current subscriptions). Falls back
        // to the URL when nothing's known yet.
        auto it = app_->feed_titles.find(e.feed_url);
        if (it != app_->feed_titles.end())
            return wxString::FromUTF8(it->second);
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
