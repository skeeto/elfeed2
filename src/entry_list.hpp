#ifndef ELFEED_ENTRY_LIST_HPP
#define ELFEED_ENTRY_LIST_HPP

#include <wx/dataview.h>
#include "elfeed.hpp"

class EntryListModel;

// Virtual wxDataViewCtrl displaying `app->entries`. Columns: Date,
// Title, Feed, Tags. Unread entries are drawn bold; read entries dim.
// Multi-select; users can right-click the header to hide columns and
// drag to reorder.
class EntryList : public wxDataViewCtrl {
public:
    EntryList(wxWindow *parent, Elfeed *app);

    // Re-snapshot from app->entries.
    void refresh_items();

    // Mark a single row as needing repaint (e.g. after a tag change).
    void refresh_row(long row);

    // Index of the focused row, or -1 if none.
    long primary() const;

    // Indices of all selected rows, in ascending order.
    std::vector<long> selection() const;

    // Replace the current selection with just `row` (and focus it).
    void select_only(long row);

    // Scroll `row` into view if it isn't already.
    void ensure_visible_row(long row);

    // Persist current column widths/visibility into the DB. Called on
    // app close.
    void save_columns();

    // Re-sort app->entries according to the current column header
    // sort state. Called from refresh_items so new query results
    // land in the expected order.
    void apply_sort();

private:
    void on_sort(wxDataViewEvent &);

    Elfeed *app_;
    wxObjectDataPtr<EntryListModel> model_;
};

#endif
