#include "downloads_panel.hpp"
#include "util.hpp"

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/variant.h>

#include <unordered_set>

class DownloadsPanelModel : public wxDataViewVirtualListModel {
public:
    DownloadsPanelModel(DownloadsPanel *owner) : owner_(owner) {}

    unsigned int GetColumnCount() const override { return 4; }
    wxString GetColumnType(unsigned int) const override { return "string"; }

    void GetValueByRow(wxVariant &value,
                       unsigned int row,
                       unsigned int col) const override
    {
        if (row >= owner_->snapshot_.size()) { value = wxString(); return; }
        const DownloadsPanel::Row &r = owner_->snapshot_[row];
        switch (col) {
        case 0: {
            wxString pct;
            if (r.active)
                pct = r.progress.empty() ? wxString("...")
                                         : wxString::FromUTF8(r.progress);
            else if (r.paused)
                pct = "paused";
            else
                pct = "queued";
            value = pct;
            return;
        }
        case 1: value = wxString::FromUTF8(r.total); return;
        case 2: value = wxString::FromUTF8(r.name);  return;
        case 3:
            value = r.failures > 0 ? wxString::Format("%d", r.failures)
                                   : wxString();
            return;
        }
        value = wxString();
    }

    bool SetValueByRow(const wxVariant &, unsigned int, unsigned int) override
    {
        return false;
    }

    // See EntryListModel::Compare — we sort owner_->snapshot_
    // ourselves, so the backend's native sort must be a no-op.
    int Compare(const wxDataViewItem &, const wxDataViewItem &,
                unsigned int, bool) const override
    {
        return 0;
    }

private:
    DownloadsPanel *owner_;
};

DownloadsPanel::DownloadsPanel(wxWindow *parent, Elfeed *app)
    : wxPanel(parent, wxID_ANY)
    , app_(app)
{
    auto *vsz = new wxBoxSizer(wxVERTICAL);

    auto *hsz = new wxBoxSizer(wxHORIZONTAL);
    btn_pause_  = new wxButton(this, wxID_ANY, "Pause/Resume");
    btn_remove_ = new wxButton(this, wxID_ANY, "Remove");
    hsz->Add(btn_pause_,  0, wxALL, FromDIP(4));
    hsz->Add(btn_remove_, 0, wxALL, FromDIP(4));
    vsz->Add(hsz, 0, wxEXPAND);

    list_ = new wxDataViewCtrl(this, wxID_ANY,
                               wxDefaultPosition, wxDefaultSize,
                               wxDV_MULTIPLE | wxDV_ROW_LINES |
                               wxDV_VERT_RULES);
    model_ = new DownloadsPanelModel(this);
    list_->AssociateModel(model_.get());

    default_order_ = {"%", "Size", "Name", "Fails"};
    std::string saved_cols = db_load_ui_state(app_, "cols.downloads");
    build_columns(dataview_parse_column_order(saved_cols));
    dataview_apply_columns(list_, saved_cols);
    dataview_apply_sort(list_, db_load_ui_state(app_, "sort.downloads"));
    list_->Bind(wxEVT_DATAVIEW_COLUMN_HEADER_RIGHT_CLICK,
                [this](wxDataViewEvent &) {
                    dataview_show_column_menu(list_,
                                              [this] { save_columns(); });
                });
    list_->Bind(wxEVT_DATAVIEW_COLUMN_SORTED,
                &DownloadsPanel::on_sort, this);
    vsz->Add(list_, 1, wxEXPAND);

    SetSizer(vsz);

    btn_pause_->Bind(wxEVT_BUTTON,  &DownloadsPanel::on_pause,  this);
    btn_remove_->Bind(wxEVT_BUTTON, &DownloadsPanel::on_remove, this);

    refresh();
}

static int ci_cmp_dl(const std::string &a, const std::string &b)
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

// Extract the leading number from a progress or size string like
// "73.4%" or "114.29MiB". Used for numeric sort on columns whose
// display text is not straight numeric.
static double parse_leading_number(const std::string &s)
{
    size_t i = 0;
    while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
    size_t start = i;
    while (i < s.size() && (std::isdigit((unsigned char)s[i]) ||
                            s[i] == '.' || s[i] == '-'))
        i++;
    if (i == start) return 0;
    try { return std::stod(s.substr(start, i - start)); }
    catch (...) { return 0; }
}

void DownloadsPanel::apply_sort()
{
    DataViewSort s = dataview_current_sort(list_);
    if (s.col < 0) return;  // insertion order — handled elsewhere
    std::stable_sort(
        snapshot_.begin(), snapshot_.end(),
        [col = s.col, asc = s.ascending](const Row &a, const Row &b) {
            int c = 0;
            switch (col) {
            case 0:
                c = (int)(parse_leading_number(a.progress) -
                          parse_leading_number(b.progress) > 0) -
                    (int)(parse_leading_number(a.progress) -
                          parse_leading_number(b.progress) < 0);
                break;
            case 1: {
                double na = parse_leading_number(a.total);
                double nb = parse_leading_number(b.total);
                if (na != nb) c = na < nb ? -1 : 1;
                break;
            }
            case 2: c = ci_cmp_dl(a.name, b.name); break;
            case 3: c = a.failures - b.failures;  break;
            }
            return asc ? c < 0 : c > 0;
        });
}

