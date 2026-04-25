#include "elfeed.hpp"
#include "util.hpp"

#include <wx/filename.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <regex>

#include <sqlite3.h>

static const char *schema_sql = R"(
CREATE TABLE IF NOT EXISTS feed (
    url           TEXT PRIMARY KEY,
    -- title is the feed's self-declared title (from the feed XML).
    -- title_user is the user's override from config; takes precedence
    -- at display time. Display uses COALESCE(title_user, title, url).
    title         TEXT,
    title_user    TEXT,
    author        TEXT,
    etag          TEXT,
    last_modified TEXT,
    canonical_url TEXT,
    failures      INTEGER DEFAULT 0,
    last_update   REAL
);

CREATE TABLE IF NOT EXISTS entry (
    namespace    TEXT NOT NULL,
    id           TEXT NOT NULL,
    feed_url     TEXT NOT NULL REFERENCES feed(url),
    title        TEXT,
    link         TEXT,
    date         REAL NOT NULL,
    content      TEXT,
    content_type TEXT,
    PRIMARY KEY (namespace, id)
);

CREATE INDEX IF NOT EXISTS idx_entry_date ON entry(date DESC, namespace, id);
CREATE INDEX IF NOT EXISTS idx_entry_feed ON entry(feed_url);

CREATE TABLE IF NOT EXISTS entry_tag (
    namespace TEXT NOT NULL,
    entry_id  TEXT NOT NULL,
    tag       TEXT NOT NULL,
    PRIMARY KEY (namespace, entry_id, tag),
    FOREIGN KEY (namespace, entry_id) REFERENCES entry(namespace, id)
);

CREATE INDEX IF NOT EXISTS idx_tag ON entry_tag(tag);

CREATE TABLE IF NOT EXISTS entry_author (
    namespace TEXT NOT NULL,
    entry_id  TEXT NOT NULL,
    seq       INTEGER NOT NULL,   -- preserves source order
    name      TEXT,
    email     TEXT,
    uri       TEXT,
    PRIMARY KEY (namespace, entry_id, seq),
    FOREIGN KEY (namespace, entry_id) REFERENCES entry(namespace, id)
);

CREATE TABLE IF NOT EXISTS entry_enclosure (
    namespace TEXT NOT NULL,
    entry_id  TEXT NOT NULL,
    seq       INTEGER NOT NULL,
    url       TEXT,
    type      TEXT,
    length    INTEGER,
    PRIMARY KEY (namespace, entry_id, seq),
    FOREIGN KEY (namespace, entry_id) REFERENCES entry(namespace, id)
);

CREATE TABLE IF NOT EXISTS ui_state (
    key   TEXT PRIMARY KEY,
    value TEXT
);

-- Inline-image cache for the preview pane. Images referenced by
-- <img src="http(s)://..."> inside entry content are fetched once
-- and stored here, then served to wxHtmlWindow as data: URIs.
-- last_used is bumped on every cache hit; LRU eviction keeps total
-- blob bytes under image_cache's configured cap.
CREATE TABLE IF NOT EXISTS image_cache (
    url       TEXT PRIMARY KEY,
    mime_type TEXT NOT NULL,
    bytes     BLOB NOT NULL,
    size      INTEGER NOT NULL,   -- duplicate of length(bytes) so
                                  -- SUM() doesn't scan the blobs
    last_used REAL NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_image_cache_lru
    ON image_cache(last_used);

-- Log entries persisted across runs. UI thread drains the in-memory
-- log into this table on every wake; startup loads back the last
-- log_retention_days worth and purges anything older.
CREATE TABLE IF NOT EXISTS log_entry (
    time    REAL    NOT NULL,
    kind    INTEGER NOT NULL,
    message TEXT    NOT NULL
);

CREATE INDEX IF NOT EXISTS idx_log_entry_time
    ON log_entry(time);

-- Full-text search index over every entry's title + link + content.
-- Shadows the main entry table via rowid: triggers below mirror
-- INSERT / UPDATE / DELETE so the two stay in lockstep without the
-- write paths needing to know about FTS. The default unicode61
-- tokenizer case-folds and splits on unicode word boundaries,
-- which matches what a feed reader's bare keyword filter wants
-- (no special tokenizer knobs, same behavior on every platform).
CREATE VIRTUAL TABLE IF NOT EXISTS entry_fts USING fts5(
    title, link, content,
    tokenize='unicode61'
);

CREATE TRIGGER IF NOT EXISTS entry_fts_ai AFTER INSERT ON entry
BEGIN
    INSERT INTO entry_fts(rowid, title, link, content)
    VALUES (new.rowid, new.title, new.link, new.content);
END;

CREATE TRIGGER IF NOT EXISTS entry_fts_ad AFTER DELETE ON entry
BEGIN
    DELETE FROM entry_fts WHERE rowid = old.rowid;
END;

-- db_add_entries uses ON CONFLICT(namespace,id) DO UPDATE on
-- refetches, which fires AFTER UPDATE (not INSERT+DELETE), so
-- the rowid is stable and we just refresh the indexed columns.
CREATE TRIGGER IF NOT EXISTS entry_fts_au AFTER UPDATE ON entry
BEGIN
    UPDATE entry_fts SET
        title   = new.title,
        link    = new.link,
        content = new.content
    WHERE rowid = old.rowid;
END;
)";

