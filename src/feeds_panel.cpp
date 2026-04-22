#include "feeds_panel.hpp"
#include "main_frame.hpp"
#include "util.hpp"

#include <wx/clipbrd.h>
#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/utils.h>
#include <wx/variant.h>

#include <algorithm>
#include <cctype>
#include <unordered_set>

class FeedsPanelModel : public wxDataViewVirtualListModel {
public:
    FeedsPanelModel(FeedsPanel *owner) : owner_(owner) {}

    unsigned int GetColumnCount() const override { return 3; }
    wxString GetColumnType(unsigned int) const override { return "string"; }

    void GetValueByRow(wxVariant &value,
                       unsigned int row,
                       unsigned int col) const override
    {
        if (row >= owner_->rows_.size()) { value = wxString(); return; }
        const FeedsPanel::Row &r = owner_->rows_[row];
        if (col == 0) {
            // Title, with a small trailing arrow if the feed's fetch
            // URL resolved through a redirect. Users who want to see
            // the redirect target can enable the Canonical URL
            // column from the header right-click menu.
            std::string label = r.title;
            if (!r.canonical_url.empty()) label += "  ↳";
            value = wxString::FromUTF8(label);
        } else if (col == 1) {
            value = wxString::FromUTF8(format_date(r.updated));
        } else if (col == 2) {
            value = wxString::FromUTF8(r.canonical_url);
        } else {
            value = wxString();
        }
    }

    bool SetValueByRow(const wxVariant &, unsigned int, unsigned int) override
    {
        return false;
    }

    // See EntryListModel::Compare — we sort owner_->rows_ ourselves,
    // so the backend's native sort must be a no-op.
    int Compare(const wxDataViewItem &, const wxDataViewItem &,
                unsigned int, bool) const override
    {
        return 0;
    }

private:
    FeedsPanel *owner_;
};

FeedsPanel::FeedsPanel(wxWindow *parent, Elfeed *app,
                       std::function<void(const std::string &)> on_activate)
    : wxPanel(parent, wxID_ANY)
    , app_(app)
    , on_activate_(std::move(on_activate))
{
    auto *sz = new wxBoxSizer(wxVERTICAL);

    list_ = new wxDataViewCtrl(this, wxID_ANY,
                               wxDefaultPosition, wxDefaultSize,
                               wxDV_SINGLE | wxDV_ROW_LINES);
    model_ = new FeedsPanelModel(this);
    list_->AssociateModel(model_.get());

    default_order_ = {"Title", "Updated", "Canonical URL"};

    // Build columns in the saved display order (if any), then apply
    // saved widths / hidden / sort on top. First-run: no saved data,
    // build_columns(empty) falls back to default_order_.
    std::string saved_cols = db_load_ui_state(app_, "cols.feeds");
    build_columns(dataview_parse_column_order(saved_cols));
    dataview_apply_columns(list_, saved_cols);
    dataview_apply_sort(list_, db_load_ui_state(app_, "sort.feeds"));
    sz->Add(list_, 1, wxEXPAND);
    SetSizer(sz);

    list_->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED,
                &FeedsPanel::on_activated, this);
    list_->Bind(wxEVT_DATAVIEW_COLUMN_HEADER_RIGHT_CLICK,
                [this](wxDataViewEvent &) {
                    dataview_show_column_menu(list_,
                                              [this] { save_columns(); });
                });
    list_->Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU,
                &FeedsPanel::on_context_menu, this);
    list_->Bind(wxEVT_CHAR_HOOK, &FeedsPanel::on_key, this);
    list_->Bind(wxEVT_DATAVIEW_COLUMN_SORTED,
                &FeedsPanel::on_sort, this);

    refresh();
}

// Lowercase compare so "apple" sorts next to "Apple" rather than
// after all uppercase entries. Kept local to avoid pulling in a
// locale-collator dependency.
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

void FeedsPanel::apply_sort()
{
    DataViewSort s = dataview_current_sort(list_);
    // No explicit sort → default to title ascending, matching our
    // pre-sortable-headers behavior (most users expect to scan
    // feeds alphabetically).
    int col = s.col < 0 ? 0 : s.col;
    bool asc = s.col < 0 ? true : s.ascending;

    std::sort(rows_.begin(), rows_.end(),
              [col, asc](const Row &a, const Row &b) {
                  int c = 0;
                  switch (col) {
                  case 0: c = ci_compare(a.title, b.title); break;
                  case 1:
                      // Numeric compare on epoch; ties fall back to
                      // title so the order is deterministic.
                      if (a.updated != b.updated)
                          c = a.updated < b.updated ? -1 : 1;
                      else
                          c = ci_compare(a.title, b.title);
                      break;
                  case 2: c = ci_compare(a.canonical_url, b.canonical_url);
                          break;
                  default: c = 0;
                  }
                  return asc ? c < 0 : c > 0;
              });
}

