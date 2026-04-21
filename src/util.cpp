#include "util.hpp"

#include <wx/app.h>
#include <wx/dataview.h>
#include <wx/datetime.h>
#include <wx/filename.h>
#include <wx/menu.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

// Ensure wxStandardPaths uses XDG layout on Linux/BSD; on macOS and
// Windows this is a no-op.
static wxStandardPaths &std_paths()
{
    auto &sp = wxStandardPaths::Get();
    sp.SetFileLayout(wxStandardPaths::FileLayout_XDG);
    return sp;
}

std::string elfeed_user_agent()
{
    // "Elfeed2/<ver> (+https://github.com/skeeto/elfeed2; feed reader)"
    // — the "+URL" convention identifies the source to webmasters,
    // and the "feed reader" token makes intent obvious at a glance
    // so servers (and log-reading humans) can treat us accordingly.
    return std::string("Elfeed2/") + ELFEED_VERSION +
           " (+https://github.com/skeeto/elfeed2; feed reader)";
}

std::string user_data_dir()
{
    return std_paths().GetUserDataDir().utf8_string();
}

std::string user_config_dir()
{
    // Every platform: hand-edited config lives at ~/.config/<app>/config
    // (or wherever XDG_CONFIG_HOME points). This puts the file somewhere
    // reachable from a terminal rather than buried under AppData or
    // ~/Library/Application Support. The cascade:
    //   1. $XDG_CONFIG_HOME/<app>   (explicit opt-in, wins on all OSes)
    //   2. $HOME/.config/<app>
    //   3. $USERPROFILE/.config/<app>   (Windows-only fallback)
    // wxGetEnv is wide internally on Windows, so this is Unicode-safe.
    wxString base;
    if (!wxGetEnv("XDG_CONFIG_HOME", &base) || base.empty()) {
        wxString home;
        if (!wxGetEnv("HOME", &home) || home.empty()) {
#ifdef __WXMSW__
            if (!wxGetEnv("USERPROFILE", &home) || home.empty())
                home = wxGetHomeDir();
#else
            home = wxGetHomeDir();
#endif
        }
        base = home + "/.config";
    }
    wxString app = (wxTheApp && !wxTheApp->GetAppName().empty())
        ? wxTheApp->GetAppName()
        : wxString("elfeed2");
    return (base + "/" + app).utf8_string();
}

std::string user_home_dir()
{
    return wxGetHomeDir().utf8_string();
}

bool make_directory(const std::string &path)
{
    return wxFileName::Mkdir(wxString::FromUTF8(path),
                             wxS_DIR_DEFAULT,
                             wxPATH_MKDIR_FULL);
}

std::string format_date(double epoch)
{
    if (epoch <= 0) return {};
    wxDateTime dt((time_t)epoch);
    return dt.Format("%Y-%m-%d").utf8_string();
}

std::string format_datetime(double epoch)
{
    if (epoch <= 0) return {};
    wxDateTime dt((time_t)epoch);
    return dt.Format("%Y-%m-%d %H:%M:%S").utf8_string();
}

std::string format_date_compact(double epoch)
{
    if (epoch <= 0) return {};
    wxDateTime dt((time_t)epoch);
    return dt.Format("%Y%m%d").utf8_string();
}

// ---- Filename building --------------------------------------------

std::string sanitize_filename(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    bool pending_underscore = false;
    for (unsigned char c : s) {
        if (c >= 'A' && c <= 'Z') {
            if (pending_underscore) { out += '_'; pending_underscore = false; }
            out += (char)(c | 0x20);
        } else if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')) {
            if (pending_underscore) { out += '_'; pending_underscore = false; }
            out += (char)c;
        } else if (c >= 0x80) {
            if (pending_underscore) { out += '_'; pending_underscore = false; }
            out += (char)c;
        } else {
            pending_underscore = true;
        }
    }
    // Trim leading / trailing '_'. (Trailing was never emitted because
    // pending_underscore is dropped at end-of-input; leading may have
    // slipped through if the string started with non-alnum ASCII.)
    size_t b = 0, e = out.size();
    while (b < e && out[b] == '_') ++b;
    while (e > b && out[e - 1] == '_') --e;
    return out.substr(b, e - b);
}

static std::string lower_ascii(const std::string &s)
{
    std::string out = s;
    for (auto &c : out) if (c >= 'A' && c <= 'Z') c = (char)(c | 0x20);
    return out;
}

