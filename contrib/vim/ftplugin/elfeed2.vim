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
" depend on file content. This function harvests `alias <name> …`
" definitions from the buffer and registers a single syn match for
" all known names appearing as line-leading tokens (= invocation
" sites). Re-runs on file open and on save so renames and additions
" stay live without restarting Vim.
function! s:Elfeed2RefreshAliases() abort
  " First call has no group to clear — silent! suppresses E28.
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
  " name would break this — worth a comment in the README.
  let l:pat = '\v^\s*\zs(' . join(l:names, '|') . ')>'
  exec 'syn match elfeed2AliasInvoke ' . string(l:pat)
  hi def link elfeed2AliasInvoke Function
endfunction

augroup elfeed2_aliases
  autocmd! * <buffer>
  autocmd BufReadPost,BufWritePost <buffer> call s:Elfeed2RefreshAliases()
augroup END

" First-load pass — without this, alias invocations don't light up
" until the user saves, which is jarring on a freshly-opened config.
call s:Elfeed2RefreshAliases()

let b:undo_ftplugin = 'setlocal commentstring< comments< iskeyword<'
