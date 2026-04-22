# Invoked by add_custom_command (see EmbedFile.cmake) to generate
# OUT from SRC as a C++ source defining `extern const char SYM[]`.
# Lives in its own file so the custom command's depend tracking
# can pin to it (regenerate when the script itself changes).

# `cmake -P` runs this in a fresh interpreter that doesn't inherit
# the parent CMakeLists.txt's cmake_minimum_required, so policies
# land at their pre-3.0 defaults. Match the main project's floor
# so CMP0053 (and friends up through 3.25) are NEW — otherwise
# the file(CONFIGURE) below triggers a dev-warning on recent
# CMake about escape sequences inside a double-quoted string.
cmake_minimum_required(VERSION 3.25)

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
