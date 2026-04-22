#ifndef ELFEED_INSTANCE_LOCK_HPP
#define ELFEED_INSTANCE_LOCK_HPP

#include <memory>
#include <string>

class wxSingleInstanceChecker;

// Cross-process single-instance guard scoped to a particular SQLite
// database file. Prevents two copies of elfeed2 from running against
// the same DB, which would race on writes and clobber each other's
// AUI / geometry state at close time.
//
// Windows: thin wrapper over wxSingleInstanceChecker, which takes a
// named kernel mutex. OS-managed lifetime, nothing on disk, auto-
// released on process termination even if the process crashes.
//
// POSIX: directly flock()s a zero-byte "<db_path>.lock" sidecar
// file. The kernel releases the lock when the process exits (clean
// or otherwise), so stale locks never persist semantically. The
// file itself may remain after a crash but is reused by the next
// launch — no PID-scraping handshake, no cleanup dance, and the
// file is clearly tied to the DB by name.
class InstanceLock {
public:
    InstanceLock();
    ~InstanceLock();
    InstanceLock(const InstanceLock &) = delete;
    InstanceLock &operator=(const InstanceLock &) = delete;

    // Try to claim exclusive ownership for `db_path`. Returns true
    // on success, false if another process already holds it. On
    // platforms where the underlying mechanism can't be initialized
    // (e.g. a read-only filesystem for the sidecar), returns true
    // — better to run without the guard than to refuse startup.
    bool try_acquire(const std::string &db_path);

private:
#ifdef __WXMSW__
    std::unique_ptr<wxSingleInstanceChecker> checker_;
#else
    int fd_ = -1;
#endif
};

#endif
