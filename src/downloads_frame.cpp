#include "downloads_frame.hpp"

#include <wx/button.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>

DownloadsFrame::DownloadsFrame(wxWindow *parent, Elfeed *app)
    : wxFrame(parent, wxID_ANY, "Downloads", wxDefaultPosition,
              parent->FromDIP(wxSize(700, 360)))
    , app_(app)
{
    auto *panel = new wxPanel(this);
    auto *vsz = new wxBoxSizer(wxVERTICAL);

    auto *hsz = new wxBoxSizer(wxHORIZONTAL);
    btn_pause_  = new wxButton(panel, wxID_ANY, "Pause/Resume");
    btn_remove_ = new wxButton(panel, wxID_ANY, "Remove");
    hsz->Add(btn_pause_,  0, wxALL, FromDIP(4));
    hsz->Add(btn_remove_, 0, wxALL, FromDIP(4));
    vsz->Add(hsz, 0, wxEXPAND);

    list_ = new wxListCtrl(panel, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxLC_REPORT);
    list_->AppendColumn("%",     wxLIST_FORMAT_LEFT, FromDIP(80));
    list_->AppendColumn("Size",  wxLIST_FORMAT_LEFT, FromDIP(80));
    list_->AppendColumn("Name",  wxLIST_FORMAT_LEFT, FromDIP(380));
    list_->AppendColumn("Fails", wxLIST_FORMAT_LEFT, FromDIP(50));
    vsz->Add(list_, 1, wxEXPAND);

    panel->SetSizer(vsz);

    auto *outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(panel, 1, wxEXPAND);
    SetSizer(outer);

    btn_pause_->Bind(wxEVT_BUTTON,  &DownloadsFrame::on_pause,  this);
    btn_remove_->Bind(wxEVT_BUTTON, &DownloadsFrame::on_remove, this);
    Bind(wxEVT_CLOSE_WINDOW, &DownloadsFrame::on_close, this);

    refresh();
}

void DownloadsFrame::refresh()
{
    snapshot_.clear();
    int active_id = app_->download_active_id;
    {
        std::lock_guard lock(app_->download_mutex);
        for (auto &d : app_->downloads) {
            Row r;
            r.id = d.id;
            r.paused = d.paused;
            r.active = (d.id == active_id);
            r.progress = d.progress;
            r.total = d.total;
            r.name = d.title.empty() ? d.url : d.title;
            r.failures = d.failures;
            snapshot_.push_back(std::move(r));
        }
    }

    // wxListCtrl in non-virtual report mode: clear and repopulate. For
    // typical download counts (<20) this is cheap.
    list_->DeleteAllItems();
    for (size_t i = 0; i < snapshot_.size(); i++) {
        const Row &r = snapshot_[i];
        wxString pct;
        if (r.active)
            pct = r.progress.empty() ? "..." : wxString::FromUTF8(r.progress);
        else if (r.paused)
            pct = "paused";
        else
            pct = "queued";
        long item = list_->InsertItem((long)i, pct);
        list_->SetItem(item, 1, wxString::FromUTF8(r.total));
        list_->SetItem(item, 2, wxString::FromUTF8(r.name));
        list_->SetItem(item, 3, r.failures > 0
                                    ? wxString::Format("%d", r.failures)
                                    : wxString());
        list_->SetItemData(item, (wxUIntPtr)r.id);
    }
}

void DownloadsFrame::on_pause(wxCommandEvent &)
{
    long idx = -1;
    while ((idx = list_->GetNextItem(idx, wxLIST_NEXT_ALL,
                                     wxLIST_STATE_SELECTED)) != -1) {
        int id = (int)list_->GetItemData(idx);
        download_pause(app_, id);
    }
    refresh();
}

void DownloadsFrame::on_remove(wxCommandEvent &)
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

void DownloadsFrame::on_close(wxCloseEvent &e)
{
    if (e.CanVeto()) {
        Hide();
        app_->show_downloads = false;
        e.Veto();
    } else {
        Destroy();
    }
}
