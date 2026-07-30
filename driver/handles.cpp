#include "driver/handles.h"

#include <algorithm>
#include <cstring>

namespace ifxodbc {

SQLRETURN DiagQueue::getDiagRec(SQLSMALLINT recNumber, SQLCHAR* sqlState, SQLINTEGER* nativeErrorPtr,
                                SQLCHAR* messageText, SQLSMALLINT bufferLength,
                                SQLSMALLINT* textLengthPtr) const {
    if (recNumber < 1) return SQL_ERROR;
    auto index = static_cast<std::size_t>(recNumber - 1);
    if (index >= records_.size()) return SQL_NO_DATA;

    const DiagRecord& rec = records_[index];
    if (sqlState) {
        std::memcpy(sqlState, rec.sqlstate.c_str(), 5);
        sqlState[5] = '\0';
    }
    if (nativeErrorPtr) *nativeErrorPtr = rec.nativeError;

    // SQLGetDiagRec truncates into the caller's buffer and reports the
    // *untruncated* length via textLengthPtr, returning
    // SQL_SUCCESS_WITH_INFO (with a 01004 diag of its own, per spec) when
    // truncation occurred - mirrored here for messageText specifically.
    auto fullLen = static_cast<SQLSMALLINT>(rec.message.size());
    if (textLengthPtr) *textLengthPtr = fullLen;

    if (!messageText || bufferLength <= 0) return SQL_SUCCESS;

    auto toCopy = std::min<std::size_t>(rec.message.size(), static_cast<std::size_t>(bufferLength - 1));
    std::memcpy(messageText, rec.message.data(), toCopy);
    messageText[toCopy] = '\0';
    return (static_cast<SQLSMALLINT>(toCopy) < fullLen) ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
}

} // namespace ifxodbc
