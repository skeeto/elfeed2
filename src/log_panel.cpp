#include "log_panel.hpp"
#include "util.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/variant.h>

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

class LogListModel : public wxDataViewVirtualListModel {
public:
    LogListModel(LogPanel *owner) : owner_(owner) {}

    unsigned int GetColumnCount() const override { return 4; }
    wxString GetColumnType(unsigned int) const override { return "string"; }

    void GetValueByRow(wxVariant &value,
                       unsigned int row,
                       unsigned int col) const override
    {
        if (row >= owner_->snapshot_.size()) { value = wxString(); return; }
        const LogEntry &e = owner_->snapshot_[row];
        switch (col) {
        case 0:
            value = wxString::FromUTF8(format_datetime(e.time));
            return;
        case 1:
            value = wxString::FromUTF8(kind_name(e.kind));
            return;
        case 2:
        case 3: {
            auto colon = e.message.find(": ");
            if (colon == std::string::npos) {
                value = (col == 2) ? wxString::FromUTF8(e.message)
                                   : wxString();
                return;
            }
            if (col == 2)
                value = wxString::FromUTF8(e.message.substr(0, colon));
            else
                value = wxString::FromUTF8(e.message.substr(colon + 2));
            return;
        }
        }
        value = wxString();
    }

    bool SetValueByRow(const wxVariant &, unsigned int, unsigned int) override
    {
        return false;
    }

private:
    LogPanel *owner_;
};

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

    list_ = new wxDataViewCtrl(this, wxID_ANY,
                               wxDefaultPosition, wxDefaultSize,
                               wxDV_ROW_LINES | wxDV_VERT_RULES);
    model_ = new LogListModel(this);
    list_->AssociateModel(model_.get());

    const int col_flags = wxDATAVIEW_COL_RESIZABLE |
                          wxDATAVIEW_COL_REORDERABLE;
    list_->AppendTextColumn("Time",   0, wxDATAVIEW_CELL_INERT,
                            FromDIP(140), wxALIGN_LEFT, col_flags);
    list_->AppendTextColumn("Type",   1, wxDATAVIEW_CELL_INERT,
                            FromDIP(50),  wxALIGN_LEFT, col_flags);
    list_->AppendTextColumn("URL",    2, wxDATAVIEW_CELL_INERT,
                            FromDIP(250), wxALIGN_LEFT, col_flags);
    list_->AppendTextColumn("Result", 3, wxDATAVIEW_CELL_INERT,
                            FromDIP(400), wxALIGN_LEFT, col_flags);
    dataview_apply_columns(list_, db_load_ui_state(app_, "cols.log"));
    list_->Bind(wxEVT_DATAVIEW_COLUMN_HEADER_RIGHT_CLICK,
                [this](wxDataViewEvent &) {
                    dataview_show_column_menu(list_,
                                              [this] { save_columns(); });
                });
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
    model_->Reset((unsigned int)snapshot_.size());
    if (app_->log_auto_scroll && !snapshot_.empty()) {
        wxDataViewItem last = model_->GetItem(
            (unsigned int)snapshot_.size() - 1);
        list_->EnsureVisible(last);
    }
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

void LogPanel::save_columns()
{
    db_save_ui_state(app_, "cols.log",
                     dataview_serialize_columns(list_).c_str());
}
