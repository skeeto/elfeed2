#include "elfeed.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

#include <curl/curl.h>
#include <SDL3/SDL_events.h>

struct CurlTransfer {
    std::string url;
    std::string etag;
    std::string last_modified;
    std::string body;
    std::string response_etag;
    std::string response_last_modified;
    std::string effective_url;
    long status_code = 0;
};

static size_t write_callback(char *data, size_t size, size_t nmemb,
                             void *userp)
{
    auto *transfer = (CurlTransfer *)userp;
    size_t bytes = size * nmemb;
    transfer->body.append(data, bytes);
    return bytes;
}

static size_t header_callback(char *data, size_t size, size_t nmemb,
                              void *userp)
{
    auto *transfer = (CurlTransfer *)userp;
    size_t bytes = size * nmemb;
    std::string line(data, bytes);

    // Parse headers we care about
    if (line.size() > 5 && (line[0] == 'E' || line[0] == 'e') &&
        strncasecmp(line.c_str(), "etag:", 5) == 0) {
        transfer->response_etag = line.substr(5);
        // Trim whitespace
        auto &s = transfer->response_etag;
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
            s.erase(s.begin());
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                              s.back() == ' '))
            s.pop_back();
    } else if (line.size() > 14 &&
               strncasecmp(line.c_str(), "last-modified:", 14) == 0) {
        transfer->response_last_modified = line.substr(14);
        auto &s = transfer->response_last_modified;
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
            s.erase(s.begin());
        while (!s.empty() && (s.back() == '\r' || s.back() == '\n' ||
                              s.back() == ' '))
            s.pop_back();
    }
    return bytes;
}

static void wake_main_thread(Elfeed *app)
{
    if (app->wake_event_type) {
        SDL_Event event = {};
        event.type = app->wake_event_type;
        SDL_PushEvent(&event);
    }
}

static void fetch_thread_func(Elfeed *app,
                               std::vector<CurlTransfer> transfers)
{
    CURLM *multi = curl_multi_init();
    if (!multi) return;

    curl_multi_setopt(multi, CURLMOPT_MAXCONNECTS,
                      (long)app->max_connections);

    std::vector<CURL *> handles;
    for (auto &t : transfers) {
        CURL *easy = curl_easy_init();
        if (!easy) continue;

        curl_easy_setopt(easy, CURLOPT_URL, t.url.c_str());
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, &t);
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, &t);
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 5L);
        curl_easy_setopt(easy, CURLOPT_TIMEOUT, (long)app->fetch_timeout);
        curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(easy, CURLOPT_USERAGENT,
                         "Elfeed/" ELFEED_VERSION);

        // Conditional fetch headers
        struct curl_slist *headers = nullptr;
        if (!t.etag.empty()) {
            std::string h = "If-None-Match: " + t.etag;
            headers = curl_slist_append(headers, h.c_str());
        }
        if (!t.last_modified.empty()) {
            std::string h = "If-Modified-Since: " + t.last_modified;
            headers = curl_slist_append(headers, h.c_str());
        }
        if (headers)
            curl_easy_setopt(easy, CURLOPT_HTTPHEADER, headers);

        curl_easy_setopt(easy, CURLOPT_PRIVATE, &t);
        curl_multi_add_handle(multi, easy);
        handles.push_back(easy);
    }

    // Drive the multi handle
    int still_running = 0;
    curl_multi_perform(multi, &still_running);

    while (still_running && app->fetch_running.load()) {
        int numfds;
        curl_multi_poll(multi, nullptr, 0, 1000, &numfds);
        curl_multi_perform(multi, &still_running);

        // Process completed transfers
        CURLMsg *msg;
        int msgs_left;
        while ((msg = curl_multi_info_read(multi, &msgs_left))) {
            if (msg->msg != CURLMSG_DONE) continue;

            CurlTransfer *t;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &t);
            curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE,
                              &t->status_code);
            char *eff_url;
            if (curl_easy_getinfo(msg->easy_handle, CURLINFO_EFFECTIVE_URL,
                                  &eff_url) == CURLE_OK && eff_url) {
                t->effective_url = eff_url;
            }

            FetchResult result;
            result.url = t->url;
            result.status_code = (int)t->status_code;
            result.body = std::move(t->body);
            result.etag = std::move(t->response_etag);
            result.last_modified = std::move(t->response_last_modified);
            result.final_url = std::move(t->effective_url);

            if (msg->data.result != CURLE_OK) {
                result.error = curl_easy_strerror(msg->data.result);
            }

            {
                std::lock_guard lock(app->fetch_mutex);
                app->fetch_inbox.push_back(std::move(result));
            }
            app->fetches_active--;
            wake_main_thread(app);
        }
    }

    // Cleanup
    for (auto *h : handles) {
        struct curl_slist *headers = nullptr;
        curl_easy_getinfo(h, CURLINFO_PRIVATE, &headers);
        curl_multi_remove_handle(multi, h);
        curl_easy_cleanup(h);
    }
    curl_multi_cleanup(multi);

    app->fetch_running = false;
    wake_main_thread(app);
}

