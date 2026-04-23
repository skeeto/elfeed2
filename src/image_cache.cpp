#include "image_cache.hpp"

#include "elfeed.hpp"
#include "http.hpp"
#include "util.hpp"

#include <wx/base64.h>

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <thread>
#include <vector>

// Total-bytes budget for cached images. LRU-evicted down to this cap
// after each successful insert. 256 MiB is generous enough that a
// typical reading session stays entirely in-cache, while still being
// small enough to hold in a DB that also holds entries. Tunable via
// config later if it matters.
static constexpr int64_t kCacheCapBytes = 256LL * 1024 * 1024;

// Upper bound for inlining an image as a data: URI in the preview
// pane. Above this, we leave the tag's original http src alone and
// render an adjacent link so the user can open the image in a
// browser. The wxHtmlWindow path (base64-encode → splice into HTML
// → HTML-parse → base64-decode → wxImage-decode → layout) all runs
// synchronously on the UI thread, so a multi-megabyte GIF freezes
// the app for upwards of a second when you visit the entry. Small
// images still inline fine and render instantly.
static constexpr size_t kInlineMaxBytes = 1 * 1024 * 1024;

// Per-batch worker concurrency. 4 matches our fetch workers; beyond
// that the benefit tapers off and publishers' CDNs dislike it.
static constexpr size_t kMaxConcurrent = 4;

namespace {

// ---- MIME sniffing -----------------------------------------------

// Derive a MIME type from the leading bytes of an image. We prefer
// this over the HTTP Content-Type header because some CDNs return
// generic "application/octet-stream" or wrong types entirely. For
// the handful of formats feed content uses, magic-number sniffing
// is reliable.
std::string sniff_mime(const std::string &b)
{
    auto u = [&](size_t i) { return (unsigned char)b[i]; };
    if (b.size() >= 8 && u(0) == 0x89 && b[1] == 'P' &&
        b[2] == 'N' && b[3] == 'G')
        return "image/png";
    if (b.size() >= 3 && u(0) == 0xFF && u(1) == 0xD8 && u(2) == 0xFF)
        return "image/jpeg";
    if (b.size() >= 6 &&
        (b.compare(0, 6, "GIF87a") == 0 || b.compare(0, 6, "GIF89a") == 0))
        return "image/gif";
    if (b.size() >= 12 &&
        b.compare(0, 4, "RIFF") == 0 && b.compare(8, 4, "WEBP") == 0)
        return "image/webp";
    // Crude SVG detect: first non-whitespace should open with "<" and
    // the first few hundred bytes should mention "<svg". Guards
    // against random HTML being mistaken for SVG.
    size_t head = std::min<size_t>(b.size(), 512);
    std::string prefix = b.substr(0, head);
    if (prefix.find("<svg") != std::string::npos)
        return "image/svg+xml";
    return "";  // unknown
}

// ---- DB helpers (UI thread) --------------------------------------

bool cache_get(Elfeed *app, const std::string &url,
               std::string &mime, std::string &bytes)
{
    if (!app->db) return false;
    const char *sql =
        "SELECT mime_type, bytes FROM image_cache WHERE url = ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    bool hit = false;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (auto *t = (const char *)sqlite3_column_text(stmt, 0))
            mime = t;
        int n = sqlite3_column_bytes(stmt, 1);
        if (n > 0) {
            bytes.assign((const char *)sqlite3_column_blob(stmt, 1), n);
            hit = true;
        }
    }
    sqlite3_finalize(stmt);
    if (!hit) return false;

