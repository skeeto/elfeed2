#include "downloads_panel.hpp"

#include <wx/button.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>

#include <unordered_set>

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

    list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxLC_REPORT);
    list_->AppendColumn("%",     wxLIST_FORMAT_LEFT, FromDIP(80));
    list_->AppendColumn("Size",  wxLIST_FORMAT_LEFT, FromDIP(80));
    list_->AppendColumn("Name",  wxLIST_FORMAT_LEFT, FromDIP(380));
    list_->AppendColumn("Fails", wxLIST_FORMAT_LEFT, FromDIP(50));
    vsz->Add(list_, 1, wxEXPAND);

    SetSizer(vsz);

    btn_pause_->Bind(wxEVT_BUTTON,  &DownloadsPanel::on_pause,  this);
    btn_remove_->Bind(wxEVT_BUTTON, &DownloadsPanel::on_remove, this);

    refresh();
}

void DownloadsPanel::refresh()
{
    // Build new snapshot. Downloads live on the UI thread only;
    // no mutex needed.
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

    // Differential update. Wrapping in Freeze/Thaw prevents intermediate
    // repaints when rows are added/removed mid-batch.
    list_->Freeze();

    // Phase 1: drop rows whose ids are no longer present. Walk backward
    // so prior indices stay valid as we delete.
    std::unordered_set<int> new_ids;
    new_ids.reserve(new_snap.size());
    for (auto &r : new_snap) new_ids.insert(r.id);
    for (long i = list_->GetItemCount() - 1; i >= 0; i--) {
        int id = (int)list_->GetItemData(i);
        if (!new_ids.count(id)) list_->DeleteItem(i);
    }

    // Phase 2: for each row in the new snapshot, make sure position i
    // holds its id. Insert a new row if it's not already there; then
    // update column text differentially.
    for (size_t i = 0; i < new_snap.size(); i++) {
        long row = (long)i;
        bool mismatch = row >= list_->GetItemCount() ||
                        (int)list_->GetItemData(row) != new_snap[i].id;
        if (mismatch) {
            list_->InsertItem(row, wxEmptyString);
            list_->SetItemData(row, (wxUIntPtr)new_snap[i].id);
        }
        update_row(row, new_snap[i]);
    }

    list_->Thaw();

    snapshot_ = std::move(new_snap);
}

void DownloadsPanel::update_row(long row, const Row &r)
{
    wxString pct;
    if (r.active)
        pct = r.progress.empty() ? wxString("...")
                                 : wxString::FromUTF8(r.progress);
    else if (r.paused)
        pct = "paused";
    else
        pct = "queued";
    wxString total = wxString::FromUTF8(r.total);
    wxString name  = wxString::FromUTF8(r.name);
    wxString fails = r.failures > 0 ? wxString::Format("%d", r.failures)
                                    : wxString();

    // Only SetItem on columns whose text changed — wx repaints the
    // cell on every SetItem regardless.
    if (list_->GetItemText(row, 0) != pct)   list_->SetItem(row, 0, pct);
    if (list_->GetItemText(row, 1) != total) list_->SetItem(row, 1, total);
    if (list_->GetItemText(row, 2) != name)  list_->SetItem(row, 2, name);
    if (list_->GetItemText(row, 3) != fails) list_->SetItem(row, 3, fails);
}

void DownloadsPanel::on_pause(wxCommandEvent &)
{
    long idx = -1;
    while ((idx = list_->GetNextItem(idx, wxLIST_NEXT_ALL,
                                     wxLIST_STATE_SELECTED)) != -1) {
        int id = (int)list_->GetItemData(idx);
        download_pause(app_, id);
    }
    refresh();
}

void DownloadsPanel::on_remove(wxCommandEvent &)
{
    // Collect selected ids first; download_remove mutates the vector.
    std::vector<int> ids;
    long idx = -1;
    while ((idx = list_->GetNextItem(idx, wxLIST_NEXT_ALL,
                                     wxLIST_STATE_SELECTED)) != -1) {
        ids.push_back((int)list_->GetItemData(idx));
    }
    for (int id : ids) download_remove(app_, id);
    refresh();
}
