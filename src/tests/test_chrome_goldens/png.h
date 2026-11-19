#pragma once
// Minimal deterministic PNG writer for golden images: 8-bit RGB, filter
// None, zlib with fixed-Huffman deflate and greedy LZ77. The same pixels
// always encode to the same bytes, so goldens compare byte for byte.
#include <cstdint>
#include <vector>

namespace chrome_png {

inline uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
    void Bits(uint32_t value, int count) { // LSB first
        for (int i = 0; i < count; ++i) {
            acc_ |= ((value >> i) & 1u) << used_;
            if (++used_ == 8) { out_.push_back(static_cast<uint8_t>(acc_)); acc_ = 0; used_ = 0; }
        }
    }
    void Code(uint32_t code, int length) { // Huffman codes are MSB first
        for (int i = length - 1; i >= 0; --i) Bits((code >> i) & 1u, 1);
    }
    void Flush() { if (used_) { out_.push_back(static_cast<uint8_t>(acc_)); acc_ = 0; used_ = 0; } }
private:
    std::vector<uint8_t>& out_;
    uint32_t acc_ = 0;
    int used_ = 0;
};

inline void Literal(BitWriter& w, unsigned symbol) {
    if (symbol < 144) w.Code(0x30 + symbol, 8);
    else if (symbol < 256) w.Code(0x190 + symbol - 144, 9);
    else if (symbol < 280) w.Code(symbol - 256, 7);
    else w.Code(0xc0 + symbol - 280, 8);
}

inline void Match(BitWriter& w, unsigned length, unsigned distance) {
    static const unsigned lengthBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                            35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const unsigned lengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                             3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const unsigned distBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                          257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                          8193, 12289, 16385, 24577};
    static const unsigned distExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                           7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    int l = 28;
    while (lengthBase[l] > length) --l;
    Literal(w, 257 + l);
    w.Bits(length - lengthBase[l], lengthExtra[l]);
    int d = 29;
    while (distBase[d] > distance) --d;
    w.Code(d, 5);
    w.Bits(distance - distBase[d], distExtra[d]);
}

inline std::vector<uint8_t> Deflate(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out;
    BitWriter w(out);
    w.Bits(1, 1); // final block
    w.Bits(1, 2); // fixed Huffman
    constexpr unsigned HashBits = 16, Window = 32768, MaxMatch = 258;
    std::vector<int64_t> head(1u << HashBits, -1);
    auto hash = [&](size_t i) {
        return ((in[i] << 16 | in[i + 1] << 8 | in[i + 2]) * 2654435761u) >> (32 - HashBits);
    };
    size_t i = 0;
    while (i < in.size()) {
        unsigned best = 0, bestDistance = 0;
        if (i + 3 <= in.size()) {
            const uint32_t h = hash(i);
            const int64_t candidate = head[h];
            head[h] = static_cast<int64_t>(i);
            // The latest position with the same 3 bytes, and the previous byte (runs).
            for (int64_t c : {candidate, static_cast<int64_t>(i) - 1}) {
                if (c < 0 || i - c > Window || static_cast<size_t>(c) >= i) continue;
                unsigned n = 0;
                while (n < MaxMatch && i + n < in.size() && in[c + n] == in[i + n]) ++n;
                if (n > best) { best = n; bestDistance = static_cast<unsigned>(i - c); }
            }
        }
        if (best >= 3) {
            Match(w, best, bestDistance);
            for (size_t k = 1; k < best && i + k + 3 <= in.size(); ++k) head[hash(i + k)] = static_cast<int64_t>(i + k);
            i += best;
        } else {
            Literal(w, in[i]);
            ++i;
        }
    }
    Literal(w, 256);
    w.Flush();
    return out;
}

inline void Chunk(std::vector<uint8_t>& png, const char* type, const std::vector<uint8_t>& data) {
    const uint32_t n = static_cast<uint32_t>(data.size());
    for (int s = 24; s >= 0; s -= 8) png.push_back(static_cast<uint8_t>(n >> s));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), data.begin(), data.end());
    png.insert(png.end(), body.begin(), body.end());
    const uint32_t crc = Crc32(body.data(), body.size());
    for (int s = 24; s >= 0; s -= 8) png.push_back(static_cast<uint8_t>(crc >> s));
}

// pixels: 0xAARRGGBB; alpha is not stored (the CPU scanout is XRGB).
inline std::vector<uint8_t> EncodeRgb(const uint32_t* pixels, uint32_t width, uint32_t height) {
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (width * 3 + 1));
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        for (uint32_t x = 0; x < width; ++x) {
            const uint32_t p = pixels[static_cast<size_t>(y) * width + x];
            raw.push_back(static_cast<uint8_t>(p >> 16));
            raw.push_back(static_cast<uint8_t>(p >> 8));
            raw.push_back(static_cast<uint8_t>(p));
        }
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    const std::vector<uint8_t> d = Deflate(raw);
    z.insert(z.end(), d.begin(), d.end());
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    const uint32_t adler = b << 16 | a;
    for (int s = 24; s >= 0; s -= 8) z.push_back(static_cast<uint8_t>(adler >> s));

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::vector<uint8_t> ihdr;
    for (uint32_t v : {width, height})
        for (int s = 24; s >= 0; s -= 8) ihdr.push_back(static_cast<uint8_t>(v >> s));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});
    return png;
}

} // namespace chrome_png
