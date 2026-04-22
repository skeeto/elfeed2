#include "entry_list.hpp"
#include "util.hpp"

#include <wx/dc.h>
#include <wx/dcscreen.h>
#include <wx/font.h>
#include <wx/variant.h>

#include <algorithm>
#include <unordered_set>

#ifdef __WXMAC__
#include <CoreText/CoreText.h>

// Build a wxFont based on the system font at `point_size` with the
// "monospaced numbers" OpenType feature enabled. Letters and
// punctuation stay in their natural proportional shapes; only
// digits get equal advance widths so successive dates line up.
// macOS exposes this via Core Text feature settings; wxFont has a
// CTFontRef constructor we can pass the result to.
static wxFont tabular_system_font(int point_size)
{
    CTFontRef base = CTFontCreateUIFontForLanguage(
        kCTFontUIFontSystem, (CGFloat)point_size, NULL);
    if (!base) return *wxNORMAL_FONT;

    int feat_type = kNumberSpacingType;
    int feat_sel  = kMonospacedNumbersSelector;
    CFNumberRef nType = CFNumberCreate(NULL, kCFNumberIntType, &feat_type);
    CFNumberRef nSel  = CFNumberCreate(NULL, kCFNumberIntType, &feat_sel);

    const void *fkeys[] = {
        kCTFontFeatureTypeIdentifierKey,
        kCTFontFeatureSelectorIdentifierKey,
    };
    const void *fvals[] = { nType, nSel };
    CFDictionaryRef feature = CFDictionaryCreate(
        NULL, fkeys, fvals, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(nType); CFRelease(nSel);

    const void *farr[] = { feature };
    CFArrayRef features = CFArrayCreate(NULL, farr, 1,
                                        &kCFTypeArrayCallBacks);
    CFRelease(feature);

    const void *akeys[] = { kCTFontFeatureSettingsAttribute };
    const void *avals[] = { features };
    CFDictionaryRef attrs = CFDictionaryCreate(
        NULL, akeys, avals, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(features);

    CTFontDescriptorRef desc =
        CTFontDescriptorCreateWithAttributes(attrs);
    CFRelease(attrs);

    CTFontRef tabular =
        CTFontCreateCopyWithAttributes(base, 0.0, NULL, desc);
    CFRelease(base);
    CFRelease(desc);

    if (!tabular) return *wxNORMAL_FONT;
    wxFont f(tabular);
    CFRelease(tabular);
    return f;
}
#endif  // __WXMAC__

// Renders the cell text using the system font with tabular figures
// so successive rows' digits align. On macOS the system font's
// "monospaced numbers" OpenType feature does this without changing
// the typeface — letters/dashes look identical to the rest of the
// list. On other platforms we fall back to the default font (the
// renderer becomes a no-op wrapper); fix per-platform if it ever
// matters there.
class TabularTextRenderer : public wxDataViewCustomRenderer {
public:
    TabularTextRenderer()
        : wxDataViewCustomRenderer(
              "string", wxDATAVIEW_CELL_INERT, wxALIGN_LEFT) {}

    bool SetValue(const wxVariant &v) override {
        value_ = v.GetString();
        return true;
    }
    bool GetValue(wxVariant &v) const override {
        v = value_;
        return true;
    }

    wxSize GetSize() const override {
        wxScreenDC dc;
        dc.SetFont(font_for_view());
        return dc.GetTextExtent(value_);
    }

    bool Render(wxRect rect, wxDC *dc, int state) override {
        wxFont f = font_for_view();
        const wxDataViewItemAttr &attr = GetAttr();
        if (attr.GetBold())   f.MakeBold();
        if (attr.GetItalic()) f.MakeItalic();

        wxFont saved_font = dc->GetFont();
        wxColour saved_fg = dc->GetTextForeground();
        dc->SetFont(f);
        if (attr.HasColour()) dc->SetTextForeground(attr.GetColour());
        RenderText(value_, 0, rect, dc, state);
        dc->SetFont(saved_font);
        dc->SetTextForeground(saved_fg);
        return true;
    }

private:
    wxFont font_for_view() const {
        wxDataViewCtrl *view = GetView();
        wxFont base = view ? view->GetFont() : *wxNORMAL_FONT;
#ifdef __WXMAC__
        return tabular_system_font(base.GetPointSize());
#else
        return base;
#endif
    }

    wxString value_;
};

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

    // We sort app_->entries ourselves in EntryList::apply_sort, so
    // the backend's native sort needs to be a no-op. Without this,
    // wxDataViewVirtualListModel's default implementation reverses
    // the display for descending sorts (it returns pos2-pos1),
    // stacking a second sort on top of ours. The display would end
    // up reverse-ordered from our vector and row+1 advance would
    // visit rows "at random".
    int Compare(const wxDataViewItem &, const wxDataViewItem &,
                unsigned int, bool) const override
    {
        return 0;
    }

    bool GetAttrByRow(unsigned int row, unsigned int /*col*/,
                      wxDataViewItemAttr &attr) const override
    {
        if (row >= app_->entries.size()) return false;
        const Entry &e = app_->entries[row];
        bool used = false;
        if (has_tag(e, "unread")) {
            attr.SetBold(true);
            used = true;
        }
        // First-match-wins per `color TAG #RGB` directives. Walked
        // in config order so the user controls priority by stanza
        // sequence; duplicate `color` entries for the same tag let
        // the earliest definition win.
        for (auto &tc : app_->tag_colors) {
            if (has_tag(e, tc.first.c_str())) {
                uint32_t c = tc.second;
                attr.SetColour(wxColour(
                    (unsigned char)((c >> 16) & 0xFF),
                    (unsigned char)((c >> 8)  & 0xFF),
                    (unsigned char)( c        & 0xFF)));
                used = true;
                break;
            }
        }
        return used;
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

    default_order_ = {"Date", "Title", "Feed", "Tags"};

    std::string saved_cols = db_load_ui_state(app_, "cols.entry_list");
    build_columns(dataview_parse_column_order(saved_cols));
    dataview_apply_columns(this, saved_cols);
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

void EntryList::append_column(const wxString &title)
{
    const int flags = wxDATAVIEW_COL_RESIZABLE |
                      wxDATAVIEW_COL_REORDERABLE |
                      wxDATAVIEW_COL_SORTABLE;
    if (title == "Date") {
        // Custom renderer (instead of AppendTextColumn) so dates
        // render in tabular figures and digits line up across rows
        // — proportional shapes for everything else.
        AppendColumn(new wxDataViewColumn(
            "Date", new TabularTextRenderer(), 0,
            FromDIP(90), wxALIGN_LEFT, flags));
    } else if (title == "Title") {
        AppendTextColumn("Title", 1, wxDATAVIEW_CELL_INERT,
                         FromDIP(400), wxALIGN_LEFT, flags);
    } else if (title == "Feed") {
        AppendTextColumn("Feed",  2, wxDATAVIEW_CELL_INERT,
                         FromDIP(150), wxALIGN_LEFT, flags);
    } else if (title == "Tags") {
        AppendTextColumn("Tags",  3, wxDATAVIEW_CELL_INERT,
                         FromDIP(120), wxALIGN_LEFT, flags);
    }
}

void EntryList::build_columns(const std::vector<std::string> &order)
{
    ClearColumns();
    std::unordered_set<std::string> known(default_order_.begin(),
                                          default_order_.end());
    std::unordered_set<std::string> added;
    for (const auto &t : order) {
        if (!known.count(t) || added.count(t)) continue;
        append_column(wxString::FromUTF8(t));
        added.insert(t);
    }
    for (const auto &t : default_order_) {
        if (added.count(t)) continue;
        append_column(wxString::FromUTF8(t));
    }
}

void EntryList::reset_layout()
{
    build_columns(default_order_);
    db_save_ui_state(app_, "cols.entry_list", "");
    db_save_ui_state(app_, "sort.entry_list", "");
    // Caller (MainFrame) is expected to requery() afterwards to
    // restore SQL-order (date DESC) rows in app->entries.
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
