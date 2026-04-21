// HTTP client for Windows using WinHTTP + the system CA store (Schannel).
// On POSIX, see http_posix.cpp instead.

#include "http.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <windows.h>
#include <winhttp.h>

std::string http_init() { return {}; }

static std::wstring to_wide(const std::string &s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(),
                                nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

static std::string from_wide(const std::wstring &s)
{
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n,
                        nullptr, nullptr);
    return out;
}

static std::string win_error(DWORD err)
{
    LPWSTR msg = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_FROM_HMODULE,
        GetModuleHandleW(L"winhttp.dll"), err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPWSTR)&msg, 0, nullptr);
    std::string out;
    if (n > 0 && msg) {
        out = from_wide(std::wstring(msg, n));
        while (!out.empty() &&
               (out.back() == '\r' || out.back() == '\n' || out.back() == ' '))
            out.pop_back();
    } else {
        out = "Windows error " + std::to_string((unsigned)err);
    }
    if (msg) LocalFree(msg);
    return out;
}

// Query a single response header. Returns empty if not present.
static std::string query_header(HINTERNET req, const wchar_t *name)
{
    DWORD size = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, nullptr, &size,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0)
        return {};
    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, name, buf.data(),
                             &size, WINHTTP_NO_HEADER_INDEX))
        return {};
    buf.resize(size / sizeof(wchar_t));
    // Strip trailing null if present
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return from_wide(buf);
}

HttpResponse http_fetch(const HttpRequest &req)
{
    HttpResponse out;

    auto wurl = to_wide(req.url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength  = DWORD(-1);
    uc.dwUrlPathLength   = DWORD(-1);
    uc.dwExtraInfoLength = DWORD(-1);
    uc.dwSchemeLength    = DWORD(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
        out.error = "bad URL: " + win_error(GetLastError());
        return out;
    }

    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength)
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    bool is_https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    INTERNET_PORT port = uc.nPort;

    auto wua = to_wide(req.user_agent.empty()
                           ? std::string("elfeed2")
                           : req.user_agent);
    HINTERNET session = WinHttpOpen(wua.c_str(),
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        out.error = "WinHttpOpen: " + win_error(GetLastError());
        return out;
    }

    DWORD timeout_ms = (DWORD)req.timeout_seconds * 1000;
    WinHttpSetTimeouts(session, timeout_ms, timeout_ms,
                       timeout_ms, timeout_ms);

    // Automatic redirect following (default), bound the count.
    DWORD max_redirs = (DWORD)req.max_redirects;
    WinHttpSetOption(session, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS,
                     &max_redirs, sizeof(max_redirs));

    HINTERNET conn = WinHttpConnect(session, host.c_str(), port, 0);
    if (!conn) {
        out.error = "WinHttpConnect: " + win_error(GetLastError());
        WinHttpCloseHandle(session);
        return out;
    }

    DWORD flags = is_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hreq = WinHttpOpenRequest(conn, L"GET", path.c_str(),
                                        nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hreq) {
        out.error = "WinHttpOpenRequest: " + win_error(GetLastError());
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return out;
    }

    auto add_header = [&](const std::string &line) {
        auto wh = to_wide(line);
        WinHttpAddRequestHeaders(hreq, wh.c_str(), (DWORD)-1,
                                 WINHTTP_ADDREQ_FLAG_ADD);
    };
    if (!req.etag.empty())
        add_header("If-None-Match: " + req.etag);
    if (!req.last_modified.empty())
        add_header("If-Modified-Since: " + req.last_modified);
    // WinHTTP can decompress via WINHTTP_OPTION_DECOMPRESSION, but we
    // don't enable it; keep the response identity-encoded so pugixml
    // can parse it directly.

    BOOL ok = WinHttpSendRequest(hreq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok || !WinHttpReceiveResponse(hreq, nullptr)) {
        out.error = "WinHttp: " + win_error(GetLastError());
        WinHttpCloseHandle(hreq);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return out;
    }

    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(hreq,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    out.status = (int)status;
    out.etag = query_header(hreq, L"ETag");
    out.last_modified = query_header(hreq, L"Last-Modified");

    // Final URL after redirects
    {
        DWORD size = 0;
        WinHttpQueryOption(hreq, WINHTTP_OPTION_URL, nullptr, &size);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && size > 0) {
            std::wstring final(size / sizeof(wchar_t), L'\0');
            if (WinHttpQueryOption(hreq, WINHTTP_OPTION_URL, final.data(),
                                   &size)) {
                while (!final.empty() && final.back() == L'\0')
                    final.pop_back();
                out.final_url = from_wide(final);
            }
        }
    }

    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hreq, &avail) && avail) {
        std::vector<char> buf(avail);
        DWORD got = 0;
        if (!WinHttpReadData(hreq, buf.data(), avail, &got)) break;
        out.body.append(buf.data(), got);
    }

    WinHttpCloseHandle(hreq);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return out;
}

