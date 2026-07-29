#pragma once

namespace ifxsqli {

// Library version of this driver's SQLI protocol implementation (not the
// Informix server/protocol version, which is negotiated at connect time).
struct LibraryVersion {
    int major;
    int minor;
    int patch;
};

[[nodiscard]] LibraryVersion version() noexcept;

} // namespace ifxsqli
