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
    title         TEXT,
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

    // Main row upsert. content/content_type can change on refetch; the
    // primary key (namespace,id) is stable.
    const char *upsert =
        "INSERT INTO entry (namespace,id,feed_url,title,link,date,"
        "content,content_type)"
        " VALUES (?,?,?,?,?,?,?,?)"
        " ON CONFLICT(namespace,id) DO UPDATE SET"
        " title=excluded.title,link=excluded.link,"
        " date=excluded.date,content=excluded.content,"
        " content_type=excluded.content_type";

    // For tags we INSERT OR IGNORE so user-applied tags (e.g. read/unread
    // toggled on a previously-fetched entry) survive a refetch.
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
    sqlite3_stmt *tag_stmt = nullptr;
    sqlite3_stmt *auth_del_stmt = nullptr;
    sqlite3_stmt *auth_ins_stmt = nullptr;
    sqlite3_stmt *enc_del_stmt = nullptr;
    sqlite3_stmt *enc_ins_stmt = nullptr;

    auto prep = [&](const char *sql, sqlite3_stmt **out) {
        return sqlite3_prepare_v2(app->db, sql, -1, out, nullptr) == SQLITE_OK;
    };
    if (!prep(upsert, &stmt) || !prep(tag_sql, &tag_stmt) ||
        !prep(author_del, &auth_del_stmt) || !prep(author_ins, &auth_ins_stmt) ||
        !prep(enc_del, &enc_del_stmt) || !prep(enc_ins, &enc_ins_stmt)) {
        if (stmt)          sqlite3_finalize(stmt);
        if (tag_stmt)      sqlite3_finalize(tag_stmt);
        if (auth_del_stmt) sqlite3_finalize(auth_del_stmt);
        if (auth_ins_stmt) sqlite3_finalize(auth_ins_stmt);
        if (enc_del_stmt)  sqlite3_finalize(enc_del_stmt);
        if (enc_ins_stmt)  sqlite3_finalize(enc_ins_stmt);
        sqlite3_exec(app->db, "ROLLBACK", nullptr, nullptr, nullptr);
        return;
    }

    for (auto &e : entries) {
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

        for (auto &tag : e.tags) {
            sqlite3_reset(tag_stmt);
            sqlite3_bind_text(tag_stmt, 1, e.namespace_.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(tag_stmt, 2, e.id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(tag_stmt, 3, tag.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(tag_stmt);
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

        // Feed URL/title match (= prefix)
        if (keep && !filter.feeds.empty()) {
            bool any_match = false;
            // Look up feed title
            std::string feed_title;
            for (auto &f : app->feeds) {
                if (f.url == e.feed_url) {
                    feed_title = f.title;
                    break;
                }
            }
            for (auto &pat : filter.feeds) {
                try {
                    std::regex re(pat, std::regex_constants::icase);
                    if (std::regex_search(e.feed_url, re) ||
                        std::regex_search(feed_title, re)) {
                        any_match = true;
                        break;
                    }
                } catch (...) {
                    if (e.feed_url.find(pat) != std::string::npos ||
                        feed_title.find(pat) != std::string::npos) {
                        any_match = true;
                        break;
                    }
                }
            }
            if (!any_match) keep = false;
        }

        // Feed URL/title NOT match (~ prefix)
        if (keep) {
            for (auto &pat : filter.not_feeds) {
                std::string feed_title;
                for (auto &f : app->feeds) {
                    if (f.url == e.feed_url) {
                        feed_title = f.title;
                        break;
                    }
                }
                try {
                    std::regex re(pat, std::regex_constants::icase);
                    if (std::regex_search(e.feed_url, re) ||
                        std::regex_search(feed_title, re)) {
                        keep = false;
                        break;
                    }
                } catch (...) {
                    if (e.feed_url.find(pat) != std::string::npos ||
                        feed_title.find(pat) != std::string::npos) {
                        keep = false;
                        break;
                    }
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
