if exists('b:did_ftplugin') | finish | endif
let b:did_ftplugin = 1

" Comments are `#`-introduced; the engine's `commentstring` and
" `comments` settings cover both vim-commentary and the built-in
" `gqip`/etc. flow.
setlocal commentstring=#\ %s
setlocal comments=:#

" Directives like `download-dir`, `yt-dlp-program`, and
" `max-download-failures` are single tokens in this format. Treating
" the hyphen as part of the word makes `*` (search-word-under-cursor),
" `<C-]>`, and `iw`/`aw` text objects DTRT.
setlocal iskeyword+=-

" Live alias recognition. The static syntax/elfeed2.vim doesn't know
" what `alias` directives a particular config defines — the names
" depend on file content. Harvest `alias <name> …` definitions from
" the buffer and register a single syn match for those names
" appearing as line-leading tokens (= invocation sites). Re-runs on
" syntax (re)load and on save so renames and additions stay live.
"
" Global function name (no s: prefix) so the autocmds below can
" call it from any script context — Vim's Syntax autocmd dispatches
" outside this script's SID. Idempotent — re-defining is fine.
function! ElfeedTwoRefreshAliases() abort
  silent! syn clear elfeed2AliasInvoke
  let l:names = []
  for l:lnum in range(1, line('$'))
    let l:m = matchlist(getline(l:lnum), '\v^\s*alias\s+(\S+)')
    if !empty(l:m) && index(l:names, l:m[1]) < 0
      call add(l:names, l:m[1])
    endif
  endfor
  if empty(l:names) | return | endif
  " Splicing names into a regex via | alternation: alias names are
  " single whitespace-delimited tokens per the parser, in practice
  " alphanumeric / underscore / hyphen. Regex metacharacters in a
  " name would break this — see contrib/vim/README.md.
  let l:pat = '\v^\s*\zs(' . join(l:names, '|') . ')>'
  exec 'syn match elfeed2AliasInvoke ' . string(l:pat)
  hi def link elfeed2AliasInvoke Function
endfunction

" Hook after syntax/elfeed2.vim finishes loading. SynSet() in
" synload.vim does `syn clear` first and then sources the syntax
" file — anything we register before that runs gets wiped, which
" is exactly what bit a previous version of this hook. The Syntax
" event fires after the syntax script has run, so our match
" survives.
augroup elfeed2_aliases
  autocmd! Syntax elfeed2
  autocmd Syntax elfeed2 call ElfeedTwoRefreshAliases()
augroup END

" Save-triggered refresh, scoped to this buffer.
augroup elfeed2_aliases_buf
  autocmd! BufWritePost <buffer>
  autocmd BufWritePost <buffer> call ElfeedTwoRefreshAliases()
augroup END

" If syntax has already loaded for this buffer (e.g. ftplugin gets
" re-sourced via `:setfiletype` on a buffer that's already been
" syntax-coloured), the Syntax event won't fire again — so do an
" immediate pass to cover that.
if exists('b:current_syntax') && b:current_syntax ==# 'elfeed2'
  call ElfeedTwoRefreshAliases()
endif

let b:undo_ftplugin = 'setlocal commentstring< comments< iskeyword<'
