// Force-included (via -include) at the top of every mbedTLS TU
// when building Windows + cpp-httplib. Gets mbedTLS compiling into
// code that runs on XP, where msvcrt.dll is missing some symbols
// that current mingw-w64 headers cheerfully reference.
//
// Current issue: mbedTLS's platform.c has
//
//     #if defined(_TRUNCATE)
//         ret = vsnprintf_s(s, n, _TRUNCATE, fmt, arg);
//     #else
//         ret = vsnprintf(s, n, fmt, arg);
//         ...  // manual length/-1 handling
//     #endif
//
// vsnprintf_s is Vista-era in msvcrt. Recent mingw-w64 <stdio.h>
// defines _TRUNCATE unconditionally, so mbedTLS takes the
// vsnprintf_s branch, the linker pulls in a msvcrt import for it,
// and XP's kernel loader rejects the .exe at startup.
//
// The fallback branch (plain vsnprintf) is XP-safe. Pull <stdio.h>
// in here once — subsequent includes from platform.c become no-ops
// through its header guard — then clear _TRUNCATE so mbedTLS sees
// it undefined and picks the fallback branch.
//
// Scoped to _WIN32 so this header is a no-op when force-included
// into a non-Windows build (shouldn't happen, but defensive).

#ifndef ELFEED_MBEDTLS_XP_FIXUPS_H
#define ELFEED_MBEDTLS_XP_FIXUPS_H

#ifdef _WIN32
#include <stdio.h>
#ifdef _TRUNCATE
#undef _TRUNCATE
#endif
#endif

#endif // ELFEED_MBEDTLS_XP_FIXUPS_H
