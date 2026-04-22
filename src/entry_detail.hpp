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

    // Show the given entry. Pass nullptr to clear. The pointer is
    // cached so relayout() can re-render after images land from the
    // background cache. Callers must keep the pointed-to Entry alive
    // (typically app->entries[row]) until show_entry is called again.
    void show_entry(const Entry *entry);

    // Re-render the currently-shown entry. Called after new image
    // bytes arrive in the cache so wxHtmlWindow picks them up. Scroll
    // position is preserved so the user's reading spot doesn't jump.
    void relayout();

    // Give keyboard focus to the HTML body (the only tabstop child).
    // Used by MainFrame when Enter is pressed in the listing to drop
    // the user into reader mode.
    void focus_body();

    // Scroll the HTML body by `lines` lines (negative = up). Called
    // from MainFrame::on_detail_key for j/k in reader mode — the
    // html window doesn't bind letter keys to scrolling, so we
    // have to drive it explicitly.
    void scroll_lines(int lines);

private:
    void on_link_click(wxMouseEvent &);
    void render();  // rebuild HTML from current_ and push to body_

    Elfeed *app_;
    wxStaticText *title_ = nullptr;
    wxStaticText *subtitle_ = nullptr;
    wxStaticText *link_ = nullptr;
    std::string   link_url_;   // click target for link_
    wxHtmlWindow *body_ = nullptr;
    const Entry  *current_ = nullptr;
};

#endif
