// Shims for Vista- and Win8-era Windows APIs that cpp-httplib uses
// unconditionally, so an XP-targeting mingw toolchain (w32devkit
// and friends — Windows SDKs that leave _WIN32_WINNT undefined or
// set it low) can still compile and link the library.
//
// Why macro-renaming rather than replacement declarations: a
// `static inline` with the same name as the system declaration
// trips "was declared extern and later static [-fpermissive]" on
// modern mingw-w64 headers that declare the symbol regardless of
// _WIN32_WINNT (e.g. memoryapi.h's unconditional
// CreateFileMappingFromApp). Instead, we give the shims distinct
// names (elfeed_xp_*) and #define the original identifiers to
// redirect cpp-httplib's call sites only. The system declaration
// is untouched; any TU that actually wants the system function
// stays wired up normally.
//
// Why #undef _WIN32_WINNT just before <httplib.h>: v0.43's header
// has `#if defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0A00 / #error`
// — a hard block for anything pre-Win10. Clearing the macro makes
// `defined()` false, short-circuiting the error. cpp-httplib never
// references _WIN32_WINNT anywhere else, so this is safe.
//
// No effect on non-Windows platforms: the whole header is gated on
// _WIN32. On the normal modern-Windows mingw cross build it's
// harmless — the struct pollfd block is skipped (system already
// has it), the shim functions are an unused pair of static inline
// definitions, and the macro rename routes cpp-httplib's calls
// through them instead of WS2_32's WSAPoll. select() is a hair
// slower per poll than WSAPoll in theory, but the cost is
// invisible at feed-reader traffic.

#ifndef ELFEED_XP_SHIMS_HPP
#define ELFEED_XP_SHIMS_HPP

#ifdef _WIN32

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstring>

// struct pollfd and POLL* constants: declared by winsock2.h only
// when _WIN32_WINNT >= 0x0600. On an XP toolchain with
// _WIN32_WINNT unset (or set to 0x0501), we declare our own.
// Layout matches WSAPOLLFD so runtime-equivalent calls agree on
// the struct's byte layout.
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
struct pollfd {
    SOCKET fd;
    short  events;
    short  revents;
};
typedef unsigned long nfds_t;
#define POLLRDNORM 0x0100
#define POLLRDBAND 0x0200
#define POLLIN     (POLLRDNORM | POLLRDBAND)
#define POLLPRI    0x0400
#define POLLWRNORM 0x0010
#define POLLOUT    (POLLWRNORM)
#define POLLWRBAND 0x0020
#define POLLERR    0x0001
#define POLLHUP    0x0002
#define POLLNVAL   0x0004
#endif

// ---- Shim implementations (distinct names) -------------------------

// WSAPoll replacement via select(). FD_SETSIZE (64 on Windows by
// default) comfortably covers our concurrent fetch pool (max 16).
static inline int elfeed_xp_wsapoll(struct pollfd *fdArray,
                                    unsigned long fds, int timeout)
{
    if (fds == 0) {
        if (timeout > 0) Sleep((DWORD)timeout);
        return 0;
    }

    fd_set read_fds, write_fds, except_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    FD_ZERO(&except_fds);
    for (unsigned long i = 0; i < fds; i++) {
        if (fdArray[i].fd == (SOCKET)INVALID_SOCKET) continue;
        if (fdArray[i].events & POLLIN)
            FD_SET(fdArray[i].fd, &read_fds);
        if (fdArray[i].events & POLLOUT)
            FD_SET(fdArray[i].fd, &write_fds);
        // Always register on except set so hangups / errors
        // surface as POLLERR regardless of requested events.
        FD_SET(fdArray[i].fd, &except_fds);
    }

    TIMEVAL tv;
    TIMEVAL *ptv = nullptr;
    if (timeout >= 0) {
        tv.tv_sec  = timeout / 1000;
        tv.tv_usec = (timeout % 1000) * 1000;
        ptv = &tv;
    }
    int rv = select(0, &read_fds, &write_fds, &except_fds, ptv);

    for (unsigned long i = 0; i < fds; i++) {
        fdArray[i].revents = 0;
        if (fdArray[i].fd == (SOCKET)INVALID_SOCKET) continue;
        if (FD_ISSET(fdArray[i].fd, &read_fds))
            fdArray[i].revents |= POLLIN;
        if (FD_ISSET(fdArray[i].fd, &write_fds))
            fdArray[i].revents |= POLLOUT;
        if (FD_ISSET(fdArray[i].fd, &except_fds))
            fdArray[i].revents |= POLLERR;
    }
    return rv;  // poll-compatible: >0 ready, 0 timeout, -1 error
}

