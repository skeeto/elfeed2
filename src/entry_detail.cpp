#include "entry_detail.hpp"
#include "image_cache.hpp"
#include "main_frame.hpp"
#include "util.hpp"

#include <wx/colour.h>
#include <wx/cursor.h>
#include <wx/font.h>
#include <wx/html/htmlwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>

#include <cstdio>

EntryDetail::EntryDetail(wxWindow *parent, Elfeed *app)
    : wxPanel(parent, wxID_ANY)
    , app_(app)
{
    // wxST_ELLIPSIZE_* only affects drawing — wxStaticText's best-size
    // still reflects the full label width, so the sizer still pins the
    // pane's min width unless we also force a small MinSize. Picking a
    // tiny floor lets the sizer shrink the widget freely; the ellipsize
    // style kicks in to truncate the displayed text.
    const wxSize kShrinkable(FromDIP(20), -1);

    title_ = new wxStaticText(this, wxID_ANY, "(no entry selected)",
                              wxDefaultPosition, wxDefaultSize,
                              wxST_ELLIPSIZE_END);
    wxFont title_font = title_->GetFont();
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    title_font.SetPointSize(title_font.GetPointSize() + 2);
    title_->SetFont(title_font);
    title_->SetMinSize(kShrinkable);

    subtitle_ = new wxStaticText(this, wxID_ANY, "",
                                 wxDefaultPosition, wxDefaultSize,
                                 wxST_ELLIPSIZE_END);
    subtitle_->SetMinSize(kShrinkable);

    // Clickable URL label. We use wxStaticText instead of
    // wxHyperlinkCtrl because the latter has no ellipsize support and
    // a long URL would pin the pane's min width. We apply the
    // hyperlink look (blue + underline) manually and launch the
    // browser on left-click.
    link_ = new wxStaticText(this, wxID_ANY, "",
                             wxDefaultPosition, wxDefaultSize,
                             wxST_ELLIPSIZE_MIDDLE);
    link_->SetForegroundColour(
        wxSystemSettings::GetColour(wxSYS_COLOUR_HOTLIGHT));
    {
        wxFont f = link_->GetFont();
        f.SetUnderlined(true);
        link_->SetFont(f);
    }
    link_->SetCursor(wxCursor(wxCURSOR_HAND));
    link_->Bind(wxEVT_LEFT_UP, &EntryDetail::on_link_click, this);
    link_->SetMinSize(kShrinkable);
    link_->Hide();

    body_ = new wxHtmlWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxHW_SCROLLBAR_AUTO);
    // wxHtmlWindow's default link behavior is to navigate (load the
    // URL as a new HTML page in this same control), which fails for
    // http(s) since we don't register an internet FS handler — and
    // even if we did, in-pane navigation isn't what users want.
    // Route every link click to the system default browser.
    body_->Bind(wxEVT_HTML_LINK_CLICKED,
                [](wxHtmlLinkEvent &e) {
                    wxLaunchDefaultBrowser(e.GetLinkInfo().GetHref());
                });
    // Re-render on system theme switch (light ↔ dark) so the
    // preview's <body> wrapper picks up the new system colors.
    // wxEVT_SYS_COLOUR_CHANGED bubbles up to top-level windows;
    // bind on the panel and forward to relayout (which preserves
    // scroll position).
    Bind(wxEVT_SYS_COLOUR_CHANGED, [this](wxSysColourChangedEvent &e) {
        relayout();
        e.Skip();
    });
    // Forward keypresses to MainFrame's on_detail_key, which
    // dispatches the reader-mode bindings (q/Escape to return to
    // the list, n/p to step entries, b/y/d/u to act on selection)
    // and falls through to the preset dispatcher and wxHtmlWindow's
    // native scrolling for anything else. wxEVT_CHAR_HOOK fires
    // before the html window's own key handling, which is what lets
    // us intercept plain letters at all.
    body_->Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent &e) {
        if (auto *frame =
                dynamic_cast<MainFrame *>(wxGetTopLevelParent(this))) {
            frame->on_detail_key(e);
        } else {
            e.Skip();
        }
    });

    auto *sz = new wxBoxSizer(wxVERTICAL);
    int pad = FromDIP(6);
    sz->Add(title_,    0, wxEXPAND | wxALL, pad);
    sz->Add(subtitle_, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    sz->Add(link_,     0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);
    sz->Add(body_,     1, wxEXPAND);
    SetSizer(sz);

    show_entry(nullptr);
}

void EntryDetail::on_link_click(wxMouseEvent &)
{
    if (!link_url_.empty())
        wxLaunchDefaultBrowser(wxString::FromUTF8(link_url_));
}

void EntryDetail::focus_body()
{
    if (body_) body_->SetFocus();
}

void EntryDetail::scroll_lines(int lines)
{
    // Drive the scroll the same way wxScrolled's native arrow-key
    // handler does: read the current view start and Scroll() to a
    // new row. wxHtmlWindow's ScrollLines() can no-op depending on
    // how its internal scroll rate ends up configured — Scroll()
    // moves a scroll unit reliably, which is also what WXK_DOWN /
    // WXK_UP produce for a single press.
    if (!body_ || lines == 0) return;
    int vx = 0, vy = 0;
    body_->GetViewStart(&vx, &vy);
    int new_vy = vy + lines;
    if (new_vy < 0) new_vy = 0;
    body_->Scroll(vx, new_vy);
}