void FeedsPanel::on_sort(wxDataViewEvent &)
{
    apply_sort();
    model_->Reset((unsigned int)rows_.size());
    db_save_ui_state(app_, "sort.feeds",
                     dataview_serialize_sort(list_).c_str());
}

const FeedsPanel::Row *FeedsPanel::selected_row() const
{
    wxDataViewItem item = list_->GetSelection();
    if (!item.IsOk()) return nullptr;
    unsigned row = model_->GetRow(item);
    if (row >= rows_.size()) return nullptr;
    return &rows_[row];
}

bool FeedsPanel::copy_to_clipboard(const std::string &text)
{
    if (text.empty()) return false;
    if (!wxTheClipboard->Open()) return false;
    wxTheClipboard->SetData(
        new wxTextDataObject(wxString::FromUTF8(text)));
    wxTheClipboard->Close();
    // Confirm in the status bar via the parent frame. Walking the
    // window tree avoids hardcoding a back-pointer to MainFrame.
    if (auto *frame =
            dynamic_cast<MainFrame *>(wxGetTopLevelParent(this))) {
        frame->flash_status(wxString::FromUTF8("Copied: " + text));
    }
    return true;
}

void FeedsPanel::on_context_menu(wxDataViewEvent &e)
{
    wxDataViewItem item = e.GetItem();
    if (!item.IsOk()) return;
    unsigned idx = model_->GetRow(item);
    if (idx >= rows_.size()) return;
    const Row &r = rows_[idx];

    // Shortcut hints in the labels are cosmetic (the actual keystroke
    // handling lives in on_key). Using \t so the hint right-aligns
    // native-style; using platform-appropriate modifier spelling so
    // macOS users see ⌘ rather than "Ctrl". The y/Ctrl+C keystrokes
    // always prefer the canonical URL when one exists, so the hint
    // is attached to whichever item actually receives those presses.
#ifdef __WXOSX__
    const char *kCopyHint = "y, \u2318C";  // U+2318 PLACE OF INTEREST SIGN
#else
    const char *kCopyHint = "y, Ctrl+C";
#endif

    enum { ID_CopyFeed = wxID_HIGHEST + 1,
           ID_CopyCanonical,
           ID_OpenBrowser };
    wxMenu menu;
    // Canonical URL copy only shows up when there is one — hiding
    // rather than graying it out keeps the menu short on the common
    // case (most feeds don't redirect).
    if (!r.canonical_url.empty()) {
        menu.Append(ID_CopyFeed, "Copy Feed &URL");
        menu.Append(ID_CopyCanonical,
                    wxString("Copy &Canonical URL\t") + kCopyHint);
    } else {
        menu.Append(ID_CopyFeed,
                    wxString("Copy Feed &URL\t") + kCopyHint);
    }
    menu.AppendSeparator();
    menu.Append(ID_OpenBrowser, "Open in &Browser");

    // The row doesn't auto-select on right-click (macOS especially);
    // select it so the keystroke bindings below act on the same row
    // the user clicked.
    list_->Select(item);

    int choice = list_->GetPopupMenuSelectionFromUser(menu);
    if (choice == ID_CopyFeed)          copy_to_clipboard(r.url);
    else if (choice == ID_CopyCanonical) copy_to_clipboard(r.canonical_url);
    else if (choice == ID_OpenBrowser) {
        // Prefer the canonical URL if known — the user likely wants
        // to verify *where it redirects to*, not watch the redirect
        // happen again.
        const std::string &target =
            r.canonical_url.empty() ? r.url : r.canonical_url;
        wxLaunchDefaultBrowser(wxString::FromUTF8(target));
    }
}