void fetch_all(Elfeed *app)
{
    if (app->fetch_running.load()) return;
    if (app->feeds.empty()) return;

    std::vector<CurlTransfer> transfers;
    for (auto &feed : app->feeds) {
        CurlTransfer t;
        t.url = feed.url;
        t.etag = feed.etag;
        t.last_modified = feed.last_modified;
        transfers.push_back(std::move(t));
    }

    elfeed_log(app, LOG_INFO, "Fetching %d feeds", (int)transfers.size());
    for (auto &t : transfers)
        elfeed_log(app, LOG_REQUEST, "%s", t.url.c_str());

    app->fetch_running = true;
    app->fetches_total = (int)transfers.size();
    app->fetches_active = (int)transfers.size();

    // Ensure any prior thread is joined
    if (app->fetch_thread.joinable())
        app->fetch_thread.join();

    app->fetch_thread = std::thread(fetch_thread_func, app,
                                     std::move(transfers));
}

void fetch_stop(Elfeed *app)
{
    app->fetch_running = false;
    if (app->fetch_thread.joinable())
        app->fetch_thread.join();
}

void fetch_process_results(Elfeed *app)
{
    std::vector<FetchResult> results;
    {
        std::lock_guard lock(app->fetch_mutex);
        results = std::move(app->fetch_inbox);
        app->fetch_inbox.clear();
    }

    for (auto &r : results) {
        // Find the feed
        Feed *feed = nullptr;
        for (auto &f : app->feeds) {
            if (f.url == r.url) { feed = &f; break; }
        }
        if (!feed) continue;

        if (!r.error.empty()) {
            elfeed_log(app, LOG_ERROR, "%s: %s", r.url.c_str(),
                       r.error.c_str());
            feed->failures++;
            db_update_feed(app, *feed);
            continue;
        }

        if (r.status_code == 304) {
            elfeed_log(app, LOG_INFO, "%s: 304 Not Modified",
                       r.url.c_str());
            feed->last_update = (double)time(nullptr);
            db_update_feed(app, *feed);
            continue;
        }

        if (r.status_code < 200 || r.status_code >= 400) {
            elfeed_log(app, LOG_ERROR, "%s: HTTP %d", r.url.c_str(),
                       r.status_code);
            feed->failures++;
            db_update_feed(app, *feed);
            continue;
        }

        elfeed_log(app, LOG_SUCCESS, "%s: HTTP %d (%zu bytes)",
                   r.url.c_str(), r.status_code, r.body.size());

        // Parse the feed
        auto parsed = parse_feed(r.url, r.body);
        elfeed_log(app, LOG_INFO, "%s: parsed \"%s\" (%zu entries)",
                   r.url.c_str(), parsed.feed_title.c_str(),
                   parsed.entries.size());

        // Apply autotags + unread
        for (auto &entry : parsed.entries) {
            entry.tags.push_back("unread");
            for (auto &tag : feed->autotags)
                entry.tags.push_back(tag);
            // Sort and deduplicate tags
            std::sort(entry.tags.begin(), entry.tags.end());
            entry.tags.erase(std::unique(entry.tags.begin(),
                                          entry.tags.end()),
                              entry.tags.end());
        }

        // Update feed metadata (must happen before adding entries
        // because of the foreign key on entry.feed_url)
        if (!parsed.feed_title.empty())
            feed->title = parsed.feed_title;
        if (!r.etag.empty())
            feed->etag = r.etag;
        if (!r.last_modified.empty())
            feed->last_modified = r.last_modified;
        if (!r.final_url.empty())
            feed->canonical_url = r.final_url;
        feed->failures = 0;
        feed->last_update = (double)time(nullptr);
        db_update_feed(app, *feed);

        // Add entries to database
        db_add_entries(app, parsed.entries);

        app->filter_dirty = true;
    }
}
