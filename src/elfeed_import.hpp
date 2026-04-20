#ifndef ELFEED_IMPORT_HPP
#define ELFEED_IMPORT_HPP

#include <string>

struct Elfeed;

struct ImportStats {
    int feeds_imported = 0;
    int entries_imported = 0;
    int entries_skipped = 0;    // entries whose feed wasn't found
    std::string error;          // empty on success
};

// Parse a Classic Elfeed index file (an Emacs Lisp s-expression) and
// merge its feeds and entries into the SQLite database. Content blobs
// (stored externally by Classic Elfeed as elfeed-ref hashes) are
// skipped — imported entries arrive with empty content.
// Runs on the UI thread; call from a menu handler. The app's in-memory
// feed list is updated so the new feeds appear in the Feeds panel and
// are resolvable by title.
ImportStats import_classic_elfeed(Elfeed *app, const std::string &path);

#endif
