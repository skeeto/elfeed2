#ifndef ELFEED_ENTRY_DETAIL_HPP
#define ELFEED_ENTRY_DETAIL_HPP

#include <wx/panel.h>
#include "elfeed.hpp"

class wxStaticText;
class wxHtmlWindow;
class wxMouseEvent;

// Right/bottom pane showing the focused entry: small header strip
// (title, feed · date · authors, link) followed by a wxHtmlWindow
// rendering the body content. All three header rows use
// wxST_ELLIPSIZE_* styles so the pane can be resized narrower than
// the full label width — important when a title or URL is long.
class EntryDetail : public wxPanel {
public:
    EntryDetail(wxWindow *parent, Elfeed *app);

    // Show the given entry. Pass nullptr to clear.
    void show_entry(const Entry *entry);

private:
    void on_link_click(wxMouseEvent &);

    Elfeed *app_;
    wxStaticText *title_ = nullptr;
    wxStaticText *subtitle_ = nullptr;
    wxStaticText *link_ = nullptr;
    std::string   link_url_;   // click target for link_
    wxHtmlWindow *body_ = nullptr;
};

#endif