void EntryDetail::show_entry(Entry *e)
{
    // Same entry as last time → nothing to do. Re-rendering is
    // expensive (DB detail load + HTML build + image cache pass)
    // and the visible content wouldn't change. Callers that want
    // to refresh the *same* entry's display (after a theme switch,
    // after image-cache draining) call relayout() instead, which
    // bypasses this guard. Important on platforms where
    // wxDataViewCtrl's selection-changed event fires on
    // programmatic Select() (macOS): without the guard, j/k would
    // render twice — once from the event handler and once from
    // move_selection's explicit sync_preview() call.
    if (e == current_) return;
    current_ = e;

    if (!e) {
        title_->SetLabel("(no entry selected)");
        subtitle_->SetLabel("");
        link_->Hide();
        body_->SetPage("");
        Layout();
        return;
    }

    // Deferred load: the listing query skips author/enclosure
    // sub-queries for speed. Fill them in now (cheap: one-entry
    // read) so the subtitle and enclosure strip render properly.
    db_entry_load_details(app_, *e);

    title_->SetLabel(wxString::FromUTF8(html_strip(e->title)));

    std::string feed_title;
    auto fit = app_->feed_titles.find(e->feed_url);
    if (fit != app_->feed_titles.end()) feed_title = fit->second;
    else                                feed_title = e->feed_url;

    std::string authors;
    for (auto &a : e->authors) {
        if (!authors.empty()) authors += ", ";
        authors += a.name.empty() ? a.email : a.name;
    }

    std::string tags;
    for (auto &t : e->tags) {
        if (!tags.empty()) tags += ", ";
        tags += t;
    }

    std::string sub = feed_title + "  ·  " + format_date(e->date);
    if (!authors.empty()) sub += "  ·  " + authors;
    if (!tags.empty()) sub += "  (" + tags + ")";
    subtitle_->SetLabel(wxString::FromUTF8(sub));

    if (!e->link.empty()) {
        link_url_ = e->link;
        link_->SetLabel(wxString::FromUTF8(e->link));
        link_->Show();
    } else {
        link_url_.clear();
        link_->Hide();
    }

    render();
    Layout();
}

void EntryDetail::relayout()
{
    // Called when the image cache drained new bytes — the HTML body
    // needs to be re-rendered so wxHtmlWindow picks up the freshly
    // cached images as data URIs. Preserve the user's scroll position
    // so their reading spot doesn't jump.
    if (!current_) return;
    int vx = 0, vy = 0;
    body_->GetViewStart(&vx, &vy);
    render();
    body_->Scroll(vx, vy);
}

void EntryDetail::render()
{
    if (!current_) return;
    const Entry *e = current_;

    // Enclosures rendered above the body. wxHtmlWindow handles
    // <a href> clicks by opening the default browser.
    std::string body;
    if (!e->enclosures.empty()) {
        body += "<p>";
        for (size_t i = 0; i < e->enclosures.size(); i++) {
            const Enclosure &enc = e->enclosures[i];
            if (i > 0) body += "<br>";
            body += "<b>Enclosure:</b> ";
            body += "<a href=\"" + enc.url + "\">" + enc.url + "</a>";
            if (!enc.type.empty() || enc.length > 0) {
                body += " <i>(";
                if (!enc.type.empty()) body += enc.type;
                if (enc.length > 0) {
                    if (!enc.type.empty()) body += ", ";
                    body += std::to_string(enc.length) + " bytes";
                }
                body += ")</i>";
            }
        }
        body += "</p><hr>";
    }

    // wxHtmlWindow expects HTML; Atom/RSS bodies are HTML by convention.
    if (!e->content.empty()) {
        body += e->content;
    } else if (e->enclosures.empty()) {
        body += "<p><i>(no content)</i></p>";
    }

    // Swap external http(s) image URLs for cached data: URIs in the
    // same pass that queues fetches for the ones we don't have yet.
    // Cheap for a document with no images; only the fetched-and-not-
    // yet-cached images trigger background work. Skippable via the
    // `inline-images no` config directive — for slow hardware where
    // wx's image decoding stalls the UI, running with this off
    // leaves the http(s) src untouched so wxHtmlWindow shows its
    // broken-image placeholder without fetching.
    if (app_->inline_images)
        body = image_cache_inline(app_, body);

    // Wrap in a body tag whose colors track the system theme so the
    // preview pane reads correctly in dark mode. wxHtmlWindow has
    // very limited CSS but does honor legacy <body bgcolor= text=
    // link= vlink=> attributes — exactly what we need. Sites that
    // set their own colors inline will still override these, which
    // is fine: this is a baseline, not a theme enforcement.
    wxColour bg   = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
    wxColour fg   = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT);
    wxColour link = wxSystemSettings::GetColour(wxSYS_COLOUR_HOTLIGHT);
    char wrapper[256];
    snprintf(wrapper, sizeof(wrapper),
             "<body bgcolor='#%02x%02x%02x' text='#%02x%02x%02x' "
             "link='#%02x%02x%02x' vlink='#%02x%02x%02x'>",
             bg.Red(), bg.Green(), bg.Blue(),
             fg.Red(), fg.Green(), fg.Blue(),
             link.Red(), link.Green(), link.Blue(),
             link.Red(), link.Green(), link.Blue());
    body = std::string(wrapper) + body + "</body>";

    // Match the wxHtmlWindow's own background to the system color
    // too — without this, the area outside the rendered HTML
    // (margins around content) keeps the wxWidgets default white
    // and looks wrong in dark mode.
    body_->SetBackgroundColour(bg);

    body_->SetPage(wxString::FromUTF8(body));
}
