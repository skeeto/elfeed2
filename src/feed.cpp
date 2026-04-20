#include "elfeed.hpp"
#include "util.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <regex>

#include <pugixml.hpp>

// --- URL parsing helpers ---

// Extract protocol from URL (e.g. "https" from "https://example.com/feed")
static std::string url_protocol(const std::string &url)
{
    auto pos = url.find("://");
    if (pos != std::string::npos)
        return url.substr(0, pos);
    return {};
}

// Extract host from URL
static std::string url_host(const std::string &url)
{
    auto start = url.find("://");
    if (start == std::string::npos) return url;
    start += 3;
    auto end = url.find('/', start);
    if (end == std::string::npos) end = url.size();
    // Strip port
    auto colon = url.find(':', start);
    if (colon != std::string::npos && colon < end) end = colon;
    return url.substr(start, end - start);
}

// RFC 3986 §5.2.4 - Remove dot segments
static std::string remove_dot_segments(const std::string &input)
{
    std::string out;
    std::string s = input;
    while (!s.empty()) {
        // A: ../  or ./
        if (s.starts_with("../")) { s = s.substr(3); continue; }
        if (s.starts_with("./"))  { s = s.substr(2); continue; }
        // B: /./  or /.
        if (s.starts_with("/./")) { s = "/" + s.substr(3); continue; }
        if (s == "/.")            { s = "/"; continue; }
        // C: /../ or /..
        if (s.starts_with("/../")) {
            s = "/" + s.substr(4);
            auto pos = out.rfind('/');
            if (pos != std::string::npos) out.erase(pos);
            continue;
        }
        if (s == "/..") {
            s = "/";
            auto pos = out.rfind('/');
            if (pos != std::string::npos) out.erase(pos);
            continue;
        }
        // D: . or ..
        if (s == "." || s == "..") break;
        // E: move first path segment to output
        size_t seg_end;
        if (s[0] == '/') {
            seg_end = s.find('/', 1);
        } else {
            seg_end = s.find('/');
        }
        if (seg_end == std::string::npos) seg_end = s.size();
        out += s.substr(0, seg_end);
        s = s.substr(seg_end);
    }
    return out;
}

// Resolve a potentially relative URL against a base URL
static std::string resolve_url(const std::string &base, const std::string &ref)
{
    if (ref.empty()) return base;
    // Absolute URL?
    if (ref.find("://") != std::string::npos) return ref;
    // Protocol-relative?
    if (ref.size() >= 2 && ref[0] == '/' && ref[1] == '/') {
        return url_protocol(base) + ":" + ref;
    }

    std::string scheme = url_protocol(base);
    auto scheme_end = base.find("://");
    std::string authority;
    std::string base_path;
    if (scheme_end != std::string::npos) {
        auto path_start = base.find('/', scheme_end + 3);
        if (path_start != std::string::npos) {
            authority = base.substr(scheme_end + 3, path_start - scheme_end - 3);
            base_path = base.substr(path_start);
        } else {
            authority = base.substr(scheme_end + 3);
            base_path = "/";
        }
    }

    std::string merged;
    if (ref[0] == '/') {
        merged = remove_dot_segments(ref);
    } else {
        auto last_slash = base_path.rfind('/');
        if (last_slash != std::string::npos)
            merged = base_path.substr(0, last_slash + 1) + ref;
        else
            merged = "/" + ref;
        merged = remove_dot_segments(merged);
    }

    return scheme + "://" + authority + merged;
}

// Prepend protocol to protocol-relative URL
static std::string fixup_protocol(const std::string &protocol,
                                  const std::string &url)
{
    if (!protocol.empty() && url.size() >= 3 &&
        url[0] == '/' && url[1] == '/' && url[2] != '/') {
        return protocol + ":" + url;
    }
    return url;
}

// Compute namespace from URL (hostname)
static std::string url_to_namespace(const std::string &url)
{
    std::string host = url_host(url);
    return host.empty() ? url : host;
}

// --- XML helpers ---

// Strip namespace prefix from tag name: "ns:tag" -> "tag"
static std::string strip_ns(const char *name)
{
    const char *colon = strrchr(name, ':');
    return colon ? std::string(colon + 1) : std::string(name);
}

// Get text content of first matching child element (namespace-stripped)
static std::string child_text(pugi::xml_node node, const char *tag)
{
    for (auto child : node.children()) {
        if (strip_ns(child.name()) == tag)
            return child.child_value();
    }
    return {};
}

// Get all text content of matching children concatenated
static std::string child_text_all(pugi::xml_node node, const char *tag)
{
    std::string result;
    for (auto child : node.children()) {
        if (strip_ns(child.name()) == tag) {
            // Concatenate all text children
            for (auto text : child.children()) {
                if (text.type() == pugi::node_pcdata ||
                    text.type() == pugi::node_cdata)
                    result += text.value();
            }
        }
    }
    return result;
}

