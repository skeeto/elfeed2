// Classic Elfeed index importer.
//
// Classic Elfeed serializes its database as a single Emacs Lisp
// s-expression in `~/.elfeed/index`. Top-level shape:
//
//   (:version 4
//    :feeds   #s(hash-table test equal data ("url" #s(elfeed-feed ...)
//                                            "url" #s(elfeed-feed ...) ...))
//    :entries #s(hash-table test equal data (("ns" . "id") #s(elfeed-entry ...)
//                                            ("ns" . "id") #s(elfeed-entry ...) ...))
//    :index   [cl-struct-avl-tree- ...])
//
//   #s(elfeed-feed  ID URL TITLE AUTHOR META)
//   #s(elfeed-entry ID TITLE LINK DATE CONTENT CONTENT-TYPE ENCLOSURES TAGS FEED-ID META)
//
// We only parse the subset of Emacs Lisp needed to handle this file:
// symbols (`nil`, `foo`, `:keyword`), strings ("..."), integers/floats,
// lists `(a b c)`, cons cells `(a . b)`, hash tables / cl-structs
// `#s(...)`, vectors `[...]`, reader reference/label forms `#NUM=...`
// and `#NUM#` (ignored — our file doesn't actually use them, but they
// can appear in rare cases). The `:index` AVL tree is skipped.
//
// To keep memory reasonable on large indexes (tens of MB), we stream:
// the top-level plist is parsed, but within :feeds and :entries we
// walk the `data (...)` list key-by-key, extracting one Feed/Entry at
// a time and dropping the parsed Value after each.

#include "elfeed_import.hpp"

#include "elfeed.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <wx/ffile.h>

namespace {

// ---- Parsed value tree -----------------------------------------------

struct Value;
using ValuePtr = std::unique_ptr<Value>;

struct Value {
    enum class Type {
        Nil, Int, Float, String, Symbol,
        List, Cons, Struct, Vector,
    };
    Type type = Type::Nil;

    int64_t i = 0;
    double f = 0;
    std::string s;                     // string / symbol / struct name
    std::vector<ValuePtr> children;    // list / vector / struct fields
    ValuePtr car, cdr;                 // for Cons

    bool is_nil() const {
        return type == Type::Nil ||
               (type == Type::Symbol && s == "nil");
    }
    bool is_symbol(const char *name) const {
        return type == Type::Symbol && s == name;
    }
    bool is_keyword(const char *name) const {
        return type == Type::Symbol && s.size() > 1 &&
               s[0] == ':' && s.compare(1, std::string::npos, name) == 0;
    }
    const Value *get(const char *name) const {
        // Walks this list as a plist, returning the value after :name.
        if (type != Type::List) return nullptr;
        for (size_t i = 0; i + 1 < children.size(); i += 2) {
            if (children[i] && children[i]->is_keyword(name))
                return children[i + 1].get();
        }
        return nullptr;
    }
};

// ---- Parser ----------------------------------------------------------

class Parser {
public:
    Parser(const char *src, size_t len) : src_(src), end_(src + len) {}

    // Whitespace and comments.
    void skip_ws()
    {
        while (src_ < end_) {
            char c = *src_;
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++src_;
            } else if (c == ';') {
                // comment to end of line
                while (src_ < end_ && *src_ != '\n') ++src_;
            } else {
                break;
            }
        }
    }

    bool eof() { skip_ws(); return src_ >= end_; }

    // Parse a single value.
    ValuePtr read()
    {
        skip_ws();
        if (src_ >= end_) {
            error_ = "unexpected EOF";
            return nullptr;
        }
        char c = *src_;
        if (c == '(') return read_list();
        if (c == '[') return read_vector();
        if (c == '"') return read_string();
        if (c == '#') return read_hash_form();
        if (c == '?') return read_char();
        if (c == '\'') { ++src_; return read(); }  // quote: ignore
        if (c == '`' || c == ',') { ++src_; return read(); }  // backquote: ignore
        return read_atom();
    }

    // Skip a single value (faster than read() when we don't care).
    void skip() {
        // For simplicity, just call read() and drop the result.
        (void)read();
    }

