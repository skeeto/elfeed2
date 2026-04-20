// HTTP client for POSIX (macOS, Linux, BSD) using cpp-httplib + mbedTLS.
// On Windows, see http_win.cpp instead.

#include <httplib.h>

#include "http.hpp"

#include <mutex>
#include <string>
#include <sys/stat.h>
#include <vector>

// Common system CA bundle locations. First file that exists wins.
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

std::string http_init()
{
    std::lock_guard lock(g_init_mutex);
    if (g_inited) return {};
    g_inited = true;

    for (const char **p = kCaPaths; *p; p++) {
        struct stat st;
        if (stat(*p, &st) == 0 && S_ISREG(st.st_mode)) {
            g_ca_path = *p;
            return {};
        }
    }
    return "no system CA bundle found";
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

HttpResponse http_fetch(const HttpRequest &req)
{
    HttpResponse out;

    std::string origin, path;
    if (!split_url(req.url, origin, path)) {
        out.error = "invalid URL";
        return out;
    }

    httplib::Client cli(origin);
    cli.set_connection_timeout(req.timeout_seconds, 0);
    cli.set_read_timeout(req.timeout_seconds, 0);
    cli.set_write_timeout(req.timeout_seconds, 0);
    cli.set_follow_location(true);
    // Don't ask for compression: cpp-httplib only decompresses when
    // built with CPPHTTPLIB_ZLIB_SUPPORT, which we don't link. Without
    // this, Accept-Encoding: gzip would cause pugixml to choke on
    // gzipped bytes. Identity encoding keeps the pipeline simple.
    cli.set_compress(false);
    cli.set_decompress(false);
    cli.set_keep_alive(false);

    // TLS: verify server certificate against the system CA bundle.
    if (!g_ca_path.empty()) {
        cli.set_ca_cert_path(g_ca_path.c_str());
    }
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
        out.error = httplib::to_string(res.error());
        return out;
    }

    out.status = res->status;
    out.body = std::move(res->body);

    auto etag_it = res->headers.find("ETag");
    if (etag_it != res->headers.end()) out.etag = etag_it->second;
    auto lm_it = res->headers.find("Last-Modified");
    if (lm_it != res->headers.end()) out.last_modified = lm_it->second;

    // cpp-httplib's Result doesn't expose the final URL after redirects.
    // We can only report the original URL; leave final_url empty so callers
    // fall back to the request URL.
    return out;
}
