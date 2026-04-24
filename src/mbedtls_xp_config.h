// MBEDTLS_USER_CONFIG_FILE for the Windows + cpp-httplib build.
// Loaded by mbedtls/build_info.h immediately after the default
// mbedtls_config.h, so any `#define` here overrides the default
// (or sets one), and `#undef` cancels one the default enabled.
//
// Scope: XP-compatible build only. CMake wires this in via
//        target_compile_definitions(mbedcrypto PUBLIC
//            MBEDTLS_USER_CONFIG_FILE="...path...")
//        under `if(WIN32 AND TARGET mbedcrypto)` — so the macOS,
//        Linux, and modern-Windows (WinHTTP) builds are unaffected.

// --- Entropy ---------------------------------------------------------
//
// Drop the default BCryptGenRandom path (bcrypt.dll, Vista+) and
// wire in our own mbedtls_hardware_poll() in src/xp_entropy.cpp,
// which uses CryptGenRandom via advapi32.dll — the XP-era API.
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

// --- Protocol version cap --------------------------------------------
//
// Disable TLS 1.3. mbedTLS 3.6.2's 1.3 client can end up
// returning MBEDTLS_ERR_SSL_INTERNAL_ERROR from deep inside
// ssl_tls13_client.c during real-world handshakes — we've
// reproduced this on the w64devkit XP build against several
// Cloudflare-fronted servers. TLS 1.2 is universally supported,
// well-tested, and the appropriate ceiling for an XP-targeted
// binary. cpp-httplib already pins min = TLS 1.2, so after this
// undef the negotiation is locked to 1.2.
//
// No other mbedtls config requires TLS 1.3 in our default
// configuration (MBEDTLS_SSL_RECORD_SIZE_LIMIT and
// MBEDTLS_SSL_EARLY_DATA are both off; the
// MBEDTLS_SSL_TLS1_3_KEY_EXCHANGE_MODE_* flags are self-contained
// and their check_config.h predicates don't demand TLS 1.3).
#ifdef MBEDTLS_SSL_PROTO_TLS1_3
#undef MBEDTLS_SSL_PROTO_TLS1_3
#endif
