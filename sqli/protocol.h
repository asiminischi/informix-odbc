#pragma once

#include <cstdint>

namespace ifxsqli::opcode {

// SQLI statement-protocol tagged-record opcodes. Confirmed against a live
// server capture for the subset this driver currently uses; see
// docs/protocol-notes.md. Named to match the (reconstructed, not copied)
// meaning of each tag rather than any vendor source.
inline constexpr std::uint16_t kCommand = 1;
inline constexpr std::uint16_t kPrepare = 2;
inline constexpr std::uint16_t kCursorName = 3;
inline constexpr std::uint16_t kId = 4;
inline constexpr std::uint16_t kBind = 5;
inline constexpr std::uint16_t kOpen = 6;
inline constexpr std::uint16_t kExecute = 7;
inline constexpr std::uint16_t kDescribe = 8;
inline constexpr std::uint16_t kFetch = 9;
inline constexpr std::uint16_t kClose = 10;
inline constexpr std::uint16_t kRelease = 11;
inline constexpr std::uint16_t kEot = 12;
inline constexpr std::uint16_t kError = 13;
inline constexpr std::uint16_t kTuple = 14;
inline constexpr std::uint16_t kDone = 15;
inline constexpr std::uint16_t kInsertDone = 94;
inline constexpr std::uint16_t kNonCursorDescribe = 22;
inline constexpr std::uint16_t kReturnType = 100;
inline constexpr std::uint16_t kWantDone = 49;
inline constexpr std::uint16_t kCost = 55;
inline constexpr std::uint16_t kExit = 56;
inline constexpr std::uint16_t kProtocols = 126;

} // namespace ifxsqli::opcode
