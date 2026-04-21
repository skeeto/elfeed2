#include "feeds_panel.hpp"
#include "util.hpp"

#include <wx/sizer.h>
#include <wx/variant.h>

#include <algorithm>
#include <cctype>

class FeedsPanelModel : public wxDataViewVirtualListModel {
public:
    FeedsPanelModel(FeedsPanel *owner) : owner_(owner) {}

    unsigned int GetColumnCount() const override { return 2; }
    wxString GetColumnType(unsigned int) const override { return "string"; }

    void GetValueByRow(wxVariant &value,
                       unsigned int row,
                       unsigned int col) const override
    {
        if (row >= owner_->rows_.size()) { value = wxString(); return; }
        const FeedsPanel::Row &r = owner_->rows_[row];
        if (col == 0)      value = wxString::FromUTF8(r.title);
        else if (col == 1) value = wxString::FromUTF8(format_date(r.updated));
        else               value = wxString();
    }

    bool SetValueByRow(const wxVariant &, unsigned int, unsigned int) override
    {
        return false;
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

    const int col_flags = wxDATAVIEW_COL_RESIZABLE |
                          wxDATAVIEW_COL_REORDERABLE;
    list_->AppendTextColumn("Title",   0, wxDATAVIEW_CELL_INERT,
                            FromDIP(200), wxALIGN_LEFT, col_flags);
    list_->AppendTextColumn("Updated", 1, wxDATAVIEW_CELL_INERT,
                            FromDIP(90),  wxALIGN_LEFT, col_flags);
    dataview_apply_columns(list_, db_load_ui_state(app_, "cols.feeds"));
    sz->Add(list_, 1, wxEXPAND);
    SetSizer(sz);

    list_->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED,
                &FeedsPanel::on_activated, this);
    list_->Bind(wxEVT_DATAVIEW_COLUMN_HEADER_RIGHT_CLICK,
                [this](wxDataViewEvent &) {
                    dataview_show_column_menu(list_,
                                              [this] { save_columns(); });
                });

    refresh();
}

void FeedsPanel::save_columns()
{
    db_save_ui_state(app_, "cols.feeds",
                     dataview_serialize_columns(list_).c_str());
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
        r.updated = f.last_update;
        rows_.push_back(std::move(r));
    }

    std::sort(rows_.begin(), rows_.end(),
              [](const Row &a, const Row &b) {
                  // Case-insensitive title sort
                  size_t n = std::min(a.title.size(), b.title.size());
                  for (size_t i = 0; i < n; i++) {
                      int ca = std::tolower((unsigned char)a.title[i]);
                      int cb = std::tolower((unsigned char)b.title[i]);
                      if (ca != cb) return ca < cb;
                  }
                  return a.title.size() < b.title.size();
              });

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
