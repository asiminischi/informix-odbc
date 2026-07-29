#pragma once

#include <string>

namespace ifxsqli {

enum class ErrorKind {
    Transport,  // socket connect/read/write failure
    Protocol,   // malformed/unexpected bytes from the server
    Server,     // the server reported an error for our request
};

struct Error {
    ErrorKind kind;
    std::string message;
};

} // namespace ifxsqli
