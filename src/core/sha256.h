// SHA-256 (FIPS 180-4), bundled so the launcher does not link libcrypto.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace cl {

class Sha256 {
public:
    Sha256();
    void update(const void* data, size_t len);
    // Lowercase hex digest; the object must not be used afterwards.
    std::string final_hex();

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint8_t buf_[64];
    size_t buf_len_ = 0;
    uint64_t total_ = 0;
};

std::string sha256_hex(const std::string& data);
// Hash a file; done (if given) counts bytes hashed, cancel (if given) aborts.
// Returns false if the file cannot be read or the hash was cancelled.
bool sha256_file(const std::filesystem::path& path, std::string& hex,
                 std::atomic<uint64_t>* done = nullptr, const std::atomic<bool>* cancel = nullptr);

}  // namespace cl
