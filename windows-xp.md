# Windows XP build notes

Everything we learned making Elfeed2 run on Windows XP. Lives on the
`windows-xp` branch (seven commits on top of `480f63a`); this file
preserves the context so the branch can sit a while before merging
without the reasoning evaporating.

## Context

Default Windows builds use WinHTTP + Schannel (see `src/http_win.cpp`).
Schannel on XP bottoms out at TLS 1.0 with legacy cipher suites — no
server most of the web has talked to in the last decade will negotiate
with it. So for XP we bypass the OS stack entirely and build against
**cpp-httplib + mbedTLS** (the same stack our non-Windows builds use),
then make that combo actually compile and run against an XP-targeting
SDK and XP's msvcrt.dll.

The result is a second HTTP backend selectable at configure time. The
WinHTTP default is still the right choice for Windows 7+; cpp-httplib is
opt-in and primarily there for XP.

## Building for XP

Prerequisites:

1. An XP-targeting mingw-w64 toolchain. We use `w64devkit` with
   `src/variant-x86.patch` applied. Any mingw built with
   `--with-default-win32-winnt=0x0501` and targeting msvcrt (not UCRT)
   works. Check a test binary with
   `objdump -p foo.exe | grep 'DLL Name:'` — no `api-ms-win-crt-*`
   imports means msvcrt.
2. Configure with the backend opt-in:

   ```
   cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/Toolchain-Mingw64.cmake \
         -DELFEED2_HTTP_BACKEND=cpp-httplib
   ```

   (Substitute your own toolchain file if not using our mingw-cross
   one. w64devkit is typically invoked natively on Windows, no
   toolchain file needed.)
3. Build. All the XP accommodations are conditional on `_WIN32` plus
   various `_WIN32_WINNT` thresholds, so they're no-ops on a modern
   Windows mingw cross build.

## The seven things XP needed

Ordered by the layer they touch:

### 1. Backend selector

`CMakeLists.txt` has `ELFEED2_HTTP_BACKEND` (`winhttp` / `cpp-httplib`)
as a cache variable, defaulting to `winhttp` on Windows and
`cpp-httplib` elsewhere. It controls:

