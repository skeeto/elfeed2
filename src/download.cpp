// Download manager. Two dispatch paths that share the DownloadItem
// queue and the Downloads UI:
//
//   DownloadKind::Subprocess -> yt-dlp / curl via wxProcess. All state
//     on the UI thread, driven by wxProcess::OnTerminate + a wxTimer
//     that polls the child's stdout.
//
//   DownloadKind::HttpDirect -> a std::thread running http_download()
//     into a chosen file path. Communicates with the UI via atomics
//     and app_wake_ui(); the UI thread reaps finished workers from
//     download_tick().

#include "elfeed.hpp"

#include "http.hpp"
#include "util.hpp"

#include <wx/ffile.h>
#include <wx/process.h>
#include <wx/stream.h>
#include <wx/timer.h>
#include <wx/utils.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <regex>
#include <thread>

// Find the next item to run: highest (priority - failures), not paused,
// not over max failures (5).
static DownloadItem *pick_next(std::vector<DownloadItem> &items)
{
    DownloadItem *best = nullptr;
    int best_score = -999;
    for (auto &item : items) {
        if (item.paused) continue;
        if (item.failures >= 5) continue;
        int score = item.priority - item.failures;
        if (!best || score > best_score) {
            best = &item;
            best_score = score;
        }
    }
    return best;
}

// ---- Subprocess path (yt-dlp / curl via wxProcess) -------------------

namespace {

class DownloadProcess : public wxProcess {
public:
    DownloadProcess(Elfeed *app, int item_id)
        : wxProcess(wxPROCESS_REDIRECT)
        , app_(app)
        , item_id_(item_id)
        , timer_(this)
    {
        Bind(wxEVT_TIMER, &DownloadProcess::on_timer, this);
    }

    void begin(long pid) { pid_ = pid; timer_.Start(200); }

    void kill(bool paused)
    {
        paused_ = paused;
        if (pid_ > 0) wxProcess::Kill(pid_, wxSIGTERM);
    }

    void OnTerminate(int /*pid*/, int status) override
    {
        timer_.Stop();
        drain();
        finish(status);
    }

private:
    void on_timer(wxTimerEvent &) { drain(); }

    void drain()
    {
        drain_stream(GetInputStream(), true);
        drain_stream(GetErrorStream(), false);
    }

    void drain_stream(wxInputStream *in, bool parse)
    {
        if (!in) return;
        while (in->CanRead()) {
            char buf[4096];
            in->Read(buf, sizeof(buf));
            size_t n = in->LastRead();
            if (n == 0) break;
            line_buf_.append(buf, n);
            flush_lines(parse);
        }
    }

    void flush_lines(bool parse)
    {
        static const std::regex progress_re("([0-9.]+%) +of +([^ ]+)");
        static const std::regex dest_re("Destination: (.+)");

        size_t pos;
        while ((pos = line_buf_.find('\n')) != std::string::npos) {
            std::string line = line_buf_.substr(0, pos);
            line_buf_.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();

            std::smatch m;
            for (auto &d : app_->downloads) {
                if (d.id != item_id_) continue;
                d.log.push_back(line);
                if (parse) {
                    if (std::regex_search(line, m, progress_re)) {
                        d.progress = m[1].str();
                        d.total = m[2].str();
                    }
                    if (std::regex_search(line, m, dest_re))
                        d.destination = m[1].str();
                }
                break;
            }
        }
        app_wake_ui(app_);
    }

    void finish(int status)
    {
        bool success = (status == 0);
        if (success) {
            app_->downloads.erase(
                std::remove_if(app_->downloads.begin(), app_->downloads.end(),
                               [this](auto &d) { return d.id == item_id_; }),
                app_->downloads.end());
        } else {
            for (auto &d : app_->downloads) {
                if (d.id != item_id_) continue;
                if (!paused_) {
                    d.failures++;
                    d.log.push_back("Process exited with error");
                }
                d.progress.clear();
                break;
            }
        }
        app_->download_active_id = 0;
        app_->download_process = nullptr;
        app_wake_ui(app_);
    }

    Elfeed *app_;
    int item_id_;
    wxTimer timer_;
    std::string line_buf_;
    long pid_ = 0;
    bool paused_ = false;
};

// ---- HTTP-direct path ------------------------------------------------

struct HttpJob {
    int item_id = 0;
    std::string dest;
    std::thread thread;

