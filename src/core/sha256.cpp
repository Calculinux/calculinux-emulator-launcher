#include "sha256.h"

#include <cstdio>
#include <cstring>
#include <vector>

namespace cl {

namespace {

const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

}  // namespace

Sha256::Sha256()
    : h_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 |
               (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6], h = h_[7];
    for (int i = 0; i < 64; i++) {
        uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + K[i] + w[i];
        uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += h;
}

void Sha256::update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    total_ += len;
    if (buf_len_) {
        size_t take = 64 - buf_len_;
        if (take > len) take = len;
        memcpy(buf_ + buf_len_, p, take);
        buf_len_ += take;
        p += take;
        len -= take;
        if (buf_len_ < 64) return;
        block(buf_);
        buf_len_ = 0;
    }
    while (len >= 64) {
        block(p);
        p += 64;
        len -= 64;
    }
    if (len) {
        memcpy(buf_, p, len);
        buf_len_ = len;
    }
}

std::string Sha256::final_hex() {
    uint64_t bits = total_ * 8;
    uint8_t pad[72] = {0x80};
    size_t pad_len = (buf_len_ < 56 ? 56 : 120) - buf_len_;
    uint8_t len_be[8];
    for (int i = 0; i < 8; i++) len_be[i] = (uint8_t)(bits >> (56 - 8 * i));
    update(pad, pad_len);
    update(len_be, 8);

    static const char* hex = "0123456789abcdef";
    std::string out(64, '0');
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++) {
            uint8_t byte = (uint8_t)(h_[i] >> (24 - 8 * j));
            out[i * 8 + j * 2] = hex[byte >> 4];
            out[i * 8 + j * 2 + 1] = hex[byte & 15];
        }
    return out;
}

std::string sha256_hex(const std::string& data) {
    Sha256 s;
    s.update(data.data(), data.size());
    return s.final_hex();
}

bool sha256_file(const std::filesystem::path& path, std::string& hex, std::atomic<uint64_t>* done,
                 const std::atomic<bool>* cancel) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    Sha256 s;
    std::vector<uint8_t> buf(1 << 20);
    size_t n;
    bool ok = true;
    while ((n = fread(buf.data(), 1, buf.size(), f)) > 0) {
        s.update(buf.data(), n);
        if (done) *done += n;
        if (cancel && cancel->load()) {
            ok = false;
            break;
        }
    }
    if (ferror(f)) ok = false;
    fclose(f);
    if (ok) hex = s.final_hex();
    return ok;
}

}  // namespace cl
