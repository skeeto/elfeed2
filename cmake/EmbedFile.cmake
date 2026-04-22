# elfeed2_embed_file(SRC OUT SYM)
#
# Generate a C++ source at OUT that defines `extern const char SYM[]`
# holding the byte content of SRC. Uses a C++11 raw string literal
# with a unique delimiter so quotes, backslashes, and newlines pass
# through verbatim — no escaping needed.
#
# Adds an add_custom_command so OUT is regenerated whenever SRC
# changes; the caller adds OUT to the target's source list.
#
# Caveat: SRC must not contain the literal sequence
# `)ELFEED2_EMBED"` (highly unlikely in our config / HTML assets).

function(elfeed2_embed_file SRC OUT SYM)
  add_custom_command(
    OUTPUT  "${OUT}"
    COMMAND ${CMAKE_COMMAND}
            -DSRC=${SRC}
            -DOUT=${OUT}
            -DSYM=${SYM}
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedFileScript.cmake"
    DEPENDS "${SRC}"
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/EmbedFileScript.cmake"
    COMMENT "Embedding ${SRC} -> ${SYM}"
    VERBATIM
  )
endfunction()
