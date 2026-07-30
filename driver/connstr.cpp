#include "driver/connstr.h"

#include <cctype>

namespace ifxodbc {

namespace {
std::string toUpper(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}
} // namespace

std::map<std::string, std::string> parseConnectionString(std::string_view s) {
    std::map<std::string, std::string> result;
    std::size_t pos = 0;
    while (pos < s.size()) {
        std::size_t semi = s.find(';', pos);
        std::string_view entry = s.substr(pos, semi == std::string_view::npos ? std::string_view::npos : semi - pos);
        pos = (semi == std::string_view::npos) ? s.size() : semi + 1;

        std::size_t eq = entry.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view key = entry.substr(0, eq);
        std::string_view value = entry.substr(eq + 1);
        if (key.empty()) continue;
        result[toUpper(key)] = std::string(value);
    }
    return result;
}

std::string lookupAny(const std::map<std::string, std::string>& attrs, std::initializer_list<const char*> names,
                       std::string fallback) {
    for (const char* name : names) {
        auto it = attrs.find(name);
        if (it != attrs.end()) return it->second;
    }
    return fallback;
}

} // namespace ifxodbc
