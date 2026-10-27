// Node-API binding over the sqli/ protocol layer - no ODBC, no Driver
// Manager. Deliberately thin: one native Connection object owning one
// SqliConnection plus its prepared statements, with every blocking network
// call run on a libuv worker thread (Napi::AsyncWorker) and returned to JS
// as a Promise. Pooling, transactions-as-a-scope, metadata queries and the
// proxy-compatible InformixClient API all live in node/lib/ instead.
//
// Concurrency: SqliConnection is not thread-safe and the SQLI wire is
// strictly request/response, so a Connection allows exactly one in-flight
// operation (`busy_`); a second call while busy rejects rather than
// queueing. node/lib/connection.js serializes calls in front of this.

#include <napi.h>

#include "driver/convert.h"
#include "driver/insert_table_name.h"
#include "sqli/connection.h"
#include "sqli/statement.h"
#include "sqli/types.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using ifxsqli::ColumnDesc;
namespace ifxtype = ifxsqli::ifxtype;

// ---------------------------------------------------------------------------
// Type helpers
// ---------------------------------------------------------------------------

// BLOB/CLOB share wire type kBoolean (41) with real BOOLEAN - only
// extendedTypeName tells them apart (same check as driver/statement.cpp's
// isSmartLobColumn).
bool isSmartLob(const ColumnDesc& col) {
    return col.ifxType == ifxtype::kBoolean && (col.extendedTypeName == "blob" || col.extendedTypeName == "clob");
}

bool isSimpleLob(const ColumnDesc& col) { return col.ifxType == ifxtype::kByte || col.ifxType == ifxtype::kText; }

std::string typeNameFor(const ColumnDesc& col) {
    switch (col.ifxType) {
        case ifxtype::kChar: return "char";
        case ifxtype::kSmallInt: return "smallint";
        case ifxtype::kInteger: return "integer";
        case ifxtype::kFloat: return "float";
        case ifxtype::kSmallFloat: return "smallfloat";
        case ifxtype::kDecimal: return "decimal";
        case ifxtype::kSerial: return "serial";
        case ifxtype::kMoney: return "money";
        case ifxtype::kDate: return "date";
        case ifxtype::kDatetime: return "datetime";
        case ifxtype::kByte: return "byte";
        case ifxtype::kText: return "text";
        case ifxtype::kVarchar: return "varchar";
        case ifxtype::kInterval: return "interval";
        case ifxtype::kNVarchar: return "nvarchar";
        case ifxtype::kBigInt: return "bigint";
        case ifxtype::kBigSerial: return "bigserial";
        case ifxtype::kLvarchar: return "lvarchar";
        case ifxtype::kBoolean:
            return col.extendedTypeName.empty() ? std::string("boolean") : col.extendedTypeName;
        default:
            return col.extendedTypeName.empty() ? "type" + std::to_string(col.ifxType) : col.extendedTypeName;
    }
}