    // Worker → UI
    std::atomic<uint64_t> cur{0};
    std::atomic<uint64_t> total{0};
    std::atomic<bool>     finished{false};

    // UI → worker
    std::atomic<bool>     cancelled{false};

    // Written on the worker before `finished` flips; only read after.
    int         status = 0;
    std::string error;
};

static std::unique_ptr<HttpJob> g_http;

static std::string format_bytes(uint64_t n)
{
    const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int u = 0;
    double d = (double)n;
    while (d >= 1024 && u + 1 < (int)(sizeof(units) / sizeof(*units))) {
        d /= 1024;
        u++;
    }
    char buf[32];
    if (u == 0) snprintf(buf, sizeof(buf), "%llu%s",
                         (unsigned long long)n, units[u]);
    else        snprintf(buf, sizeof(buf), "%.1f%s", d, units[u]);
    return buf;
}

static void http_worker(Elfeed *app, HttpJob *job, std::string url)
{
    // Make sure http_init() has run — it initializes the CA bundle on
    // POSIX. Cheap if already done.
    http_init();

    wxFFile out;
    if (!out.Open(wxString::FromUTF8(job->dest), "wb")) {
        job->error = "cannot open output file: " + job->dest;
        job->finished = true;
        app_wake_ui(app);
        return;
    }

    HttpDownloadRequest req;
    req.url = std::move(url);
    req.user_agent = elfeed_user_agent();

    req.write = [&out, job](const char *data, size_t n) -> bool {
        if (job->cancelled.load()) return false;
        if (out.Write(data, n) != n || out.Error()) return false;
        return true;
    };
    req.progress = [job, app](uint64_t cur, uint64_t total) -> bool {
        if (job->cancelled.load()) return false;
        job->cur.store(cur);
        job->total.store(total);
        app_wake_ui(app);
        return true;
    };

    HttpDownloadResult res = http_download(req);
    out.Close();

    if (res.cancelled) {
        job->error = "cancelled";
    } else if (!res.error.empty()) {
        job->error = res.error;
    } else if (res.status < 200 || res.status >= 300) {
        char buf[64];
        snprintf(buf, sizeof(buf), "HTTP %d", res.status);
        job->error = buf;
    }
    job->status = res.status;

    job->finished = true;
    app_wake_ui(app);
}

// Called from the UI thread at each on_wake. Reads worker-published
// atomics into the matching DownloadItem and, if the worker is done,
// joins the thread and finalizes the queue state.
static void http_pump(Elfeed *app)
{
    if (!g_http) return;

    DownloadItem *item = nullptr;
    for (auto &d : app->downloads) {
        if (d.id == g_http->item_id) { item = &d; break; }
    }

    // Publish current progress even for running jobs.
    if (item) {
        uint64_t cur = g_http->cur.load();
        uint64_t tot = g_http->total.load();
        if (tot > 0) {
            double pct = 100.0 * (double)cur / (double)tot;
            char buf[16];
            snprintf(buf, sizeof(buf), "%.1f%%", pct);
            item->progress = buf;
            item->total = format_bytes(tot);
        } else if (cur > 0) {
            item->progress = format_bytes(cur);
            item->total.clear();
        }
    }

    if (!g_http->finished.load()) return;

    if (g_http->thread.joinable()) g_http->thread.join();

    if (g_http->error.empty() && g_http->status >= 200 && g_http->status < 300) {
        // Success: drop the item from the queue.
        int id = g_http->item_id;
        app->downloads.erase(
            std::remove_if(app->downloads.begin(), app->downloads.end(),
                           [id](auto &d) { return d.id == id; }),
            app->downloads.end());
    } else if (item) {
        if (g_http->error != "cancelled") {
            item->failures++;
            item->log.push_back(g_http->error.empty()
                                    ? "HTTP download failed"
                                    : g_http->error);
        }
        item->progress.clear();
    }

    app->download_active_id = 0;
    g_http.reset();
}

} // anonymous namespace

// ---- Public API ------------------------------------------------------

void download_enqueue(Elfeed *app, const std::string &url,
                      const std::string &title, bool is_video)
{
    DownloadItem item;
    item.id = app->download_next_id++;
    item.url = url;
    item.title = title;
    item.is_video = is_video;
    item.directory = app->download_dir;
    item.kind = DownloadKind::Subprocess;
    app->downloads.push_back(std::move(item));
}