// Find first child element with given tag (namespace-stripped)
static pugi::xml_node find_child(pugi::xml_node node, const char *tag)
{
    for (auto child : node.children()) {
        if (strip_ns(child.name()) == tag)
            return child;
    }
    return {};
}

// Find all children with given tag
static std::vector<pugi::xml_node> find_children(pugi::xml_node node,
                                                  const char *tag)
{
    std::vector<pugi::xml_node> result;
    for (auto child : node.children()) {
        if (strip_ns(child.name()) == tag)
            result.push_back(child);
    }
    return result;
}

// Get attribute value from node (namespace-stripped attribute search)
static std::string attr_val(pugi::xml_node node, const char *attr_name)
{
    // Try direct first
    auto a = node.attribute(attr_name);
    if (a) return a.value();
    // Try namespace-stripped
    for (auto a2 : node.attributes()) {
        if (strip_ns(a2.name()) == attr_name)
            return a2.value();
    }
    return {};
}

// Serialize XML node back to string (for XHTML content)
static void xml_unparse(pugi::xml_node node, std::string &out)
{
    if (node.type() == pugi::node_pcdata || node.type() == pugi::node_cdata) {
        out += node.value();
        return;
    }
    if (node.type() != pugi::node_element) return;

    out += '<';
    out += node.name();
    for (auto a : node.attributes()) {
        out += ' ';
        out += a.name();
        out += "='";
        out += a.value();
        out += '\'';
    }
    if (!node.first_child()) {
        out += "/>";
        return;
    }
    out += '>';
    for (auto child : node.children())
        xml_unparse(child, out);
    out += "</";
    out += node.name();
    out += '>';
}

// --- Date parsing ---

// Parse any date string, return epoch seconds or 0
static double parse_date(const std::string &s)
{
    if (s.empty()) return 0;
    double d = parse_iso8601(s);
    if (d > 0) return d;
    d = parse_rfc822(s);
    if (d > 0) return d;
    return 0;
}

// SHA1 hash (simple implementation for ID generation)
// We only need this for fallback ID generation
#include <sstream>
#include <iomanip>

static uint32_t sha1_rotl(uint32_t x, int n)
{
    return (x << n) | (x >> (32 - n));
}

static std::string sha1_hex(const std::string &input)
{
    uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE;
    uint32_t h3 = 0x10325476, h4 = 0xC3D2E1F0;

    // Pre-processing: pad message
    std::string msg = input;
    uint64_t orig_bits = msg.size() * 8;
    msg += (char)0x80;
    while (msg.size() % 64 != 56)
        msg += (char)0x00;
    for (int i = 7; i >= 0; i--)
        msg += (char)((orig_bits >> (i * 8)) & 0xFF);

    // Process each 512-bit chunk
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[80];
        for (size_t i = 0; i < 16; i++) {
            w[i] = ((uint32_t)(uint8_t)msg[chunk + i*4] << 24) |
                    ((uint32_t)(uint8_t)msg[chunk + i*4+1] << 16) |
                    ((uint32_t)(uint8_t)msg[chunk + i*4+2] << 8) |
                    ((uint32_t)(uint8_t)msg[chunk + i*4+3]);
        }
        for (size_t i = 16; i < 80; i++)
            w[i] = sha1_rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);

        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (size_t i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;          k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else              { f = b ^ c ^ d;          k = 0xCA62C1D6; }
            uint32_t temp = sha1_rotl(a, 5) + f + e + k + w[i];
            e = d; d = c; c = sha1_rotl(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }

    char hex[41];
    snprintf(hex, sizeof(hex), "%08x%08x%08x%08x%08x", h0, h1, h2, h3, h4);
    return hex;
}

static std::string generate_id(const std::string &content)
{
    if (content.empty()) {
        return "urn:sha1:" + sha1_hex(std::to_string((double)time(nullptr)));
    }
    return "urn:sha1:" + sha1_hex(content);
}

// --- Atom parsing ---

static std::string atom_content(pugi::xml_node entry)
{
    auto content_node = find_child(entry, "content");
    if (!content_node)
        content_node = find_child(entry, "summary");
    if (!content_node) return {};

    std::string type = attr_val(content_node, "type");
    if (type == "xhtml") {
        // Unparse child elements back to markup
        std::string result;
        for (auto child : content_node.children())
            xml_unparse(child, result);
        return result;
    }

    // Concatenate all text content
    std::string result;
    for (auto child : content_node.children()) {
        if (child.type() == pugi::node_pcdata ||
            child.type() == pugi::node_cdata)
            result += child.value();
    }
    return result;
}

