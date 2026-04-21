#ifndef ELFEED_FEEDS_PANEL_HPP
#define ELFEED_FEEDS_PANEL_HPP

#include <wx/dataview.h>
#include <wx/panel.h>
#include <functional>
#include "elfeed.hpp"

class FeedsPanelModel;

// wxAUI-dockable panel listing all configured feeds. Double-click a
// feed to set the filter to `=<feed_url>` (show only that feed).
class FeedsPanel : public wxPanel {
public:
    // `on_activate` is invoked with the selected feed's URL when the
    // user double-clicks a row. MainFrame uses it to set the filter
    // bar text.
    FeedsPanel(wxWindow *parent, Elfeed *app,
               std::function<void(const std::string &)> on_activate);

    // Re-snapshot from app->feeds and repopulate. UI-thread only.
    void refresh();

    // Snapshot of the rows currently displayed, in display order.
    // Read by the model; lookup target for the activate event.
    struct Row { std::string url, title; double updated; };

private:
    void on_activated(wxDataViewEvent &);

    Elfeed *app_;
    std::function<void(const std::string &)> on_activate_;
    wxDataViewCtrl *list_ = nullptr;
    wxObjectDataPtr<FeedsPanelModel> model_;

    friend class FeedsPanelModel;
    std::vector<Row> rows_;
};

#endif
