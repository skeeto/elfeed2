#ifndef ELFEED_HTTP_HPP
#define ELFEED_HTTP_HPP

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

// Initialize the HTTP subsystem (loads CA bundle on POSIX, etc.).
// Returns empty string on success, or an error message on failure.
// Safe to call multiple times (only the first call does work).
std::string http_init();

#endif
