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

    const int col_flags = wxDATAVIEW_COL_RESIZABLE |
                          wxDATAVIEW_COL_REORDERABLE;
    list_->AppendTextColumn("%",     0, wxDATAVIEW_CELL_INERT,
                            FromDIP(80),  wxALIGN_LEFT, col_flags);
    list_->AppendTextColumn("Size",  1, wxDATAVIEW_CELL_INERT,
                            FromDIP(80),  wxALIGN_LEFT, col_flags);
    list_->AppendTextColumn("Name",  2, wxDATAVIEW_CELL_INERT,
                            FromDIP(380), wxALIGN_LEFT, col_flags);
    list_->AppendTextColumn("Fails", 3, wxDATAVIEW_CELL_INERT,
                            FromDIP(50),  wxALIGN_LEFT, col_flags);
    dataview_apply_columns(list_, db_load_ui_state(app_, "cols.downloads"));
    list_->Bind(wxEVT_DATAVIEW_COLUMN_HEADER_RIGHT_CLICK,
                [this](wxDataViewEvent &) {
                    dataview_show_column_menu(list_,
                                              [this] { save_columns(); });
                });
    vsz->Add(list_, 1, wxEXPAND);

    SetSizer(vsz);

    btn_pause_->Bind(wxEVT_BUTTON,  &DownloadsPanel::on_pause,  this);
    btn_remove_->Bind(wxEVT_BUTTON, &DownloadsPanel::on_remove, this);

    refresh();
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

    // Differential update so selection and scroll position survive
    // the frequent progress-driven refreshes. download.cpp doesn't
    // reorder app->downloads (only push_back for new, erase for
    // finished), so old and new snapshots are aligned subsequences.

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
