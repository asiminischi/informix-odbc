#pragma once

// Converts one fetched column's raw bytes to text, for the Phase 4 slice 2
// SQL_C_CHAR-only conversion path (see the plan): every MVP scalar type
// either already has a string form (VARCHAR, CHAR, DECIMAL) or trivially
// gets one here (integers via std::to_string, DATE/DATETIME via their own
// formatting). Native C-type conversions (SQL_C_SLONG, SQL_DATE_STRUCT,
// etc.) are Phase 4 slice 3's job, not this one's.

#include "sqli/error.h"
#include "sqli/statement.h"

#include <cstddef>
#include <expected>
#include <span>
#include <string>

namespace ifxodbc {

struct CellText {
    bool isNull = false;
    std::string text; // meaningless if isNull
};

[[nodiscard]] std::expected<CellText, ifxsqli::Error> formatCellAsText(const ifxsqli::ColumnDesc& col,
                                                                         std::span<const std::byte> raw);

} // namespace ifxodbc
