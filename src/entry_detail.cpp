#include "entry_detail.hpp"
#include "image_cache.hpp"
#include "util.hpp"

#include <wx/cursor.h>
#include <wx/font.h>
#include <wx/html/htmlwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/utils.h>

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

void EntryDetail::show_entry(const Entry *e)
{
    current_ = e;

    if (!e) {
        title_->SetLabel("(no entry selected)");
        subtitle_->SetLabel("");
        link_->Hide();
        body_->SetPage("");
        Layout();
        return;
    }

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
    // yet-cached images trigger background work.
    body = image_cache_inline(app_, body);

    body_->SetPage(wxString::FromUTF8(body));
}
