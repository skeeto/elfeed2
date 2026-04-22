#include "elfeed.hpp"
#include "util.hpp"

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
)";

// --- Database operations ---

void db_open(Elfeed *app)
{
    std::string dir = user_data_dir();
    make_directory(dir);
    app->db_path = dir + "/elfeed.db";

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
            app->feed_titles.emplace(url, title);
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
                      std::vector<Entry> &out)
{
    if (!app->db) return;
    out.clear();

    // Build query: date range goes into SQL, regex filters applied in memory
    std::string sql =
        "SELECT e.namespace, e.id, e.feed_url, e.title, e.link, e.date,"
        " e.content, e.content_type"
        " FROM entry e";

    std::vector<std::string> where;
    std::vector<double> bind_doubles;

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

    // Tag filters via subquery
    for (auto &tag : filter.must_have) {
        where.push_back(
            "EXISTS (SELECT 1 FROM entry_tag t WHERE"
            " t.namespace=e.namespace AND t.entry_id=e.id AND t.tag='" + tag + "')");
    }
    for (auto &tag : filter.must_not_have) {
        where.push_back(
            "NOT EXISTS (SELECT 1 FROM entry_tag t WHERE"
            " t.namespace=e.namespace AND t.entry_id=e.id AND t.tag='" + tag + "')");
    }

    if (!where.empty()) {
        sql += " WHERE ";
        for (size_t i = 0; i < where.size(); i++) {
            if (i > 0) sql += " AND ";
            sql += where[i];
        }
    }

    sql += " ORDER BY e.date DESC, e.namespace, e.id";

    if (filter.limit > 0) {
        sql += " LIMIT " + std::to_string(filter.limit);
    }

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK)
        return;

    int bind_idx = 1;
    for (double d : bind_doubles)
        sqlite3_bind_double(stmt, bind_idx++, d);

    // Per-entry lookup statements (tags, authors, enclosures).
    const char *tags_sql =
        "SELECT tag FROM entry_tag WHERE namespace=? AND entry_id=? ORDER BY tag";
    const char *authors_sql =
        "SELECT name,email,uri FROM entry_author"
        " WHERE namespace=? AND entry_id=? ORDER BY seq";
    const char *encs_sql =
        "SELECT url,type,length FROM entry_enclosure"
        " WHERE namespace=? AND entry_id=? ORDER BY seq";
    sqlite3_stmt *tags_stmt    = nullptr;
    sqlite3_stmt *authors_stmt = nullptr;
    sqlite3_stmt *encs_stmt    = nullptr;
    bool have_tags =
        sqlite3_prepare_v2(app->db, tags_sql,    -1, &tags_stmt,    nullptr) == SQLITE_OK;
    bool have_auth =
        sqlite3_prepare_v2(app->db, authors_sql, -1, &authors_stmt, nullptr) == SQLITE_OK;
    bool have_encs =
        sqlite3_prepare_v2(app->db, encs_sql,    -1, &encs_stmt,    nullptr) == SQLITE_OK;

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

        auto bind_ns_id = [&](sqlite3_stmt *s) {
            sqlite3_reset(s);
            sqlite3_bind_text(s, 1, e.namespace_.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(s, 2, e.id.c_str(),         -1, SQLITE_TRANSIENT);
        };

        if (have_tags) {
            bind_ns_id(tags_stmt);
            while (sqlite3_step(tags_stmt) == SQLITE_ROW) {
                if (auto *t = (const char *)sqlite3_column_text(tags_stmt, 0))
                    e.tags.push_back(t);
            }
        }

        if (have_auth) {
            bind_ns_id(authors_stmt);
            while (sqlite3_step(authors_stmt) == SQLITE_ROW) {
                Author a;
                if (auto *t = (const char *)sqlite3_column_text(authors_stmt, 0))
                    a.name = t;
                if (auto *t = (const char *)sqlite3_column_text(authors_stmt, 1))
                    a.email = t;
                if (auto *t = (const char *)sqlite3_column_text(authors_stmt, 2))
                    a.uri = t;
                e.authors.push_back(std::move(a));
            }
        }

        if (have_encs) {
            bind_ns_id(encs_stmt);
            while (sqlite3_step(encs_stmt) == SQLITE_ROW) {
                Enclosure enc;
                if (auto *t = (const char *)sqlite3_column_text(encs_stmt, 0))
                    enc.url = t;
                if (auto *t = (const char *)sqlite3_column_text(encs_stmt, 1))
                    enc.type = t;
                enc.length = sqlite3_column_int64(encs_stmt, 2);
                e.enclosures.push_back(std::move(enc));
            }
        }

        // In-memory regex filtering
        bool keep = true;

        // Title/link match (bare regex terms)
        for (auto &pat : filter.matches) {
            try {
                std::regex re(pat, std::regex_constants::icase);
                if (!std::regex_search(e.title, re) &&
                    !std::regex_search(e.link, re)) {
                    keep = false;
                    break;
                }
            } catch (...) {
                if (e.title.find(pat) == std::string::npos &&
                    e.link.find(pat) == std::string::npos) {
                    keep = false;
                    break;
                }
            }
        }

        // Title/link NOT match
        if (keep) {
            for (auto &pat : filter.not_matches) {
                try {
                    std::regex re(pat, std::regex_constants::icase);
                    if (std::regex_search(e.title, re) ||
                        std::regex_search(e.link, re)) {
                        keep = false;
                        break;
                    }
                } catch (...) {
                    if (e.title.find(pat) != std::string::npos ||
                        e.link.find(pat) != std::string::npos) {
                        keep = false;
                        break;
                    }
                }
            }
        }

        // Look up feed title once, lazily, for = / ~ checks below.
        auto find_feed_title = [&]() -> std::string {
            for (auto &f : app->feeds)
                if (f.url == e.feed_url) return f.title;
            return {};
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

        if (keep)
            out.push_back(std::move(e));
    }

    if (have_tags) sqlite3_finalize(tags_stmt);
    if (have_auth) sqlite3_finalize(authors_stmt);
    if (have_encs) sqlite3_finalize(encs_stmt);
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
