# Vim filetype for Elfeed2 config

Filetype detection, syntax highlighting, and a small ftplugin for the
ssh_config-style Elfeed2 config (`$XDG_CONFIG_HOME/elfeed2/config`).

The syntax follows the parser in `src/config.cpp` exactly — especially
the comment rule, which exempts `#`-prefixed values like hex colors so
`color youtube #ff99ff` highlights as a directive + tag + color rather
than a directive followed by a comment.

## Install

### vim-plug

```vim
Plug 'skeeto/elfeed2', {'rtp': 'contrib/vim'}
```

### Manual

Either symlink or append the runtimepath:

```vim
" In ~/.vimrc or ~/.config/nvim/init.vim
set runtimepath+=~/path/to/elfeed2/contrib/vim
```

Or copy the three files into the corresponding `~/.vim/`
(`~/.config/nvim/`) subdirectories:

```
~/.vim/ftdetect/elfeed2.vim
~/.vim/ftplugin/elfeed2.vim
~/.vim/syntax/elfeed2.vim
```

## What's covered

- Detects `*/elfeed2/config` and `*.elfeed2`. For other paths, drop a
  modeline at the top of the file: `# vim: ft=elfeed2`.
- Highlights global directives (`download-dir`, `yt-dlp-program`, …),
  stanza definitions (`alias`, `preset`, `color`), per-stanza
  `title` / `tag` lines, hex colors, URLs, the `{}` alias placeholder,
  and the unambiguous filter-DSL prefixes (`+tag`, `-tag`, `@age`,
  `#limit`).
- **Live alias names.** `alias` definitions in the file are scanned
  on open and on save; any line whose first token is a known alias
  name is highlighted as `Function` (an invocation site). Renaming
  or adding aliases takes effect on the next `:write`. Caveat: the
  names get spliced into a regex via `|` alternation, so regex
  metacharacters in an alias name would break the match — in
  practice aliases are alphanumeric / underscore / hyphen, which the
  parser already requires as single-token names.
- Sets `commentstring=# %s` and adds `-` to `iskeyword` so
  hyphenated directive names (`download-dir`,
  `max-download-failures`) act as one word for `*`, `<C-]>`, `iw`/`aw`.

## What's not covered

- The `=feed` / `~feed` / `!title` filter prefixes are unhighlighted
  — they collide too readily with substrings in URL queries and free
  text to be worth marking globally.
- No indenting rules; the format is flat.
