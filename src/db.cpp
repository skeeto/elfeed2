#include "elfeed.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstdio>
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
    author       TEXT,
    enclosures   TEXT,
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

    const char *upsert =
        "INSERT INTO entry (namespace,id,feed_url,title,link,date,"
        "content,content_type,author,enclosures)"
        " VALUES (?,?,?,?,?,?,?,?,?,?)"
        " ON CONFLICT(namespace,id) DO UPDATE SET"
        " title=excluded.title,link=excluded.link,"
        " date=excluded.date,content=excluded.content,"
        " content_type=excluded.content_type,"
        " author=excluded.author,enclosures=excluded.enclosures";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(app->db, upsert, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_exec(app->db, "ROLLBACK", nullptr, nullptr, nullptr);
        return;
    }

    const char *tag_sql =
        "INSERT OR IGNORE INTO entry_tag (namespace,entry_id,tag)"
        " VALUES (?,?,?)";
    sqlite3_stmt *tag_stmt;
    if (sqlite3_prepare_v2(app->db, tag_sql, -1, &tag_stmt, nullptr) != SQLITE_OK) {
        sqlite3_finalize(stmt);
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
        // TODO: serialize authors/enclosures as JSON
        sqlite3_bind_null(stmt, 9);
        sqlite3_bind_null(stmt, 10);
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
    }

    sqlite3_finalize(stmt);
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
        " e.content, e.content_type, e.author, e.enclosures"
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

    // Load tags query
    const char *tags_sql =
        "SELECT tag FROM entry_tag WHERE namespace=? AND entry_id=? ORDER BY tag";
    sqlite3_stmt *tags_stmt;
    bool have_tags_stmt =
        sqlite3_prepare_v2(app->db, tags_sql, -1, &tags_stmt, nullptr) == SQLITE_OK;

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

        // Load tags
        if (have_tags_stmt) {
            sqlite3_reset(tags_stmt);
            sqlite3_bind_text(tags_stmt, 1, e.namespace_.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(tags_stmt, 2, e.id.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(tags_stmt) == SQLITE_ROW) {
                if (auto *t = (const char *)sqlite3_column_text(tags_stmt, 0))
                    e.tags.push_back(t);
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

    if (have_tags_stmt) sqlite3_finalize(tags_stmt);
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
