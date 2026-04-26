" Detect Elfeed2 config files.
"
" Canonical path is $XDG_CONFIG_HOME/elfeed2/config (no extension),
" with $HOME/.config/elfeed2/config as the typical resolution. The
" trailing /elfeed2/config suffix is a strong signal even without a
" full XDG match. *.elfeed2 catches alternate / scratch copies kept
" under non-standard names.
au BufNewFile,BufRead */elfeed2/config setfiletype elfeed2
au BufNewFile,BufRead *.elfeed2        setfiletype elfeed2
