#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace on {

// Incremental MD5 (file checksums, compatible with Zotero's storageHash).
class Md5 {
public:
    Md5();
    void update(const void* data, size_t len);
    std::string hexDigest();

private:
    void transform(const uint8_t block[64]);
    uint32_t state_[4];
    uint64_t count_ = 0;
    uint8_t buffer_[64];
};

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    std::string digest();  // 32 raw bytes
    std::string hexDigest();

private:
    void transform(const uint8_t block[64]);
    uint32_t state_[8];
    uint64_t count_ = 0;
    uint8_t buffer_[64];
};

std::string md5Hex(std::string_view data);
std::string md5File(const std::filesystem::path& path);
std::string sha256Hex(std::string_view data);
std::string hmacSha256(std::string_view key, std::string_view msg);  // raw
std::string pbkdf2Sha256(std::string_view password, std::string_view salt, int iterations, size_t len = 32);

std::string toHex(std::string_view raw);
std::string randomBytes(size_t n);
std::string randomToken(size_t bytes = 32);  // hex
bool constantTimeEquals(std::string_view a, std::string_view b);

} // namespace on
