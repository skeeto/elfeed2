#ifndef ELFEED_IMAGE_CACHE_HPP
#define ELFEED_IMAGE_CACHE_HPP

#include <string>

struct Elfeed;

// Rewrite `html` so every <img src="http(s)://..."> whose payload we
// already have in the cache becomes <img src="data:MIME;base64,...">.
// URLs NOT yet cached are left as-is (they render as a broken image
// in wxHtmlWindow briefly) and queued for background fetch; once the
// fetch lands, the UI thread re-renders the entry and the data URI
// takes over. UI-thread only.
std::string image_cache_inline(Elfeed *app, const std::string &html);

// Drain the worker->UI inbox and write any newly-downloaded image
// bytes into the DB cache table. Returns true if at least one new
// image was absorbed (so the caller knows to re-render the preview).
// UI-thread only.
bool image_cache_process_results(Elfeed *app);

#endif