static std::vector<Author> atom_authors(pugi::xml_node node)
{
    std::vector<Author> result;
    for (auto child : find_children(node, "author")) {
        Author a;
        a.name = elfeed_cleanup(child_text(child, "name"));
        a.email = elfeed_cleanup(child_text(child, "email"));
        a.uri = elfeed_cleanup(child_text(child, "uri"));
        if (!a.name.empty() || !a.email.empty())
            result.push_back(std::move(a));
    }
    // Dublin Core creator fallback
    for (auto child : node.children()) {
        if (strip_ns(child.name()) == "creator") {
            Author a;
            a.name = elfeed_cleanup(child.child_value());
            if (!a.name.empty()) result.push_back(std::move(a));
        }
    }
    return result;
}

static void parse_atom(const std::string &url, pugi::xml_node root,
                       FeedParseResult &result)
{
    auto feed_node = root;
    if (strip_ns(root.name()) != "feed") {
        feed_node = find_child(root, "feed");
        if (!feed_node) feed_node = root;
    }

    result.feed_title = elfeed_cleanup(child_text(feed_node, "title"));

    auto feed_authors = atom_authors(feed_node);
    if (!feed_authors.empty())
        result.feed_author = feed_authors[0].name;

    std::string protocol = url_protocol(url);
    std::string ns = url_to_namespace(url);
    std::string xml_base = attr_val(feed_node, "base");
    if (xml_base.empty()) xml_base = url;

    for (auto entry_node : find_children(feed_node, "entry")) {
        Entry e;
        e.feed_url = url;
        e.namespace_ = ns;

        e.title = elfeed_cleanup(child_text(entry_node, "title"));

        // xml:base for this entry
        std::string entry_base = attr_val(entry_node, "base");
        if (!entry_base.empty())
            entry_base = resolve_url(xml_base, entry_base);
        else
            entry_base = xml_base;

        // Link: prefer rel="alternate", fall back to any link href
        std::string link, any_link;
        for (auto link_node : find_children(entry_node, "link")) {
            std::string rel = attr_val(link_node, "rel");
            std::string href = attr_val(link_node, "href");
            if (href.empty()) continue;
            if (any_link.empty()) any_link = href;
            if (rel.empty() || rel == "alternate") {
                link = href;
                break;
            }
        }
        if (link.empty()) link = any_link;
        link = fixup_protocol(protocol, resolve_url(entry_base, link));
        e.link = elfeed_cleanup(link);

        // Date: published > updated > date > modified > issued
        std::string date_str = child_text(entry_node, "published");
        if (date_str.empty()) date_str = child_text(entry_node, "updated");
        if (date_str.empty()) date_str = child_text(entry_node, "date");
        if (date_str.empty()) date_str = child_text(entry_node, "modified");
        if (date_str.empty()) date_str = child_text(entry_node, "issued");
        e.date = parse_date(date_str);
        if (e.date <= 0) e.date = (double)time(nullptr);

        e.authors = atom_authors(entry_node);

        e.content = atom_content(entry_node);

        // Content type: check content/@type or summary/@type
        auto content_node = find_child(entry_node, "content");
        if (!content_node) content_node = find_child(entry_node, "summary");
        if (content_node) {
            std::string type = attr_val(content_node, "type");
            if (type.find("html") != std::string::npos)
                e.content_type = "html";
        }

        // ID: id > link > sha1(content)
        std::string id_str = child_text(entry_node, "id");
        if (id_str.empty()) id_str = e.link;
        if (id_str.empty()) id_str = generate_id(e.content);
        e.id = elfeed_cleanup(id_str);

        // Tags: start with "unread"
        e.tags.push_back("unread");

        // Enclosures: <link rel="enclosure">
        for (auto link_node : find_children(entry_node, "link")) {
            if (attr_val(link_node, "rel") == "enclosure") {
                Enclosure enc;
                enc.url = attr_val(link_node, "href");
                enc.type = attr_val(link_node, "type");
                std::string len = attr_val(link_node, "length");
                if (!len.empty()) enc.length = std::strtoll(len.c_str(), nullptr, 10);
                if (!enc.url.empty())
                    e.enclosures.push_back(std::move(enc));
            }
        }

        result.entries.push_back(std::move(e));
    }
}

// --- RSS 2.0 parsing ---

static std::vector<Author> rss_author(const std::string &author_str)
{
    std::vector<Author> result;
    std::string clean = elfeed_cleanup(author_str);
    if (clean.empty()) return result;

    // RSS format: "email (Name)" or just "email"
    Author a;
    auto paren = clean.find('(');
    if (paren != std::string::npos) {
        a.email = elfeed_cleanup(clean.substr(0, paren));
        auto end = clean.find(')', paren);
        if (end != std::string::npos)
            a.name = elfeed_cleanup(clean.substr(paren + 1, end - paren - 1));
    } else {
        a.email = clean;
    }
    result.push_back(std::move(a));
    return result;
}

