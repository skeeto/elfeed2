#ifndef ELFEED_ENTRY_LIST_HPP
#define ELFEED_ENTRY_LIST_HPP

#include <wx/listctrl.h>
#include "elfeed.hpp"

// Virtual wxListCtrl displaying `app->entries`. Columns: Date, Title,
// Feed, Tags. Unread entries are drawn bold; read entries dim.
// Selection model uses wx's native multi-select with extended style.
class EntryList : public wxListCtrl {
public:
    EntryList(wxWindow *parent, Elfeed *app);

    // Call after `app->entries` changes (filter update, fetch complete)
    // to re-trigger native list redraw.
    void refresh_items();

    // Return the index of the focused/primary item (wxNOT_FOUND if none).
    long primary() const;

    // Indices of all selected items, in ascending order.
    std::vector<long> selection() const;

protected:
    // wxListCtrl virtual-mode callbacks. Called on the UI thread during
    // paint; must be fast.
    wxString OnGetItemText(long item, long column) const override;
    wxListItemAttr *OnGetItemAttr(long item) const override;

private:
    Elfeed *app_;
    // Per-row attributes allocated once; reused via returning the
    // pointer from OnGetItemAttr.
    mutable wxListItemAttr attr_unread_;
    mutable wxListItemAttr attr_read_;
};

#endif
