#define _CRT_SECURE_NO_WARNINGS
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>

static std::vector<uint8_t> readF(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    std::vector<uint8_t> buf((size_t)sz);
    if (sz > 0 && fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) { fclose(f); return {}; }
    fclose(f); return buf;
}

static uint16_t r16(const uint8_t* d, size_t o) { return (uint16_t)((d[o]<<8)|d[o+1]); }

int main(int, char**) {
    auto over = readF("C:/Users/mitch/source/repos/MythTech/extractor/out/over/screens/overhead_tag.bin");
    auto def  = readF("C:/Users/mitch/source/repos/MythTech/extractor/out/default/screens/overhead_tag.bin");
    
    // Bitmap header at file offset 0xB30 (2864)
    for (int off = 0; off < 48; off += 2) {
        uint16_t ov = r16(over.data(), 2864 + off);
        uint16_t dv = r16(def.data(), 2864 + off);
        printf("  +%2d: over=0x%04X (%5d) default=0x%04X (%5d)%s\n", 
            off, ov, ov, dv, dv, ov != dv ? " ***" : "");
    }
    return 0;
}