- Whether `FetchTLS.cmake` + `FetchCppHttplib.cmake` are included at
  all (they aren't for the WinHTTP backend).
- Which HTTP source is compiled: `src/http_win.cpp` vs
  `src/http_cpphttplib.cpp` (the latter is what `src/http_posix.cpp`
  was renamed to — the name was always a lie since it's actually
  cpp-httplib-specific, not POSIX-specific, and the rename makes it
  correct now that it also builds on Windows).
- Which libs are linked: WinHTTP pulls `winhttp`; cpp-httplib pulls
  `ws2_32 crypt32 advapi32` + `tls + cpp-httplib` interface targets.
- Whether the CA-bundle embed step runs (see §2).

The `.rc` resource block is independent of the backend — it always
ships on Windows for version info + the DPI manifest.

### 2. Embedded Mozilla CA bundle

WinHTTP uses the OS trust store. cpp-httplib + mbedTLS needs a PEM
file path. Windows has no equivalent system bundle, so we ship one:

- `assets/cacert.pem` — 226 KB, pinned by content (SHA256 match to a
  specific snapshot of `https://curl.se/ca/cacert.pem`). Bumping is a
  deliberate commit.
- `cmake/EmbedFile.cmake` turns it into `embedded_cacert_pem[]` (a C++
  raw string literal wrapper); the rule only fires on
  `WIN32 AND ELFEED2_HTTP_BACKEND STREQUAL "cpp-httplib"`.
- `ELFEED_EMBED_CA_BUNDLE` compile define gates the reference in
  `src/app.cpp`, which in `elfeed_init()` writes the bundle to
  `<user_data_dir>/cacert.pem` on every launch and passes that path
  into `http_init()`. Rewriting on every launch is cheap (~220 KB of
  plain text) and means in-tree updates propagate immediately.
- `http_init()` got an optional parameter:
  `std::string http_init(const std::string &forced_ca_path = {})`. A
  non-empty path is used verbatim after a `stat()` check; empty falls
  back to probing the `kCaPaths[]` list (macOS / Linux / BSD). The
  WinHTTP version of `http_init` accepts the parameter and ignores
  it, for signature parity.

### 3. zlib FetchContent fallback

cpp-httplib with `CPPHTTPLIB_ZLIB_SUPPORT` auto-decompresses gzipped
responses — several feed publishers (Cloudflare especially) send gzip
regardless of `Accept-Encoding`, so without this pugixml blows up on
the raw deflate stream. Zlib is present on macOS, every Linux distro,
and Debian's mingw-w64 sysroot, but **Homebrew's mingw-w64 doesn't
bundle it**, and neither does w64devkit. `cmake/FetchCppHttplib.cmake`
falls back to fetching zlib 1.3.1 (pinned tarball + SHA-256) when
`find_package(ZLIB)` misses, and links `zlibstatic` instead of
`ZLIB::ZLIB` in that case.

### 4. Vista+ API shims (cpp-httplib call sites)

cpp-httplib 0.43 unconditionally references these Vista-/Win8-era
APIs:

| Symbol | Intro | Used in |
|---|---|---|
| `WSAPoll` | Vista (2007) | every request's socket-ready wait |
| `inet_pton` | Vista | host-string validation |
| `struct pollfd` + POLL* | Vista | type for the above |
| `CreateFile2` | Win8 (2012) | `mmap::open` |
| `CreateFileMappingFromApp` | Win8 | `mmap::open` |

An XP-targeting SDK (`_WIN32_WINNT=0x0501`) leaves most of these
undeclared. cpp-httplib also has an explicit
`#error "doesn't support Windows 8 or lower"` near the top of
`httplib.h` that fires when `_WIN32_WINNT < 0x0A00`.

`src/xp_shims.hpp` handles all of this:

- Declares `struct pollfd` + `POLLIN` / `POLLOUT` / `POLLERR` / etc.
  (gated on `_WIN32_WINNT < 0x0600` so modern builds don't collide
  with the system decl).
- Defines `elfeed_xp_wsapoll` (implemented via `select()` — FD_SETSIZE
  of 64 covers our concurrent fetch pool, which caps at 16),
  `elfeed_xp_inet_pton` (via XP-era `WSAStringToAddressA`), plus
  `elfeed_xp_create_file2` and `elfeed_xp_create_file_mapping_from_app`
  as failure stubs. Both `mmap::open` helpers are on cpp-httplib's
  server-side file-sending path, which our client-only code never
  exercises — when mmap::open sees `INVALID_HANDLE_VALUE` / `NULL` it
  returns false and cpp-httplib moves on.
- Macro-renames cpp-httplib's call sites to the `elfeed_xp_*` names:

  ```cpp
  #define WSAPoll                  elfeed_xp_wsapoll
  #define inet_pton                elfeed_xp_inet_pton
  #define CreateFile2              elfeed_xp_create_file2
  #define CreateFileMappingFromApp elfeed_xp_create_file_mapping_from_app
  ```

  The macro-rename-to-distinct-names pattern (rather than defining a
  `static inline WSAPoll(…)` with the system name) sidesteps the
  "was declared extern and later static [-fpermissive]" conflict that
  trips on mingw-w64 headers that declare some of these unconditionally
  regardless of `_WIN32_WINNT` — `memoryapi.h` for
  `CreateFileMappingFromApp` is the usual offender. Our shim functions
  have different names from the MS symbols, so there's no collision;
  the #defines route cpp-httplib's call sites onto them and nothing
  else in the TU notices.
- `#undef _WIN32_WINNT` at the tail, so cpp-httplib's version-check
  `#error` sees `defined(_WIN32_WINNT)` as false and short-circuits.
  Safe because cpp-httplib never references `_WIN32_WINNT` anywhere
  else in its header.

`src/http_cpphttplib.cpp` includes `xp_shims.hpp` immediately before
`<httplib.h>`. On non-Windows it's an `_WIN32`-guarded no-op; on a
modern-Windows mingw it runs but produces identical behavior through
the renamed shims (select() instead of WSAPoll — irrelevant overhead
at feed-reader traffic).

**Rejected alternative: gc-sections.** We tried overriding
`MBEDTLS_PLATFORM_STD_VSNPRINTF` to plain `vsnprintf` and enabling
`-ffunction-sections` / `-Wl,--gc-sections` to drop the now-unreferenced
helpers. mingw-ld doesn't drop them: PE's `.pdata` / `.xdata` exception
tables cross-reference the `.text` sections and anchor them live. The
macro-rename approach bypasses the problem entirely.

### 5. mbedTLS entropy: BCryptGenRandom → CryptGenRandom

mbedTLS's `library/entropy_poll.c` reaches for `BCryptGenRandom` from
`bcrypt.dll` on `_WIN32`, unconditionally. bcrypt.dll arrived in Vista;
on XP the kernel loader rejects the whole .exe at startup because
bcrypt.dll isn't present.

Swapped via mbedTLS's documented hook in `src/mbedtls_xp_config.h`
(`MBEDTLS_USER_CONFIG_FILE`; see §7):

```c
#define MBEDTLS_NO_PLATFORM_ENTROPY     // disable built-in BCrypt path
#define MBEDTLS_ENTROPY_HARDWARE_ALT    // expect user mbedtls_hardware_poll
```

Implementation: `src/xp_entropy.cpp` defines `mbedtls_hardware_poll`
using `CryptGenRandom` via `advapi32.dll` — the XP-era API, still
available and working on Windows 10/11. It's a ~30-line function;
`CryptAcquireContextA(CRYPT_VERIFYCONTEXT | CRYPT_SILENT)` plus a
chunk-loop around `CryptGenRandom` to honor the DWORD-bounded size.

Linker pulls in advapi32 for this.

### 6. msvcrt!_vsnprintf_s avoidance

mbedTLS's `library/platform.c` has:

```c
#if defined(_TRUNCATE)
    ret = vsnprintf_s(s, n, _TRUNCATE, fmt, arg);
#else
    ret = vsnprintf(s, n, fmt, arg);
    // ... manual truncation handling
#endif
```

Recent mingw-w64 `<stdio.h>` defines `_TRUNCATE` unconditionally, so
mbedTLS takes the `vsnprintf_s` branch — and that resolves to
`msvcrt!_vsnprintf_s`, which XP's msvcrt.dll doesn't export. The
fallback `vsnprintf` branch is XP-safe; we just need to convince
mbedTLS to pick it.

`src/mbedtls_xp_fixups.h` is force-included (via `-include`) at the
top of every mbedTLS TU. It pulls in `<stdio.h>` once, then
`#undef _TRUNCATE`. Subsequent `#include <stdio.h>` from within
mbedTLS sources are no-ops (header guard), so the undef sticks for
the rest of the TU. `platform.c`'s `#if defined(_TRUNCATE)` then goes
false and the plain `vsnprintf` branch compiles. No reference to
`vsnprintf_s` reaches the linker.

Wired in CMake as
`target_compile_options(mbedcrypto PRIVATE -include …)` — PRIVATE
because the fixup only matters when mbedTLS's own TUs are being
compiled, not when consumers pull in mbedTLS headers.

**Again rejected: gc-sections + STD_VSNPRINTF override.** Same
pdata/xdata anchor problem as §4.

### 7. TLS 1.2 cap

With the above, the binary ran on XP and got as far as the TLS
handshake before failing with

```
SSL connection failed [ssl_err=4 backend=-0x6C00:
                       SSL - Internal error …]
```

for every HTTPS host. `MBEDTLS_ERR_SSL_INTERNAL_ERROR` (-0x6C00) is a
catch-all; the `http_init()` self-tests for CA parsing and DRBG
seeding (see §8) both succeeded, so the failure was inside the
handshake itself. Root cause:

> **mbedTLS 3's TLS 1.3 is a separate crypto code path, funneled
> through PSA Crypto.** Every TLS 1.3 handshake calls
> `psa_crypto_init()` in `ssl_tls13_generic.c:40`. cpp-httplib hand-
> wires the classic `mbedtls_ctr_drbg_*` + `mbedtls_entropy_*` APIs
> in `create_client_context` — the TLS 1.2 path uses those directly —
> but never calls `psa_crypto_init()`. On TLS 1.3 handshake, PSA's
> `BAD_STATE` / unmapped errors translate to
> `MBEDTLS_ERR_SSL_INTERNAL_ERROR` via the `psa_to_ssl_errors[]`
> table in `library/psa_util.c`, and we see -0x6C00.
>
> cpp-httplib pins mbedTLS's **minimum** version at TLS 1.2 but
> leaves the **maximum** unset, so any server offering 1.3 lands
> on the broken path.

Fix: `src/mbedtls_xp_config.h` (see §8 for how it's wired in) has

```c
#ifdef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_PROTO_TLS1_3
#endif
```

`check_config.h` predicates don't require TLS 1.3 in our default
config (`MBEDTLS_SSL_RECORD_SIZE_LIMIT` and `MBEDTLS_SSL_EARLY_DATA`
are off; the three `MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_*` flags are
self-contained).

The "proper" fix, if TLS 1.3 ever mattered on the XP target, would
be to patch cpp-httplib's `create_client_context` to call
`psa_crypto_init()` before SSL setup, then diagnose why PSA init
reports `BAD_STATE` on this specific environment (probably takes
`mbedtls_debug_set_threshold` + `mbedtls_ssl_conf_dbg` instrumentation).
Not worth it for a feed reader; TLS 1.2 is universally supported and
well-tested in mbedTLS.

### 8. Observability (and the MBEDTLS_USER_CONFIG_FILE wiring)

Two things added during the investigation that are worth keeping:

**Structured SSL error detail.** `describe_http_error()` at the top of
`src/http_cpphttplib.cpp` enriches cpp-httplib's bare
`"SSL connection failed"` with:

- `res.ssl_error()` — cpp-httplib's `ErrorCode` category as int
  (Fatal=4, CertVerifyFailed=6, HostnameMismatch=7, …).
- `res.ssl_backend_error()` — raw mbedTLS return value.
- For mbedTLS builds only, `mbedtls_strerror()` turns the backend
  code into text.

So instead of "SSL connection failed", the log now shows:

```
SSL connection failed [ssl_err=4 backend=-0x6C00: SSL - Internal
                       error (eg, unexpected failure in lower-level
                       module)]
```

This is what told us to look at `MBEDTLS_ERR_SSL_INTERNAL_ERROR`
specifically, which led to the PSA theory.

**http_init self-tests.** On mbedTLS builds, `http_init()` now does
two probes up front:

1. `mbedtls_x509_crt_parse_file` on the CA bundle path, with a count
   of successfully-parsed certificates.
2. `mbedtls_ctr_drbg_seed` with `mbedtls_entropy_func` +
   (transitively) our `mbedtls_hardware_poll`.

If either fails, we log a specific message (`CA parse failed: …` or
`entropy seed failed: …`) at startup. Without the probes, those
failures show up as an opaque SSL failure at first fetch. During
XP bring-up this confirmed the CA bundle was fine and our entropy
was fine — eliminating the two most likely causes and pointing us
at the handshake itself (which turned out to be TLS 1.3 / PSA).

**The MBEDTLS_USER_CONFIG_FILE wiring.** The entropy defines in §5
and the TLS 1.2 cap in §7 are both `#define`s / `#undef`s that need
to be visible to every mbedTLS TU **and** to every consumer TU that
pulls in mbedTLS headers (via cpp-httplib, in our case), because
`build_info.h` re-processes the config in each. Raw `-D` flags work
for simple defines but not for `#undef`, so we consolidated into:

```c
// src/mbedtls_xp_config.h
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#undef  MBEDTLS_SSL_PROTO_TLS1_3   // if defined
```

wired via

```cmake
target_compile_definitions(${_t} PUBLIC
    MBEDTLS_USER_CONFIG_FILE=${_xp_mbedtls_cfg})
```

on each of the `mbedcrypto` / `mbedx509` / `mbedtls` targets. PUBLIC
because elfeed2's own TUs consume mbedtls headers transitively. The
quoting matters — CMake needs `"…path…"` (with the inner quotes) so
the preprocessor directive `#include MBEDTLS_USER_CONFIG_FILE`
resolves to a string literal.

## Files added / touched

New files:

- `assets/cacert.pem` — Mozilla CA bundle
- `src/xp_shims.hpp` — Vista+ API shims for cpp-httplib
- `src/xp_entropy.cpp` — mbedtls_hardware_poll via CryptGenRandom
- `src/mbedtls_xp_fixups.h` — force-included, clears `_TRUNCATE`
- `src/mbedtls_xp_config.h` — `MBEDTLS_USER_CONFIG_FILE` payload

Renamed:

- `src/http_posix.cpp` → `src/http_cpphttplib.cpp` (88% similarity)

Modified:

- `CMakeLists.txt` — backend selector, mbedTLS target defines, link
  line changes, conditional CA embed
- `cmake/FetchCppHttplib.cmake` — zlib fetch fallback
- `README.md` — build-section note
- `src/app.cpp` — writes embedded CA bundle, calls http_init with
  forced path when `ELFEED_EMBED_CA_BUNDLE`
- `src/http.hpp` — `http_init()` takes optional forced_ca_path
- `src/http_cpphttplib.cpp` — top comment, http_init extension,
  `describe_http_error()`, self-tests, includes xp_shims.hpp
- `src/http_win.cpp` — http_init signature parity

## Commits, in order

```
53f9934  Windows: opt-in cpp-httplib HTTP backend for XP compat
11fb5d2  Windows: shim Vista+ APIs for XP-targeting mingw toolchains
ff5810d  Windows: swap mbedTLS entropy source from BCryptGenRandom to CryptGenRandom
6d61ce3  Windows: avoid msvcrt!_vsnprintf_s in mbedTLS for XP
d2841ac  HTTP: surface TLS backend detail on SSL errors
361513c  http_init: self-test CA parsing and entropy seed up front
05573ac  Windows: cap mbedTLS at TLS 1.2 on the XP build
```

## Caveats

- **Clock**: mbedTLS's cert-expiry check uses `time()`. An XP machine
  with CMOS drift wide enough to put Mozilla's CA bundle "not yet
  valid" (e.g. reverted to 2002) will fail every HTTPS connection
  with a certificate-date mbedTLS error. Not specific to this build,
  but worth noting when debugging.
- **The PSA / TLS 1.3 story** is specific to cpp-httplib's mbedTLS
  integration in v0.43 and mbedTLS 3.6.x. Future upstream versions
  may change the picture; if cpp-httplib starts calling
  `psa_crypto_init` or mbedTLS's 1.3 path stops routing through PSA,
  `MBEDTLS_SSL_PROTO_TLS1_3` could be re-enabled.
- **Downloads**: the direct-HTTP enclosure download path uses the
  same cpp-httplib stack, so it inherits all the fixes. yt-dlp /
  curl subprocess downloads depend on whatever the user has on the
  XP machine and are not our concern.
