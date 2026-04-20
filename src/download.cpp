// Download manager built on wxProcess. All state is UI-thread-local:
// wxExecute() is asynchronous, its child-termination events are
// delivered on the UI thread via wxProcess::OnTerminate, and a wxTimer
// drains the child's stdout at 200 ms intervals so progress updates
// reach the UI promptly.

#include "elfeed.hpp"

#include <wx/process.h>
#include <wx/stream.h>
#include <wx/timer.h>
#include <wx/utils.h>

#include <algorithm>
#include <regex>

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

    // Called once right after a successful wxExecute so we can start
    // polling the child's stdout stream.
    void begin(long pid)
    {
        pid_ = pid;
        timer_.Start(200);
    }

    // Send SIGTERM to the child. If `paused` is true, OnTerminate will
    // treat the non-zero exit as an intentional pause rather than a
    // failure.
    void kill(bool paused)
    {
        paused_ = paused;
        if (pid_ > 0) wxProcess::Kill(pid_, wxSIGTERM);
    }

    // wxProcess fires this on the UI thread when the child exits.
    // wxProcess self-deletes after this returns.
    void OnTerminate(int /*pid*/, int status) override
    {
        timer_.Stop();
        drain();
        finish(status);
    }

private:
    void on_timer(wxTimerEvent &) { drain(); }

    // Read whatever's available from the child's stdout (and stderr),
    // split into complete lines, and dispatch each line.
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
                    if (std::regex_search(line, m, dest_re)) {
                        d.destination = m[1].str();
                    }
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

} // anonymous namespace

void download_enqueue(Elfeed *app, const std::string &url,
                      const std::string &title, bool is_video)
{
    DownloadItem item;
    item.id = app->download_next_id++;
    item.url = url;
    item.title = title;
    item.is_video = is_video;
    item.directory = app->download_dir;
    app->downloads.push_back(std::move(item));
}

void download_tick(Elfeed *app)
{
    if (app->download_active_id != 0) return;
    if (app->downloads.empty()) return;

    DownloadItem *next = pick_next(app->downloads);
    if (!next) return;

    // Build argv. argv_owned holds the backing strings; argv is a
    // NULL-terminated array of c_str pointers into argv_owned.
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
        // wxExecute failure; wxProcess must be deleted manually since
        // no OnTerminate will fire.
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
    if (id == app->download_active_id && app->download_process) {
        static_cast<DownloadProcess *>(app->download_process)->kill(false);
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
        if (d.paused && id == app->download_active_id && app->download_process) {
            static_cast<DownloadProcess *>(app->download_process)->kill(true);
        }
        break;
    }
}

void download_stop(Elfeed *app)
{
    // Called from elfeed_shutdown. The wx event loop is on its way out
    // so OnTerminate may not fire; send SIGTERM and let the OS reap.
    // yt-dlp leaves a resumable partial file behind, which is fine.
    if (app->download_process) {
        static_cast<DownloadProcess *>(app->download_process)->kill(false);
        app->download_process = nullptr;
        app->download_active_id = 0;
    }
}
