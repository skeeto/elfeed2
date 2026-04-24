// HTTP client built on cpp-httplib + TLS (OpenSSL or mbedTLS,
// picked at build time by FetchTLS.cmake). Used on macOS, Linux,
// and BSD always; also on Windows when the build was configured
// with -DELFEED2_HTTP_BACKEND=cpp-httplib. The Windows default is
// WinHTTP (see http_win.cpp); the cpp-httplib backend exists for
// Windows XP, whose WinHTTP / Schannel can't negotiate modern TLS.
//
// CA trust: on Unix we probe well-known system bundle paths; on
// Windows+cpp-httplib the caller (app.cpp) writes an embedded
// Mozilla cacert.pem to the user data dir and hands us that path
// via http_init(forced_ca_path) — there's no system trust store
// to probe on XP.

// XP / Vista-targeted Windows SDKs don't declare the APIs
// cpp-httplib uses unconditionally (WSAPoll, inet_pton, struct
// pollfd, CreateFile2, CreateFileMappingFromApp). xp_shims.hpp
// provides file-scope replacements BEFORE httplib.h so its call
// sites compile. No-op on every other toolchain.
#include "xp_shims.hpp"

#include <httplib.h>

#include "http.hpp"

#ifdef CPPHTTPLIB_MBEDTLS_SUPPORT
#include <mbedtls/error.h>
#endif

#include <cstdio>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

// Common system CA bundle locations. First file that exists wins.
// Only used when the caller doesn't supply an explicit path (the
// Windows+cpp-httplib build always supplies one; none of these
// paths would resolve there anyway).
static const char *kCaPaths[] = {
    "/etc/ssl/cert.pem",                                      // macOS, *BSD
    "/etc/ssl/certs/ca-certificates.crt",                     // Debian, Ubuntu
    "/etc/pki/tls/certs/ca-bundle.crt",                       // Fedora, RHEL
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",      // newer RHEL
    "/opt/homebrew/etc/ca-certificates/cert.pem",             // Homebrew, arm64
    "/usr/local/etc/ca-certificates/cert.pem",                // Homebrew, x86_64
    nullptr,
};

static std::string g_ca_path;
static std::mutex g_init_mutex;
static bool g_inited = false;

std::string http_init(const std::string &forced_ca_path)
{
    std::lock_guard lock(g_init_mutex);
    if (g_inited) return {};
    g_inited = true;

    // Caller-supplied path wins — used by the Windows+cpp-httplib
    // build where the embedded Mozilla bundle has just been written
    // to disk. Other platforms pass an empty string and we probe
    // system locations.
    if (!forced_ca_path.empty()) {
        struct stat st;
        if (stat(forced_ca_path.c_str(), &st) == 0 &&
            S_ISREG(st.st_mode)) {
            g_ca_path = forced_ca_path;
            return {};
        }
        return "CA bundle does not exist: " + forced_ca_path;
    }

    for (const char **p = kCaPaths; *p; p++) {
        struct stat st;
        if (stat(*p, &st) == 0 && S_ISREG(st.st_mode)) {
            g_ca_path = *p;
            return {};
        }
    }
    return "no system CA bundle found";
}

// Format cpp-httplib's Result error (plus any TLS-specific detail)
// into a single line suitable for elfeed_log. Without this, every
// SSL failure collapses to "SSL connection failed" regardless of
// the underlying cause — unhelpful on old / XP systems where
// handshake failures are common (clock skew, cipher mismatch,
// server not in CA bundle, etc.). cpp-httplib surfaces:
//   ssl_error()          — its ErrorCode category (int)
//                          (Fatal=4, CertVerifyFailed=6, HostnameMismatch=7…)
//   ssl_backend_error()  — mbedTLS / OpenSSL backend code (uint64)
// For mbedTLS builds we additionally resolve the backend code to
// text via mbedtls_strerror. Safe on any TLS backend — the
// accessors are always present in an SSL-enabled build; non-SSL
// failures leave them at 0 and we just emit the top-line message.
template <class ResultT>
static std::string describe_http_error(const ResultT &res)
{
    std::string out = httplib::to_string(res.error());
#ifdef CPPHTTPLIB_SSL_ENABLED
    int        ssl_err = res.ssl_error();
    uint64_t   backend = res.ssl_backend_error();
    if (ssl_err != 0 || backend != 0) {
        char buf[256];
# ifdef CPPHTTPLIB_MBEDTLS_SUPPORT
        // mbedTLS returns negative error codes; mbedtls_strerror
        // expects the negated form. backend is stored as uint64;
        // cast accordingly.
        char msg[160] = {0};
        mbedtls_strerror(-(int)backend, msg, sizeof(msg));
        std::snprintf(buf, sizeof(buf),
                      " [ssl_err=%d backend=-0x%04X: %s]",
                      ssl_err, (unsigned)backend, msg);
# else
        std::snprintf(buf, sizeof(buf),
                      " [ssl_err=%d backend=0x%llX]",
                      ssl_err, (unsigned long long)backend);
# endif
        out += buf;
    }
#endif
    return out;
}

