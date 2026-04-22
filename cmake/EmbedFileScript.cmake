# Invoked by add_custom_command (see EmbedFile.cmake) to generate
# OUT from SRC as a C++ source defining `extern const char SYM[]`.
# Lives in its own file so the custom command's depend tracking
# can pin to it (regenerate when the script itself changes).

file(READ "${SRC}" RAW_CONTENT)

# `file(CONFIGURE ... @ONLY)` substitutes @VAR@ tokens in CONTENT
# but leaves ${VAR} alone. The raw-string delimiter ELFEED2_EMBED
# bracketing guarantees the asset bytes pass through unmolested.
file(CONFIGURE
  OUTPUT  "${OUT}"
  CONTENT
"// Generated from @SRC@ — DO NOT EDIT.
extern const char @SYM@[];
const char @SYM@[] = R\"ELFEED2_EMBED(@RAW_CONTENT@)ELFEED2_EMBED\";
"
  @ONLY
)