// Whether fetchRow + decodeCell below can decode this column. Checked at
// prepare time so an unsupported column fails loudly before any fetch
// rather than risking a wire desync mid-result-set (LVARCHAR is known to
// hang the connection - see node/NOTES.md).
bool isFetchSupported(const ColumnDesc& col) {
    switch (col.ifxType) {
        case ifxtype::kChar:
        case ifxtype::kSmallInt:
        case ifxtype::kInteger:
        case ifxtype::kFloat:
        case ifxtype::kSmallFloat:
        case ifxtype::kDecimal:
        case ifxtype::kMoney:
        case ifxtype::kSerial:
        case ifxtype::kDate:
        case ifxtype::kDatetime:
        case ifxtype::kByte:
        case ifxtype::kText:
        case ifxtype::kVarchar:
        case ifxtype::kInterval:
        case ifxtype::kNVarchar:
        case ifxtype::kBigInt:
        case ifxtype::kBigSerial:
            return true;
        case ifxtype::kBoolean:
            return col.extendedTypeName.empty() || col.extendedTypeName == "boolean" || isSmartLob(col);
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Decoded cell values (built on the worker thread, turned into JS on the
// main thread)
// ---------------------------------------------------------------------------

struct DecimalText {
    std::string text;
};
struct Bytes {
    std::vector<std::byte> data;
};
using Value = std::variant<std::monostate, std::int64_t, double, bool, std::string, DecimalText, Bytes>;
using Row = std::vector<Value>;

enum class Int64Mode { kNumber, kBigInt, kString };
enum class DecimalMode { kNumber, kString };

struct Options {
    Int64Mode int64Mode = Int64Mode::kNumber;
    DecimalMode decimalMode = DecimalMode::kNumber;
};

template <class T>
std::expected<Value, ifxsqli::Error> wrapInt(std::expected<std::optional<T>, ifxsqli::Error> v) {
    if (!v) return std::unexpected(v.error());
    if (!*v) return Value{};
    return Value{static_cast<std::int64_t>(**v)};
}

std::string bytesToString(const std::vector<std::byte>& b) {
    return std::string(reinterpret_cast<const char*>(b.data()), b.size());
}

// Mirrors driver/statement.cpp's per-column dispatch: LOBs need a second
// round trip over the same connection; everything else is pure decoding.
std::expected<Value, ifxsqli::Error> decodeCell(ifxsqli::SqliConnection& conn, const ColumnDesc& col,
                                                std::span<const std::byte> raw) {
    using namespace ifxsqli;
    if (isSmartLob(col)) {
        if (raw.empty()) return std::unexpected(Error{ErrorKind::Protocol, "malformed smart LOB column"});
        if (raw[0] == std::byte{1}) return Value{};
        auto content = conn.fetchSmartLob(raw.subspan(1));
        if (!content) return std::unexpected(content.error());
        if (col.extendedTypeName == "clob") return Value{bytesToString(*content)};
        return Value{Bytes{std::move(*content)}};
    }
    if (isSimpleLob(col)) {
        if (raw.size() != kSimpleLobDescriptorSize) {
            return std::unexpected(Error{ErrorKind::Protocol, "malformed simple LOB descriptor"});
        }
        // NULL sentinel: tb_flags (offset 38-39) == 1.
        if (std::to_integer<unsigned>(raw[38]) == 0 && std::to_integer<unsigned>(raw[39]) == 1) return Value{};
        auto content = conn.fetchSimpleLob(raw);
        if (!content) return std::unexpected(content.error());
        if (col.ifxType == ifxtype::kText) return Value{bytesToString(*content)};
        return Value{Bytes{std::move(*content)}};
    }
    switch (col.ifxType) {
        case ifxtype::kSmallInt: return wrapInt(decodeSmallInt(raw));
        case ifxtype::kInteger:
        case ifxtype::kSerial: return wrapInt(decodeInteger(raw));
        case ifxtype::kBigInt:
        case ifxtype::kBigSerial: return wrapInt(decodeBigInt(raw));
        case ifxtype::kFloat: {
            auto v = decodeFloat(raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) return Value{};
            return Value{**v};
        }
        case ifxtype::kSmallFloat: {
            auto v = decodeSmallFloat(raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) return Value{};
            return Value{static_cast<double>(**v)};
        }
        case ifxtype::kBoolean: {
            auto v = decodeBoolean(raw);
            if (!v) return std::unexpected(v.error());
            if (!*v) return Value{};
            return Value{**v};
        }
        default: {
            auto cell = ifxodbc::formatCellAsText(col, raw);
            if (!cell) return std::unexpected(cell.error());
            if (cell->isNull) return Value{};
            if (col.ifxType == ifxtype::kDecimal || col.ifxType == ifxtype::kMoney) {
                return Value{DecimalText{std::move(cell->text)}};
            }
            return Value{std::move(cell->text)};
        }
    }
}

Napi::Value toJs(Napi::Env env, const Value& v, const Options& opts) {
    return std::visit(
        [&](const auto& x) -> Napi::Value {
            using T = std::decay_t<decltype(x)>;
            if constexpr (std::is_same_v<T, std::monostate>) {
                return env.Null();
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                switch (opts.int64Mode) {
                    case Int64Mode::kBigInt: return Napi::BigInt::New(env, x);
                    case Int64Mode::kString: return Napi::String::New(env, std::to_string(x));
                    case Int64Mode::kNumber: break;
                }
                return Napi::Number::New(env, static_cast<double>(x));
            } else if constexpr (std::is_same_v<T, double>) {
                return Napi::Number::New(env, x);
            } else if constexpr (std::is_same_v<T, bool>) {
                return Napi::Boolean::New(env, x);
            } else if constexpr (std::is_same_v<T, std::string>) {
                return Napi::String::New(env, x);
            } else if constexpr (std::is_same_v<T, DecimalText>) {
                if (opts.decimalMode == DecimalMode::kString) return Napi::String::New(env, x.text);
                return Napi::Number::New(env, std::strtod(x.text.c_str(), nullptr));
            } else {
                return Napi::Buffer<char>::Copy(env, reinterpret_cast<const char*>(x.data.data()), x.data.size());
            }
        },
        v);
}

Napi::Object columnToJs(Napi::Env env, const ColumnDesc& col) {
    auto o = Napi::Object::New(env);
    o.Set("name", col.name);
    o.Set("type", typeNameFor(col));
    o.Set("ifxType", col.ifxType);
    unsigned precision = col.encodedLength;
    unsigned scale = 0;
    if (col.ifxType == ifxtype::kDecimal || col.ifxType == ifxtype::kMoney) {
        precision = (col.encodedLength >> 8) & 0xFF;
        scale = col.encodedLength & 0xFF;
        if (scale == ifxsqli::kFloatingDecimalScale) scale = 0;
    }
    o.Set("precision", precision);
    o.Set("scale", scale);
    o.Set("nullable", !col.notNull);
    return o;
}

// ---------------------------------------------------------------------------
// Parameter encoding (JS value -> BindParam), done on the main thread
// ---------------------------------------------------------------------------

// A BindParam, or - for a smart LOB target - what's needed to create the
// LOB first (createSmartLob needs the network, so it runs on the worker).
struct PendingParam {
    ifxsqli::BindParam param;
    struct SmartLob {
        std::string tableDotColumn;
        std::vector<std::byte> content;
        std::string typeName;
    };
    std::optional<SmartLob> smartLob;
};

std::vector<std::byte> toBytes(const void* p, std::size_t n) {
    std::vector<std::byte> out(n);
    if (n) std::memcpy(out.data(), p, n);
    return out;
}

ifxsqli::BindParam smartLobBindParam(std::span<const std::byte> locator, const std::string& typeName) {
    ifxsqli::BindParam p;
    p.ifxType = ifxtype::kUdt;
    p.extendedOwnerName = "informix";
    p.extendedTypeName = typeName;
    p.precisionField = static_cast<std::uint16_t>(locator.size());
    p.data.resize(4 + locator.size());
    auto len = static_cast<std::uint32_t>(locator.size());
    p.data[0] = static_cast<std::byte>((len >> 24) & 0xFF);
    p.data[1] = static_cast<std::byte>((len >> 16) & 0xFF);
    p.data[2] = static_cast<std::byte>((len >> 8) & 0xFF);
    p.data[3] = static_cast<std::byte>(len & 0xFF);
    std::memcpy(p.data.data() + 4, locator.data(), locator.size());
    return p;
}

ifxsqli::BindParam simpleLobParam(int ifxType, std::vector<std::byte> content) {
    ifxsqli::BindParam p;
    p.ifxType = ifxType;
    p.data = ifxsqli::encodeSimpleLobPlaceholder(static_cast<std::uint32_t>(content.size()));
    p.precisionField = 0;
    p.simpleLobContent = std::move(content);
    return p;
}

void setBooleanIdentity(ifxsqli::BindParam& p) {
    p.extendedOwnerName = "informix";
    p.extendedTypeName = "boolean";
}

// NULL binds still carry the real target type (see node/NOTES.md); with no
// target description (e.g. a SELECT's WHERE-clause "?") CHAR is the
// server-convertible fallback.
ifxsqli::BindParam nullParam(const ColumnDesc* hint) {
    ifxsqli::BindParam p;
    p.isNull = true;
    p.ifxType = ifxtype::kChar;
    if (!hint) return p;
    if (isSmartLob(*hint)) {
        p.ifxType = ifxtype::kBoolean;
        p.extendedOwnerName = "informix";
        p.extendedTypeName = hint->extendedTypeName;
    } else if (hint->ifxType == ifxtype::kBoolean) {
        p.ifxType = ifxtype::kBooleanBindType;
        setBooleanIdentity(p);
    } else if (hint->ifxType == ifxtype::kSerial) {
        p.ifxType = ifxtype::kInteger;
    } else if (hint->ifxType == ifxtype::kBigSerial) {
        p.ifxType = ifxtype::kBigInt;
    } else if (isFetchSupported(*hint)) {
        p.ifxType = hint->ifxType;
    }
    return p;
}

std::expected<ifxsqli::BindParam, std::string> datetimeParam(double ms) {
    using namespace std::chrono;
    auto msInt = static_cast<std::int64_t>(std::floor(ms));
    sys_time<milliseconds> tp{milliseconds{msInt}};
    auto day = floor<days>(tp);
    year_month_day ymd{day};
    hh_mm_ss hms{tp - day};
    ifxsqli::DatetimeValue v{static_cast<int>(ymd.year()),        static_cast<int>(unsigned(ymd.month())),
                             static_cast<int>(unsigned(ymd.day())), static_cast<int>(hms.hours().count()),
                             static_cast<int>(hms.minutes().count()), static_cast<int>(hms.seconds().count())};
    std::uint32_t qualifier = (14u << 8) | 0x0A; // YEAR TO SECOND
    auto subMs = hms.subseconds().count();
    if (subMs != 0) {
        v.fraction = static_cast<int>(subMs * 100); // ms -> FRACTION(5) digits
        v.fractionDigits = 5;
        qualifier = (19u << 8) | 0x0F; // YEAR TO FRACTION(5)
    }
    auto encoded = ifxsqli::encodeDatetime(v, qualifier);
    if (!encoded) return std::unexpected(encoded.error().message);
    ifxsqli::BindParam p;
    p.ifxType = ifxtype::kDatetime;
    p.data = std::move(*encoded);
    p.precisionField = static_cast<std::uint16_t>(qualifier);
    return p;
}

ifxsqli::BindParam dateParam(std::int32_t unixDays) {
    ifxsqli::BindParam p;
    p.ifxType = ifxtype::kDate;
    p.data = ifxsqli::encodeDateUnixDays(unixDays);
    p.precisionField = 4;
    return p;
}

std::optional<std::int32_t> parseIsoDate(const std::string& s) {
    using namespace std::chrono;
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return std::nullopt;
    for (std::size_t i : {0, 1, 2, 3, 5, 6, 8, 9}) {
        if (s[i] < '0' || s[i] > '9') return std::nullopt;
    }
    // Digits already validated above. (Not std::stoi: it inlines strtol,
    // which glibc 2.38+ headers redirect to __isoc23_strtol - a symbol that
    // would stop the prebuilt binary loading on older distros.)
    auto num = [&](std::size_t pos, std::size_t len) {
        int v = 0;
        for (std::size_t i = pos; i < pos + len; ++i) v = v * 10 + (s[i] - '0');
        return v;
    };
    year_month_day ymd{year{num(0, 4)}, month{static_cast<unsigned>(num(5, 2))},
                       day{static_cast<unsigned>(num(8, 2))}};
    if (!ymd.ok()) return std::nullopt;
    return static_cast<std::int32_t>(sys_days{ymd}.time_since_epoch().count());
}

// Max CHAR length the SQ_BIND precision field (and CHAR itself) can carry.
constexpr std::size_t kMaxCharBind = 32767;

// Picks the bind type from the target column's description when the server
// gave one (`hint`: INSERT/UPDATE-style statements), otherwise from the JS
// value's own type. The server converts between compatible types (e.g. a
// CHAR bind compared against an INTEGER column), so the JS-type fallback
// only needs to be value-preserving, not an exact column match.
std::expected<PendingParam, std::string> encodeParam(const Napi::Value& v, const ColumnDesc* hint,
                                                     const std::string& sql) {
    PendingParam out;
    if (v.IsNull() || v.IsUndefined()) {
        out.param = nullParam(hint);
        return out;
    }

    // Smart LOB target: content goes through createSmartLob on the worker.
    if (hint && isSmartLob(*hint) && (v.IsBuffer() || v.IsString())) {
        auto table = ifxodbc::parseInsertTableName(sql);
        if (!table) {
            return std::unexpected("cannot determine target table for BLOB/CLOB parameter " + hint->name +
                                   " (only INSERT INTO <table> is supported)");
        }
        std::vector<std::byte> content;
        if (v.IsBuffer()) {
            auto b = v.As<Napi::Buffer<char>>();
            content = toBytes(b.Data(), b.Length());
        } else {
            auto s = v.As<Napi::String>().Utf8Value();
            content = toBytes(s.data(), s.size());
        }
        out.smartLob = PendingParam::SmartLob{*table + "." + hint->name, std::move(content), hint->extendedTypeName};
        return out;
    }

    if (v.IsBuffer()) {
        auto b = v.As<Napi::Buffer<char>>();
        int type = (hint && hint->ifxType == ifxtype::kText) ? ifxtype::kText : ifxtype::kByte;
        out.param = simpleLobParam(type, toBytes(b.Data(), b.Length()));
        return out;
    }

    if (v.IsBoolean()) {
        out.param.ifxType = ifxtype::kBooleanBindType;
        out.param.data = ifxsqli::encodeBoolean(v.As<Napi::Boolean>().Value());
        out.param.precisionField = ifxsqli::kBooleanBindPrecision;
        setBooleanIdentity(out.param);
        return out;
    }

    if (v.IsBigInt()) {
        bool lossless = false;
        auto i = v.As<Napi::BigInt>().Int64Value(&lossless);
        if (!lossless) return std::unexpected(std::string("bigint parameter out of 64-bit range"));
        out.param.ifxType = ifxtype::kBigInt;
        out.param.data = ifxsqli::encodeBigInt(i);
        out.param.precisionField = ifxsqli::kBigIntBindPrecision;
        return out;
    }

    if (v.IsNumber()) {
        double d = v.As<Napi::Number>().DoubleValue();
        bool integral = std::isfinite(d) && std::trunc(d) == d;
        if (integral && d >= -2147483647.0 && d <= 2147483647.0) {
            out.param.ifxType = ifxtype::kInteger;
            out.param.data = ifxsqli::encodeInteger(static_cast<std::int32_t>(d));
            out.param.precisionField = ifxsqli::kIntegerBindPrecision;
        } else if (integral && std::fabs(d) <= 9007199254740991.0) {
            out.param.ifxType = ifxtype::kBigInt;
            out.param.data = ifxsqli::encodeBigInt(static_cast<std::int64_t>(d));
            out.param.precisionField = ifxsqli::kBigIntBindPrecision;
        } else {
            out.param.ifxType = ifxtype::kFloat;
            out.param.data = ifxsqli::encodeFloat(d);
            out.param.precisionField = ifxsqli::kFloatBindPrecision;
        }
        return out;
    }

    if (v.IsDate()) {
        double ms = v.As<Napi::Date>().ValueOf();
        if (!std::isfinite(ms)) return std::unexpected(std::string("invalid Date parameter"));
        if (hint && hint->ifxType == ifxtype::kDate) {
            out.param = dateParam(static_cast<std::int32_t>(std::floor(ms / 86400000.0)));
            return out;
        }
        auto p = datetimeParam(ms);
        if (!p) return std::unexpected(p.error());
        out.param = std::move(*p);
        return out;
    }

    if (v.IsString()) {
        auto s = v.As<Napi::String>().Utf8Value();
        if (hint && isSimpleLob(*hint)) {
            out.param = simpleLobParam(hint->ifxType, toBytes(s.data(), s.size()));
            return out;
        }
        if (hint && hint->ifxType == ifxtype::kDate) {
            // ISO dates bypass the server's DBDATE-dependent CHAR->DATE parse.
            if (auto days = parseIsoDate(s)) {
                out.param = dateParam(*days);
                return out;
            }
        }
        if (hint && (hint->ifxType == ifxtype::kDecimal || hint->ifxType == ifxtype::kMoney)) {
            if (auto enc = ifxsqli::encodeDecimal(s)) {
                out.param.ifxType = ifxtype::kDecimal;
                out.param.data = std::move(enc->data);
                out.param.precisionField = enc->scale;
                return out;
            }
        }
        if (s.size() > kMaxCharBind) {
            return std::unexpected("string parameter longer than " + std::to_string(kMaxCharBind) +
                                   " bytes; bind a TEXT/CLOB column or a Buffer instead");
        }
        out.param.ifxType = ifxtype::kVarchar;
        out.param.data = ifxsqli::encodeChar(s);
        out.param.precisionField = static_cast<std::uint16_t>(s.size());
        return out;
    }

    return std::unexpected(std::string("unsupported parameter type (use string, number, bigint, boolean, Date, "
                                       "Buffer or null)"));
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

const char* kindName(ifxsqli::ErrorKind k) {
    switch (k) {
        case ifxsqli::ErrorKind::Transport: return "transport";
        case ifxsqli::ErrorKind::Protocol: return "protocol";
        case ifxsqli::ErrorKind::Server: return "server";
    }
    return "unknown";
}

struct StatementEntry {
    std::string sql;
    ifxsqli::SqliStatement stmt;
    bool cursorOpen = false;
};

class Connection : public Napi::ObjectWrap<Connection> {
public:
    static Napi::Function Init(Napi::Env env) {
        return DefineClass(env, "NativeConnection",
                           {
                               InstanceMethod<&Connection::Connect>("connect"),
                               InstanceMethod<&Connection::Disconnect>("disconnect"),
                               InstanceMethod<&Connection::Prepare>("prepare"),
                               InstanceMethod<&Connection::Execute>("execute"),
                               InstanceMethod<&Connection::Fetch>("fetch"),
                               InstanceMethod<&Connection::CloseStatement>("closeStatement"),
                               InstanceMethod<&Connection::Command>("command"),
                               InstanceMethod<&Connection::Begin>("begin"),
                               InstanceMethod<&Connection::Commit>("commit"),
                               InstanceMethod<&Connection::Rollback>("rollback"),
                               InstanceAccessor<&Connection::IsConnected>("connected"),
                           });
    }

    explicit Connection(const Napi::CallbackInfo& info) : Napi::ObjectWrap<Connection>(info) {}

    ~Connection() override {
        // GC'd without disconnect(): drop statements before the connection
        // they reference; the SqliConnection destructor closes the socket.
        stmts_.clear();
        conn_.reset();
    }

    // Worker-side state. Only touched by the single in-flight worker
    // (busy_ guards it) or by the main thread while not busy.
    std::unique_ptr<ifxsqli::SqliConnection> conn_;
    std::map<std::uint32_t, std::unique_ptr<StatementEntry>> stmts_;
    std::uint32_t nextStmtId_ = 1;
    bool busy_ = false;
    bool dead_ = false;
    Options opts_;

    // A transport/protocol error leaves the wire in an unknown state - every
    // later call would read garbage or hang - so the connection is closed
    // and refuses further use rather than limping on.
    void markDead() {
        dead_ = true;
        if (conn_) conn_->transport().close();
    }

private:
    Napi::Value Connect(const Napi::CallbackInfo& info);
    Napi::Value Disconnect(const Napi::CallbackInfo& info);
    Napi::Value Prepare(const Napi::CallbackInfo& info);
    Napi::Value Execute(const Napi::CallbackInfo& info);
    Napi::Value Fetch(const Napi::CallbackInfo& info);
    Napi::Value CloseStatement(const Napi::CallbackInfo& info);
    Napi::Value Command(const Napi::CallbackInfo& info);
    Napi::Value Begin(const Napi::CallbackInfo& info);
    Napi::Value Commit(const Napi::CallbackInfo& info);
    Napi::Value Rollback(const Napi::CallbackInfo& info);
    Napi::Value IsConnected(const Napi::CallbackInfo& info) {
        return Napi::Boolean::New(info.Env(), conn_ && !dead_);
    }

    // Returns a rejected promise if this connection can't take a new
    // operation right now, nullopt if it can.
    std::optional<Napi::Value> checkUsable(Napi::Env env, bool requireOpen = true);
    StatementEntry* findStmt(const Napi::Value& id);
};

Napi::Value rejected(Napi::Env env, const std::string& message, const char* kind = "usage") {
    auto d = Napi::Promise::Deferred::New(env);
    auto err = Napi::Error::New(env, message).Value();
    err.Set("kind", kind);
    d.Reject(err);
    return d.Promise();
}

std::optional<Napi::Value> Connection::checkUsable(Napi::Env env, bool requireOpen) {
    if (busy_) return rejected(env, "connection is busy with another operation");
    if (requireOpen && dead_) return rejected(env, "connection is broken (a previous network/protocol error closed it)");
    if (requireOpen && !conn_) return rejected(env, "not connected");
    return std::nullopt;
}

StatementEntry* Connection::findStmt(const Napi::Value& id) {
    if (!id.IsNumber()) return nullptr;
    auto it = stmts_.find(id.As<Napi::Number>().Uint32Value());
    return it == stmts_.end() ? nullptr : it->second.get();
}

// Base worker: holds a strong reference to the JS Connection so it can't
// be garbage-collected mid-operation, owns the busy_ flag for its lifetime,
// and settles a Promise.
class OpWorker : public Napi::AsyncWorker {
public:
    OpWorker(Connection* c, const char* name)
        : Napi::AsyncWorker(c->Env(), name), deferred_(Napi::Promise::Deferred::New(c->Env())), c_(c) {
        self_ = Napi::Persistent(c->Value());
        c_->busy_ = true;
    }

    Napi::Promise Start() {
        auto p = deferred_.Promise();
        Queue();
        return p;
    }

protected:
    virtual Napi::Value Result(Napi::Env env) { return env.Undefined(); }

    void Fail(const ifxsqli::Error& e) {
        kind_ = kindName(e.kind);
        if (e.kind != ifxsqli::ErrorKind::Server) c_->markDead();
        SetError(e.message);
    }
    void FailUsage(const std::string& msg) {
        kind_ = "usage";
        SetError(msg);
    }

    void OnOK() override {
        c_->busy_ = false;
        auto env = Env();
        Napi::HandleScope scope(env);
        Napi::Value result;
        try {
            result = Result(env);
        } catch (const Napi::Error& e) {
            deferred_.Reject(e.Value());
            return;
        }
        deferred_.Resolve(result);
    }

    void OnError(const Napi::Error& e) override {
        c_->busy_ = false;
        Napi::HandleScope scope(Env());
        auto err = e.Value();
        err.Set("kind", kind_);
        deferred_.Reject(err);
    }

    Napi::Promise::Deferred deferred_;
    Connection* c_;
    Napi::ObjectReference self_;
    std::string kind_ = "unknown";
};

Options parseOptions(const Napi::Object& o) {
    Options opts;
    if (o.Has("int64")) {
        auto m = o.Get("int64").ToString().Utf8Value();
        if (m == "bigint") opts.int64Mode = Int64Mode::kBigInt;
        else if (m == "string") opts.int64Mode = Int64Mode::kString;
    }
    if (o.Has("decimal") && o.Get("decimal").ToString().Utf8Value() == "string") {
        opts.decimalMode = DecimalMode::kString;
    }
    return opts;
}

std::string getString(const Napi::Object& o, const char* key, const std::string& fallback = {}) {
    if (!o.Has(key)) return fallback;
    auto v = o.Get(key);
    if (v.IsUndefined() || v.IsNull()) return fallback;
    return v.ToString().Utf8Value();
}

int getInt(const Napi::Object& o, const char* key, int fallback) {
    if (!o.Has(key)) return fallback;
    auto v = o.Get(key);
    return v.IsNumber() ? v.As<Napi::Number>().Int32Value() : fallback;
}

// --- connect ---------------------------------------------------------------

class ConnectWorker : public OpWorker {
public:
    ConnectWorker(Connection* c, std::string host, int port, ifxsqli::ConnectParams params, int queryTimeoutMs)
        : OpWorker(c, "informix:connect"), host_(std::move(host)), port_(port), params_(std::move(params)),
          queryTimeoutMs_(queryTimeoutMs) {}

    void Execute() override {
        auto r = ifxsqli::SqliConnection::connect(host_, port_, params_);
        if (!r) {
            kind_ = kindName(r.error().kind);
            SetError(r.error().message);
            return;
        }
        conn_ = std::make_unique<ifxsqli::SqliConnection>(std::move(*r));
        // params_.timeoutMs only covered connection setup; statements get
        // their own (opt-in) bound.
        if (queryTimeoutMs_ > 0) conn_->transport().setTimeouts(queryTimeoutMs_);
    }

    Napi::Value Result(Napi::Env env) override {
        c_->conn_ = std::move(conn_);
        c_->dead_ = false;
        auto o = Napi::Object::New(env);
        const auto& si = c_->conn_->serverInfo();
        o.Set("serverVersion", si.serverVersion);
        o.Set("serialNumber", si.serialNumber);
        return o;
    }

private:
    std::string host_;
    int port_;
    ifxsqli::ConnectParams params_;
    int queryTimeoutMs_;
    std::unique_ptr<ifxsqli::SqliConnection> conn_;
};

Napi::Value Connection::Connect(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (busy_) return rejected(env, "connection is busy with another operation");
    if (conn_ && !dead_) return rejected(env, "already connected");
    if (info.Length() < 1 || !info[0].IsObject()) return rejected(env, "connect(options) requires an object");
    auto o = info[0].As<Napi::Object>();
    stmts_.clear();
    conn_.reset();

    // These fallbacks only matter for direct callers of the addon: the JS
    // layer always passes every field, resolved from lib/defaults.js (the
    // single place defaults are controlled).
    ifxsqli::ConnectParams params;
    params.username = getString(o, "username");
    params.password = getString(o, "password");
    params.serverName = getString(o, "server", "informix");
    params.database = getString(o, "database");
    params.appName = getString(o, "appName", "informix-native");
    params.timeoutMs = getInt(o, "connectTimeoutMs", 0);
    int queryTimeoutMs = getInt(o, "queryTimeoutMs", 0);
    int port = getInt(o, "port", 9088);
    opts_ = parseOptions(o);
    return (new ConnectWorker(this, getString(o, "host", "localhost"), port, std::move(params), queryTimeoutMs))
        ->Start();
}

// --- disconnect ------------------------------------------------------------

class DisconnectWorker : public OpWorker {
public:
    explicit DisconnectWorker(Connection* c) : OpWorker(c, "informix:disconnect") {}
    void Execute() override {
        // Statements are dropped without close/release round trips: the
        // server frees them along with the session on SQ_EXIT.
        c_->stmts_.clear();
        if (!c_->dead_) {
            auto r = c_->conn_->disconnectOrderly();
            (void)r; // best effort; the socket is closed either way
        }
        c_->conn_.reset();
        c_->dead_ = false;
    }
};

Napi::Value Connection::Disconnect(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (busy_) return rejected(env, "connection is busy with another operation");
    if (!conn_) {
        auto d = Napi::Promise::Deferred::New(env);
        d.Resolve(env.Undefined());
        return d.Promise();
    }
    return (new DisconnectWorker(this))->Start();
}

// --- prepare ---------------------------------------------------------------

class PrepareWorker : public OpWorker {
public:
    PrepareWorker(Connection* c, std::string sql) : OpWorker(c, "informix:prepare"), sql_(std::move(sql)) {}

    void Execute() override {
        auto r = ifxsqli::SqliStatement::prepare(*c_->conn_, sql_);
        if (!r) return Fail(r.error());
        if (r->hasResultSet()) {
            for (const auto& col : r->columns()) {
                if (!isFetchSupported(col)) {
                    auto rel = r->release();
                    if (!rel) return Fail(rel.error());
                    return FailUsage("column \"" + col.name + "\" has type " + typeNameFor(col) +
                                     ", which this driver cannot fetch yet (cast it to a supported type in SQL)");
                }
            }
        }
        entry_ = std::make_unique<StatementEntry>(StatementEntry{sql_, std::move(*r)});
    }

    Napi::Value Result(Napi::Env env) override {
        auto id = c_->nextStmtId_++;
        const auto& st = entry_->stmt;
        auto o = Napi::Object::New(env);
        o.Set("id", id);
        o.Set("hasResultSet", st.hasResultSet());
        o.Set("paramCount", static_cast<double>(st.paramCount()));
        auto cols = Napi::Array::New(env);
        for (std::size_t i = 0; i < st.columns().size(); ++i) cols.Set(i, columnToJs(env, st.columns()[i]));
        o.Set("columns", cols);
        c_->stmts_.emplace(id, std::move(entry_));
        return o;
    }

private:
    std::string sql_;
    std::unique_ptr<StatementEntry> entry_;
};

Napi::Value Connection::Prepare(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (auto r = checkUsable(env)) return *r;
    if (info.Length() < 1 || !info[0].IsString()) return rejected(env, "prepare(sql) requires a string");
    return (new PrepareWorker(this, info[0].As<Napi::String>().Utf8Value()))->Start();
}

// --- execute ---------------------------------------------------------------

class ExecuteWorker : public OpWorker {
public:
    ExecuteWorker(Connection* c, StatementEntry* e, std::vector<PendingParam> params)
        : OpWorker(c, "informix:execute"), e_(e), params_(std::move(params)) {}

    void Execute() override {
        // Re-executing a SELECT whose cursor is still open: close it first.
        if (e_->cursorOpen) {
            auto r = e_->stmt.close();
            if (!r) return Fail(r.error());
            e_->cursorOpen = false;
        }
        std::vector<ifxsqli::BindParam> binds;
        binds.reserve(params_.size());
        for (auto& pp : params_) {
            if (pp.smartLob) {
                auto locator = c_->conn_->createSmartLob(pp.smartLob->tableDotColumn, pp.smartLob->content);
                if (!locator) return Fail(locator.error());
                binds.push_back(smartLobBindParam(*locator, pp.smartLob->typeName));
            } else {
                binds.push_back(std::move(pp.param));
            }
        }
        auto r = e_->stmt.execute(binds);
        if (!r) return Fail(r.error());
        e_->cursorOpen = e_->stmt.hasResultSet();
        rowCount_ = e_->stmt.hasResultSet() ? -1 : static_cast<double>(e_->stmt.rowCount());
    }

    Napi::Value Result(Napi::Env env) override {
        auto o = Napi::Object::New(env);
        o.Set("rowCount", rowCount_);
        return o;
    }

private:
    StatementEntry* e_;
    std::vector<PendingParam> params_;
    double rowCount_ = 0;
};

Napi::Value Connection::Execute(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (auto r = checkUsable(env)) return *r;
    auto* e = info.Length() > 0 ? findStmt(info[0]) : nullptr;
    if (!e) return rejected(env, "unknown or closed statement");
    std::vector<PendingParam> params;
    std::size_t given = 0;
    Napi::Array arr;
    if (info.Length() > 1 && info[1].IsArray()) {
        arr = info[1].As<Napi::Array>();
        given = arr.Length();
    }
    if (given != e->stmt.paramCount()) {
        return rejected(env, "statement has " + std::to_string(e->stmt.paramCount()) + " parameter marker(s), " +
                                 std::to_string(given) + " value(s) given");
    }
    const auto& descs = e->stmt.paramDescs();
    for (std::size_t i = 0; i < given; ++i) {
        const ColumnDesc* hint = i < descs.size() ? &descs[i] : nullptr;
        auto p = encodeParam(arr.Get(i), hint, e->sql);
        if (!p) return rejected(env, "parameter " + std::to_string(i + 1) + ": " + p.error());
        params.push_back(std::move(*p));
    }
    return (new ExecuteWorker(this, e, std::move(params)))->Start();
}

// --- fetch -----------------------------------------------------------------

class FetchWorker : public OpWorker {
public:
    FetchWorker(Connection* c, StatementEntry* e, std::uint32_t max)
        : OpWorker(c, "informix:fetch"), e_(e), max_(max) {}

    void Execute() override {
        const auto& cols = e_->stmt.columns();
        while (rows_.size() < max_) {
            auto r = e_->stmt.fetchRow();
            if (!r) return Fail(r.error());
            if (!*r) {
                done_ = true;
                // Close the exhausted cursor now so the statement can be
                // re-executed or released without another round trip.
                auto c = e_->stmt.close();
                if (!c) return Fail(c.error());
                e_->cursorOpen = false;
                break;
            }
            const auto& raw = **r;
            Row row;
            row.reserve(cols.size());
            for (std::size_t i = 0; i < cols.size() && i < raw.size(); ++i) {
                auto v = decodeCell(*c_->conn_, cols[i], raw[i]);
                if (!v) return Fail(v.error());
                row.push_back(std::move(*v));
            }
            rows_.push_back(std::move(row));
        }
    }

    Napi::Value Result(Napi::Env env) override {
        auto o = Napi::Object::New(env);
        auto rows = Napi::Array::New(env, rows_.size());
        for (std::size_t r = 0; r < rows_.size(); ++r) {
            auto row = Napi::Array::New(env, rows_[r].size());
            for (std::size_t i = 0; i < rows_[r].size(); ++i) row.Set(i, toJs(env, rows_[r][i], c_->opts_));
            rows.Set(r, row);
        }
        o.Set("rows", rows);
        o.Set("done", done_);
        return o;
    }

private:
    StatementEntry* e_;
    std::uint32_t max_;
    std::vector<Row> rows_;
    bool done_ = false;
};

Napi::Value Connection::Fetch(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (auto r = checkUsable(env)) return *r;
    auto* e = info.Length() > 0 ? findStmt(info[0]) : nullptr;
    if (!e) return rejected(env, "unknown or closed statement");
    if (!e->stmt.hasResultSet()) return rejected(env, "statement has no result set");
    if (!e->cursorOpen) {
        auto d = Napi::Promise::Deferred::New(env);
        auto o = Napi::Object::New(env);
        o.Set("rows", Napi::Array::New(env));
        o.Set("done", true);
        d.Resolve(o);
        return d.Promise();
    }
    std::uint32_t max = 100;
    if (info.Length() > 1 && info[1].IsNumber()) max = std::max(1u, info[1].As<Napi::Number>().Uint32Value());
    return (new FetchWorker(this, e, max))->Start();
}

// --- close statement -------------------------------------------------------

class CloseStatementWorker : public OpWorker {
public:
    CloseStatementWorker(Connection* c, std::uint32_t id) : OpWorker(c, "informix:closeStatement"), id_(id) {}
    void Execute() override {
        auto it = c_->stmts_.find(id_);
        auto entry = std::move(it->second);
        c_->stmts_.erase(it);
        if (entry->cursorOpen) {
            auto r = entry->stmt.close();
            if (!r) return Fail(r.error());
        }
        auto r = entry->stmt.release();
        if (!r) return Fail(r.error());
    }

private:
    std::uint32_t id_;
};

Napi::Value Connection::CloseStatement(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (busy_) return rejected(env, "connection is busy with another operation");
    auto* e = info.Length() > 0 ? findStmt(info[0]) : nullptr;
    if (!e || dead_ || !conn_) {
        // Nothing to release server-side (unknown id, or the session is gone).
        if (e) stmts_.erase(info[0].As<Napi::Number>().Uint32Value());
        auto d = Napi::Promise::Deferred::New(env);
        d.Resolve(env.Undefined());
        return d.Promise();
    }
    return (new CloseStatementWorker(this, info[0].As<Napi::Number>().Uint32Value()))->Start();
}

// --- simple commands -------------------------------------------------------

class SimpleWorker : public OpWorker {
public:
    enum class Op { kCommand, kBegin, kCommit, kRollback };
    SimpleWorker(Connection* c, Op op, std::string sql = {})
        : OpWorker(c, "informix:command"), op_(op), sql_(std::move(sql)) {}
    void Execute() override {
        std::expected<void, ifxsqli::Error> r;
        switch (op_) {
            case Op::kCommand: r = c_->conn_->executeCommand(sql_); break;
            case Op::kBegin: r = c_->conn_->beginTransaction(); break;
            case Op::kCommit: r = c_->conn_->commitTransaction(); break;
            case Op::kRollback: r = c_->conn_->rollbackTransaction(); break;
        }
        if (!r) Fail(r.error());
    }

private:
    Op op_;
    std::string sql_;
};

Napi::Value Connection::Command(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (auto r = checkUsable(env)) return *r;
    if (info.Length() < 1 || !info[0].IsString()) return rejected(env, "command(sql) requires a string");
    return (new SimpleWorker(this, SimpleWorker::Op::kCommand, info[0].As<Napi::String>().Utf8Value()))->Start();
}
Napi::Value Connection::Begin(const Napi::CallbackInfo& info) {
    if (auto r = checkUsable(info.Env())) return *r;
    return (new SimpleWorker(this, SimpleWorker::Op::kBegin))->Start();
}
Napi::Value Connection::Commit(const Napi::CallbackInfo& info) {
    if (auto r = checkUsable(info.Env())) return *r;
    return (new SimpleWorker(this, SimpleWorker::Op::kCommit))->Start();
}
Napi::Value Connection::Rollback(const Napi::CallbackInfo& info) {
    if (auto r = checkUsable(info.Env())) return *r;
    return (new SimpleWorker(this, SimpleWorker::Op::kRollback))->Start();
}

Napi::Object InitModule(Napi::Env env, Napi::Object exports) {
    exports.Set("NativeConnection", Connection::Init(env));
    return exports;
}

} // namespace

NODE_API_MODULE(informix_native, InitModule)