// inet_pton replacement via WSAStringToAddressA (XP-era API).
// Same return contract as Unix inet_pton: 1 ok, 0 not a valid
// address for `af`, -1 unsupported family.
static inline int elfeed_xp_inet_pton(int af, const char *src,
                                      void *dst)
{
    if (!src || !dst) return 0;
    if (af != AF_INET && af != AF_INET6) {
        WSASetLastError(WSAEAFNOSUPPORT);
        return -1;
    }
    // WSAStringToAddressA's first arg is non-const LPSTR; copy
    // the input to honor C's const-string contract. IPv6
    // textual form + NUL fits in 64 bytes with plenty of room
    // for bracketed forms cpp-httplib might pass.
    char buf[64];
    size_t n = std::strlen(src);
    if (n >= sizeof(buf)) return 0;
    std::memcpy(buf, src, n + 1);

    struct sockaddr_storage ss;
    std::memset(&ss, 0, sizeof(ss));
    int ss_len = (int)sizeof(ss);
    ((sockaddr *)&ss)->sa_family = (ADDRESS_FAMILY)af;
    if (WSAStringToAddressA(buf, af, nullptr,
                            (LPSOCKADDR)&ss, &ss_len) != 0)
        return 0;

    if (af == AF_INET) {
        std::memcpy(dst, &((sockaddr_in *)&ss)->sin_addr,
                    sizeof(struct in_addr));
    } else {
        std::memcpy(dst, &((sockaddr_in6 *)&ss)->sin6_addr,
                    sizeof(struct in6_addr));
    }
    return 1;
}

// CreateFile2 / CreateFileMappingFromApp stubs. cpp-httplib only
// uses these inside mmap::open(), part of its server-side file
// sending path that client GETs never exercise. A stub that
// returns failure propagates as mmap::open() returning false.
//
// Parameter types are deliberately weak (void *) so the shims
// don't depend on struct layouts that differ across Windows
// SDK versions (LPCREATEFILE2_EXTENDED_PARAMETERS isn't always
// declared). cpp-httplib passes NULL for the extended params
// anyway, which converts to void * without issue.
static inline HANDLE WINAPI elfeed_xp_create_file2(
    LPCWSTR, DWORD, DWORD, DWORD, void *)
{
    return INVALID_HANDLE_VALUE;
}

static inline HANDLE WINAPI elfeed_xp_create_file_mapping_from_app(
    HANDLE, LPSECURITY_ATTRIBUTES, ULONG, ULONG64, LPCWSTR)
{
    return NULL;
}

// ---- Redirect cpp-httplib's call sites -----------------------------

#define WSAPoll                  elfeed_xp_wsapoll
#define inet_pton                elfeed_xp_inet_pton
#define CreateFile2              elfeed_xp_create_file2
#define CreateFileMappingFromApp elfeed_xp_create_file_mapping_from_app

// Clear _WIN32_WINNT so cpp-httplib's "Windows 10 or later" #error
// sees `defined(_WIN32_WINNT)` as false and short-circuits. The
// value has already informed every system header inclusion above,
// so nothing else in this TU needs it.
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif

#endif // _WIN32

#endif // ELFEED_XP_SHIMS_HPP
