#pragma once

// Minimal ODBC connection-string parsing: "KEY1=VALUE1;KEY2=VALUE2;...".
// Deliberately doesn't handle the full ODBC connection-string grammar
// (braced values for embedded ';'/'=', DSN lookups via odbc.ini) - out of
// scope for the Phase 4 MVP, which only needs SQLDriverConnect's
// driver-keyword form to work (see docs/protocol-notes.md and the plan's
// Phase 4 slice 1 for the keyword set: HOST/SERVICE/SERVER/DATABASE/UID/PWD).

#include <map>
#include <string>
#include <string_view>

namespace ifxodbc {

// Parses "K=V;K=V;..." into a map with upper-cased keys (ODBC connection
// string keywords are conventionally case-insensitive).
[[nodiscard]] std::map<std::string, std::string> parseConnectionString(std::string_view s);

// Looks up any of `names` (tried in order) in `attrs`, returning the first
// match or `fallback`. Used since ODBC drivers conventionally accept a few
// synonym spellings for the same keyword (e.g. UID/USER, PWD/PASSWORD).
[[nodiscard]] std::string lookupAny(const std::map<std::string, std::string>& attrs,
                                     std::initializer_list<const char*> names, std::string fallback = "");

} // namespace ifxodbc
