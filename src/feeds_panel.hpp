#ifndef ELFEED_FEEDS_PANEL_HPP
#define ELFEED_FEEDS_PANEL_HPP

#include <wx/panel.h>
#include <functional>
#include "elfeed.hpp"

class wxListCtrl;
class wxListEvent;

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

private:
    void on_activated(wxListEvent &);

    Elfeed *app_;
    std::function<void(const std::string &)> on_activate_;
    wxListCtrl *list_ = nullptr;

    // Shadow of the rows currently displayed; parallel to list_ items.
    // Stored so on_activated can map row index back to a feed URL
    // regardless of sort order.
    std::vector<std::string> row_urls_;
};

#endif
