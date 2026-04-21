#include "entry_list.hpp"
#include "util.hpp"

#include <wx/font.h>
#include <wx/settings.h>
#include <wx/variant.h>

#include <algorithm>

static bool has_tag(const Entry &e, const char *tag)
{
    return std::find(e.tags.begin(), e.tags.end(), tag) != e.tags.end();
}

// ---- Model ----------------------------------------------------------

class EntryListModel : public wxDataViewVirtualListModel {
public:
    EntryListModel(Elfeed *app) : app_(app) {}

    unsigned int GetColumnCount() const override { return 4; }

    wxString GetColumnType(unsigned int) const override
    {
        return "string";
    }

    void GetValueByRow(wxVariant &value,
                       unsigned int row,
                       unsigned int col) const override
    {
        if (row >= app_->entries.size()) { value = wxString(); return; }
        const Entry &e = app_->entries[row];
        switch (col) {
        case 0:
            value = wxString::FromUTF8(format_date(e.date));
            return;
        case 1:
            value = wxString::FromUTF8(html_strip(e.title));
            return;
        case 2: {
            auto it = app_->feed_titles.find(e.feed_url);
            value = wxString::FromUTF8(
                it != app_->feed_titles.end() ? it->second : e.feed_url);
            return;
        }
        case 3: {
            std::string tags;
            for (auto &t : e.tags) {
                if (!tags.empty()) tags += ',';
                tags += t;
            }
            value = wxString::FromUTF8(tags);
            return;
        }
        }
        value = wxString();
    }

    bool SetValueByRow(const wxVariant &, unsigned int, unsigned int) override
    {
        return false;  // read-only
    }

    bool GetAttrByRow(unsigned int row, unsigned int /*col*/,
                      wxDataViewItemAttr &attr) const override
    {
        if (row >= app_->entries.size()) return false;
        if (has_tag(app_->entries[row], "unread")) {
            attr.SetBold(true);
        } else {
            attr.SetColour(
                wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        }
        return true;
    }

private:
    Elfeed *app_;
};

// ---- View -----------------------------------------------------------

EntryList::EntryList(wxWindow *parent, Elfeed *app)
    : wxDataViewCtrl(parent, wxID_ANY,
                     wxDefaultPosition, wxDefaultSize,
                     wxDV_MULTIPLE | wxDV_ROW_LINES |
                     wxDV_VERT_RULES)
    , app_(app)
    , model_(new EntryListModel(app))
{
    AssociateModel(model_.get());

    // Column flags: REORDERABLE lets users drag headers; allowing
    // columns to be HIDDEN lets the right-click header context menu
    // expose a per-column visibility checkbox.
    const int col_flags = wxDATAVIEW_COL_RESIZABLE |
                          wxDATAVIEW_COL_REORDERABLE |
                          wxDATAVIEW_COL_HIDDEN;

    AppendTextColumn("Date",  0, wxDATAVIEW_CELL_INERT,
                     FromDIP(90),  wxALIGN_LEFT, col_flags);
    AppendTextColumn("Title", 1, wxDATAVIEW_CELL_INERT,
                     FromDIP(400), wxALIGN_LEFT, col_flags);
    AppendTextColumn("Feed",  2, wxDATAVIEW_CELL_INERT,
                     FromDIP(150), wxALIGN_LEFT, col_flags);
    AppendTextColumn("Tags",  3, wxDATAVIEW_CELL_INERT,
                     FromDIP(120), wxALIGN_LEFT, col_flags);
}

void EntryList::refresh_items()
{
    model_->Reset((unsigned int)app_->entries.size());
}

void EntryList::refresh_row(long row)
{
    if (row >= 0 && (size_t)row < app_->entries.size())
        model_->RowChanged((unsigned int)row);
}

long EntryList::primary() const
{
    wxDataViewItem item = GetCurrentItem();
    if (!item.IsOk()) return -1;
    return (long)model_->GetRow(item);
}

std::vector<long> EntryList::selection() const
{
    wxDataViewItemArray sel;
    GetSelections(sel);
    std::vector<long> rows;
    rows.reserve(sel.size());
    for (auto &item : sel) {
        if (item.IsOk()) rows.push_back((long)model_->GetRow(item));
    }
    std::sort(rows.begin(), rows.end());
    return rows;
}

void EntryList::select_only(long row)
{
    if (row < 0 || (size_t)row >= app_->entries.size()) return;
    UnselectAll();
    wxDataViewItem item = model_->GetItem((unsigned int)row);
    Select(item);
    SetCurrentItem(item);
}

void EntryList::ensure_visible_row(long row)
{
    if (row < 0 || (size_t)row >= app_->entries.size()) return;
    EnsureVisible(model_->GetItem((unsigned int)row));
}