static void parse_rss2(const std::string &url, pugi::xml_node root,
                       FeedParseResult &result)
{
    auto rss = root;
    if (strip_ns(root.name()) != "rss")
        rss = find_child(root, "rss");
    auto channel = find_child(rss ? rss : root, "channel");
    if (!channel) return;

    result.feed_title = elfeed_cleanup(child_text(channel, "title"));

    std::string protocol = url_protocol(url);
    std::string ns = url_to_namespace(url);

    for (auto item : find_children(channel, "item")) {
        Entry e;
        e.feed_url = url;
        e.namespace_ = ns;
        e.content_type = "html";

        e.title = elfeed_cleanup(child_text(item, "title"));

        std::string guid = child_text(item, "guid");
        std::string link = child_text(item, "link");
        link = fixup_protocol(protocol, link.empty() ? guid : link);
        e.link = elfeed_cleanup(link);

        // Date
        std::string date_str = child_text(item, "pubDate");
        if (date_str.empty()) date_str = child_text(item, "date");
        e.date = parse_date(date_str);
        if (e.date <= 0) e.date = (double)time(nullptr);

        // Authors
        std::string author_str = child_text(item, "author");
        e.authors = rss_author(author_str);
        // Dublin Core creator
        for (auto child : item.children()) {
            if (strip_ns(child.name()) == "creator") {
                Author a;
                a.name = elfeed_cleanup(child.child_value());
                if (!a.name.empty()) e.authors.push_back(std::move(a));
            }
        }

        // Content: prefer content:encoded over description
        std::string content = child_text_all(item, "encoded");
        if (content.empty()) content = child_text_all(item, "description");
        e.content = content;

        // ID
        std::string id_str = guid;
        if (id_str.empty()) id_str = e.link;
        if (id_str.empty()) id_str = generate_id(e.content);
        e.id = elfeed_cleanup(id_str);

        e.tags.push_back("unread");

        // Enclosures: <enclosure url="..." type="..." length="..."/>
        for (auto enc_node : find_children(item, "enclosure")) {
            Enclosure enc;
            enc.url = attr_val(enc_node, "url");
            enc.type = attr_val(enc_node, "type");
            std::string len = attr_val(enc_node, "length");
            if (!len.empty()) enc.length = std::strtoll(len.c_str(), nullptr, 10);
            if (!enc.url.empty())
                e.enclosures.push_back(std::move(enc));
        }

        result.entries.push_back(std::move(e));
    }
}

// --- RSS 1.0 (RDF) parsing ---

static void parse_rss1(const std::string &url, pugi::xml_node root,
                       FeedParseResult &result)
{
    auto rdf = root;
    auto channel = find_child(rdf, "channel");
    if (channel)
        result.feed_title = elfeed_cleanup(child_text(channel, "title"));

    std::string ns = url_to_namespace(url);

    for (auto item : find_children(rdf, "item")) {
        Entry e;
        e.feed_url = url;
        e.namespace_ = ns;
        e.content_type = "html";

        e.title = elfeed_cleanup(child_text(item, "title"));
        e.link = elfeed_cleanup(child_text(item, "link"));

        std::string date_str = child_text(item, "pubDate");
        if (date_str.empty()) date_str = child_text(item, "date");
        e.date = parse_date(date_str);
        if (e.date <= 0) e.date = (double)time(nullptr);

        e.content = child_text_all(item, "description");

        std::string id_str = e.link;
        if (id_str.empty()) id_str = generate_id(e.content);
        e.id = elfeed_cleanup(id_str);

        e.tags.push_back("unread");
        result.entries.push_back(std::move(e));
    }
}

// --- Public API ---

// Detect feed type from root element name
enum class FeedType { UNKNOWN, ATOM, RSS2, RSS1 };

static FeedType detect_feed_type(pugi::xml_node root)
{
    std::string name = strip_ns(root.name());
    if (name == "feed") return FeedType::ATOM;
    if (name == "rss")  return FeedType::RSS2;
    if (name == "RDF")  return FeedType::RSS1;
    return FeedType::UNKNOWN;
}

FeedParseResult parse_feed(const std::string &url, const std::string &xml_body)
{
    FeedParseResult result;

    pugi::xml_document doc;
    auto parse_result = doc.load_buffer(xml_body.data(), xml_body.size(),
                                        pugi::parse_default |
                                        pugi::parse_cdata);
    if (!parse_result) return result;

    auto root = doc.first_child();
    if (!root) return result;

    switch (detect_feed_type(root)) {
    case FeedType::ATOM: parse_atom(url, root, result); break;
    case FeedType::RSS2: parse_rss2(url, root, result); break;
    case FeedType::RSS1: parse_rss1(url, root, result); break;
    case FeedType::UNKNOWN: break;
    }

    return result;
}