void download_enqueue_http(Elfeed *app, const std::string &url,
                           const std::string &title,
                           const std::string &output_path)
{
    DownloadItem item;
    item.id = app->download_next_id++;
    item.url = url;
    item.title = title;
    item.output_path = output_path;
    item.kind = DownloadKind::HttpDirect;
    app->downloads.push_back(std::move(item));
}

void download_tick(Elfeed *app)
{
    // First, pump the HTTP worker — this covers both progress updates
    // and the transition to done.
    http_pump(app);

    if (app->download_active_id != 0) return;
    if (app->downloads.empty()) return;

    DownloadItem *next = pick_next(app->downloads);
    if (!next) return;

    if (next->kind == DownloadKind::HttpDirect) {
        g_http = std::make_unique<HttpJob>();
        g_http->item_id = next->id;
        g_http->dest = next->output_path;

        app->download_active_id = next->id;
        std::string url = next->url;  // copy before thread launch
        HttpJob *job = g_http.get();
        g_http->thread = std::thread([app, job, url = std::move(url)]() {
            http_worker(app, job, url);
        });
        return;
    }

    // Subprocess path (yt-dlp / curl).
    std::vector<std::string> argv_owned;
    if (next->is_video) {
        argv_owned.push_back(app->ytdlp_program);
        argv_owned.push_back("--newline");
        for (auto &a : app->ytdlp_args) argv_owned.push_back(a);
        if (next->slow) argv_owned.push_back("--rate-limit=2M");
        if (!next->destination.empty()) {
            argv_owned.push_back("--output");
            argv_owned.push_back(next->destination);
        }
        argv_owned.push_back("--");
        argv_owned.push_back(next->url);
    } else {
        argv_owned.push_back("curl");
        argv_owned.push_back("-fSL");
        if (!next->destination.empty()) {
            argv_owned.push_back("-o");
            argv_owned.push_back(next->destination);
        } else {
            argv_owned.push_back("-O");
        }
        argv_owned.push_back("--");
        argv_owned.push_back(next->url);
    }
    std::vector<const char *> argv;
    argv.reserve(argv_owned.size() + 1);
    for (auto &s : argv_owned) argv.push_back(s.c_str());
    argv.push_back(nullptr);

    auto *proc = new DownloadProcess(app, next->id);

    wxExecuteEnv env;
    if (!next->directory.empty())
        env.cwd = wxString::FromUTF8(next->directory);

    long pid = wxExecute(argv.data(),
                         wxEXEC_ASYNC | wxEXEC_HIDE_CONSOLE |
                         wxEXEC_MAKE_GROUP_LEADER,
                         proc, &env);

    if (pid == 0) {
        delete proc;
        for (auto &d : app->downloads) {
            if (d.id != next->id) continue;
            d.failures++;
            d.log.push_back("Failed to start process");
            break;
        }
        return;
    }

    proc->begin(pid);
    app->download_active_id = next->id;
    app->download_process = proc;
}

void download_remove(Elfeed *app, int id)
{
    if (id == app->download_active_id) {
        if (app->download_process) {
            static_cast<DownloadProcess *>(app->download_process)->kill(false);
        } else if (g_http && g_http->item_id == id) {
            g_http->cancelled = true;
            // Worker will finish shortly; http_pump() will reap it. For
            // the synchronous remove we just let the item go — the
            // worker won't touch it after it's gone (it only reads the
            // cancelled flag and writes to its own file).
        }
    }
    app->downloads.erase(
        std::remove_if(app->downloads.begin(), app->downloads.end(),
                       [&](auto &d) { return d.id == id; }),
        app->downloads.end());
}

void download_pause(Elfeed *app, int id)
{
    for (auto &d : app->downloads) {
        if (d.id != id) continue;
        d.paused = !d.paused;
        if (d.paused && id == app->download_active_id) {
            if (app->download_process) {
                static_cast<DownloadProcess *>(app->download_process)
                    ->kill(true);
            } else if (g_http && g_http->item_id == id) {
                g_http->cancelled = true;
            }
        }
        break;
    }
}

void download_stop(Elfeed *app)
{
    if (app->download_process) {
        static_cast<DownloadProcess *>(app->download_process)->kill(false);
        app->download_process = nullptr;
        app->download_active_id = 0;
    }
    if (g_http) {
        g_http->cancelled = true;
        if (g_http->thread.joinable()) g_http->thread.join();
        g_http.reset();
        app->download_active_id = 0;
    }
}
