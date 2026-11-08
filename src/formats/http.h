#pragma once

// What the online lookups (metadata.cpp) and the translation (translate.cpp) share; only
// for the formats library's own files, which link cpp-httplib.

#include <httplib.h>

#include <string>

namespace on::formats {

std::string userAgent();

// A MetadataError saying what went wrong, in words for the user.
[[noreturn]] void throwNetworkError(httplib::Error err, const std::string& host);

} // namespace on::formats