std::string mime_to_extension(const std::string &mime_type,
                              const std::string &url_fallback)
{
    // Strip parameters after ';' and whitespace, lowercase.
    std::string mime = mime_type;
    auto semi = mime.find(';');
    if (semi != std::string::npos) mime.resize(semi);
    while (!mime.empty() && (mime.back() == ' ' || mime.back() == '\t'))
        mime.pop_back();
    mime = lower_ascii(mime);

    struct Row { const char *type; const char *ext; };
    static const Row table[] = {
        // audio
        {"audio/mpeg",           "mp3"},
        {"audio/mp3",            "mp3"},
        {"audio/mp4",            "m4a"},
        {"audio/x-m4a",          "m4a"},
        {"audio/aac",            "aac"},
        {"audio/ogg",            "ogg"},
        {"audio/opus",           "opus"},
        {"audio/flac",           "flac"},
        {"audio/wav",            "wav"},
        {"audio/x-wav",          "wav"},
        {"audio/webm",           "webm"},
        // video
        {"video/mp4",            "mp4"},
        {"video/mpeg",           "mpeg"},
        {"video/webm",           "webm"},
        {"video/quicktime",      "mov"},
        {"video/x-matroska",     "mkv"},
        // application
        {"application/pdf",      "pdf"},
        {"application/zip",      "zip"},
        {"application/epub+zip", "epub"},
        {"application/x-tar",    "tar"},
        {"application/gzip",     "gz"},
        // images
        {"image/jpeg",           "jpg"},
        {"image/png",            "png"},
        {"image/gif",            "gif"},
        {"image/webp",           "webp"},
        {"image/svg+xml",        "svg"},
    };
    for (auto &r : table) if (mime == r.type) return r.ext;

    // Fallback: tail of URL path before any ? or #.
    if (!url_fallback.empty()) {
        size_t end = url_fallback.size();
        for (size_t i = 0; i < end; i++) {
            if (url_fallback[i] == '?' || url_fallback[i] == '#') {
                end = i;
                break;
            }
        }
        size_t dot = url_fallback.rfind('.', end);
        if (dot != std::string::npos && end - dot - 1 > 0 &&
            end - dot - 1 <= 5) {
            std::string ext = url_fallback.substr(dot + 1, end - dot - 1);
            bool ok = !ext.empty();
            for (char c : ext) {
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9'))) {
                    ok = false;
                    break;
                }
            }
            if (ok) return lower_ascii(ext);
        }
    }
    return "bin";
}

std::string disambiguate_path(const std::string &dir,
                              const std::string &base,
                              const std::string &ext)
{
    auto compose = [&](int n) -> std::string {
        std::string s = dir;
        if (!s.empty() && s.back() != '/') s += '/';
        s += base;
        if (n > 0) s += " (" + std::to_string(n) + ")";
        if (!ext.empty()) { s += '.'; s += ext; }
        return s;
    };
    std::string path = compose(0);
    if (!wxFileName::FileExists(wxString::FromUTF8(path))) return path;
    for (int n = 1; n < 1000; n++) {
        std::string candidate = compose(n);
        if (!wxFileName::FileExists(wxString::FromUTF8(candidate)))
            return candidate;
    }
    // Give up and overwrite — caller will just open whatever's there.
    return path;
}

// Compute UTC epoch seconds from Gregorian (Y, M, D, h, m, s) without
// any platform's timegm(). Uses std::chrono's civil-calendar routines,
// which are guaranteed UTC and need no timezone data.
static double utc_epoch(int y, int mo, int d, int h, int mi, int se)
{
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    using namespace std::chrono;
    sys_days days = year{y} / month{(unsigned)mo} / day{(unsigned)d};
    return (double)(days.time_since_epoch().count() * 86400LL
                    + h * 3600 + mi * 60 + se);
}

double parse_iso8601(const std::string &s)
{
    if (s.empty()) return 0;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (sscanf(s.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
               &y, &mo, &d, &h, &mi, &se) >= 3) {
        // ok
    } else if (sscanf(s.c_str(), "%4d%2d%2dT%2d%2d%2d",
                      &y, &mo, &d, &h, &mi, &se) >= 3) {
        // ok
    } else if (sscanf(s.c_str(), "%4d-%2d-%2d %2d:%2d:%2d",
                      &y, &mo, &d, &h, &mi, &se) >= 3) {
        // ok
    } else if (sscanf(s.c_str(), "%4d-%2d", &y, &mo) >= 2) {
        d = 1;
    } else {
        return 0;
    }
    if (d == 0) d = 1;
    return utc_epoch(y, mo, d, h, mi, se);
}

double parse_rfc822(const std::string &s)
{
    if (s.empty()) return 0;
    wxDateTime dt;
    if (!dt.ParseRfc822Date(wxString::FromUTF8(s))) return 0;
    // wxDateTime::ParseRfc822Date honors the timezone suffix in the
    // input string; GetTicks() returns UTC epoch seconds.
    return (double)dt.GetTicks();
}

// ---- wxDataViewCtrl column persistence ---------------------------

// Format: comma-separated entries, each "Title=Width:Hidden". Title is
// the column's wxString title (assumed not to contain ',', '=', or
// ':' — true for all our columns). Hidden is "0" or "1".

