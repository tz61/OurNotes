#include "formats/http.h"

#include "formats/metadata.h"
#include "ournotes/version.h"

namespace on::formats {

std::string userAgent()
{
    return std::string("OurNotes/") + OURNOTES_VERSION + " (mailto:ournotes@users.noreply.github.com)";
}

[[noreturn]] void throwNetworkError(httplib::Error err, const std::string& host)
{
    using E = httplib::Error;
    switch (err) {
    case E::SSLConnection:
    case E::SSLServerVerification:
    case E::SSLServerHostnameVerification:
        throw MetadataError("Could not establish a secure connection to " + host + " (" + httplib::to_string(err) +
                            ").");
    case E::SSLLoadingCerts:
        throw MetadataError("HTTPS is unavailable: the system's certificate store could not be loaded.");
    case E::ConnectionTimeout:
    case E::Timeout:
        throw MetadataError(host + " did not respond in time. Please try again later.");
    case E::Connection:
        throw MetadataError("Could not connect to " + host + ". Check your internet connection.");
    default:
        throw MetadataError("Network error while contacting " + host + ": " + httplib::to_string(err) + ".");
    }
}

} // namespace on::formats
