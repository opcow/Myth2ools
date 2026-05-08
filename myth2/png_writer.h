#pragma once

#include <cstdint>
#include <cstdio>
#include <cstddef>
#include <string>
#include <vector>

namespace myth2_png {

inline void appendBE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((uint8_t)((v >> 24) & 0xFF));
    out.push_back((uint8_t)((v >> 16) & 0xFF));
    out.push_back((uint8_t)((v >> 8) & 0xFF));
    out.push_back((uint8_t)(v & 0xFF));
}

inline uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)-(int)(crc & 1));
    }
    return ~crc;
}

inline uint32_t adler32(const std::vector<uint8_t>& data) {
    const uint32_t MOD = 65521u;
    uint32_t a = 1;
    uint32_t b = 0;
    for (uint8_t v : data) {
        a = (a + v) % MOD;
        b = (b + a) % MOD;
    }
    return (b << 16) | a;
}

inline void appendChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
    appendBE32(out, (uint32_t)data.size());
    size_t typeOffset = out.size();
    out.push_back((uint8_t)type[0]);
    out.push_back((uint8_t)type[1]);
    out.push_back((uint8_t)type[2]);
    out.push_back((uint8_t)type[3]);
    out.insert(out.end(), data.begin(), data.end());
    appendBE32(out, crc32Update(0, out.data() + typeOffset, 4 + data.size()));
}

inline std::vector<uint8_t> zlibStore(const std::vector<uint8_t>& raw) {
    std::vector<uint8_t> out;
    out.reserve(raw.size() + raw.size() / 65535u * 5u + 16u);
    out.push_back(0x78);
    out.push_back(0x01);

    size_t pos = 0;
    while (pos < raw.size()) {
        uint16_t blockLen = (uint16_t)((raw.size() - pos) > 65535u ? 65535u : (raw.size() - pos));
        bool finalBlock = (pos + blockLen) == raw.size();
        out.push_back(finalBlock ? 0x01 : 0x00);
        out.push_back((uint8_t)(blockLen & 0xFF));
        out.push_back((uint8_t)((blockLen >> 8) & 0xFF));
        uint16_t nlen = (uint16_t)~blockLen;
        out.push_back((uint8_t)(nlen & 0xFF));
        out.push_back((uint8_t)((nlen >> 8) & 0xFF));
        out.insert(out.end(), raw.begin() + (ptrdiff_t)pos, raw.begin() + (ptrdiff_t)(pos + blockLen));
        pos += blockLen;
    }

    appendBE32(out, adler32(raw));
    return out;
}

inline bool encodeRGBA(std::vector<uint8_t>& png, const std::vector<uint8_t>& rgba, int w, int h) {
    if (w <= 0 || h <= 0 || rgba.size() != (size_t)w * (size_t)h * 4u) return false;

    std::vector<uint8_t> filtered;
    filtered.reserve((size_t)h * ((size_t)w * 4u + 1u));
    for (int y = 0; y < h; y++) {
        filtered.push_back(0);
        const uint8_t* row = rgba.data() + (size_t)y * (size_t)w * 4u;
        filtered.insert(filtered.end(), row, row + (size_t)w * 4u);
    }

    png.clear();
    const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    png.insert(png.end(), sig, sig + 8);

    std::vector<uint8_t> ihdr;
    appendBE32(ihdr, (uint32_t)w);
    appendBE32(ihdr, (uint32_t)h);
    ihdr.push_back(8);
    ihdr.push_back(6);
    ihdr.push_back(0);
    ihdr.push_back(0);
    ihdr.push_back(0);
    appendChunk(png, "IHDR", ihdr);

    appendChunk(png, "IDAT", zlibStore(filtered));
    appendChunk(png, "IEND", {});
    return true;
}

inline bool writeRGBA(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) {
    std::vector<uint8_t> png;
    if (!encodeRGBA(png, rgba, w, h)) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(png.data(), 1, png.size(), f) == png.size();
    fclose(f);
    return ok;
}

} // namespace myth2_png