// Split "https://host:port/path?query" into (scheme+host+port, path+query).
// Returns true on success.
static bool split_url(const std::string &url,
                      std::string &origin, std::string &path)
{
    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) return false;
    size_t path_start = url.find('/', scheme_end + 3);
    if (path_start == std::string::npos) {
        origin = url;
        path = "/";
    } else {
        origin = url.substr(0, path_start);
        path = url.substr(path_start);
    }
    return true;
}

// Resolve a Location header (possibly relative) against a base URL.
// Handles the four cases that show up in real-world 3xx Location
// values; full RFC 3986 normalization is overkill for feed fetching.
//
//   absolute:         "https://host/new"    -> use as-is
//   scheme-relative:  "//host/new"          -> prepend base scheme
//   origin-relative:  "/new/path"           -> base origin + loc
//   path-relative:    "new/file"            -> base directory + loc
//
// Fragments (#...) and query strings pass through verbatim.
static std::string resolve_url(const std::string &base,
                               const std::string &loc)
{
    if (loc.empty()) return base;
    // Absolute URL.
    if (loc.compare(0, 7, "http://") == 0 ||
        loc.compare(0, 8, "https://") == 0)
        return loc;

    size_t scheme_end = base.find("://");
    if (scheme_end == std::string::npos) return loc;

    // Scheme-relative.
    if (loc.size() >= 2 && loc[0] == '/' && loc[1] == '/')
        return base.substr(0, scheme_end + 1) + loc;

    std::string scheme = base.substr(0, scheme_end);
    size_t host_start = scheme_end + 3;
    size_t path_start = base.find('/', host_start);
    std::string origin = (path_start == std::string::npos)
                             ? base
                             : base.substr(0, path_start);

    // Origin-relative.
    if (loc[0] == '/') return origin + loc;

    // Path-relative: keep the base's directory, drop its filename.
    std::string base_path = (path_start == std::string::npos)
                                ? std::string("/")
                                : base.substr(path_start);
    // Trim off ?query / #fragment before finding the last slash.
    size_t q = base_path.find_first_of("?#");
    if (q != std::string::npos) base_path.erase(q);
    size_t slash = base_path.rfind('/');
    std::string dir = (slash == std::string::npos)
                          ? std::string("/")
                          : base_path.substr(0, slash + 1);
    return origin + dir + loc;
}

