// Feed fetch orchestration. Pulls feed URLs from app->feeds, hands them
// to a thread pool running http_fetch(), pushes results into app->fetch_inbox.
// The UI thread drains the inbox from fetch_process_results().

#include "elfeed.hpp"
#include "http.hpp"
#include "util.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>

struct FetchJob {
    std::string url;
    std::string etag;
    std::string last_modified;
};

namespace {

struct FetchQueue {
    std::mutex m;
    std::queue<FetchJob> jobs;
};

void worker_loop(Elfeed *app,
                 std::shared_ptr<FetchQueue> q,
                 std::shared_ptr<std::atomic<int>> live,
                 std::string user_agent)
{
    for (;;) {
        if (!app->fetch_running.load()) break;

        FetchJob job;
        {
            std::lock_guard lock(q->m);
            if (q->jobs.empty()) break;
            job = std::move(q->jobs.front());
            q->jobs.pop();
        }

        HttpRequest req;
        req.url = job.url;
        req.etag = job.etag;
        req.last_modified = job.last_modified;
        req.user_agent = user_agent;
        req.timeout_seconds = app->fetch_timeout;
        HttpResponse resp = http_fetch(req);

        FetchResult r;
        r.url = std::move(job.url);
        r.status_code = resp.status;
        r.body = std::move(resp.body);
        r.etag = std::move(resp.etag);
        r.last_modified = std::move(resp.last_modified);
        r.final_url = std::move(resp.final_url);
        r.error = std::move(resp.error);

        {
            std::lock_guard lock(app->fetch_mutex);
            app->fetch_inbox.push_back(std::move(r));
        }
        app->fetches_active--;
        app_wake_ui(app);
    }

    // Last worker out clears the "running" flag and wakes the UI one
    // more time so it can see the final state.
    if (--(*live) == 0) {
        app->fetch_running = false;
        app_wake_ui(app);
    }
}

} // namespace

void fetch_all(Elfeed *app)
{
    if (app->fetch_running.load()) return;
    if (app->feeds.empty()) return;

    // Initialize HTTP subsystem (loads CA bundle on POSIX)
    std::string err = http_init();
    if (!err.empty())
        elfeed_log(app, LOG_ERROR, "http_init: %s", err.c_str());

    auto queue = std::make_shared<FetchQueue>();
    for (auto &feed : app->feeds) {
        FetchJob job;
        job.url = feed.url;
        job.etag = feed.etag;
        job.last_modified = feed.last_modified;
        queue->jobs.push(std::move(job));
    }
    int total = (int)queue->jobs.size();

    elfeed_log(app, LOG_INFO, "Fetching %d feeds", total);
    for (auto &feed : app->feeds)
        elfeed_log(app, LOG_REQUEST, "%s", feed.url.c_str());

    app->fetch_running = true;
    app->fetches_total = total;
    app->fetches_active = total;

    // Reap any previous workers that may still be joinable
    for (auto &t : app->fetch_workers)
        if (t.joinable()) t.join();
    app->fetch_workers.clear();

    int nworkers = std::min(app->max_connections, total);
    if (nworkers < 1) nworkers = 1;
    auto live = std::make_shared<std::atomic<int>>(nworkers);

    std::string user_agent = elfeed_user_agent();

    for (int i = 0; i < nworkers; i++) {
        app->fetch_workers.emplace_back(
            worker_loop, app, queue, live, user_agent);
    }
}

void fetch_stop(Elfeed *app)
{
    app->fetch_running = false;
    for (auto &t : app->fetch_workers)
        if (t.joinable()) t.join();
    app->fetch_workers.clear();
}

bool fetch_process_results(Elfeed *app)
{
    std::vector<FetchResult> results;
    {
        std::lock_guard lock(app->fetch_mutex);
        results = std::move(app->fetch_inbox);
        app->fetch_inbox.clear();
    }

    // Reap finished worker threads when the batch is over
    if (!app->fetch_running.load()) {
        for (auto &t : app->fetch_workers)
            if (t.joinable()) t.join();
        app->fetch_workers.clear();
    }

    if (results.empty()) return false;

    for (auto &r : results) {
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
            elfeed_log(app, LOG_INFO, "%s: 304 Not Modified", r.url.c_str());
            feed->last_update = (double)time(nullptr);
            db_update_feed(app, *feed);
            continue;
        }

        // Anything outside 2xx (except 304 above) is an error. That
        // now includes 3xx — with manual redirect following we can
        // surface a 301/302/307 to the caller when the redirect chain
        // hit max_redirects or a server sent 3xx with no Location.
        if (r.status_code < 200 || r.status_code >= 300) {
            elfeed_log(app, LOG_ERROR, "%s: HTTP %d", r.url.c_str(),
                       r.status_code);
            feed->failures++;
            db_update_feed(app, *feed);
            continue;
        }

        elfeed_log(app, LOG_SUCCESS, "%s: HTTP %d (%zu bytes)",
                   r.url.c_str(), r.status_code, r.body.size());

        auto parsed = parse_feed(r.url, r.body);
        elfeed_log(app, LOG_INFO, "%s: parsed \"%s\" (%zu entries)",
                   r.url.c_str(), parsed.feed_title.c_str(),
                   parsed.entries.size());

        for (auto &entry : parsed.entries) {
            entry.tags.push_back("unread");
            for (auto &tag : feed->autotags)
                entry.tags.push_back(tag);
            std::sort(entry.tags.begin(), entry.tags.end());
            entry.tags.erase(std::unique(entry.tags.begin(),
                                          entry.tags.end()),
                              entry.tags.end());
        }

        if (!parsed.feed_title.empty())
            feed->title = parsed.feed_title;
        if (!r.etag.empty())
            feed->etag = r.etag;
        if (!r.last_modified.empty())
            feed->last_modified = r.last_modified;
        if (!r.final_url.empty() && r.final_url != r.url) {
            // Log only when the canonical URL first appears or
            // changes, so repeated fetches of a stable redirect
            // chain don't spam the log on every cycle.
            if (feed->canonical_url != r.final_url) {
                elfeed_log(app, LOG_INFO,
                           "%s -> %s (consider updating config)",
                           r.url.c_str(), r.final_url.c_str());
            }
            feed->canonical_url = r.final_url;
        }
        feed->failures = 0;
        feed->last_update = (double)time(nullptr);
        db_update_feed(app, *feed);

        db_add_entries(app, parsed.entries);
    }
    // A successful fetch may have changed self-declared feed titles
    // (or filled them in for the first time). Refresh the display map
    // so the entry list and detail panel pick the new title up.
    db_load_feed_titles(app);
    return true;
}
