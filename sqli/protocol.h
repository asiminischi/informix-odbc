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
// Simple large object (BYTE/TEXT) content transfer, reverse-engineered
// from the decompiled JDBC reference's sendFetchBlob/receiveText/
// receiveBlob and sendBind's blob-chunk-stream tail. kFetchBlob/kBlob's
// fetch-direction use is confirmed live (see
// SqliConnection::fetchSimpleLob's doc comment); the bind-direction use
// of kBlob (a chunk-stream upload terminated by a zero-length chunk) and
// kFerr are decompiled-source-only, not yet implemented or confirmed.
// kFetchBlob: client request, one per simple-LOB descriptor, carries the
// 56-byte descriptor verbatim. kBlob: the response/request's actual data
// chunks (bidirectional - also used for the bind-direction chunk stream,
// terminated there by a zero-length chunk). kFerr: sent instead of a
// final kBlob chunk if the bind-direction stream's source read fails
// mid-transfer.
inline constexpr std::uint16_t kFetchBlob = 38;
inline constexpr std::uint16_t kBlob = 39;
inline constexpr std::uint16_t kFerr = 40;
inline constexpr std::uint16_t kNonCursorDescribe = 22;
inline constexpr std::uint16_t kReturnType = 100;
inline constexpr std::uint16_t kWantDone = 49;
inline constexpr std::uint16_t kCost = 55;
inline constexpr std::uint16_t kExit = 56;
inline constexpr std::uint16_t kProtocols = 126;

// Transaction opcodes. Confirmed live via
// tests/integration/test_transactions.cpp - see docs/protocol-notes.md's
// Transactions section.
inline constexpr std::uint16_t kBegin = 35;
inline constexpr std::uint16_t kCommitWork = 19;
inline constexpr std::uint16_t kRollbackWork = 20;

} // namespace ifxsqli::opcode