HttpResponse http_fetch(const HttpRequest &req)
{
    HttpResponse out;

    // We drive redirects manually rather than relying on
    // set_follow_location(true) so we can (a) honor req.max_redirects
    // and (b) expose the final URL (cpp-httplib's Result doesn't
    // surface it). Conditional headers and User-Agent are re-sent on
    // every hop — servers past a redirect see the same preconditions,
    // which is how WinHTTP and browsers behave too.
    std::string current_url = req.url;

    for (int hop = 0; hop <= req.max_redirects; hop++) {
        std::string origin, path;
        if (!split_url(current_url, origin, path)) {
            out.error = "invalid URL";
            return out;
        }

        httplib::Client cli(origin);
        cli.set_connection_timeout(req.timeout_seconds, 0);
        cli.set_read_timeout(req.timeout_seconds, 0);
        cli.set_write_timeout(req.timeout_seconds, 0);
        cli.set_follow_location(false);  // we handle redirects
        // CPPHTTPLIB_ZLIB_SUPPORT is compiled in (see
        // cmake/FetchCppHttplib.cmake). With that define, cpp-httplib
        // automatically adds Accept-Encoding: gzip, deflate and
        // auto-decompresses the response before returning it — which
        // is what we want, because some feed servers (Cloudflare,
        // various nginx configs) send gzipped content regardless of
        // whether we asked for it. The decompressed body is what
        // pugixml parses. set_compress(false) still disables sending
        // compressed *request* bodies; that's not a direction we use.
        cli.set_compress(false);
        cli.set_decompress(true);
        cli.set_keep_alive(false);

        // TLS: verify server cert against the system CA bundle.
        if (!g_ca_path.empty())
            cli.set_ca_cert_path(g_ca_path.c_str());
        cli.enable_server_certificate_verification(true);

        httplib::Headers headers;
        if (!req.user_agent.empty())
            headers.emplace("User-Agent", req.user_agent);
        if (!req.etag.empty())
            headers.emplace("If-None-Match", req.etag);
        if (!req.last_modified.empty())
            headers.emplace("If-Modified-Since", req.last_modified);

        auto res = cli.Get(path, headers);
        if (!res) {
            out.error = describe_http_error(res);
            return out;
        }

        // 3xx with a Location header → follow manually.
        if (res->status >= 300 && res->status < 400) {
            auto loc = res->headers.find("Location");
            if (loc == res->headers.end() || loc->second.empty()) {
                // No Location: treat as the final response.
                out.status = res->status;
                out.body = std::move(res->body);
                if (current_url != req.url) out.final_url = current_url;
                return out;
            }
            current_url = resolve_url(current_url, loc->second);
            continue;
        }

        // 2xx / 4xx / 5xx: this is the final response.
        out.status = res->status;
        out.body = std::move(res->body);
        auto etag_it = res->headers.find("ETag");
        if (etag_it != res->headers.end()) out.etag = etag_it->second;
        auto lm_it = res->headers.find("Last-Modified");
        if (lm_it != res->headers.end()) out.last_modified = lm_it->second;
        if (current_url != req.url) out.final_url = current_url;
        return out;
    }

    out.error = "too many redirects";
    return out;
}

// --- Streaming download -----------------------------------------------

HttpDownloadResult http_download(const HttpDownloadRequest &req)
{
    HttpDownloadResult out;

    // Manual redirect loop, same rationale as http_fetch: honor
    // req.max_redirects and avoid spraying partial bytes into the
    // write callback on a 3xx response.
    std::string current_url = req.url;

    for (int hop = 0; hop <= req.max_redirects; hop++) {
        std::string origin, path;
        if (!split_url(current_url, origin, path)) {
            out.error = "invalid URL";
            return out;
        }

        httplib::Client cli(origin);
        if (req.timeout_seconds > 0) {
            cli.set_connection_timeout(req.timeout_seconds, 0);
            cli.set_read_timeout(req.timeout_seconds, 0);
            cli.set_write_timeout(req.timeout_seconds, 0);
        } else {
            // Long-lived download. Only cap the initial connect.
            cli.set_connection_timeout(30, 0);
        }
        cli.set_follow_location(false);
        cli.set_compress(false);
        cli.set_decompress(false);
        cli.set_keep_alive(false);

        if (!g_ca_path.empty())
            cli.set_ca_cert_path(g_ca_path.c_str());
        cli.enable_server_certificate_verification(true);

        httplib::Headers headers;
        if (!req.user_agent.empty())
            headers.emplace("User-Agent", req.user_agent);

        uint64_t total = 0;
        bool is_redirect = false;
        std::string redirect_to;

        auto on_response = [&](const httplib::Response &resp) -> bool {
            if (resp.status >= 300 && resp.status < 400) {
                auto it = resp.headers.find("Location");
                if (it != resp.headers.end() && !it->second.empty()) {
                    redirect_to = it->second;
                    is_redirect = true;
                    // Abort the body read — we'll re-issue against
                    // the new URL at the top of the loop.
                    return false;
                }
                // 3xx with no Location: treat as final response.
                return true;
            }
            auto it = resp.headers.find("Content-Length");
            if (it != resp.headers.end()) {
                try { total = std::stoull(it->second); }
                catch (...) { total = 0; }
            }
            return true;
        };

        auto on_chunk = [&](const char *data, size_t n) -> bool {
            if (req.write && !req.write(data, n)) {
                out.cancelled = true;
                return false;
            }
            out.bytes += n;
            if (req.progress && !req.progress(out.bytes, total)) {
                out.cancelled = true;
                return false;
            }
            return true;
        };

        auto res = cli.Get(path, headers, on_response, on_chunk);

        if (is_redirect) {
            // Intentional abort from on_response — `res` will carry
            // a cancel error; ignore it and follow the Location.
            current_url = resolve_url(current_url, redirect_to);
            continue;
        }

        if (!res) {
            if (!out.cancelled)
                out.error = describe_http_error(res);
            return out;
        }
        out.status = res->status;
        return out;
    }

    out.error = "too many redirects";
    return out;
}