    const std::string &error() const { return error_; }

private:
    ValuePtr read_list()
    {
        ++src_;  // '('
        auto v = std::make_unique<Value>();
        v->type = Value::Type::List;

        // Detect cons `(a . b)` vs list `(a b c)`. We read the first
        // element, then peek for `.` before the second.
        skip_ws();
        if (src_ < end_ && *src_ == ')') { ++src_; return v; }

        while (src_ < end_) {
            skip_ws();
            if (src_ < end_ && *src_ == ')') { ++src_; return v; }
            // Cons dot?
            if (*src_ == '.' && src_ + 1 < end_ && is_delim(src_[1])) {
                ++src_;  // '.'
                auto tail = read();
                skip_ws();
                if (src_ >= end_ || *src_ != ')') {
                    error_ = "expected ')' after cons tail";
                    return nullptr;
                }
                ++src_;  // ')'
                // Convert list with single car + tail into a Cons.
                if (v->children.size() != 1) {
                    error_ = "dotted pair must have exactly one car";
                    return nullptr;
                }
                auto cons = std::make_unique<Value>();
                cons->type = Value::Type::Cons;
                cons->car = std::move(v->children[0]);
                cons->cdr = std::move(tail);
                return cons;
            }
            auto elem = read();
            if (!elem) return nullptr;
            v->children.push_back(std::move(elem));
        }
        error_ = "unterminated list";
        return nullptr;
    }

    ValuePtr read_vector()
    {
        ++src_;  // '['
        auto v = std::make_unique<Value>();
        v->type = Value::Type::Vector;
        while (src_ < end_) {
            skip_ws();
            if (src_ < end_ && *src_ == ']') { ++src_; return v; }
            auto elem = read();
            if (!elem) return nullptr;
            v->children.push_back(std::move(elem));
        }
        error_ = "unterminated vector";
        return nullptr;
    }

