#include "log_panel.hpp"
#include "util.hpp"

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/clipbrd.h>
#include <wx/filedlg.h>
#include <wx/file.h>
#include <wx/menu.h>
#include <wx/sizer.h>
#include <wx/variant.h>

#include <unordered_set>

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

    // See EntryListModel::Compare — we sort owner_->snapshot_
    // ourselves, so the backend's native sort must be a no-op.
    int Compare(const wxDataViewItem &, const wxDataViewItem &,
                unsigned int, bool) const override
    {
        return 0;
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

    default_order_ = {"Time", "Type", "URL", "Result"};
    std::string saved_cols = db_load_ui_state(app_, "cols.log");
    build_columns(dataview_parse_column_order(saved_cols));
    dataview_apply_columns(list_, saved_cols);
    dataview_apply_sort(list_, db_load_ui_state(app_, "sort.log"));
    list_->Bind(wxEVT_DATAVIEW_COLUMN_HEADER_RIGHT_CLICK,
                [this](wxDataViewEvent &) {
                    dataview_show_column_menu(list_,
                                              [this] { save_columns(); });
                });
    list_->Bind(wxEVT_DATAVIEW_COLUMN_SORTED,
                &LogPanel::on_sort, this);
    list_->Bind(wxEVT_DATAVIEW_ITEM_CONTEXT_MENU,
                &LogPanel::on_context_menu, this);
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

static int ci_compare_str(const std::string &a, const std::string &b)
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

void LogPanel::apply_sort()
{
    DataViewSort s = dataview_current_sort(list_);
    // Default: leave chronological order untouched. Auto-scroll
    // below relies on the last row being the newest message.
    if (s.col < 0) return;
    std::stable_sort(
        snapshot_.begin(), snapshot_.end(),
        [col = s.col, asc = s.ascending]
        (const LogEntry &a, const LogEntry &b) {
            int c = 0;
            switch (col) {
            case 0:
                if (a.time != b.time) c = a.time < b.time ? -1 : 1;
                break;
            case 1:
                // LogKind enum order already reflects severity
                // roughly; numeric compare is cheap and stable.
                c = (int)a.kind - (int)b.kind;
                break;
            case 2: // URL and Result share the message string, split
            case 3: // on the first ": " — same split the model does.
            {
                auto ca = a.message.find(": ");
                auto cb = b.message.find(": ");
                std::string as = (col == 2 && ca != std::string::npos)
                    ? a.message.substr(0, ca)
                    : (ca != std::string::npos
                        ? a.message.substr(ca + 2) : a.message);
                std::string bs = (col == 2 && cb != std::string::npos)
                    ? b.message.substr(0, cb)
                    : (cb != std::string::npos
                        ? b.message.substr(cb + 2) : b.message);
                if (col != 2 && ca == std::string::npos) as = {};
                if (col != 2 && cb == std::string::npos) bs = {};
                c = ci_compare_str(as, bs);
                break;
            }
            }
            return asc ? c < 0 : c > 0;
        });
}

void LogPanel::refresh()
{
    // Called from on_wake for every UI wake event — many per second
    // during a fetch storm, almost all unrelated to the log. Skip
    // the full snapshot rebuild when neither the log size nor the
    // kind-filter mask has changed; otherwise the unconditional
    // model->Reset() below scrolls the view back to the top and
    // makes the log un-scrollable while anything is ticking.
    size_t current_size;
    {
        std::lock_guard lock(app_->log_mutex);
        current_size = app_->log.size();
    }
    int filter_mask =
        (app_->log_show_info     ? 1 : 0) |
        (app_->log_show_requests ? 2 : 0) |
        (app_->log_show_success  ? 4 : 0) |
        (app_->log_show_errors   ? 8 : 0);

    bool need_rebuild =
        (current_size != last_log_size_) ||
        (filter_mask  != last_filter_mask_);

    if (need_rebuild) {
        snapshot_.clear();
        {
            std::lock_guard lock(app_->log_mutex);
            snapshot_.reserve(app_->log.size());
            for (auto &e : app_->log) {
                bool keep = false;
                switch (e.kind) {
                case LOG_INFO:    keep = app_->log_show_info;     break;
                case LOG_REQUEST: keep = app_->log_show_requests; break;
                case LOG_SUCCESS: keep = app_->log_show_success;  break;
                case LOG_ERROR:   keep = app_->log_show_errors;   break;
                }
                if (keep) snapshot_.push_back(e);
            }
        }
        apply_sort();
        model_->Reset((unsigned int)snapshot_.size());
        last_log_size_    = current_size;
        last_filter_mask_ = filter_mask;
    }

    // Auto-scroll only makes sense when the newest message is last —
    // i.e. chronological (no sort, or Time ascending). For other
    // orderings we'd fight the user; leave scroll where it is.
    //
    // And only trigger it when there's a reason: fresh data just
    // arrived (need_rebuild) OR the user just flipped auto-scroll
    // ON (so the panel should catch up to the bottom). Otherwise
    // this call would fight the user's manual scroll.
    bool autoscroll_on      = app_->log_auto_scroll;
    bool autoscroll_enabled = autoscroll_on && !last_autoscroll_;
    last_autoscroll_ = autoscroll_on;

    DataViewSort s = dataview_current_sort(list_);
    bool chronological = (s.col < 0) || (s.col == 0 && s.ascending);
    bool should_scroll =
        autoscroll_on && chronological && !snapshot_.empty() &&
        (need_rebuild || autoscroll_enabled);
    if (should_scroll) {
        wxDataViewItem last = model_->GetItem(
            (unsigned int)snapshot_.size() - 1);
        list_->EnsureVisible(last);
    }
}

void LogPanel::on_sort(wxDataViewEvent &)
{
    apply_sort();
    model_->Reset((unsigned int)snapshot_.size());
    db_save_ui_state(app_, "sort.log",
                     dataview_serialize_sort(list_).c_str());
}

void LogPanel::on_context_menu(wxDataViewEvent &event)
{
    wxDataViewItem item = event.GetItem();
    if (!item.IsOk()) return;
    if (!list_->IsSelected(item)) {
        list_->UnselectAll();
        list_->Select(item);
    }

    // Whether to enable Copy depends on having a selection — empty
    // selection (right-click on empty area) shouldn't be reachable
    // here since item.IsOk() above would have returned false.
    enum { ID_Copy = wxID_HIGHEST + 1, ID_Export, ID_Clear };
    wxMenu menu;
    menu.Append(ID_Copy,   "&Copy");
    menu.Append(ID_Export, "&Export to File…");
    menu.AppendSeparator();
    menu.Append(ID_Clear,  "Clear &Log");

    int choice = list_->GetPopupMenuSelectionFromUser(menu);
    if (choice == ID_Export) {
        // Save the FULL in-memory log (which includes whatever we
        // loaded from DB on startup plus this session's entries),
        // ignoring panel filter checkboxes — bug reports want the
        // unfiltered context. Header lines name the program and
        // export time so the recipient can tell what they're
        // looking at.
        wxFileDialog dlg(this, "Export Log", wxString(),
                         "elfeed2-log.txt",
                         "Text files (*.txt)|*.txt|All files|*",
                         wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dlg.ShowModal() != wxID_OK) return;
        std::vector<LogEntry> all;
        {
            std::lock_guard lock(app_->log_mutex);
            all = app_->log;
        }
        std::string out;
        out += "Elfeed2 " ELFEED_VERSION "\n";
        out += "Exported " + format_datetime((double)::time(nullptr));
        out += "\n";
        out += "----------------------------------------\n";
        for (auto &e : all) {
            const char *kind = "?";
            switch (e.kind) {
            case LOG_INFO:    kind = "info"; break;
            case LOG_REQUEST: kind = "req";  break;
            case LOG_SUCCESS: kind = "ok";   break;
            case LOG_ERROR:   kind = "err";  break;
            }
            out += format_datetime(e.time);
            out += " [";
            out += kind;
            out += "] ";
            out += e.message;
            out += "\n";
        }
        wxFile f(dlg.GetPath(), wxFile::write);
        if (f.IsOpened()) f.Write(wxString::FromUTF8(out));
        return;
    }
    if (choice == ID_Copy) {
        // Stitch the selected rows into a clipboard string. Each
        // line is "<datetime> [<kind>] <message>" — same shape as
        // the panel itself, so what you copy reads like what you
        // see. Cell alignment isn't preserved, but a textual log
        // pasted into a chat or issue tracker doesn't need that.
        wxDataViewItemArray sel;
        list_->GetSelections(sel);
        std::string out;
        for (auto &it : sel) {
            unsigned r = model_->GetRow(it);
            if (r >= snapshot_.size()) continue;
            const LogEntry &e = snapshot_[r];
            const char *kind = "?";
            switch (e.kind) {
            case LOG_INFO:    kind = "info"; break;
            case LOG_REQUEST: kind = "req";  break;
            case LOG_SUCCESS: kind = "ok";   break;
            case LOG_ERROR:   kind = "err";  break;
            }
            if (!out.empty()) out += '\n';
            out += format_datetime(e.time);
            out += " [";
            out += kind;
            out += "] ";
            out += e.message;
        }
        if (!out.empty() && wxTheClipboard->Open()) {
            wxTheClipboard->SetData(
                new wxTextDataObject(wxString::FromUTF8(out)));
            wxTheClipboard->Close();
        }
    } else if (choice == ID_Clear) {
        wxCommandEvent dummy;
        on_clear(dummy);
    }
}

void LogPanel::on_filter_changed(wxCommandEvent &)
{
    app_->log_show_info     = cb_info_->GetValue();
    app_->log_show_requests = cb_req_->GetValue();
    app_->log_show_success  = cb_ok_->GetValue();
    app_->log_show_errors   = cb_err_->GetValue();
    app_->log_auto_scroll   = cb_autoscroll_->GetValue();
    // Persist so the next launch starts with the same kinds
    // visible / hidden as the user last had.
    auto save = [&](const char *k, bool v) {
        db_save_ui_state(app_, k, v ? "1" : "0");
    };
    save("log.show_info",     app_->log_show_info);
    save("log.show_requests", app_->log_show_requests);
    save("log.show_success",  app_->log_show_success);
    save("log.show_errors",   app_->log_show_errors);
    save("log.auto_scroll",   app_->log_auto_scroll);
    refresh();
}

void LogPanel::on_clear(wxCommandEvent &)
{
    {
        std::lock_guard lock(app_->log_mutex);
        app_->log.clear();
        // Reset the persisted-up-to mark — we just dropped every
        // entry from memory and the DB. The next log_drain_to_db
        // starts fresh from index 0.
        app_->log_db_committed = 0;
    }
    db_log_clear(app_);
    refresh();
}

void LogPanel::save_columns()
{
    db_save_ui_state(app_, "cols.log",
                     dataview_serialize_columns(list_).c_str());
}

void LogPanel::append_column(const wxString &title)
{
    const int flags = wxDATAVIEW_COL_RESIZABLE |
                      wxDATAVIEW_COL_REORDERABLE |
                      wxDATAVIEW_COL_SORTABLE;
    if (title == "Time") {
        list_->AppendTextColumn("Time",   0, wxDATAVIEW_CELL_INERT,
                                FromDIP(140), wxALIGN_LEFT, flags);
    } else if (title == "Type") {
        list_->AppendTextColumn("Type",   1, wxDATAVIEW_CELL_INERT,
                                FromDIP(50),  wxALIGN_LEFT, flags);
    } else if (title == "URL") {
        list_->AppendTextColumn("URL",    2, wxDATAVIEW_CELL_INERT,
                                FromDIP(250), wxALIGN_LEFT, flags);
    } else if (title == "Result") {
        list_->AppendTextColumn("Result", 3, wxDATAVIEW_CELL_INERT,
                                FromDIP(400), wxALIGN_LEFT, flags);
    }
}

void LogPanel::build_columns(const std::vector<std::string> &order)
{
    list_->ClearColumns();
    for (const auto &t :
         dataview_merge_column_order(order, default_order_)) {
        append_column(wxString::FromUTF8(t));
    }
}

void LogPanel::reset_layout()
{
    build_columns(default_order_);
    db_save_ui_state(app_, "cols.log", "");
    db_save_ui_state(app_, "sort.log", "");
    refresh();
}
