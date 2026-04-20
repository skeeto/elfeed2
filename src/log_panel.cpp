#include "log_panel.hpp"
#include "util.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>

// Virtual list that reads from LogPanel::snapshot_.
class LogList : public wxListCtrl {
public:
    LogList(wxWindow *parent, LogPanel *owner)
        : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                     wxLC_REPORT | wxLC_VIRTUAL)
        , owner_(owner)
    {
        AppendColumn("Time",   wxLIST_FORMAT_LEFT, FromDIP(140));
        AppendColumn("Type",   wxLIST_FORMAT_LEFT, FromDIP(50));
        AppendColumn("URL",    wxLIST_FORMAT_LEFT, FromDIP(250));
        AppendColumn("Result", wxLIST_FORMAT_LEFT, FromDIP(400));
    }

protected:
    wxString OnGetItemText(long item, long column) const override;

private:
    LogPanel *owner_;
};

static const char *kind_name(LogKind k)
{
    switch (k) {
    case LOG_INFO:    return "info";
    case LOG_REQUEST: return "req";
    case LOG_SUCCESS: return "ok";
    case LOG_ERROR:   return "err";
    }
    return "?";
}

wxString LogList::OnGetItemText(long item, long column) const
{
    if (item < 0 || (size_t)item >= owner_->snapshot_.size()) return {};
    const LogEntry &e = owner_->snapshot_[(size_t)item];

    switch (column) {
    case 0:
        return wxString::FromUTF8(format_datetime(e.time));
    case 1:
        return wxString::FromUTF8(kind_name(e.kind));
    case 2:
    case 3: {
        auto colon = e.message.find(": ");
        if (colon == std::string::npos) {
            return column == 2 ? wxString::FromUTF8(e.message) : wxString{};
        }
        if (column == 2)
            return wxString::FromUTF8(e.message.substr(0, colon));
        return wxString::FromUTF8(e.message.substr(colon + 2));
    }
    }
    return {};
}

LogPanel::LogPanel(wxWindow *parent, Elfeed *app)
    : wxPanel(parent, wxID_ANY)
    , app_(app)
{
    auto *vsz = new wxBoxSizer(wxVERTICAL);

    // Toolbar-like row of filter checkboxes and Clear button
    auto *hsz = new wxBoxSizer(wxHORIZONTAL);
    cb_info_       = new wxCheckBox(this, wxID_ANY, "Info");
    cb_req_        = new wxCheckBox(this, wxID_ANY, "Requests");
    cb_ok_         = new wxCheckBox(this, wxID_ANY, "Success");
    cb_err_        = new wxCheckBox(this, wxID_ANY, "Errors");
    cb_autoscroll_ = new wxCheckBox(this, wxID_ANY, "Auto-scroll");
    cb_info_->SetValue(app_->log_show_info);
    cb_req_->SetValue(app_->log_show_requests);
    cb_ok_->SetValue(app_->log_show_success);
    cb_err_->SetValue(app_->log_show_errors);
    cb_autoscroll_->SetValue(app_->log_auto_scroll);
    hsz->Add(cb_info_,       0, wxALL, FromDIP(4));
    hsz->Add(cb_req_,        0, wxALL, FromDIP(4));
    hsz->Add(cb_ok_,         0, wxALL, FromDIP(4));
    hsz->Add(cb_err_,        0, wxALL, FromDIP(4));
    hsz->Add(cb_autoscroll_, 0, wxALL, FromDIP(4));
    hsz->AddStretchSpacer();
    auto *btn_clear = new wxButton(this, wxID_CLEAR, "Clear");
    hsz->Add(btn_clear, 0, wxALL, FromDIP(4));
    vsz->Add(hsz, 0, wxEXPAND);

    list_ = new LogList(this, this);
    vsz->Add(list_, 1, wxEXPAND);

    SetSizer(vsz);

    cb_info_->Bind(wxEVT_CHECKBOX, &LogPanel::on_filter_changed, this);
    cb_req_->Bind(wxEVT_CHECKBOX, &LogPanel::on_filter_changed, this);
    cb_ok_->Bind(wxEVT_CHECKBOX, &LogPanel::on_filter_changed, this);
    cb_err_->Bind(wxEVT_CHECKBOX, &LogPanel::on_filter_changed, this);
    cb_autoscroll_->Bind(wxEVT_CHECKBOX, &LogPanel::on_filter_changed, this);
    btn_clear->Bind(wxEVT_BUTTON, &LogPanel::on_clear, this);

    refresh();
}

void LogPanel::refresh()
{
    snapshot_.clear();
    {
        std::lock_guard lock(app_->log_mutex);
        snapshot_.reserve(app_->log.size());
        for (auto &e : app_->log) {
            bool keep = false;
            switch (e.kind) {
            case LOG_INFO:    keep = app_->log_show_info; break;
            case LOG_REQUEST: keep = app_->log_show_requests; break;
            case LOG_SUCCESS: keep = app_->log_show_success; break;
            case LOG_ERROR:   keep = app_->log_show_errors; break;
            }
            if (keep) snapshot_.push_back(e);
        }
    }
    list_->SetItemCount((long)snapshot_.size());
    list_->Refresh();
    if (app_->log_auto_scroll && !snapshot_.empty())
        list_->EnsureVisible((long)snapshot_.size() - 1);
}

void LogPanel::on_filter_changed(wxCommandEvent &)
{
    app_->log_show_info     = cb_info_->GetValue();
    app_->log_show_requests = cb_req_->GetValue();
    app_->log_show_success  = cb_ok_->GetValue();
    app_->log_show_errors   = cb_err_->GetValue();
    app_->log_auto_scroll   = cb_autoscroll_->GetValue();
    refresh();
}

void LogPanel::on_clear(wxCommandEvent &)
{
    {
        std::lock_guard lock(app_->log_mutex);
        app_->log.clear();
    }
    refresh();
}