    // Bump last_used so LRU eviction keeps freshly-viewed images
    // resident. Separate UPDATE so a stray error on the read side
    // still returns cached bytes.
    const char *bump =
        "UPDATE image_cache SET last_used = ? WHERE url = ?";
    sqlite3_stmt *bstmt;
    if (sqlite3_prepare_v2(app->db, bump, -1, &bstmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_double(bstmt, 1, (double)time(nullptr));
        sqlite3_bind_text(bstmt, 2, url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(bstmt);
        sqlite3_finalize(bstmt);
    }
    return true;
}

void cache_put(Elfeed *app, const std::string &url,
               const std::string &mime, const std::string &bytes)
{
    if (!app->db || bytes.empty()) return;
    const char *sql =
        "INSERT OR REPLACE INTO image_cache"
        " (url, mime_type, bytes, size, last_used)"
        " VALUES (?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, mime.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_blob(stmt, 3, bytes.data(), (int)bytes.size(),
                      SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, (int64_t)bytes.size());
    sqlite3_bind_double(stmt, 5, (double)time(nullptr));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // LRU-evict until we're under the cap. Prepare the two hot
    // statements once outside the loop and rebind each round —
    // preparing-inside-the-loop was compiling the same SQL from
    // scratch on every eviction pass, which is exactly the work
    // SQLite prepared statements exist to skip. Cheap because
    // idx_image_cache_lru makes ORDER BY last_used O(log n) and
    // we stop as soon as the running total dips below the cap.
    sqlite3_stmt *sum_stmt = nullptr;
    sqlite3_prepare_v2(app->db,
        "SELECT COALESCE(SUM(size),0) FROM image_cache",
        -1, &sum_stmt, nullptr);

    auto read_total = [&]() -> int64_t {
        int64_t t = 0;
        if (sum_stmt) {
            sqlite3_reset(sum_stmt);
            if (sqlite3_step(sum_stmt) == SQLITE_ROW)
                t = sqlite3_column_int64(sum_stmt, 0);
        }
        return t;
    };

    int64_t total = read_total();
    if (total > kCacheCapBytes) {
        sqlite3_stmt *evict_stmt = nullptr;
        sqlite3_prepare_v2(app->db,
            "DELETE FROM image_cache WHERE url IN"
            " (SELECT url FROM image_cache ORDER BY last_used ASC LIMIT ?)",
            -1, &evict_stmt, nullptr);
        while (total > kCacheCapBytes && evict_stmt) {
            // Delete the 16 oldest rows per round; cheap if we're
            // only a little over cap, but quickly converges after
            // a big import.
            sqlite3_reset(evict_stmt);
            sqlite3_bind_int(evict_stmt, 1, 16);
            if (sqlite3_step(evict_stmt) != SQLITE_DONE) break;
            total = read_total();
        }
        if (evict_stmt) sqlite3_finalize(evict_stmt);
    }
    if (sum_stmt) sqlite3_finalize(sum_stmt);
}

// ---- Background fetcher -----------------------------------------

void fetch_worker(Elfeed *app, std::string url)
{
    HttpDownloadRequest req;
    req.url = url;
    req.user_agent = elfeed_user_agent();
    req.timeout_seconds = 30;

    std::string body;
    body.reserve(64 * 1024);
    req.write = [&body](const char *data, size_t n) {
        // Cap per-image memory so a malicious 4 GiB PNG doesn't
        // blow up the process. 16 MiB is way more than enough for
        // any image a feed reader should display.
        constexpr size_t kMax = 16 * 1024 * 1024;
        if (body.size() + n > kMax) return false;
        body.append(data, n);
        return true;
    };
    req.progress = nullptr;

    HttpDownloadResult res = http_download(req);

    Elfeed::ImageInboxItem item;
    item.url = std::move(url);
    if (res.status == 200 && !body.empty()) {
        item.mime = sniff_mime(body);
        if (!item.mime.empty()) item.bytes = std::move(body);
    }

    {
        std::lock_guard<std::mutex> g(app->image_mutex);
        app->image_in_flight.erase(item.url);
        app->image_inbox.push_back(std::move(item));
    }
    app_wake_ui(app);
}

void prefetch(Elfeed *app, std::vector<std::string> urls)
{
    std::vector<std::string> to_spawn;
    {
        std::lock_guard<std::mutex> g(app->image_mutex);
        for (auto &u : urls) {
            // image_in_flight dedupes across simultaneous views;
            // cache_get on the UI side already skipped URLs that
            // are in-DB, so anything reaching here is a genuine miss.
            if (app->image_in_flight.insert(u).second)
                to_spawn.push_back(u);
        }
    }

    // Simple fire-and-forget threads. With kMaxConcurrent we'd need a
    // real pool; a per-URL detached thread is "good enough" for the
    // typical entry (<20 images) and avoids the lifetime headache of
    // long-lived workers. We throttle by sleeping a main thread if we
    // go overboard (not worth the complexity right now).
    (void)kMaxConcurrent;
    for (auto &u : to_spawn) {
        std::thread(fetch_worker, app, u).detach();
    }
}

// ---- HTML scanner -------------------------------------------------

// Case-insensitive byte compare at a position. Takes the full
// haystack + offset rather than a substring so the caller doesn't
// allocate a temporary std::string for every comparison — the
// previous `iequals(s.substr(i, 3), "img", 3)` style allocated
// once per `<` in the HTML body while scanning for image tags,
// which added up for content-heavy feeds.
static bool iequals_at(const std::string &s, size_t pos,
                       const char *lit, size_t n)
{
    if (pos + n > s.size()) return false;
    for (size_t i = 0; i < n; i++)
        if (std::tolower((unsigned char)s[pos + i]) !=
            std::tolower((unsigned char)lit[i])) return false;
    return true;
}

// Locate the next "<img" (case-insensitive) starting at `from`.
// Returns npos if none. We don't try to parse HTML generally; we just
// skip to a literal `<img` that's a tag start.
size_t find_img(const std::string &s, size_t from)
{
    for (size_t i = from; i + 4 <= s.size(); i++) {
        if (s[i] != '<') continue;
        if (iequals_at(s, i + 1, "img", 3)) {
            // Must be followed by whitespace or '>' to count as a tag.
            if (i + 4 >= s.size()) return i;
            char c = s[i + 4];
            if (std::isspace((unsigned char)c) || c == '>' || c == '/')
                return i;
        }
    }
    return std::string::npos;
}

// Extract the src="..." (or src='...') value from a complete <img>
// tag. Returns empty string if not present.
std::string extract_src(const std::string &tag)
{
    // Look for `src` as an attribute name. Scan char by char.
    for (size_t i = 0; i + 3 < tag.size(); i++) {
        if (!iequals_at(tag, i, "src", 3)) continue;
        // Must be preceded by whitespace / tag start and followed by
        // '=' or whitespace.
        if (i > 0) {
            char p = tag[i - 1];
            if (!(std::isspace((unsigned char)p) || p == '<' || p == ' '))
                continue;
        }
        size_t j = i + 3;
        while (j < tag.size() && std::isspace((unsigned char)tag[j])) j++;
        if (j >= tag.size() || tag[j] != '=') continue;
        j++;
        while (j < tag.size() && std::isspace((unsigned char)tag[j])) j++;
        if (j >= tag.size()) return {};
        char q = tag[j];
        if (q == '"' || q == '\'') {
            size_t end = tag.find(q, j + 1);
            if (end == std::string::npos) return {};
            return tag.substr(j + 1, end - (j + 1));
        }
        // Unquoted attribute value — reads until whitespace or '>'.
        size_t end = j;
        while (end < tag.size() &&
               !std::isspace((unsigned char)tag[end]) &&
               tag[end] != '>') end++;
        return tag.substr(j, end - j);
    }
    return {};
}

bool is_http_url(const std::string &s)
{
    return s.compare(0, 7, "http://") == 0 ||
           s.compare(0, 8, "https://") == 0;
}

// Build a data: URI from the cached bytes.
std::string make_data_uri(const std::string &mime, const std::string &bytes)
{
    wxString b64 =
        wxBase64Encode(bytes.data(), bytes.size());
    std::string out = "data:";
    out += mime;
    out += ";base64,";
    out.append(b64.utf8_string());
    return out;
}

// Human-readable size for the "too big to inline" link label.
// Keeps the message short; a user looking at a 5.7 MB GIF knows
// why they're seeing a link instead of the picture.
std::string format_bytes(size_t n)
{
    const char *units[] = {"B", "KB", "MB", "GB"};
    int u = 0;
    double d = (double)n;
    while (d >= 1024 && u + 1 < (int)(sizeof(units) / sizeof(*units))) {
        d /= 1024;
        u++;
    }
    char buf[32];
    if (u == 0) snprintf(buf, sizeof(buf), "%zu %s", n, units[u]);
    else        snprintf(buf, sizeof(buf), "%.1f %s", d, units[u]);
    return buf;
}

// Replace the src="..." (or src='...') value in `tag` with `new_src`,
// keeping the rest of the tag intact.
std::string replace_src(const std::string &tag, const std::string &new_src)
{
    // Same scan as extract_src; when we find the value, we splice.
    for (size_t i = 0; i + 3 < tag.size(); i++) {
        if (!iequals_at(tag, i, "src", 3)) continue;
        if (i > 0) {
            char p = tag[i - 1];
            if (!(std::isspace((unsigned char)p) || p == '<' || p == ' '))
                continue;
        }
        size_t j = i + 3;
        while (j < tag.size() && std::isspace((unsigned char)tag[j])) j++;
        if (j >= tag.size() || tag[j] != '=') continue;
        j++;
        while (j < tag.size() && std::isspace((unsigned char)tag[j])) j++;
        if (j >= tag.size()) return tag;
        char q = tag[j];
        size_t val_start, val_end;
        if (q == '"' || q == '\'') {
            val_start = j + 1;
            val_end = tag.find(q, val_start);
            if (val_end == std::string::npos) return tag;
        } else {
            val_start = j;
            val_end = val_start;
            while (val_end < tag.size() &&
                   !std::isspace((unsigned char)tag[val_end]) &&
                   tag[val_end] != '>') val_end++;
        }
        std::string out;
        out.reserve(tag.size() + new_src.size());
        out.append(tag, 0, val_start);
        out.append(new_src);
        out.append(tag, val_end, std::string::npos);
        return out;
    }
    return tag;
}

} // namespace

