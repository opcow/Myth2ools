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
static uint32_t r32(const uint8_t* d, size_t o) { return (uint32_t)((d[o]<<24)|(d[o+1]<<16)|(d[o+2]<<8)|d[o+3]); }

int main(int, char**) {
    auto orig = readF("C:/Users/mitch/source/repos/MythTech/extractor/out/original_mesh_tag.bin");
    auto built = readF("C:/Users/mitch/source/repos/MythTech/extractor/out/85gy/raw/mesh_tag.bin");
    if (orig.empty() || built.empty()) { printf("Missing files\n"); return 1; }

    printf("Header field comparison:\n");
    struct HF { int off; const char* name; };
    HF fields[] = {
        {0, "name"}, {4, "media"}, {8, "subW"}, {10, "subH"},
        {0x10, "cell_sz"}, {0x18, "data_off"}, {0x1C, "data_sz"},
        {0x24, "ut_cnt"}, {0x28, "ut_off"}, {0x2C, "ut_sz"},
        {0x34, "inst_cnt"}, {0x38, "inst_off"}, {0x3C, "inst_sz"},
        {0x78, "mc_sz"}, {0x7C, "mc_off"},
        {0x80, "act_cnt"}, {0x84, "act_off"}, {0x88, "act_sz"},
        {0xE8, "lod_sz"}, {0xEC, "lod_off"},
        {0xF0, "conn_sz"}, {0xF4, "conn_off"},
        {0x11C, "s120_sz"}, {0x120, "s120_off"},
    };
    for (auto& f : fields) {
        auto getVal = [&](const uint8_t* d) -> uint32_t {
            if (f.off == 0 || f.off == 4) return r32(d, f.off);
            return r32(d, f.off);
        };
        uint32_t ov = r32(orig.data(), (size_t)f.off);
        uint32_t bv = r32(built.data(), (size_t)f.off);
        if (ov != bv)
            printf("  +0x%02X %-10s orig=%-10u built=%-10u\n", f.off, f.name, ov, bv);
    }

    // Check first unit type record
    uint32_t utOff = 1024 + r32(orig.data(), 0x28);
    printf("\nFirst unit type record (orig at +0x%X):\n", utOff);
    for (int i = 0; i < 4; i++) {
        printf("  [%d] u16[0]=%d u16[2]=%d u16[4]=%d u16[6]=%d tag=%c%c%c%c\n",
            i,
            r16(orig.data(), utOff + i*32),
            r16(orig.data(), utOff + i*32 + 2),
            r16(orig.data(), utOff + i*32 + 4),
            r16(orig.data(), utOff + i*32 + 6),
            orig[utOff + i*32 + 4], orig[utOff + i*32 + 5],
            orig[utOff + i*32 + 6], orig[utOff + i*32 + 7]);
    }

    // Compare our built file's unit types
    if (built.size() >= 1024 + 8) {
        uint32_t bUtOff = 1024 + r32(built.data(), 0x28);
        uint32_t bUtCnt = r32(built.data(), 0x24);
        printf("\nBuilt unit types (first %d):\n", bUtCnt < 4 ? (int)bUtCnt : 4);
        for (uint32_t i = 0; i < bUtCnt && i < 4; i++) {
            printf("  [%d] u16[0]=%d u16[2]=%d u16[4]=%d u16[6]=%d tag=%c%c%c%c\n",
                i,
                r16(built.data(), bUtOff + i*32),
                r16(built.data(), bUtOff + i*32 + 2),
                r16(built.data(), bUtOff + i*32 + 4),
                r16(built.data(), bUtOff + i*32 + 6),
                built[bUtOff + i*32 + 4], built[bUtOff + i*32 + 5],
                built[bUtOff + i*32 + 6], built[bUtOff + i*32 + 7]);
        }
    }

    // Check first 3 instance marker_type + palette_index
    uint32_t iOff = 1024 + r32(orig.data(), 0x38);
    uint32_t bIOff = built.size() >= 1024 + 8 ? 1024 + r32(built.data(), 0x38) : 0;
    printf("\nInstance records (first 3):\n");
    printf("  Orig:\n");
    for (int i = 0; i < 3; i++) {
        int mt = r16(orig.data(), iOff + i*64 + 4);
        int pi = r16(orig.data(), iOff + i*64 + 6);
        int32_t x = (int32_t)r32(orig.data(), iOff + i*64 + 12);
        int32_t y = (int32_t)r32(orig.data(), iOff + i*64 + 16);
        int32_t z = (int32_t)r32(orig.data(), iOff + i*64 + 20);
        int yaw = r16(orig.data(), iOff + i*64 + 32);
        int nxt = r16(orig.data(), iOff + i*64 + 60);
        printf("    [%d] mt=%d pi=%d pos=(%d,%d,%d) yaw=%d next=%d\n", i, mt, pi, x, y, z, yaw, nxt);
    }
    if (bIOff) {
        uint32_t bICnt = r32(built.data(), 0x34);
        printf("  Built (%d instances):\n", bICnt);
        uint32_t n = bICnt < 3 ? bICnt : 3;
        for (uint32_t i = 0; i < n; i++) {
            int mt = r16(built.data(), bIOff + i*64 + 4);
            int pi = r16(built.data(), bIOff + i*64 + 6);
            int32_t x = (int32_t)r32(built.data(), bIOff + i*64 + 12);
            int32_t y = (int32_t)r32(built.data(), bIOff + i*64 + 16);
            int32_t z = (int32_t)r32(built.data(), bIOff + i*64 + 20);
            int yaw = r16(built.data(), bIOff + i*64 + 32);
            int nxt = r16(built.data(), bIOff + i*64 + 60);
            printf("    [%d] mt=%d pi=%d pos=(%d,%d,%d) yaw=%d next=%d\n", i, mt, pi, x, y, z, yaw, nxt);
        }
    }

    return 0;
}