std::string dataview_serialize_columns(wxDataViewCtrl *ctrl)
{
    // Emit columns in display order (which can differ from model
    // order if the user drag-reordered). GetColumnPosition is the
    // portable way to ask "where does this column show on screen?".
    unsigned n = ctrl->GetColumnCount();
    std::vector<std::pair<int, wxDataViewColumn *>> by_pos;
    by_pos.reserve(n);
    for (unsigned i = 0; i < n; i++) {
        auto *col = ctrl->GetColumn(i);
        by_pos.push_back({ctrl->GetColumnPosition(col), col});
    }
    std::sort(by_pos.begin(), by_pos.end(),
              [](const auto &a, const auto &b) {
                  return a.first < b.first;
              });

    std::string out;
    for (auto &[pos, col] : by_pos) {
        (void)pos;
        if (!out.empty()) out += ',';
        out += col->GetTitle().utf8_string();
        out += '=';
        out += std::to_string(col->GetWidth());
        out += ':';
        out += col->IsHidden() ? '1' : '0';
    }
    return out;
}

std::vector<std::string>
dataview_parse_column_order(const std::string &saved)
{
    std::vector<std::string> titles;
    size_t pos = 0;
    while (pos < saved.size()) {
        size_t comma = saved.find(',', pos);
        if (comma == std::string::npos) comma = saved.size();
        std::string item = saved.substr(pos, comma - pos);
        pos = comma + 1;
        auto eq = item.find('=');
        if (eq == std::string::npos) continue;
        titles.push_back(item.substr(0, eq));
    }
    return titles;
}

void dataview_apply_columns(wxDataViewCtrl *ctrl, const std::string &saved)
{
    if (saved.empty()) return;

    std::unordered_map<std::string, std::pair<int, bool>> by_title;
    size_t pos = 0;
    while (pos < saved.size()) {
        size_t comma = saved.find(',', pos);
        if (comma == std::string::npos) comma = saved.size();
        std::string item = saved.substr(pos, comma - pos);
        pos = comma + 1;

        auto eq = item.find('=');
        if (eq == std::string::npos) continue;
        std::string title = item.substr(0, eq);
        std::string rest = item.substr(eq + 1);
        auto colon = rest.find(':');
        if (colon == std::string::npos) continue;
        int width = std::atoi(rest.substr(0, colon).c_str());
        bool hidden = (rest.substr(colon + 1) == "1");
        by_title[title] = {width, hidden};
    }

    for (unsigned i = 0; i < ctrl->GetColumnCount(); i++) {
        auto *col = ctrl->GetColumn(i);
        auto it = by_title.find(col->GetTitle().utf8_string());
        if (it == by_title.end()) continue;
        if (it->second.first > 0) col->SetWidth(it->second.first);
        col->SetHidden(it->second.second);
    }
}

void dataview_show_column_menu(wxDataViewCtrl *ctrl,
                               const std::function<void()> &on_change)
{
    wxMenu menu;
    const int base_id = wxID_HIGHEST + 100;
    for (unsigned i = 0; i < ctrl->GetColumnCount(); i++) {
        auto *col = ctrl->GetColumn(i);
        auto *item = menu.AppendCheckItem(base_id + (int)i, col->GetTitle());
        item->Check(!col->IsHidden());
    }
    menu.Bind(wxEVT_MENU,
              [ctrl, base_id, on_change](wxCommandEvent &e) {
                  unsigned idx = (unsigned)(e.GetId() - base_id);
                  if (idx < ctrl->GetColumnCount()) {
                      auto *col = ctrl->GetColumn(idx);
                      col->SetHidden(!col->IsHidden());
                      if (on_change) on_change();
                  }
              });
    ctrl->PopupMenu(&menu);
}

// ---- wxDataViewCtrl sort persistence -----------------------------

DataViewSort dataview_current_sort(wxDataViewCtrl *ctrl)
{
    DataViewSort out;
    for (unsigned i = 0; i < ctrl->GetColumnCount(); i++) {
        auto *col = ctrl->GetColumn(i);
        if (col->IsSortKey()) {
            out.col = (int)col->GetModelColumn();
            out.ascending = col->IsSortOrderAscending();
            return out;
        }
    }
    return out;
}

std::string dataview_serialize_sort(wxDataViewCtrl *ctrl)
{
    DataViewSort s = dataview_current_sort(ctrl);
    if (s.col < 0) return {};
    return std::to_string(s.col) + (s.ascending ? ",asc" : ",desc");
}

void dataview_apply_sort(wxDataViewCtrl *ctrl, const std::string &saved)
{
    if (saved.empty()) return;
    size_t comma = saved.find(',');
    if (comma == std::string::npos) return;
    int model_col = atoi(saved.substr(0, comma).c_str());
    bool asc = saved.substr(comma + 1) != "desc";

    // Clear any existing sort key, then set the one we want. wx uses
    // SetSortOrder(false) to mean "descending but sorted"; a null sort
    // (no key at all) is expressed via UnsetAsSortKey().
    for (unsigned i = 0; i < ctrl->GetColumnCount(); i++)
        ctrl->GetColumn(i)->UnsetAsSortKey();
    for (unsigned i = 0; i < ctrl->GetColumnCount(); i++) {
        auto *col = ctrl->GetColumn(i);
        if ((int)col->GetModelColumn() == model_col) {
            col->SetSortOrder(asc);
            break;
        }
    }
}
