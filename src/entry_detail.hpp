#ifndef ELFEED_ENTRY_DETAIL_HPP
#define ELFEED_ENTRY_DETAIL_HPP

#include <wx/panel.h>
#include "elfeed.hpp"

class wxStaticText;
class wxHtmlWindow;
class wxHyperlinkCtrl;

// Right/bottom pane showing the focused entry: small header strip
// (title, feed · date · authors, link) followed by a wxHtmlWindow
// rendering the body content.
class EntryDetail : public wxPanel {
public:
    EntryDetail(wxWindow *parent, Elfeed *app);

    // Show the given entry. Pass nullptr to clear.
    void show_entry(const Entry *entry);

private:
    Elfeed *app_;
    wxStaticText *title_ = nullptr;
    wxStaticText *subtitle_ = nullptr;
    wxHyperlinkCtrl *link_ = nullptr;
    wxHtmlWindow *body_ = nullptr;
};

#endif
