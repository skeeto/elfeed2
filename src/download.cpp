#include "elfeed.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <regex>

#ifndef _WIN32
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static void wake_main_thread(Elfeed *app)
{
    app_wake_ui(app);
}

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

#ifndef _WIN32
static void download_thread_func(Elfeed *app, DownloadItem item)
{
    // Build command
    std::vector<std::string> args;
    if (item.is_video) {
        args.push_back(app->ytdlp_program);
        args.push_back("--newline");
        for (auto &a : app->ytdlp_args)
            args.push_back(a);
        if (item.slow)
            args.push_back("--rate-limit=2M");
        if (!item.destination.empty()) {
            args.push_back("--output");
            args.push_back(item.destination);
        }
        args.push_back("--");
        args.push_back(item.url);
    } else {
        args.push_back("curl");
        args.push_back("-fSL");
        if (!item.destination.empty()) {
            args.push_back("-o");
            args.push_back(item.destination);
        } else {
            args.push_back("-O");
        }
        args.push_back("--");
        args.push_back(item.url);
    }

    // Create pipe for stdout
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        std::lock_guard lock(app->download_mutex);
        for (auto &d : app->downloads) {
            if (d.id == item.id) {
                d.failures++;
                d.log.push_back("Failed to create pipe");
                break;
            }
        }
        return;
    }

    pid_t pid = fork();
    if (pid == 0) {
        // Child
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        if (!item.directory.empty())
            chdir(item.directory.c_str());

        // Build argv
        std::vector<const char *> argv;
        for (auto &a : args)
            argv.push_back(a.c_str());
        argv.push_back(nullptr);
        execvp(argv[0], const_cast<char *const *>(argv.data()));
        _exit(127);
    }

    close(pipefd[1]);
    app->download_child_pid = (int)pid;

    if (pid < 0) {
        close(pipefd[0]);
        std::lock_guard lock(app->download_mutex);
        for (auto &d : app->downloads) {
            if (d.id == item.id) {
                d.failures++;
                d.log.push_back("Failed to fork");
                break;
            }
        }
        return;
    }

    // Read output and parse progress
    std::regex progress_re("([0-9.]+%) +of +([^ ]+)");
    std::regex dest_re("Destination: (.+)");
    char buf[4096];
    std::string line_buf;

    while (app->download_running.load()) {
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n <= 0) break;

        line_buf.append(buf, (size_t)n);

        // Process complete lines
        size_t pos;
        while ((pos = line_buf.find('\n')) != std::string::npos) {
            std::string line = line_buf.substr(0, pos);
            line_buf.erase(0, pos + 1);

            std::smatch m;
            std::lock_guard lock(app->download_mutex);
            for (auto &d : app->downloads) {
                if (d.id != item.id) continue;
                d.log.push_back(line);

                if (std::regex_search(line, m, progress_re)) {
                    d.progress = m[1].str();
                    d.total = m[2].str();
                }
                if (std::regex_search(line, m, dest_re)) {
                    d.destination = m[1].str();
                }
                break;
            }
            wake_main_thread(app);
        }
    }
    close(pipefd[0]);
    app->download_child_pid = 0;

    // Wait for child
    int wstatus;
    waitpid(pid, &wstatus, 0);
    bool success = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0;

    {
        std::lock_guard lock(app->download_mutex);
        if (success) {
            // Remove completed item
            app->downloads.erase(
                std::remove_if(app->downloads.begin(), app->downloads.end(),
                               [&](auto &d) { return d.id == item.id; }),
                app->downloads.end());
        } else {
            for (auto &d : app->downloads) {
                if (d.id != item.id) continue;
                // Don't count pause as a failure
                if (!d.paused) {
                    d.failures++;
                    d.log.push_back("Process exited with error");
                }
                d.progress.clear();
                break;
            }
        }
        app->download_active_id = 0;
    }

    wake_main_thread(app);
}
#endif

void download_enqueue(Elfeed *app, const std::string &url,
                      const std::string &title, bool is_video)
{
    DownloadItem item;
    item.id = app->download_next_id++;
    item.url = url;
    item.title = title;
    item.is_video = is_video;
    item.directory = app->download_dir;

    std::lock_guard lock(app->download_mutex);
    app->downloads.push_back(std::move(item));
}

void download_tick(Elfeed *app)
{
#ifndef _WIN32
    // If no active download, start the next one
    if (app->download_active_id != 0) return;
    if (app->download_thread.joinable())
        app->download_thread.join();

    std::lock_guard lock(app->download_mutex);
    if (app->downloads.empty()) return;

    DownloadItem *next = pick_next(app->downloads);
    if (!next) return;

    app->download_active_id = next->id;
    app->download_running = true;
    DownloadItem copy = *next;

    app->download_thread = std::thread(download_thread_func, app,
                                        std::move(copy));
#endif
}

void download_remove(Elfeed *app, int id)
{
    // Kill the child process if removing the active download
    if (id == app->download_active_id) {
        int pid = app->download_child_pid.load();
        if (pid > 0)
            kill((pid_t)pid, SIGTERM);
    }
    std::lock_guard lock(app->download_mutex);
    app->downloads.erase(
        std::remove_if(app->downloads.begin(), app->downloads.end(),
                       [&](auto &d) { return d.id == id; }),
        app->downloads.end());
}

void download_pause(Elfeed *app, int id)
{
    std::lock_guard lock(app->download_mutex);
    for (auto &d : app->downloads) {
        if (d.id == id) {
            d.paused = !d.paused;
            // Kill the child process if pausing the active download
            if (d.paused && id == app->download_active_id) {
                int pid = app->download_child_pid.load();
                if (pid > 0)
                    kill((pid_t)pid, SIGTERM);
            }
            break;
        }
    }
}

void download_stop(Elfeed *app)
{
    app->download_running = false;
    if (app->download_thread.joinable())
        app->download_thread.join();
}
