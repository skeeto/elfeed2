#include "feeds_panel.hpp"
#include "util.hpp"

#include <wx/listctrl.h>
#include <wx/sizer.h>

#include <algorithm>
#include <cctype>

FeedsPanel::FeedsPanel(wxWindow *parent, Elfeed *app,
                       std::function<void(const std::string &)> on_activate)
    : wxPanel(parent, wxID_ANY)
    , app_(app)
    , on_activate_(std::move(on_activate))
{
    auto *sz = new wxBoxSizer(wxVERTICAL);

    list_ = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                           wxLC_REPORT | wxLC_SINGLE_SEL);
    list_->AppendColumn("Title",   wxLIST_FORMAT_LEFT, FromDIP(200));
    list_->AppendColumn("Updated", wxLIST_FORMAT_LEFT, FromDIP(90));
    sz->Add(list_, 1, wxEXPAND);
    SetSizer(sz);

    list_->Bind(wxEVT_LIST_ITEM_ACTIVATED,
                &FeedsPanel::on_activated, this);

    refresh();
}

void FeedsPanel::refresh()
{
    // Snapshot (url, display title) pairs so the view stays coherent
    // across later mutations. Display title comes from the DB-backed
    // map (which honors user_title overrides), with a fallback to the
    // raw URL for feeds that haven't been fetched yet.
    struct Row { std::string url, title; double updated; };
    std::vector<Row> rows;
    rows.reserve(app_->feeds.size());
    for (auto &f : app_->feeds) {
        Row r;
        r.url = f.url;
        auto it = app_->feed_titles.find(f.url);
        r.title = (it != app_->feed_titles.end()) ? it->second : f.url;
        r.updated = f.last_update;
        rows.push_back(std::move(r));
    }

    std::sort(rows.begin(), rows.end(),
              [](const Row &a, const Row &b) {
                  // Case-insensitive title sort
                  auto cmp = [](const std::string &x, const std::string &y) {
                      size_t n = std::min(x.size(), y.size());
                      for (size_t i = 0; i < n; i++) {
                          int cx = std::tolower((unsigned char)x[i]);
                          int cy = std::tolower((unsigned char)y[i]);
                          if (cx != cy) return cx < cy;
                      }
                      return x.size() < y.size();
                  };
                  return cmp(a.title, b.title);
              });

    list_->DeleteAllItems();
    row_urls_.clear();
    row_urls_.reserve(rows.size());
    for (size_t i = 0; i < rows.size(); i++) {
        long item = list_->InsertItem((long)i,
                                      wxString::FromUTF8(rows[i].title));
        list_->SetItem(item, 1,
                       wxString::FromUTF8(format_date(rows[i].updated)));
        row_urls_.push_back(std::move(rows[i].url));
    }
}

void FeedsPanel::on_activated(wxListEvent &e)
{
    long idx = e.GetIndex();
    if (idx < 0 || (size_t)idx >= row_urls_.size()) return;
    if (on_activate_) on_activate_(row_urls_[(size_t)idx]);
}
