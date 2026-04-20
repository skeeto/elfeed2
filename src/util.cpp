#include "util.hpp"

#include <wx/app.h>
#include <wx/datetime.h>
#include <wx/filename.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

#include <chrono>
#include <cstdio>

// Ensure wxStandardPaths uses XDG layout on Linux/BSD; on macOS and
// Windows this is a no-op.
static wxStandardPaths &std_paths()
{
    auto &sp = wxStandardPaths::Get();
    sp.SetFileLayout(wxStandardPaths::FileLayout_XDG);
    return sp;
}

std::string user_data_dir()
{
    return std_paths().GetUserDataDir().utf8_string();
}

std::string user_config_dir()
{
#ifdef __WXMSW__
    // No established convention for hand-edited config on Windows;
    // keep it alongside the data directory.
    return std_paths().GetUserDataDir().utf8_string();
#else
    // macOS and Linux: follow XDG so the file is reachable from a
    // terminal at a familiar path (~/.config/<app>/config), not buried
    // under ~/Library/Application Support. Matches neovim / git-with-
    // XDG / rclone / etc. on macOS; matches XDG directly on Linux.
    wxString base;
    if (!wxGetEnv("XDG_CONFIG_HOME", &base) || base.empty())
        base = wxGetHomeDir() + "/.config";
    wxString app = (wxTheApp && !wxTheApp->GetAppName().empty())
        ? wxTheApp->GetAppName()
        : wxString("elfeed2");
    return (base + "/" + app).utf8_string();
#endif
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
