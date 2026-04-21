#include "entry_list.hpp"
#include "util.hpp"

#include <wx/font.h>
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
            return true;
        }
        // Read entries get the default style (no attribute).
        return false;
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

    // wxDATAVIEW_COL_HIDDEN is a STATE flag — setting it at construction
    // would make every column start hidden. We just want resizable +
    // reorderable + sortable; visibility is handled by the right-click
    // menu we pop ourselves below.
    const int col_flags = wxDATAVIEW_COL_RESIZABLE |
                          wxDATAVIEW_COL_REORDERABLE |
                          wxDATAVIEW_COL_SORTABLE;

    AppendTextColumn("Date",  0, wxDATAVIEW_CELL_INERT,
                     FromDIP(90),  wxALIGN_LEFT, col_flags);
    AppendTextColumn("Title", 1, wxDATAVIEW_CELL_INERT,
                     FromDIP(400), wxALIGN_LEFT, col_flags);
    AppendTextColumn("Feed",  2, wxDATAVIEW_CELL_INERT,
                     FromDIP(150), wxALIGN_LEFT, col_flags);
    AppendTextColumn("Tags",  3, wxDATAVIEW_CELL_INERT,
                     FromDIP(120), wxALIGN_LEFT, col_flags);

    // Restore saved column widths/visibility and sort state from DB.
    dataview_apply_columns(this, db_load_ui_state(app_, "cols.entry_list"));
    dataview_apply_sort(this, db_load_ui_state(app_, "sort.entry_list"));

    // Provide our own column-visibility menu — wxDataViewCtrl exposes
    // the right-click event but doesn't pop a menu by default.
    Bind(wxEVT_DATAVIEW_COLUMN_HEADER_RIGHT_CLICK,
         [this](wxDataViewEvent &) {
             dataview_show_column_menu(this, [this] { save_columns(); });
         });
    Bind(wxEVT_DATAVIEW_COLUMN_SORTED, &EntryList::on_sort, this);
}

static int ci_compare(const std::string &a, const std::string &b)
{
    size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; i++) {
        int ca = std::tolower((unsigned char)a[i]);
        int cb = std::tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
    }
    if (a.size() < b.size()) return -1;
    if (a.size() > b.size()) return 1;
    return 0;
}

void EntryList::apply_sort()
{
    DataViewSort s = dataview_current_sort(this);
    // Default: keep the SQL order (date DESC). That's the "recent
    // first" view users expect when they haven't chosen a sort.
    if (s.col < 0) return;

    // Stable sort so equal rows keep their date-DESC order as a
    // tiebreak — meaningful for Tags/Feed sorts where many entries
    // share the key. Preserves intuitive "within this group, newer
    // first" behavior at no cost.
    auto *app = app_;
    std::stable_sort(
        app->entries.begin(), app->entries.end(),
        [col = s.col, asc = s.ascending, app]
        (const Entry &a, const Entry &b) {
            int c = 0;
            switch (col) {
            case 0:
                if (a.date != b.date) c = a.date < b.date ? -1 : 1;
                break;
            case 1:
                c = ci_compare(html_strip(a.title), html_strip(b.title));
                break;
            case 2: {
                auto at = app->feed_titles.find(a.feed_url);
                auto bt = app->feed_titles.find(b.feed_url);
                const std::string &as = at != app->feed_titles.end()
                                            ? at->second : a.feed_url;
                const std::string &bs = bt != app->feed_titles.end()
                                            ? bt->second : b.feed_url;
                c = ci_compare(as, bs);
                break;
            }
            case 3: {
                // Join tags for a stable lex compare; small and
                // cheap for the typical 0–4 tags per entry.
                std::string ta, tb;
                for (auto &t : a.tags) { ta += t; ta += ','; }
                for (auto &t : b.tags) { tb += t; tb += ','; }
                c = ci_compare(ta, tb);
                break;
            }
            }
            return asc ? c < 0 : c > 0;
        });
}

void EntryList::on_sort(wxDataViewEvent &)
{
    apply_sort();
    model_->Reset((unsigned int)app_->entries.size());
    db_save_ui_state(app_, "sort.entry_list",
                     dataview_serialize_sort(this).c_str());
}

void EntryList::save_columns()
{
    db_save_ui_state(app_, "cols.entry_list",
                     dataview_serialize_columns(this).c_str());
}

void EntryList::refresh_items()
{
    // Re-apply current sort in case the new query results came back
    // in DB order (date DESC) but the user has a custom sort active.
    apply_sort();
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
