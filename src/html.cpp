#include "elfeed.hpp"

#include <cctype>
#include <cstring>

// Collapse whitespace and trim (equivalent to elfeed-cleanup)
std::string elfeed_cleanup(const std::string &s)
{
    std::string result;
    result.reserve(s.size());
    bool in_ws = true;
    for (char c : s) {
        if (c == '\f' || c == '\n' || c == '\r' ||
            c == '\t' || c == '\v' || c == ' ') {
            if (!in_ws) {
                result += ' ';
                in_ws = true;
            }
        } else {
            result += c;
            in_ws = false;
        }
    }
    // Trim trailing space
    if (!result.empty() && result.back() == ' ')
        result.pop_back();
    // Trim leading space
    if (!result.empty() && result.front() == ' ')
        result.erase(result.begin());
    return result;
}

// Decode a single HTML entity, return decoded char count or 0
static size_t decode_entity(const char *s, size_t len, std::string &out)
{
    if (len < 3 || s[0] != '&') return 0;

    const char *semi = (const char *)memchr(s + 1, ';', len - 1);
    if (!semi) return 0;
    size_t ent_len = (size_t)(semi - s) + 1;

    if (s[1] == '#') {
        // Numeric entity
        unsigned long cp;
        if (s[2] == 'x' || s[2] == 'X')
            cp = strtoul(s + 3, nullptr, 16);
        else
            cp = strtoul(s + 2, nullptr, 10);
        if (cp == 0) return 0;

        // Encode as UTF-8
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x110000) {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
        return ent_len;
    }

    // Named entities (common ones)
    std::string name(s + 1, semi);
    if (name == "amp")    { out += '&';  return ent_len; }
    if (name == "lt")     { out += '<';  return ent_len; }
    if (name == "gt")     { out += '>';  return ent_len; }
    if (name == "quot")   { out += '"';  return ent_len; }
    if (name == "apos")   { out += '\''; return ent_len; }
    if (name == "nbsp")   { out += ' ';  return ent_len; }
    if (name == "mdash")  { out += "\xe2\x80\x94"; return ent_len; }
    if (name == "ndash")  { out += "\xe2\x80\x93"; return ent_len; }
    if (name == "lsquo")  { out += "\xe2\x80\x98"; return ent_len; }
    if (name == "rsquo")  { out += "\xe2\x80\x99"; return ent_len; }
    if (name == "ldquo")  { out += "\xe2\x80\x9c"; return ent_len; }
    if (name == "rdquo")  { out += "\xe2\x80\x9d"; return ent_len; }
    if (name == "hellip") { out += "\xe2\x80\xa6"; return ent_len; }

    // Unknown entity: pass through
    out.append(s, ent_len);
    return ent_len;
}

// Strip HTML tags and decode entities
std::string html_strip(const std::string &html)
{
    std::string result;
    result.reserve(html.size());

    bool in_tag = false;
    size_t i = 0;
    while (i < html.size()) {
        char c = html[i];
        if (in_tag) {
            if (c == '>') in_tag = false;
            i++;
        } else if (c == '<') {
            in_tag = true;
            i++;
        } else if (c == '&') {
            size_t consumed = decode_entity(html.data() + i,
                                            html.size() - i, result);
            if (consumed > 0)
                i += consumed;
            else {
                result += c;
                i++;
            }
        } else {
            result += c;
            i++;
        }
    }

    return result;
}
