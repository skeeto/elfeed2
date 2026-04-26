" Syntax for the Elfeed2 ssh_config-style config format.
"
" Mirrors the parser in src/config.cpp — in particular the
" strip_comment rule, which is more forgiving than the usual
" "everything after #" so that hex color values (#f9f, #aabbcc)
" survive intact.

if exists('b:current_syntax')
  finish
endif

" Comments. A `#` introduces a comment iff:
"   * it's at line start OR preceded by whitespace, AND
"   * it's followed by whitespace OR end-of-line.
" So `# blah`, ` # blah`, and a bare `#` at EOL are comments;
" `#aabbcc` (color value), `foo#bar` (token-internal), and even
" `#abc` at a line start (no whitespace after) are NOT. This
" exactly matches src/config.cpp's strip_comment.
syn match elfeed2Comment "\v(^|\s)#(\s|$).*$" contains=elfeed2Todo,@Spell
syn keyword elfeed2Todo TODO FIXME XXX NOTE contained

" Stanza-body / global directives, recognized only as the first
" non-whitespace token on a line. Without the line-start anchor a
" stray `tag` or `title` mid-value would also light up. Single
" long line — vim line-continuation (leading-backslash) silently
" no-ops under cpoptions+=C, which is enabled in some scripted
" contexts (e.g. plain `vim -es`).
syn match elfeed2Directive "\v^\s*\zs(download-dir|yt-dlp-program|yt-dlp-arg|default-filter|max-connections|fetch-timeout|max-download-failures|log-retention-days|inline-images|title|tag)>"

" Stanza-introducing directives (definitions rather than values).
syn match elfeed2Stanza "\v^\s*\zs(alias|preset|color)>"

" Boolean values for `inline-images` etc. The parser is liberal about
" wording; the highlighter follows.
syn keyword elfeed2Bool yes no true false on off

" Hex color literals (#RRGGBB or #RGB). The strip_comment rule above
" leaves these alone, so we can match them safely as values.
syn match elfeed2Color "#[0-9A-Fa-f]\{6}\>"
syn match elfeed2Color "#[0-9A-Fa-f]\{3}\>"

" URL stanza heads + any URL anywhere in a value.
syn match elfeed2Url "\<\S\+://\S\+"

" Alias template placeholder. `alias youtube https://…?channel_id={}`
" — the {} is what the alias-invocation argument substitutes into.
syn match elfeed2Placeholder "{}"

" Filter-DSL tokens (used in default-filter values and preset
" filter-strings). Highlighted globally rather than scoped to those
" directives — comments take precedence and URLs claim their own
" span first, so false positives in other contexts are rare. We
" only mark the unambiguous prefixes (+, -, @, #); the substring
" prefixes (=, ~, !) match too much in URL queries and free text,
" so we leave them uncolored.
syn match elfeed2FilterTag   "\v[+-][a-zA-Z0-9_-]+>"
" Magic mode (\m) for this one — in very-magic (\v), `@` is the
" prefix for lookaround atoms (\@=, \@!, …) and a bare `@` raises
" E866. Magic mode treats it as a literal character, so we use
" `\m` and write `@` plainly.
syn match elfeed2FilterAge   "\m@[a-zA-Z0-9-]\+"
syn match elfeed2FilterLimit "\v#\d+>"

hi def link elfeed2Comment      Comment
hi def link elfeed2Todo         Todo
hi def link elfeed2Directive    Statement
hi def link elfeed2Stanza       Type
hi def link elfeed2Bool         Boolean
hi def link elfeed2Color        Constant
hi def link elfeed2Url          Underlined
hi def link elfeed2Placeholder  Special
hi def link elfeed2FilterTag    Identifier
hi def link elfeed2FilterAge    Constant
hi def link elfeed2FilterLimit  Number

let b:current_syntax = 'elfeed2'
