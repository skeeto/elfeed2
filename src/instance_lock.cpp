#include "instance_lock.hpp"

#ifdef __WXMSW__
#  include <wx/snglinst.h>
#  include <wx/string.h>
#  include <wx/utils.h>
#  include <functional>
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <unistd.h>
#endif

InstanceLock::InstanceLock() = default;

InstanceLock::~InstanceLock()
{
#ifndef __WXMSW__
    // close(2) drops any flock held via this descriptor — the
    // kernel guarantees this happens on process exit too, so a
    // SIGKILL / crash / power loss all end up releasing the lock.
    if (fd_ >= 0) ::close(fd_);
#endif
}

bool InstanceLock::try_acquire(const std::string &db_path)
{
#ifdef __WXMSW__
    // Named kernel mutex. wx builds the name from a user-supplied
    // string; we hash the DB path so two instances against
    // different DBs (e.g. a --db test override vs the default)
    // don't collide, and prefix with the user id so multi-user
    // systems don't share mutex namespace.
    auto token = std::hash<std::string>{}(db_path);
    wxString name = wxString::Format(
        "elfeed2-%s-%lx", wxGetUserId(), (unsigned long)token);
    auto checker = std::make_unique<wxSingleInstanceChecker>(name);
    if (checker->IsAnotherRunning()) return false;
    checker_ = std::move(checker);
    return true;
#else
    // flock() on a "<db>.lock" sidecar file. We don't flock the DB
    // itself because SQLite uses POSIX fcntl byte-range locks on
    // specific regions of the DB, and on macOS flock(2) is
    // implemented atop fcntl — a whole-file flock can shadow
    // SQLite's locks. A separate sidecar sidesteps the question.
    std::string lock_path = db_path + ".lock";
    int fd = ::open(lock_path.c_str(),
                    O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return true;  // can't even create: permit run
    if (::flock(fd, LOCK_EX | LOCK_NB) < 0) {
        ::close(fd);
        return false;
    }
    fd_ = fd;
    return true;
#endif
}
