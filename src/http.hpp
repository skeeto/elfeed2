#ifndef ELFEED_HTTP_HPP
#define ELFEED_HTTP_HPP

#include <cstdint>
#include <functional>
#include <string>

struct HttpRequest {
    std::string url;
    std::string etag;            // If-None-Match
    std::string last_modified;   // If-Modified-Since
    std::string user_agent;
    int timeout_seconds = 30;
    int max_redirects = 5;
};

struct HttpResponse {
    int status = 0;              // 0 on transport error (see `error`)
    std::string body;
    std::string etag;
    std::string last_modified;
    std::string final_url;       // after redirects (may be empty if unchanged)
    std::string error;           // empty on success
};

// Perform a single HTTP GET. Synchronous; call from a worker thread.
HttpResponse http_fetch(const HttpRequest &req);

// Initialize the HTTP subsystem (resolves the CA bundle to trust,
// etc.). If `forced_ca_path` is non-empty it's used verbatim;
// otherwise well-known system paths are probed (cpp-httplib build)
// or the OS trust store is used implicitly (WinHTTP build, arg
// ignored). Returns an empty string on success, or an error
// message on failure. Safe to call multiple times; only the first
// call does work.
std::string http_init(const std::string &forced_ca_path = {});

// Streaming HTTP GET. Bytes are delivered via `write`; callers stream
// them to disk. Both `write` and `progress` return false to cancel.
// For use from a worker thread — the call blocks for the full transfer.
struct HttpDownloadRequest {
    std::string url;
    std::string user_agent;
    int timeout_seconds = 0;   // 0 = no timeout (good for large files)
    int max_redirects = 5;
    std::function<bool(const char *data, size_t n)> write;
    std::function<bool(uint64_t current, uint64_t total)> progress;
};

struct HttpDownloadResult {
    int status = 0;
    uint64_t bytes = 0;
    bool cancelled = false;
    std::string error;
};

HttpDownloadResult http_download(const HttpDownloadRequest &req);

#endif