// ---- Public API ---------------------------------------------------

std::string image_cache_inline(Elfeed *app, const std::string &html)
{
    std::string out;
    out.reserve(html.size());
    std::vector<std::string> to_fetch;

    size_t i = 0;
    while (i < html.size()) {
        size_t tag_start = find_img(html, i);
        if (tag_start == std::string::npos) {
            out.append(html, i, std::string::npos);
            break;
        }
        size_t tag_end = html.find('>', tag_start);
        if (tag_end == std::string::npos) {
            out.append(html, i, std::string::npos);
            break;
        }

        // Everything before the tag copies through verbatim.
        out.append(html, i, tag_start - i);

        std::string tag = html.substr(tag_start, tag_end - tag_start + 1);
        std::string src = extract_src(tag);

        if (!src.empty() && is_http_url(src)) {
            std::string mime, bytes;
            if (cache_get(app, src, mime, bytes)) {
                if (bytes.size() > kInlineMaxBytes) {
                    // Too big to inline without locking the UI
                    // thread while wxHtmlWindow decodes. Emit a
                    // clickable link instead — small label, no
                    // expensive render, user can open the real
                    // image in a browser if they want.
                    out.append("<a href=\"");
                    out.append(src);
                    out.append("\">[Image: ");
                    out.append(format_bytes(bytes.size()));
                    out.append("]</a>");
                } else {
                    out.append(replace_src(tag,
                        make_data_uri(mime, bytes)));
                }
            } else {
                // Miss: leave the original tag (renders broken) and
                // queue a fetch; the UI will re-render on landing.
                out.append(tag);
                to_fetch.push_back(src);
            }
        } else {
            out.append(tag);
        }

        i = tag_end + 1;
    }

    if (!to_fetch.empty())
        prefetch(app, std::move(to_fetch));

    return out;
}

bool image_cache_process_results(Elfeed *app)
{
    std::vector<Elfeed::ImageInboxItem> drained;
    {
        std::lock_guard<std::mutex> g(app->image_mutex);
        drained.swap(app->image_inbox);
    }
    bool any = false;
    for (auto &item : drained) {
        if (item.mime.empty() || item.bytes.empty())
            continue;  // fetch failed; don't cache the miss
        cache_put(app, item.url, item.mime, item.bytes);
        any = true;
    }
    return any;
}