// --- Database operations ---

void db_open(Elfeed *app)
{
    // Honor a pre-set db_path (--db CLI option set it in OnInit).
    // Otherwise compute the default under user_data_dir/elfeed.db.
    if (app->db_path.empty()) {
        std::string dir = user_data_dir();
        make_directory(dir);
        app->db_path = dir + "/elfeed.db";
    } else {
        // For an explicit override, ensure the parent dir exists
        // so sqlite3_open can create the file there.
        wxFileName fn(wxString::FromUTF8(app->db_path));
        if (fn.HasName() && !fn.GetPath().empty())
            make_directory(fn.GetPath().utf8_string());
    }

    int rc = sqlite3_open(app->db_path.c_str(), &app->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "elfeed: sqlite3_open(%s): %s\n",
                app->db_path.c_str(), sqlite3_errmsg(app->db));
        return;
    }

    sqlite3_exec(app->db, "PRAGMA journal_mode=WAL", nullptr, nullptr, nullptr);
    sqlite3_exec(app->db, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr);

    char *err = nullptr;
    rc = sqlite3_exec(app->db, schema_sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "elfeed: schema: %s\n", err);
        sqlite3_free(err);
    }

    // One-time backfill: if the FTS table is empty but there are
    // entries, we just added FTS to a pre-existing DB. Bulk-copy
    // the rows in (same rowid so lookups join cleanly). Subsequent
    // writes keep in sync via triggers, so this runs at most once
    // per DB file.
    {
        sqlite3_stmt *cnt = nullptr;
        int64_t fts_rows = 0, entry_rows = 0;
        if (sqlite3_prepare_v2(app->db,
                "SELECT (SELECT COUNT(*) FROM entry_fts),"
                " (SELECT COUNT(*) FROM entry)",
                -1, &cnt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(cnt) == SQLITE_ROW) {
                fts_rows   = sqlite3_column_int64(cnt, 0);
                entry_rows = sqlite3_column_int64(cnt, 1);
            }
            sqlite3_finalize(cnt);
        }
        if (fts_rows == 0 && entry_rows > 0) {
            fprintf(stderr,
                "elfeed: indexing %lld entries for full-text search…\n",
                (long long)entry_rows);
            char *ferr = nullptr;
            int frc = sqlite3_exec(app->db,
                "INSERT INTO entry_fts(rowid, title, link, content)"
                " SELECT rowid, title, link, content FROM entry",
                nullptr, nullptr, &ferr);
            if (frc != SQLITE_OK) {
                fprintf(stderr, "elfeed: fts backfill: %s\n", ferr);
                sqlite3_free(ferr);
            }
        }
    }
}

void db_close(Elfeed *app)
{
    if (app->db) {
        sqlite3_close(app->db);
        app->db = nullptr;
    }
}

void db_update_feed(Elfeed *app, const Feed &feed)
{
    if (!app->db) return;
    const char *sql =
        "INSERT INTO feed (url,title,author,etag,last_modified,"
        "canonical_url,failures,last_update) VALUES (?,?,?,?,?,?,?,?)"
        " ON CONFLICT(url) DO UPDATE SET"
        " title=excluded.title,author=excluded.author,"
        " etag=excluded.etag,last_modified=excluded.last_modified,"
        " canonical_url=excluded.canonical_url,"
        " failures=excluded.failures,last_update=excluded.last_update";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, feed.url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, feed.title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, feed.author.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, feed.etag.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, feed.last_modified.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, feed.canonical_url.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 7, feed.failures);
    sqlite3_bind_double(stmt, 8, feed.last_update);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void db_set_user_title(Elfeed *app, const std::string &url,
                       const std::string &title)
{
    if (!app->db) return;
    // UPSERT: create the row if it doesn't exist, otherwise update only
    // the title_user column. NULL when title is empty so the COALESCE
    // in db_load_feed_titles falls through to the self-declared title.
    const char *sql =
        "INSERT INTO feed (url,title_user) VALUES (?,?)"
        " ON CONFLICT(url) DO UPDATE SET title_user=excluded.title_user";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, url.c_str(), -1, SQLITE_TRANSIENT);
    if (title.empty())
        sqlite3_bind_null(stmt, 2);
    else
        sqlite3_bind_text(stmt, 2, title.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void db_load_feed_titles(Elfeed *app)
{
    app->feed_titles.clear();
    if (!app->db) return;
    // user-set title overrides self-declared; both NULL → no entry.
    const char *sql =
        "SELECT url, COALESCE(NULLIF(title_user,''), NULLIF(title,''))"
        " FROM feed";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *url = (const char *)sqlite3_column_text(stmt, 0);
        const char *title = (const char *)sqlite3_column_text(stmt, 1);
        if (url && title && *title)
            // html_strip here is a one-shot cleanup for DBs that
            // pre-date feed.cpp's parse-time decoding — existing
            // rows may still contain raw `&#8211;`-style entities
            // until their feed is next fetched. Idempotent for
            // already-clean titles.
            app->feed_titles.emplace(url, html_strip(title));
    }
    sqlite3_finalize(stmt);
}

