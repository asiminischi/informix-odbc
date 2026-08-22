#include "driver/insert_table_name.h"

#include <cctype>
#include <string_view>

namespace ifxodbc {

std::optional<std::string> parseInsertTableName(const std::string& sql) {
    std::size_t i = 0;
    auto skipSpace = [&] {
        while (i < sql.size() && std::isspace(static_cast<unsigned char>(sql[i]))) ++i;
    };
    auto matchWord = [&](std::string_view word) {
        skipSpace();
        if (i + word.size() > sql.size()) return false;
        for (std::size_t k = 0; k < word.size(); ++k) {
            if (std::tolower(static_cast<unsigned char>(sql[i + k])) != word[k]) return false;
        }
        std::size_t after = i + word.size();
        if (after < sql.size() &&
            (std::isalnum(static_cast<unsigned char>(sql[after])) || sql[after] == '_')) {
            return false;
        }
        i = after;
        return true;
    };
    if (!matchWord("insert")) return std::nullopt;
    if (!matchWord("into")) return std::nullopt;
    skipSpace();
    std::size_t start = i;
    while (i < sql.size() && !std::isspace(static_cast<unsigned char>(sql[i])) && sql[i] != '(') {
        ++i;
    }
    if (i == start) return std::nullopt;
    return sql.substr(start, i - start);
}

} // namespace ifxodbc
