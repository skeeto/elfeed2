#include "entry_detail.hpp"

#include <wx/font.h>
#include <wx/hyperlink.h>
#include <wx/html/htmlwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <ctime>

EntryDetail::EntryDetail(wxWindow *parent, Elfeed *app)
    : wxPanel(parent, wxID_ANY)
    , app_(app)
{
    title_ = new wxStaticText(this, wxID_ANY, "(no entry selected)");
    wxFont title_font = title_->GetFont();
    title_font.SetWeight(wxFONTWEIGHT_BOLD);
    title_font.SetPointSize(title_font.GetPointSize() + 2);
    title_->SetFont(title_font);

    subtitle_ = new wxStaticText(this, wxID_ANY, "");
    // wxHyperlinkCtrl rejects an empty label+URL pair. Seed it with a
    // placeholder that show_entry() overwrites.
    link_ = new wxHyperlinkCtrl(this, wxID_ANY, " ", "about:blank");
    link_->Hide();

    body_ = new wxHtmlWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxHW_SCROLLBAR_AUTO);

    auto *sz = new wxBoxSizer(wxVERTICAL);
    int pad = FromDIP(6);
    sz->Add(title_,    0, wxEXPAND | wxALL, pad);
    sz->Add(subtitle_, 0, wxEXPAND | wxLEFT | wxRIGHT, pad);
    sz->Add(link_,     0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, pad);
    sz->Add(body_,     1, wxEXPAND);
    SetSizer(sz);

    show_entry(nullptr);
}

static std::string fmt_date(double epoch)
{
    time_t t = (time_t)epoch;
    struct tm tm;
    localtime_r(&t, &tm);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

void EntryDetail::show_entry(const Entry *e)
{
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
    for (auto &f : app_->feeds) {
        if (f.url == e->feed_url) { feed_title = f.title; break; }
    }

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

    std::string sub = feed_title + "  ·  " + fmt_date(e->date);
    if (!authors.empty()) sub += "  ·  " + authors;
    if (!tags.empty()) sub += "  (" + tags + ")";
    subtitle_->SetLabel(wxString::FromUTF8(sub));

    if (!e->link.empty()) {
        link_->SetLabel(wxString::FromUTF8(e->link));
        link_->SetURL(wxString::FromUTF8(e->link));
        link_->Show();
    } else {
        link_->Hide();
    }

    // wxHtmlWindow expects HTML; if content_type was plain, escape it.
    if (!e->content.empty()) {
        if (e->content_type == "text" || e->content_type.empty()) {
            // Best-effort: render as HTML. Atom/RSS usually have HTML
            // content anyway.
            body_->SetPage(wxString::FromUTF8(e->content));
        } else {
            body_->SetPage(wxString::FromUTF8(e->content));
        }
    } else {
        body_->SetPage("<p><i>(no content)</i></p>");
    }

    Layout();
}