void DownloadsPanel::refresh()
{
    // Build new snapshot. Downloads live on the UI thread only.
    std::vector<Row> new_snap;
    new_snap.reserve(app_->downloads.size());
    int active_id = app_->download_active_id;
    for (auto &d : app_->downloads) {
        Row r;
        r.id = d.id;
        r.paused = d.paused;
        r.active = (d.id == active_id);
        r.progress = d.progress;
        r.total = d.total;
        r.name = d.title.empty() ? d.url : d.title;
        r.failures = d.failures;
        new_snap.push_back(std::move(r));
    }

    // If the user has picked a column header sort, the differential
    // update below doesn't apply (snapshot_ and new_snap don't stay
    // aligned — rows reorder as progress ticks change their rank).
    // Fall back to the simple rebuild path in that case. For the
    // "no sort" (insertion order) default we keep the differential
    // update so selection and scroll survive frequent refreshes.
    DataViewSort sort = dataview_current_sort(list_);
    if (sort.col >= 0) {
        snapshot_ = std::move(new_snap);
        apply_sort();
        model_->Reset((unsigned int)snapshot_.size());
        return;
    }

    // Differential update path (default order):
    // download.cpp doesn't reorder app->downloads (only push_back for
    // new, erase for finished), so old and new snapshots are aligned
    // subsequences.

    // Phase 1: remove rows whose id is gone. Walk backward so prior
    // indices stay valid as we delete.
    std::unordered_set<int> new_ids;
    new_ids.reserve(new_snap.size());
    for (auto &r : new_snap) new_ids.insert(r.id);
    for (size_t i = snapshot_.size(); i-- > 0; ) {
        if (!new_ids.count(snapshot_[i].id)) {
            snapshot_.erase(snapshot_.begin() + i);
            model_->RowDeleted((unsigned int)i);
        }
    }

    // Phase 2: align snapshot_ to new_snap. Where the id matches, just
    // refresh the row data and emit RowChanged (cheap). Where it
    // doesn't, the new row is brand new — insert it.
    for (size_t i = 0; i < new_snap.size(); i++) {
        if (i >= snapshot_.size() || snapshot_[i].id != new_snap[i].id) {
            snapshot_.insert(snapshot_.begin() + i, new_snap[i]);
            model_->RowInserted((unsigned int)i);
        } else {
            snapshot_[i] = new_snap[i];
            model_->RowChanged((unsigned int)i);
        }
    }
}

void DownloadsPanel::on_sort(wxDataViewEvent &)
{
    apply_sort();
    model_->Reset((unsigned int)snapshot_.size());
    db_save_ui_state(app_, "sort.downloads",
                     dataview_serialize_sort(list_).c_str());
}

void DownloadsPanel::on_pause(wxCommandEvent &)
{
    wxDataViewItemArray sel;
    list_->GetSelections(sel);
    for (auto &item : sel) {
        if (!item.IsOk()) continue;
        unsigned row = model_->GetRow(item);
        if (row < snapshot_.size())
            download_pause(app_, snapshot_[row].id);
    }
    refresh();
}

void DownloadsPanel::on_remove(wxCommandEvent &)
{
    // Collect ids first; download_remove mutates the queue.
    wxDataViewItemArray sel;
    list_->GetSelections(sel);
    std::vector<int> ids;
    for (auto &item : sel) {
        if (!item.IsOk()) continue;
        unsigned row = model_->GetRow(item);
        if (row < snapshot_.size()) ids.push_back(snapshot_[row].id);
    }
    for (int id : ids) download_remove(app_, id);
    refresh();
}

void DownloadsPanel::save_columns()
{
    db_save_ui_state(app_, "cols.downloads",
                     dataview_serialize_columns(list_).c_str());
}

void DownloadsPanel::append_column(const wxString &title)
{
    const int flags = wxDATAVIEW_COL_RESIZABLE |
                      wxDATAVIEW_COL_REORDERABLE |
                      wxDATAVIEW_COL_SORTABLE;
    if (title == "%") {
        list_->AppendTextColumn("%",     0, wxDATAVIEW_CELL_INERT,
                                FromDIP(80),  wxALIGN_LEFT, flags);
    } else if (title == "Size") {
        list_->AppendTextColumn("Size",  1, wxDATAVIEW_CELL_INERT,
                                FromDIP(80),  wxALIGN_LEFT, flags);
    } else if (title == "Name") {
        list_->AppendTextColumn("Name",  2, wxDATAVIEW_CELL_INERT,
                                FromDIP(380), wxALIGN_LEFT, flags);
    } else if (title == "Fails") {
        list_->AppendTextColumn("Fails", 3, wxDATAVIEW_CELL_INERT,
                                FromDIP(50),  wxALIGN_LEFT, flags);
    }
}

void DownloadsPanel::build_columns(const std::vector<std::string> &order)
{
    list_->ClearColumns();
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

void DownloadsPanel::reset_layout()
{
    build_columns(default_order_);
    db_save_ui_state(app_, "cols.downloads", "");
    db_save_ui_state(app_, "sort.downloads", "");
    // Force a full rebuild — the differential update path assumes
    // aligned snapshots, which doesn't hold after clearing state.
    snapshot_.clear();
    model_->Reset(0);
    refresh();
}
