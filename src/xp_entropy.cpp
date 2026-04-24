// mbedTLS entropy source for XP-compatible builds.
//
// mbedTLS's default Windows entropy_poll uses BCryptGenRandom from
// bcrypt.dll (Vista+). On an XP-targeting mingw toolchain the link
// pulls in the bcrypt.dll import, and XP's kernel loader rejects
// the whole .exe at startup because bcrypt.dll doesn't exist there.
//
// Solution via mbedTLS's documented hook:
//   MBEDTLS_NO_PLATFORM_ENTROPY   — compile out the BCrypt path
//   MBEDTLS_ENTROPY_HARDWARE_ALT  — look for mbedtls_hardware_poll
//
// Both defines are set on the mbedcrypto / mbedx509 / mbedtls
// targets from CMakeLists. This TU provides the implementation:
// CryptGenRandom via advapi32.dll, available since Windows 95 and
// still working fine on Windows 10/11. We lose the AES-CTR-DRBG
// hardware RNG path on modern Windows but gain a single code path
// that runs on every Windows version we care about.
//
// Only compiled into the executable when WIN32 AND
// ELFEED2_HTTP_BACKEND=cpp-httplib (see CMakeLists); the WinHTTP
// build doesn't link mbedTLS at all.

#ifdef _WIN32

#include <windows.h>
#include <wincrypt.h>

extern "C" int mbedtls_hardware_poll(void *data,
                                     unsigned char *output,
                                     size_t len,
                                     size_t *olen)
{
    (void)data;
    *olen = 0;

    // CRYPT_VERIFYCONTEXT says "don't open / create a persistent
    // keyset" — we only want an ephemeral RNG handle. CRYPT_SILENT
    // suppresses UI even if one would otherwise be shown. PROV_RSA_FULL
    // is the canonical provider for CryptGenRandom; its presence is
    // universal on XP and later.
    HCRYPTPROV h = 0;
    if (!CryptAcquireContextA(&h, nullptr, nullptr, PROV_RSA_FULL,
                              CRYPT_VERIFYCONTEXT | CRYPT_SILENT)) {
        return -1;
    }

    // Pull in chunks to honor the DWORD-bounded API even though len
    // is size_t (matters on 64-bit Windows with a >4GB request —
    // never happens in mbedTLS's entropy path, but costs nothing).
    size_t remaining = len;
    unsigned char *p = output;
    int rv = 0;
    while (remaining > 0) {
        DWORD chunk = (remaining > 0x7fffffffu)
                        ? 0x7fffffffu
                        : (DWORD)remaining;
        if (!CryptGenRandom(h, chunk, (BYTE *)p)) {
            rv = -1;
            break;
        }
        p += chunk;
        remaining -= chunk;
    }

    CryptReleaseContext(h, 0);
    if (rv == 0) *olen = len;
    return rv;
}

#endif // _WIN32
