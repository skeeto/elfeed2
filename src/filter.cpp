#include "elfeed.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>

// Parse a time duration string into seconds.
// Supports: "6-months-ago", "1-year-old", "2-weeks-ago", "3-days",
// and ISO 8601 dates (which are converted to seconds-from-now).
// Units: s/sec, m/min, h/hour, d/day, w/week, M/month, y/year
static double parse_duration(const std::string &s)
{
    if (s.empty()) return 0;

    // Try to parse as a simple "N-unit-ago" pattern
    // First, normalize: replace "ago", "old", "-" with spaces
    std::string clean;
    clean.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (i + 3 <= s.size() && (s.substr(i, 3) == "ago" ||
                                   s.substr(i, 3) == "old")) {
            clean += ' ';
            i += 3;
        } else if (s[i] == '-') {
            clean += ' ';
            i++;
        } else {
            clean += s[i];
            i++;
        }
    }

    // Trim and extract number + unit
    double total = 0;
    const char *p = clean.c_str();
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        double num = strtod(p, const_cast<char **>(&p));
        if (num == 0) num = 1;

        while (*p == ' ') p++;

        // Parse unit
        double multiplier = 1;
        if (strncmp(p, "year", 4) == 0 || *p == 'y') {
            multiplier = 365.25 * 24 * 3600;
        } else if (strncmp(p, "month", 5) == 0 || *p == 'M') {
            multiplier = 30.44 * 24 * 3600;
        } else if (strncmp(p, "week", 4) == 0 || *p == 'w') {
            multiplier = 7 * 24 * 3600;
        } else if (strncmp(p, "day", 3) == 0 || *p == 'd') {
            multiplier = 24 * 3600;
        } else if (strncmp(p, "hour", 4) == 0 || *p == 'h') {
            multiplier = 3600;
        } else if (strncmp(p, "min", 3) == 0) {
            multiplier = 60;
        } else if (strncmp(p, "sec", 3) == 0 || *p == 's') {
            multiplier = 1;
        }

        // Advance past unit
        while (*p && *p != ' ' && !isdigit(*p)) p++;

        total += num * multiplier;
    }

    return total;
}

Filter filter_parse(const std::string &expr)
{
    Filter f;

    // Tokenize on whitespace
    std::vector<std::string> tokens;
    size_t pos = 0;
    while (pos < expr.size()) {
        while (pos < expr.size() && expr[pos] == ' ') pos++;
        if (pos >= expr.size()) break;
        size_t start = pos;
        while (pos < expr.size() && expr[pos] != ' ') pos++;
        tokens.push_back(expr.substr(start, pos - start));
    }

    for (auto &tok : tokens) {
        if (tok.empty()) continue;

        switch (tok[0]) {
        case '+':
            f.must_have.push_back(tok.substr(1));
            break;
        case '-':
            f.must_not_have.push_back(tok.substr(1));
            break;
        case '@': {
            // Age filter: @age or @age1--age2
            std::string age_str = tok.substr(1);
            auto sep = age_str.find("--");
            if (sep != std::string::npos) {
                f.after = parse_duration(age_str.substr(0, sep));
                f.before = parse_duration(age_str.substr(sep + 2));
            } else {
                f.after = parse_duration(age_str);
            }
            break;
        }
        case '#':
            f.limit = atoi(tok.c_str() + 1);
            break;
        case '=':
            f.feeds.push_back(tok.substr(1));
            break;
        case '~':
            f.not_feeds.push_back(tok.substr(1));
            break;
        case '!':
            f.not_matches.push_back(tok.substr(1));
            break;
        default:
            f.matches.push_back(tok);
            break;
        }
    }

    return f;
}