void db_load_feeds(Elfeed *app)
{
    if (!app->db) return;
    const char *sql = "SELECT url,title,author,etag,last_modified,"
                      "canonical_url,failures,last_update FROM feed";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;

    // Build a map of existing feeds by URL for fast lookup
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *url = (const char *)sqlite3_column_text(stmt, 0);
        if (!url) continue;
        // Find existing feed entry from config
        for (auto &f : app->feeds) {
            if (f.url == url) {
                if (auto *t = (const char *)sqlite3_column_text(stmt, 1))
                    f.title = t;
                if (auto *t = (const char *)sqlite3_column_text(stmt, 2))
                    f.author = t;
                if (auto *t = (const char *)sqlite3_column_text(stmt, 3))
                    f.etag = t;
                if (auto *t = (const char *)sqlite3_column_text(stmt, 4))
                    f.last_modified = t;
                if (auto *t = (const char *)sqlite3_column_text(stmt, 5))
                    f.canonical_url = t;
                f.failures = sqlite3_column_int(stmt, 6);
                f.last_update = sqlite3_column_double(stmt, 7);
                break;
            }
        }
    }
    sqlite3_finalize(stmt);
}

void db_add_entries(Elfeed *app, std::vector<Entry> &entries)
{
    if (!app->db || entries.empty()) return;

    sqlite3_exec(app->db, "BEGIN", nullptr, nullptr, nullptr);

    // Main row upsert. title/link/content can change on refetch; the
    // primary key (namespace,id) is stable. Date is deliberately NOT
    // updated: feeds with entries that have no published date make
    // the parser fall back to time(now), so every refetch would bump
    // the stored date forward and the entry would perpetually bubble
    // to the top of date-sorted views. Preserving first-sight date
    // also ignores the rare case where a feed corrects a published
    // date, which is an acceptable trade-off.
    const char *upsert =
        "INSERT INTO entry (namespace,id,feed_url,title,link,date,"
        "content,content_type)"
        " VALUES (?,?,?,?,?,?,?,?)"
        " ON CONFLICT(namespace,id) DO UPDATE SET"
        " title=excluded.title,link=excluded.link,"
        " content=excluded.content,"
        " content_type=excluded.content_type";

    // Existence probe so we can tell INSERT from UPDATE on the upsert
    // above. SQLite reports the same `changes()` count for both paths,
    // so we precheck. This drives the merge rule: tags are applied
    // only on first sight of an entry — see elfeed-db.el's
    // elfeed-entry-merge, which preserves the existing entry's tag
    // set verbatim across a refetch. Without this, every refetch
    // would silently restore "unread" on entries the user has read.
    const char *exists_sql =
        "SELECT 1 FROM entry WHERE namespace=? AND id=?";

    // Tag insert is only used when an entry is new (see above).
    const char *tag_sql =
        "INSERT OR IGNORE INTO entry_tag (namespace,entry_id,tag)"
        " VALUES (?,?,?)";

    // Authors and enclosures come from the feed; refresh on each write
    // by wiping the existing rows for this (namespace,entry_id) and
    // re-inserting. This lets a re-fetched entry swap its author list
    // without leaving stale rows behind.
    const char *author_del =
        "DELETE FROM entry_author WHERE namespace=? AND entry_id=?";
    const char *author_ins =
        "INSERT INTO entry_author (namespace,entry_id,seq,name,email,uri)"
        " VALUES (?,?,?,?,?,?)";
    const char *enc_del =
        "DELETE FROM entry_enclosure WHERE namespace=? AND entry_id=?";
    const char *enc_ins =
        "INSERT INTO entry_enclosure (namespace,entry_id,seq,url,type,length)"
        " VALUES (?,?,?,?,?,?)";

    sqlite3_stmt *stmt = nullptr;
    sqlite3_stmt *exists_stmt = nullptr;
    sqlite3_stmt *tag_stmt = nullptr;
    sqlite3_stmt *auth_del_stmt = nullptr;
    sqlite3_stmt *auth_ins_stmt = nullptr;
    sqlite3_stmt *enc_del_stmt = nullptr;
    sqlite3_stmt *enc_ins_stmt = nullptr;

    auto prep = [&](const char *sql, sqlite3_stmt **out) {
        return sqlite3_prepare_v2(app->db, sql, -1, out, nullptr) == SQLITE_OK;
    };
    if (!prep(upsert, &stmt) ||
        !prep(exists_sql, &exists_stmt) ||
        !prep(tag_sql, &tag_stmt) ||
        !prep(author_del, &auth_del_stmt) || !prep(author_ins, &auth_ins_stmt) ||
        !prep(enc_del, &enc_del_stmt) || !prep(enc_ins, &enc_ins_stmt)) {
        if (stmt)          sqlite3_finalize(stmt);
        if (exists_stmt)   sqlite3_finalize(exists_stmt);
        if (tag_stmt)      sqlite3_finalize(tag_stmt);
        if (auth_del_stmt) sqlite3_finalize(auth_del_stmt);
        if (auth_ins_stmt) sqlite3_finalize(auth_ins_stmt);
        if (enc_del_stmt)  sqlite3_finalize(enc_del_stmt);
        if (enc_ins_stmt)  sqlite3_finalize(enc_ins_stmt);
        sqlite3_exec(app->db, "ROLLBACK", nullptr, nullptr, nullptr);
        return;
    }

    for (auto &e : entries) {
        // First-sight check: tags only flow through on INSERT, not on
        // UPDATE. Once a tag set exists for an entry, it's the user's
        // (and the database's) — fetched tags from a refetch are
        // discarded so the user's read state is preserved.
        sqlite3_reset(exists_stmt);
        sqlite3_bind_text(exists_stmt, 1, e.namespace_.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(exists_stmt, 2, e.id.c_str(), -1, SQLITE_TRANSIENT);
        bool existed = (sqlite3_step(exists_stmt) == SQLITE_ROW);

        sqlite3_reset(stmt);
        sqlite3_bind_text(stmt, 1, e.namespace_.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, e.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, e.feed_url.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, e.title.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, e.link.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 6, e.date);
        if (!e.content.empty())
            sqlite3_bind_text(stmt, 7, e.content.c_str(), -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(stmt, 7);
        if (!e.content_type.empty())
            sqlite3_bind_text(stmt, 8, e.content_type.c_str(), -1, SQLITE_TRANSIENT);
        else
            sqlite3_bind_null(stmt, 8);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            elfeed_log(app, LOG_ERROR, "db insert entry [%s %s]: %s",
                       e.namespace_.c_str(), e.id.c_str(),
                       sqlite3_errmsg(app->db));
        }

        if (!existed) {
            for (auto &tag : e.tags) {
                sqlite3_reset(tag_stmt);
                sqlite3_bind_text(tag_stmt, 1, e.namespace_.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(tag_stmt, 2, e.id.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(tag_stmt, 3, tag.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(tag_stmt);
            }
        }

        // Refresh authors: delete existing, insert current list.
        sqlite3_reset(auth_del_stmt);
        sqlite3_bind_text(auth_del_stmt, 1, e.namespace_.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(auth_del_stmt, 2, e.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(auth_del_stmt);
        for (size_t i = 0; i < e.authors.size(); i++) {
            const Author &a = e.authors[i];
            sqlite3_reset(auth_ins_stmt);
            sqlite3_bind_text(auth_ins_stmt, 1, e.namespace_.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(auth_ins_stmt, 2, e.id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int (auth_ins_stmt, 3, (int)i);
            sqlite3_bind_text(auth_ins_stmt, 4, a.name.c_str(),  -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(auth_ins_stmt, 5, a.email.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(auth_ins_stmt, 6, a.uri.c_str(),   -1, SQLITE_TRANSIENT);
            sqlite3_step(auth_ins_stmt);
        }

        // Refresh enclosures.
        sqlite3_reset(enc_del_stmt);
        sqlite3_bind_text(enc_del_stmt, 1, e.namespace_.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(enc_del_stmt, 2, e.id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(enc_del_stmt);
        for (size_t i = 0; i < e.enclosures.size(); i++) {
            const Enclosure &enc = e.enclosures[i];
            sqlite3_reset(enc_ins_stmt);
            sqlite3_bind_text (enc_ins_stmt, 1, e.namespace_.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text (enc_ins_stmt, 2, e.id.c_str(),         -1, SQLITE_TRANSIENT);
            sqlite3_bind_int  (enc_ins_stmt, 3, (int)i);
            sqlite3_bind_text (enc_ins_stmt, 4, enc.url.c_str(),  -1, SQLITE_TRANSIENT);
            sqlite3_bind_text (enc_ins_stmt, 5, enc.type.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(enc_ins_stmt, 6, (sqlite3_int64)enc.length);
            sqlite3_step(enc_ins_stmt);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_finalize(exists_stmt);
    sqlite3_finalize(auth_del_stmt);
    sqlite3_finalize(auth_ins_stmt);
    sqlite3_finalize(enc_del_stmt);
    sqlite3_finalize(enc_ins_stmt);
    sqlite3_finalize(tag_stmt);
    sqlite3_exec(app->db, "COMMIT", nullptr, nullptr, nullptr);
}

void db_query_entries(Elfeed *app, const Filter &filter,
                      std::vector<Entry> &out,
                      int default_limit)
{
    if (!app->db) return;
    out.clear();

    // Classify bare/! patterns: simple alnum+underscore words go
    // through FTS5 (fast, index-backed, also searches content);
    // patterns with regex metacharacters fall through to the
    // in-memory regex path (same behavior as before FTS landed).
    // A single "c++ #1w"-style filter thus still works: the `c++`
    // goes in-memory, nothing goes to FTS, everything else
    // (date, tags, limit) is unchanged.
    auto is_fts_simple = [](const std::string &p) {
        if (p.empty()) return false;
        for (char c : p) {
            if (!(std::isalnum((unsigned char)c) || c == '_'))
                return false;
        }
        return true;
    };
    std::string fts_match;
    std::vector<std::string> regex_matches;
    std::vector<std::string> regex_not_matches;
    for (auto &p : filter.matches) {
        if (is_fts_simple(p)) {
            if (!fts_match.empty()) fts_match += " ";
            fts_match += "\"";
            fts_match += p;
            fts_match += "\"";
        } else {
            regex_matches.push_back(p);
        }
    }
    if (!fts_match.empty()) {
        // FTS5 rejects a bare-NOT expression (there has to be at
        // least one positive match to exclude from), so only push
        // simple !patterns through FTS when we also have an
        // anchor match. Otherwise let them go through in-memory.
        for (auto &p : filter.not_matches) {
            if (is_fts_simple(p)) {
                fts_match += " NOT \"";
                fts_match += p;
                fts_match += "\"";
            } else {
                regex_not_matches.push_back(p);
            }
        }
    } else {
        for (auto &p : filter.not_matches)
            regex_not_matches.push_back(p);
    }
    bool use_fts = !fts_match.empty();

    // Build query: FTS MATCH + date range + tag filters go into
    // SQL. Regex/substring filters (leftover complex patterns,
    // and feed-URL = / ~) apply in the C++ scan below.
    //
    // Tags ride along as a char(1)-separated bundle via a correlated
    // subquery. Previously we collected entries first and then ran a
    // separate batched `... OR (ns=? AND id=?) OR ...` query — two
    // bound parameters per entry that silently blew past
    // SQLITE_LIMIT_VARIABLE_NUMBER (999 on older SQLite, 32766 on
    // current) on a power-user DB, at which point prepare failed
    // and every entry's Tags column went blank with no log trail.
    // The correlated subquery hits the entry_tag PK index once per
    // outer row and has no parameter-count concern at all. char(1)
    // (SOH) as separator is unambiguous: tags come from user
    // config / RSS <category> / Atom term="" and never contain
    // control bytes. group_concat over zero rows returns NULL,
    // which our sqlite3_column_text reader handles as "no tags".
    std::string sql =
        "SELECT e.namespace, e.id, e.feed_url, e.title, e.link, e.date,"
        " e.content, e.content_type,"
        " (SELECT group_concat(tag, char(1)) FROM entry_tag"
        "  WHERE namespace=e.namespace AND entry_id=e.id) AS tags_joined"
        " FROM entry e";
    if (use_fts) {
        // The rowid JOIN uses the implicit PK on entry_fts.rowid;
        // MATCH narrows to matching rows before the entry scan
        // ever touches them. Note: we deliberately don't alias
        // entry_fts here — FTS5's MATCH operator resolves its
        // left-hand side as either a table name or a column name,
        // and an alias loses against "column on entry_fts" in
        // that resolution. Keeping the full table name lets
        // `entry_fts MATCH ?` parse as the table-wide match.
        sql += " JOIN entry_fts ON entry_fts.rowid = e.rowid";
    }

    std::vector<std::string> where;
    std::vector<double> bind_doubles;
    std::vector<std::string> bind_strings;

    // FTS MATCH goes first in the WHERE so the planner uses the
    // full-text index as the driving table. Bare table name is
    // required on the LHS — see the comment on the JOIN above
    // for why an alias doesn't work here.
    if (use_fts) {
        where.push_back("entry_fts MATCH ?");
    }
    if (filter.after > 0) {
        double cutoff = (double)time(nullptr) - filter.after;
        where.push_back("e.date >= ?");
        bind_doubles.push_back(cutoff);
    }
    if (filter.before > 0) {
        double cutoff = (double)time(nullptr) - filter.before;
        where.push_back("e.date <= ?");
        bind_doubles.push_back(cutoff);
    }

    // Tag filters via subquery. Bind the tag value as a parameter
    // rather than interpolating into the SQL — even though the
    // tag string ultimately comes from the local user (filter bar
    // or `preset` config, never from feed content, which goes
    // through the parameterized db_tag write path), an apostrophe
    // in a tag would otherwise break the prepared statement
    // (e.g. an autotag like `o'connor`) and a `' OR '1'='1`-shape
    // input would change the WHERE's logical structure.
    for (auto &tag : filter.must_have) {
        where.push_back(
            "EXISTS (SELECT 1 FROM entry_tag t WHERE"
            " t.namespace=e.namespace AND t.entry_id=e.id AND"
            " t.tag=?)");
        bind_strings.push_back(tag);
    }
    for (auto &tag : filter.must_not_have) {
        where.push_back(
            "NOT EXISTS (SELECT 1 FROM entry_tag t WHERE"
            " t.namespace=e.namespace AND t.entry_id=e.id AND"
            " t.tag=?)");
        bind_strings.push_back(tag);
    }

    if (!where.empty()) {
        sql += " WHERE ";
        for (size_t i = 0; i < where.size(); i++) {
            if (i > 0) sql += " AND ";
            sql += where[i];
        }
    }

    sql += " ORDER BY e.date DESC, e.namespace, e.id";

    // Explicit `#N` in the filter wins; otherwise fall back to
    // the caller's viewport-derived default (0 = unlimited).
    // The limit is enforced in the C++ scan loop below, not as
    // a SQL LIMIT clause: in-memory regex/feed filters drop rows
    // after the SQL step, so a SQL LIMIT would cap "rows
    // considered" rather than "rows kept" — a strict filter
    // like `+unread linux` would then return far fewer than N
    // entries even though many more matched further down the
    // index. The index is already ORDER BY date DESC so SQLite
    // streams rows in display order; stopping sqlite3_step when
    // we've collected enough is equivalent to LIMIT for cost.
    int effective_limit =
        filter.limit > 0 ? filter.limit : default_limit;

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql.c_str(), -1, &stmt, nullptr)
        != SQLITE_OK) {
        // Log and bail instead of silently returning an empty list.
        // An FTS syntax oops used to look exactly like "the filter
        // matched zero entries," which was invisible to debug.
        elfeed_log(app, LOG_ERROR, "db_query_entries prepare: %s",
                   sqlite3_errmsg(app->db));
        return;
    }

    // Bind order matches WHERE-build order exactly: FTS MATCH
    // (if any), date bounds, then must_have / must_not_have tag
    // values in the order they were pushed.
    int bind_idx = 1;
    if (use_fts) {
        sqlite3_bind_text(stmt, bind_idx++, fts_match.c_str(),
                          -1, SQLITE_TRANSIENT);
    }
    for (double d : bind_doubles)
        sqlite3_bind_double(stmt, bind_idx++, d);
    for (auto &s : bind_strings)
        sqlite3_bind_text(stmt, bind_idx++, s.c_str(),
                          -1, SQLITE_TRANSIENT);

    // Pre-compile bare and `!` patterns once. The previous code did
    // `std::regex re(pat, ...)` *inside* the per-entry scan loop,
    // which compiled the same regex once per candidate row —
    // easily a one-second stall on a 10k-row filter with a single
    // bare word. Each pattern tries regex first, falls back to
    // literal substring if the regex doesn't compile (user typed
    // something like `c++` or an unbalanced bracket).
    struct Matcher {
        bool have_regex = false;
        std::regex re;
        std::string literal;
    };
    auto build_matchers =
        [](const std::vector<std::string> &pats) {
        std::vector<Matcher> out;
        out.reserve(pats.size());
        for (auto &p : pats) {
            Matcher m;
            m.literal = p;
            try {
                m.re = std::regex(p, std::regex_constants::icase);
                m.have_regex = true;
            } catch (...) {
                m.have_regex = false;
            }
            out.push_back(std::move(m));
        }
        return out;
    };
    // Only complex (non-FTS) patterns reach the in-memory matcher
    // — simple words were already matched in SQL via FTS.
    std::vector<Matcher> match_matchers     = build_matchers(regex_matches);
    std::vector<Matcher> not_match_matchers = build_matchers(regex_not_matches);
    auto matches_in = [](const Matcher &m,
                         const std::string &title,
                         const std::string &link) {
        if (m.have_regex)
            return std::regex_search(title, m.re) ||
                   std::regex_search(link,  m.re);
        return title.find(m.literal) != std::string::npos ||
               link.find(m.literal)  != std::string::npos;
    };

    // Tags arrive as column 8, a char(1)-separated bundle produced
    // by the SELECT's correlated group_concat subquery. Authors /
    // enclosures still load on-demand from the preview/download
    // paths via db_entry_load_details.

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Entry e;
        if (auto *t = (const char *)sqlite3_column_text(stmt, 0))
            e.namespace_ = t;
        if (auto *t = (const char *)sqlite3_column_text(stmt, 1))
            e.id = t;
        if (auto *t = (const char *)sqlite3_column_text(stmt, 2))
            e.feed_url = t;
        if (auto *t = (const char *)sqlite3_column_text(stmt, 3))
            e.title = t;
        if (auto *t = (const char *)sqlite3_column_text(stmt, 4))
            e.link = t;
        e.date = sqlite3_column_double(stmt, 5);
        if (auto *t = (const char *)sqlite3_column_text(stmt, 6))
            e.content = t;
        if (auto *t = (const char *)sqlite3_column_text(stmt, 7))
            e.content_type = t;
        if (auto *t = (const char *)sqlite3_column_text(stmt, 8)) {
            // Split on char(1). Sort afterward to match the old
            // behavior's "ORDER BY tag" stability — the attr /
            // color-by-tag path picks "first matching tag" as a
            // priority, so callers rely on a deterministic order.
            size_t len = (size_t)sqlite3_column_bytes(stmt, 8);
            size_t start = 0;
            for (size_t j = 0; j < len; j++) {
                if (t[j] == '\x01') {
                    e.tags.emplace_back(t + start, j - start);
                    start = j + 1;
                }
            }
            if (start < len)
                e.tags.emplace_back(t + start, len - start);
            std::sort(e.tags.begin(), e.tags.end());
        }

        // In-memory regex filtering. Matchers are pre-compiled
        // above the scan loop.
        bool keep = true;
        for (auto &m : match_matchers) {
            if (!matches_in(m, e.title, e.link)) {
                keep = false;
                break;
            }
        }
        if (keep) {
            for (auto &m : not_match_matchers) {
                if (matches_in(m, e.title, e.link)) {
                    keep = false;
                    break;
                }
            }
        }

        // Look up feed title once, lazily, for = / ~ checks below.
        // Hash map rather than a linear scan of app->feeds — when
        // feed filters are active the previous scan ran O(feeds)
        // for every kept entry (up to O(entries × feeds) total,
        // hundreds of thousands of comparisons for a power-user
        // DB). feed_titles is already html_strip'd and reflects
        // user_title overrides, so it's also the right string to
        // match against from a "what the user sees" standpoint.
        auto find_feed_title = [&]() -> std::string {
            auto it = app->feed_titles.find(e.feed_url);
            return it != app->feed_titles.end() ? it->second : std::string{};
        };

        // Case-insensitive substring search. We don't treat = / ~
        // patterns as regex: real-world values are URLs full of regex
        // metacharacters (? . + etc.), so regex semantics surprise
        // users. Title-text filters (bare / !) below are still regex.
        auto icontains = [](const std::string &hay,
                            const std::string &needle) {
            if (needle.empty()) return true;
            if (needle.size() > hay.size()) return false;
            auto lc = [](unsigned char c) -> unsigned char {
                return (c >= 'A' && c <= 'Z') ? (unsigned char)(c | 0x20) : c;
            };
            for (size_t i = 0; i + needle.size() <= hay.size(); i++) {
                size_t j = 0;
                for (; j < needle.size(); j++)
                    if (lc((unsigned char)hay[i + j]) !=
                        lc((unsigned char)needle[j])) break;
                if (j == needle.size()) return true;
            }
            return false;
        };

        // Feed URL/title match (= prefix)
        if (keep && !filter.feeds.empty()) {
            bool any_match = false;
            std::string feed_title = find_feed_title();
            for (auto &pat : filter.feeds) {
                if (icontains(e.feed_url, pat) ||
                    icontains(feed_title, pat)) {
                    any_match = true;
                    break;
                }
            }
            if (!any_match) keep = false;
        }

        // Feed URL/title NOT match (~ prefix)
        if (keep && !filter.not_feeds.empty()) {
            std::string feed_title = find_feed_title();
            for (auto &pat : filter.not_feeds) {
                if (icontains(e.feed_url, pat) ||
                    icontains(feed_title, pat)) {
                    keep = false;
                    break;
                }
            }
        }

        if (keep) {
            out.push_back(std::move(e));
            // Post-filter cap: stop once we've collected enough
            // passing entries. See the comment next to
            // effective_limit above for why this is the C++
            // loop's job rather than a SQL LIMIT.
            if (effective_limit > 0 &&
                (int)out.size() >= effective_limit)
                break;
        }
    }

    sqlite3_finalize(stmt);
}

void db_entry_load_details(Elfeed *app, Entry &e)
{
    // Populate authors + enclosures for a single entry. Used by
    // the preview pane and the download action. Cheap because it
    // runs for 1 entry (or at most a small multi-selection),
    // unlike the old path which ran for every row of every
    // live-typing requery. Safe to call repeatedly — we re-clear
    // each time so a second call after tag changes still reflects
    // the DB state.
    e.authors.clear();
    e.enclosures.clear();
    if (!app->db) return;

    sqlite3_stmt *astmt = nullptr, *estmt = nullptr;
    if (sqlite3_prepare_v2(app->db,
            "SELECT name,email,uri FROM entry_author"
            " WHERE namespace=? AND entry_id=? ORDER BY seq",
            -1, &astmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(astmt, 1, e.namespace_.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(astmt, 2, e.id.c_str(),         -1, SQLITE_TRANSIENT);
        while (sqlite3_step(astmt) == SQLITE_ROW) {
            Author a;
            if (auto *t = (const char *)sqlite3_column_text(astmt, 0)) a.name  = t;
            if (auto *t = (const char *)sqlite3_column_text(astmt, 1)) a.email = t;
            if (auto *t = (const char *)sqlite3_column_text(astmt, 2)) a.uri   = t;
            e.authors.push_back(std::move(a));
        }
        sqlite3_finalize(astmt);
    }

    if (sqlite3_prepare_v2(app->db,
            "SELECT url,type,length FROM entry_enclosure"
            " WHERE namespace=? AND entry_id=? ORDER BY seq",
            -1, &estmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(estmt, 1, e.namespace_.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(estmt, 2, e.id.c_str(),         -1, SQLITE_TRANSIENT);
        while (sqlite3_step(estmt) == SQLITE_ROW) {
            Enclosure enc;
            if (auto *t = (const char *)sqlite3_column_text(estmt, 0)) enc.url  = t;
            if (auto *t = (const char *)sqlite3_column_text(estmt, 1)) enc.type = t;
            enc.length = sqlite3_column_int64(estmt, 2);
            e.enclosures.push_back(std::move(enc));
        }
        sqlite3_finalize(estmt);
    }
}

void db_entry_dates_since(Elfeed *app, double since,
                          std::vector<double> &out)
{
    out.clear();
    if (!app->db) return;
    sqlite3_stmt *stmt;
    // Just the date column — no joins to tag/author/enclosure, and
    // no deserialization of entry content. Fast even at 100k rows.
    if (sqlite3_prepare_v2(app->db,
            "SELECT date FROM entry WHERE date >= ?",
            -1, &stmt, nullptr) != SQLITE_OK) return;
    sqlite3_bind_double(stmt, 1, since);
    while (sqlite3_step(stmt) == SQLITE_ROW)
        out.push_back(sqlite3_column_double(stmt, 0));
    sqlite3_finalize(stmt);
}

int64_t db_reclaim_space(Elfeed *app)
{
    if (!app->db) return 0;
    auto db_size = [&]() -> int64_t {
        wxFileName fn(wxString::FromUTF8(app->db_path));
        return fn.FileExists() ? (int64_t)fn.GetSize().GetValue() : 0;
    };
    int64_t before = db_size();

    // The image cache is explicitly a cache: preview-pane fetches
    // re-populate on demand, so dropping it is user-safe. Clearing
    // it before VACUUM lets the rewrite skip all those blob rows
    // and gives the freed pages back to the OS rather than parking
    // them on SQLite's freelist.
    sqlite3_exec(app->db, "DELETE FROM image_cache",
                 nullptr, nullptr, nullptr);
    // VACUUM rebuilds the DB file in place — repacks every page,
    // recomputes indexes, and truncates the trailing freelist.
    // Holds a write lock for the duration. Caller must not have
    // any in-flight prepared statements on the connection.
    sqlite3_exec(app->db, "VACUUM", nullptr, nullptr, nullptr);

    int64_t after = db_size();
    return before > after ? before - after : 0;
}

void db_feed_newest_entry_dates(
    Elfeed *app, std::unordered_map<std::string, double> &out)
{
    out.clear();
    if (!app->db) return;
    sqlite3_stmt *stmt;
    // GROUP BY feed_url with MAX(date) — idx_entry_feed on feed_url
    // lets SQLite walk one row per distinct feed to find each max,
    // so this is roughly O(feed_count), not O(entry_count).
    if (sqlite3_prepare_v2(app->db,
            "SELECT feed_url, MAX(date) FROM entry GROUP BY feed_url",
            -1, &stmt, nullptr) != SQLITE_OK) return;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto *u = (const char *)sqlite3_column_text(stmt, 0);
        if (u) out[u] = sqlite3_column_double(stmt, 1);
    }
    sqlite3_finalize(stmt);
}

void db_tag(Elfeed *app, const std::string &ns, const std::string &id,
            const std::string &tag)
{
    if (!app->db) return;
    const char *sql =
        "INSERT OR IGNORE INTO entry_tag (namespace,entry_id,tag)"
        " VALUES (?,?,?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, ns.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, tag.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void db_untag(Elfeed *app, const std::string &ns, const std::string &id,
              const std::string &tag)
{
    if (!app->db) return;
    const char *sql =
        "DELETE FROM entry_tag WHERE namespace=? AND entry_id=? AND tag=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, ns.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, tag.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void db_save_ui_state(Elfeed *app, const char *key, const char *value)
{
    if (!app->db) return;
    const char *sql =
        "INSERT INTO ui_state (key,value) VALUES (?,?)"
        " ON CONFLICT(key) DO UPDATE SET value=excluded.value";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

std::string db_load_ui_state(Elfeed *app, const char *key)
{
    if (!app->db) return {};
    const char *sql = "SELECT value FROM ui_state WHERE key=?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return {};
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    std::string result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        if (auto *t = (const char *)sqlite3_column_text(stmt, 0))
            result = t;
    }
    sqlite3_finalize(stmt);
    return result;
}

// ---- Log persistence ---------------------------------------------

void db_log_load(Elfeed *app, double since_epoch,
                 std::vector<LogEntry> &out)
{
    if (!app->db) return;
    // ORDER BY time, rowid: workers stamp entries to second
    // resolution so ties happen; rowid keeps insertion order
    // stable within a tied second.
    const char *sql =
        "SELECT time, kind, message FROM log_entry"
        " WHERE time >= ? ORDER BY time, rowid";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_double(stmt, 1, since_epoch);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        LogEntry e;
        e.time = sqlite3_column_double(stmt, 0);
        e.kind = (LogKind)sqlite3_column_int(stmt, 1);
        if (auto *t = (const char *)sqlite3_column_text(stmt, 2))
            e.message = t;
        out.push_back(std::move(e));
    }
    sqlite3_finalize(stmt);
}

void db_log_save(Elfeed *app, const std::vector<LogEntry> &entries)
{
    if (!app->db || entries.empty()) return;
    sqlite3_exec(app->db, "BEGIN", nullptr, nullptr, nullptr);
    const char *sql =
        "INSERT INTO log_entry (time, kind, message) VALUES (?,?,?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        for (const auto &e : entries) {
            sqlite3_bind_double(stmt, 1, e.time);
            sqlite3_bind_int(stmt, 2, (int)e.kind);
            sqlite3_bind_text(stmt, 3, e.message.c_str(), -1,
                              SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_exec(app->db, "COMMIT", nullptr, nullptr, nullptr);
}

void db_log_purge(Elfeed *app, double older_than_epoch)
{
    if (!app->db) return;
    const char *sql = "DELETE FROM log_entry WHERE time < ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql, -1, &stmt, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_double(stmt, 1, older_than_epoch);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

void db_log_clear(Elfeed *app)
{
    if (!app->db) return;
    sqlite3_exec(app->db, "DELETE FROM log_entry",
                 nullptr, nullptr, nullptr);
}