// --- Streaming download -----------------------------------------------

HttpDownloadResult http_download(const HttpDownloadRequest &req)
{
    HttpDownloadResult out;

    auto wurl = to_wide(req.url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.dwHostNameLength  = DWORD(-1);
    uc.dwUrlPathLength   = DWORD(-1);
    uc.dwExtraInfoLength = DWORD(-1);
    uc.dwSchemeLength    = DWORD(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
        out.error = "bad URL: " + win_error(GetLastError());
        return out;
    }
    std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.dwExtraInfoLength)
        path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    if (path.empty()) path = L"/";
    bool is_https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    INTERNET_PORT port = uc.nPort;

    auto wua = to_wide(req.user_agent.empty() ? std::string("elfeed2")
                                              : req.user_agent);
    HINTERNET session = WinHttpOpen(wua.c_str(),
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        out.error = "WinHttpOpen: " + win_error(GetLastError());
        return out;
    }
    if (req.timeout_seconds > 0) {
        DWORD ms = (DWORD)req.timeout_seconds * 1000;
        WinHttpSetTimeouts(session, ms, ms, ms, ms);
    }
    DWORD max_redirs = (DWORD)req.max_redirects;
    WinHttpSetOption(session, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS,
                     &max_redirs, sizeof(max_redirs));

    HINTERNET conn = WinHttpConnect(session, host.c_str(), port, 0);
    if (!conn) {
        out.error = "WinHttpConnect: " + win_error(GetLastError());
        WinHttpCloseHandle(session);
        return out;
    }

    DWORD flags = is_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hreq = WinHttpOpenRequest(conn, L"GET", path.c_str(),
                                        nullptr, WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hreq) {
        out.error = "WinHttpOpenRequest: " + win_error(GetLastError());
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return out;
    }

    if (!WinHttpSendRequest(hreq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hreq, nullptr)) {
        out.error = "WinHttp: " + win_error(GetLastError());
        WinHttpCloseHandle(hreq);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return out;
    }

    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(hreq,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &status_size, WINHTTP_NO_HEADER_INDEX);
    out.status = (int)status;

    uint64_t total = 0;
    {
        DWORD len_bytes = 0, size = sizeof(len_bytes);
        if (WinHttpQueryHeaders(
                hreq,
                WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &len_bytes, &size, WINHTTP_NO_HEADER_INDEX)) {
            total = (uint64_t)len_bytes;
        }
    }

    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hreq, &avail)) {
        if (avail == 0) break;  // end of stream
        std::vector<char> buf(avail);
        DWORD got = 0;
        if (!WinHttpReadData(hreq, buf.data(), avail, &got)) {
            out.error = "WinHttpReadData: " + win_error(GetLastError());
            break;
        }
        if (req.write && !req.write(buf.data(), got)) {
            out.cancelled = true;
            break;
        }
        out.bytes += got;
        if (req.progress && !req.progress(out.bytes, total)) {
            out.cancelled = true;
            break;
        }
    }

    WinHttpCloseHandle(hreq);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return out;
}
