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

    // Persist current column widths/visibility into the DB.
    void save_columns();

    // Snapshot of the rows currently displayed, in display order.
    // Read by the model; lookup target for the activate event.
    struct Row {
        std::string url;
        std::string title;
        // Populated only when the feed's fetched URL resolved through
        // a redirect to a different URL. Empty means "no redirect".
        std::string canonical_url;
        double updated;
    };

private:
    void on_activated(wxDataViewEvent &);
    void on_context_menu(wxDataViewEvent &);
    void on_key(wxKeyEvent &);

    // Copy `text` to the clipboard and flash a status line so the user
    // has some feedback that something happened. Returns false if the
    // clipboard couldn't be opened.
    bool copy_to_clipboard(const std::string &text);

    // Return the currently-selected row (if any). Used by the
    // right-click menu and keystroke handlers.
    const Row *selected_row() const;

    Elfeed *app_;
    std::function<void(const std::string &)> on_activate_;
    wxDataViewCtrl *list_ = nullptr;
    wxObjectDataPtr<FeedsPanelModel> model_;

    friend class FeedsPanelModel;
    std::vector<Row> rows_;
};

#endif