    ValuePtr read_string()
    {
        ++src_;  // opening "
        auto v = std::make_unique<Value>();
        v->type = Value::Type::String;
        std::string &out = v->s;
        while (src_ < end_) {
            char c = *src_++;
            if (c == '"') return v;
            if (c == '\\' && src_ < end_) {
                char e = *src_++;
                switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '\\': out += '\\'; break;
                case '"': out += '"'; break;
                case '\'': out += '\''; break;
                case '\n': /* line continuation */ break;
                case ' ': /* line continuation variant */ break;
                default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        error_ = "unterminated string";
        return nullptr;
    }

    ValuePtr read_atom()
    {
        const char *start = src_;
        while (src_ < end_ && !is_delim(*src_)) ++src_;
        if (start == src_) {
            error_ = std::string("unexpected char: ") + *src_;
            return nullptr;
        }
        std::string tok(start, src_ - start);
        auto v = std::make_unique<Value>();

        // Number or symbol?
        if (!tok.empty() && (tok[0] == '-' || tok[0] == '+' ||
                              (tok[0] >= '0' && tok[0] <= '9'))) {
            bool is_float = tok.find('.') != std::string::npos ||
                            tok.find('e') != std::string::npos ||
                            tok.find('E') != std::string::npos;
            char *end = nullptr;
            if (is_float) {
                double d = std::strtod(tok.c_str(), &end);
                if (end && *end == 0) {
                    v->type = Value::Type::Float;
                    v->f = d;
                    return v;
                }
            } else {
                long long n = std::strtoll(tok.c_str(), &end, 10);
                if (end && *end == 0) {
                    v->type = Value::Type::Int;
                    v->i = n;
                    return v;
                }
            }
            // fall through to symbol
        }

        v->type = Value::Type::Symbol;
        v->s = std::move(tok);
        return v;
    }

    ValuePtr read_char()
    {
        // Character literal like ?a or ?\n. Translate to Int value.
        ++src_;  // '?'
        if (src_ >= end_) { error_ = "bad char"; return nullptr; }
        auto v = std::make_unique<Value>();
        v->type = Value::Type::Int;
        char c = *src_++;
        if (c == '\\' && src_ < end_) {
            c = *src_++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            default: break;
            }
        }
        v->i = (unsigned char)c;
        return v;
    }

    ValuePtr read_hash_form()
    {
        ++src_;  // '#'
        if (src_ >= end_) { error_ = "unexpected #EOF"; return nullptr; }
        char c = *src_;
        if (c == '(') {
            // Propertized string: #("text" START END (props) ...).
            // Keep just the text — we don't care about text properties.
            auto inner = read_list();
            if (!inner) return nullptr;
            auto out = std::make_unique<Value>();
            if (!inner->children.empty() &&
                inner->children[0]->type == Value::Type::String) {
                out->type = Value::Type::String;
                out->s = std::move(inner->children[0]->s);
            } else {
                out->type = Value::Type::Nil;
            }
            return out;
        }
        if (c == 's') {
            // #s(Name field1 field2 ...) — tagged struct record.
            ++src_;
            skip_ws();
            if (src_ >= end_ || *src_ != '(') {
                error_ = "expected '(' after #s";
                return nullptr;
            }
            auto inner = read_list();
            if (!inner) return nullptr;
            auto out = std::make_unique<Value>();
            out->type = Value::Type::Struct;
            if (!inner->children.empty() &&
                inner->children[0]->type == Value::Type::Symbol) {
                out->s = inner->children[0]->s;
                // Move remaining children into the struct.
                for (size_t i = 1; i < inner->children.size(); i++)
                    out->children.push_back(std::move(inner->children[i]));
            } else {
                out->children = std::move(inner->children);
            }
            return out;
        }
        if (c == '\'') {
            // function quote #'foo
            ++src_;
            return read();
        }
        if (c >= '0' && c <= '9') {
            // #N= or #N# reader references. We don't see them in the
            // index but accept them defensively: #N= VALUE  → VALUE;
            // #N# → nil (can't resolve without a table).
            while (src_ < end_ && *src_ >= '0' && *src_ <= '9') ++src_;
            if (src_ < end_ && *src_ == '=') { ++src_; return read(); }
            if (src_ < end_ && *src_ == '#') {
                ++src_;
                auto v = std::make_unique<Value>();
                v->type = Value::Type::Nil;
                return v;
            }
            error_ = "bad # reader form";
            return nullptr;
        }
        error_ = std::string("unknown # form: ") + c;
        return nullptr;
    }

    static bool is_delim(char c)
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
               c == '(' || c == ')' || c == '[' || c == ']' ||
               c == '"' || c == ';' || c == '\'' || c == '`' || c == ',';
    }

    const char *src_;
    const char *end_;
    std::string error_;
};

// ---- Extractors ------------------------------------------------------

static std::string as_string(const Value *v)
{
    if (!v) return {};
    if (v->type == Value::Type::String) return v->s;
    return {};
}

static double as_double(const Value *v)
{
    if (!v) return 0;
    if (v->type == Value::Type::Float) return v->f;
    if (v->type == Value::Type::Int)   return (double)v->i;
    return 0;
}

static int as_int(const Value *v)
{
    if (!v) return 0;
    if (v->type == Value::Type::Int)   return (int)v->i;
    if (v->type == Value::Type::Float) return (int)v->f;
    return 0;
}

// Strip the leading ':' from a keyword symbol; leave plain symbols
// untouched. Used when converting tag symbols like `unread` or
// `:unread` into plain strings "unread".
static std::string symbol_name(const Value *v)
{
    if (!v || v->type != Value::Type::Symbol) return {};
    if (!v->s.empty() && v->s[0] == ':') return v->s.substr(1);
    return v->s;
}