void FeedsPanel::on_key(wxKeyEvent &e)
{
    int code = e.GetKeyCode();
    // `y` matches the entry list's "copy link" convention; Cmd/Ctrl+C
    // covers the OS-standard expectation. Both copy the canonical URL
    // when known, otherwise the feed URL — the URL the user most
    // likely wants is always the most-up-to-date one.
    bool is_y = (code == 'Y' || code == 'y') &&
                !e.RawControlDown() && !e.AltDown() && !e.MetaDown();
    bool is_copy = (code == 'C' || code == 'c') &&
                   (e.RawControlDown() || e.ControlDown()) &&
                   !e.AltDown();
    if (is_y || is_copy) {
        if (const Row *r = selected_row()) {
            const std::string &target =
                r->canonical_url.empty() ? r->url : r->canonical_url;
            copy_to_clipboard(target);
        }
        return;
    }
    // Fall through to the main frame's preset dispatch so user
    // `preset` keys work even when this panel has focus. If no
    // preset matches, the event continues to default handlers
    // via Skip below.
    if (auto *frame = dynamic_cast<MainFrame *>(wxGetTopLevelParent(this))) {
        if (frame->try_preset_key(e)) return;
    }
    e.Skip();
}

void FeedsPanel::save_columns()
{
    db_save_ui_state(app_, "cols.feeds",
                     dataview_serialize_columns(list_).c_str());
}

void FeedsPanel::append_column(const wxString &title)
{
    const int flags = wxDATAVIEW_COL_RESIZABLE |
                      wxDATAVIEW_COL_REORDERABLE |
                      wxDATAVIEW_COL_SORTABLE;
    if (title == "Title") {
        list_->AppendTextColumn("Title", 0, wxDATAVIEW_CELL_INERT,
                                FromDIP(200), wxALIGN_LEFT, flags);
    } else if (title == "Updated") {
        list_->AppendTextColumn("Updated", 1, wxDATAVIEW_CELL_INERT,
                                FromDIP(90), wxALIGN_LEFT, flags);
    } else if (title == "Canonical URL") {
        // Power-user column; hidden at construction until the user
        // enables it from the right-click header menu.
        auto *col =
            list_->AppendTextColumn("Canonical URL", 2,
                                    wxDATAVIEW_CELL_INERT,
                                    FromDIP(300), wxALIGN_LEFT, flags);
        col->SetHidden(true);
    }
}

void FeedsPanel::build_columns(const std::vector<std::string> &order)
{
    list_->ClearColumns();
    // Append titles in the requested order first, skipping anything
    // we don't recognize (e.g. a column removed between app versions).
    std::unordered_set<std::string> known(default_order_.begin(),
                                          default_order_.end());
    std::unordered_set<std::string> added;
    for (const auto &t : order) {
        if (!known.count(t) || added.count(t)) continue;
        append_column(wxString::FromUTF8(t));
        added.insert(t);
    }
    // Fall back to default order for anything not in the saved state.
    // Handles first run (empty `order`) and newly-added columns.
    for (const auto &t : default_order_) {
        if (added.count(t)) continue;
        append_column(wxString::FromUTF8(t));
    }
}

void FeedsPanel::reset_layout()
{
    // ClearColumns + rebuild in default order gives us fresh columns
    // at construction-time widths + hidden flags, and clears any
    // header-click sort state for free (sort is attached to columns).
    build_columns(default_order_);
    db_save_ui_state(app_, "cols.feeds", "");
    db_save_ui_state(app_, "sort.feeds", "");
    refresh();
}

void FeedsPanel::refresh()
{
    // Snapshot (url, display title) pairs in display order. Display
    // title comes from the DB-backed map (which honors user_title
    // overrides), with a fallback to the raw URL for feeds that
    // haven't been fetched yet.
    rows_.clear();
    rows_.reserve(app_->feeds.size());
    for (auto &f : app_->feeds) {
        Row r;
        r.url = f.url;
        auto it = app_->feed_titles.find(f.url);
        r.title = (it != app_->feed_titles.end()) ? it->second : f.url;
        // Only surface canonical_url when it actually differs from
        // the configured URL — avoids redundant "redirects to
        // itself" noise on feeds where canonical_url got saved
        // pre-change or matches for unrelated reasons.
        if (!f.canonical_url.empty() && f.canonical_url != f.url)
            r.canonical_url = f.canonical_url;
        r.updated = f.last_update;
        rows_.push_back(std::move(r));
    }

    apply_sort();
    model_->Reset((unsigned int)rows_.size());
}

void FeedsPanel::on_activated(wxDataViewEvent &e)
{
    wxDataViewItem item = e.GetItem();
    if (!item.IsOk()) return;
    unsigned row = model_->GetRow(item);
    if (row < rows_.size() && on_activate_)
        on_activate_(rows_[row].url);
}