// Convert a classic `author` field (nil, a string, or a list of
// `(:name ... :uri ...)` plists) into a flat display string. When
// there are multiple, names are comma-joined.
static std::string flatten_author(const Value *v)
{
    if (!v || v->is_nil()) return {};
    if (v->type == Value::Type::String) return v->s;
    if (v->type == Value::Type::List) {
        std::string out;
        for (auto &child : v->children) {
            if (!child) continue;
            // Each child is a plist like (:name "N" :uri "U")
            std::string name;
            if (child->type == Value::Type::List) {
                const Value *n = child->get("name");
                if (n) name = as_string(n);
            } else if (child->type == Value::Type::String) {
                name = child->s;
            }
            if (name.empty()) continue;
            if (!out.empty()) out += ", ";
            out += name;
        }
        return out;
    }
    return {};
}

static void extract_authors(const Value *meta, std::vector<Author> &out)
{
    if (!meta || meta->type != Value::Type::List) return;
    const Value *authors = meta->get("authors");
    if (!authors || authors->type != Value::Type::List) return;
    for (auto &c : authors->children) {
        if (!c || c->type != Value::Type::List) continue;
        Author a;
        if (const Value *n = c->get("name")) a.name = as_string(n);
        if (const Value *e = c->get("email")) a.email = as_string(e);
        if (const Value *u = c->get("uri")) a.uri = as_string(u);
        if (!a.name.empty() || !a.email.empty() || !a.uri.empty())
            out.push_back(std::move(a));
    }
}

static bool extract_feed(const Value *v, Feed &out)
{
    // #s(elfeed-feed ID URL TITLE AUTHOR META) — 5 fields.
    if (!v || v->type != Value::Type::Struct) return false;
    if (v->s != "elfeed-feed") return false;
    if (v->children.size() < 5) return false;

    // Prefer URL (field 1); fall back to ID (field 0) if missing.
    out.url = as_string(v->children[1].get());
    if (out.url.empty()) out.url = as_string(v->children[0].get());
    out.title = as_string(v->children[2].get());
    out.author = flatten_author(v->children[3].get());

    const Value *meta = v->children[4].get();
    if (meta && meta->type == Value::Type::List) {
        if (const Value *t = meta->get("etag"))
            out.etag = as_string(t);
        if (const Value *t = meta->get("last-modified"))
            out.last_modified = as_string(t);
        if (const Value *t = meta->get("canonical-url"))
            out.canonical_url = as_string(t);
        if (const Value *t = meta->get("failures"))
            out.failures = as_int(t);
        if (const Value *t = meta->get("last-update"))
            out.last_update = as_double(t);
    }
    return !out.url.empty();
}

static bool extract_entry(const Value *key, const Value *v, Entry &out)
{
    // Key is a cons ("namespace" . "id").
    if (!key || key->type != Value::Type::Cons) return false;
    out.namespace_ = as_string(key->car.get());
    out.id = as_string(key->cdr.get());
    if (out.namespace_.empty() || out.id.empty()) return false;

    // #s(elfeed-entry ID TITLE LINK DATE CONTENT CONTENT-TYPE
    //                 ENCLOSURES TAGS FEED-ID META) — 10 fields.
    if (!v || v->type != Value::Type::Struct) return false;
    if (v->s != "elfeed-entry") return false;
    if (v->children.size() < 10) return false;

    out.title        = as_string(v->children[1].get());
    out.link         = as_string(v->children[2].get());
    out.date         = as_double(v->children[3].get());
    // children[4] is content: an elfeed-ref struct pointing at an
    // external file. Per the user, we skip it.
    out.content_type = symbol_name(v->children[5].get());

    // Enclosures: list of (url type length) lists.
    const Value *encs = v->children[6].get();
    if (encs && encs->type == Value::Type::List) {
        for (auto &e : encs->children) {
            if (!e || e->type != Value::Type::List) continue;
            Enclosure enc;
            if (!e->children.empty())
                enc.url = as_string(e->children[0].get());
            if (e->children.size() > 1)
                enc.type = as_string(e->children[1].get());
            if (e->children.size() > 2) {
                const Value *len = e->children[2].get();
                if (len && len->type == Value::Type::Int) enc.length = len->i;
            }
            if (!enc.url.empty()) out.enclosures.push_back(std::move(enc));
        }
    }

    // Tags: list of symbols.
    const Value *tags = v->children[7].get();
    if (tags && tags->type == Value::Type::List) {
        for (auto &t : tags->children) {
            std::string s = symbol_name(t.get());
            if (!s.empty() && s != "nil") out.tags.push_back(std::move(s));
        }
    }

    out.feed_url = as_string(v->children[8].get());

    // Meta may carry :authors and :categories.
    extract_authors(v->children[9].get(), out.authors);

    return true;
}

} // anonymous namespace

// ---- Top-level --------------------------------------------------------

ImportStats import_classic_elfeed(Elfeed *app, const std::string &path)
{
    ImportStats stats;

    wxFFile f;
    if (!f.Open(wxString::FromUTF8(path), "rb")) {
        stats.error = "cannot open: " + path;
        return stats;
    }
    wxFileOffset len = f.Length();
    if (len <= 0) {
        stats.error = "empty file";
        return stats;
    }
    std::string buf((size_t)len, '\0');
    if (f.Read(buf.data(), (size_t)len) != (size_t)len) {
        stats.error = "short read";
        return stats;
    }

    Parser p(buf.data(), buf.size());

    // There are two top-level forms: a "dummy" index for backwards
    // compat and then the real index. The real one has the feeds and
    // entries we want. Walk top-level forms; for each, check the
    // :version field — if it's an integer, it's the real index.
    ValuePtr real_index;
    while (!p.eof()) {
        auto v = p.read();
        if (!v) { stats.error = "parse error: " + p.error(); return stats; }
        if (v->type != Value::Type::List) continue;
        const Value *ver = v->get("version");
        if (ver && ver->type == Value::Type::Int) {
            real_index = std::move(v);
            break;
        }
    }
    if (!real_index) {
        stats.error = "no real index found in file";
        return stats;
    }

    // --- Feeds ----------------------------------------------------
    const Value *feeds = real_index->get("feeds");
    if (feeds && feeds->type == Value::Type::Struct &&
        feeds->s == "hash-table") {
        // Struct children for a hash-table are the flat (k1 v1 k2 v2 ...)
        // list that follows `data`. Find the `data` keyword/symbol in
        // the struct's child sequence.
        const Value *data = nullptr;
        for (size_t i = 0; i + 1 < feeds->children.size(); i++) {
            if (feeds->children[i] &&
                feeds->children[i]->is_symbol("data")) {
                data = feeds->children[i + 1].get();
                break;
            }
        }
        if (data && data->type == Value::Type::List) {
            for (size_t i = 0; i + 1 < data->children.size(); i += 2) {
                Feed feed;
                if (!extract_feed(data->children[i + 1].get(), feed)) continue;
                // The import only writes to the feed table — the set of
                // feeds the user wants fetched is determined solely by
                // the config file. Imported feeds become part of the
                // historical record but are not auto-subscribed; if the
                // user wants them refetched they add the URL to config.
                db_update_feed(app, feed);
                stats.feeds_imported++;
            }
        }
    }

    // --- Entries --------------------------------------------------
    const Value *entries = real_index->get("entries");
    if (entries && entries->type == Value::Type::Struct &&
        entries->s == "hash-table") {
        const Value *data = nullptr;
        for (size_t i = 0; i + 1 < entries->children.size(); i++) {
            if (entries->children[i] &&
                entries->children[i]->is_symbol("data")) {
                data = entries->children[i + 1].get();
                break;
            }
        }
        if (data && data->type == Value::Type::List) {
            // Build a whole batch for db_add_entries' transactional
            // insert. For very large imports we could chunk, but at
            // ~tens of thousands of entries this is still a one-shot
            // operation in a few seconds.
            std::vector<Entry> batch;
            batch.reserve(data->children.size() / 2);
            for (size_t i = 0; i + 1 < data->children.size(); i += 2) {
                Entry e;
                if (!extract_entry(data->children[i].get(),
                                   data->children[i + 1].get(), e)) {
                    stats.entries_skipped++;
                    continue;
                }
                batch.push_back(std::move(e));
            }
            stats.entries_imported = (int)batch.size();
            db_add_entries(app, batch);
        }
    }

    return stats;
}
