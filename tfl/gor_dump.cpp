// gor_dump.cpp -- diagnostic and listing tool for Myth .gor archive files
//
// Usage:
//   gor_dump <gorfile> <offset> [bytes]     -- hex dump at offset
//   gor_dump <gorfile> scan <tagname>       -- find all occurrences of a 4-char name
//   gor_dump <gorfile> list [type]          -- list all index entries (default: .256)
//
// Examples:
//   gor_dump artsound.gor list              -- list all .256 texture tags
//   gor_dump artsound.gor list mesh         -- list all mesh tags
//   gor_dump artsound.gor list all          -- list every tag in the index
//   gor_dump artsound.gor scan 00tm
//   gor_dump artsound.gor 19936 320

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

static uint32_t swap32(uint32_t n) {
    return ((n>>24)&0xFF)|((n>>8)&0xFF00)|((n<<8)&0xFF0000)|((n<<24)&0xFF000000);
}
static int32_t swap32s(int32_t n) { return (int32_t)swap32((uint32_t)n); }

static uint32_t readBE32(const uint8_t* buf, size_t off) {
    uint32_t v; memcpy(&v, buf+off, 4); return swap32(v);
}
static int32_t readBE32s(const uint8_t* buf, size_t off) {
    uint32_t v; memcpy(&v, buf+off, 4); return swap32s((int32_t)v);
}

static void hexdump(const uint8_t* buf, size_t len, long base) {
    for (size_t i = 0; i < len; i += 16) {
        printf("  %08lX: ", base+(long)i);
        for (size_t j = 0; j < 16; j++) {
            if (i+j < len) printf("%02X ", buf[i+j]); else printf("   ");
        }
        printf(" |");
        for (size_t j = 0; j < 16 && i+j < len; j++) {
            uint8_t c = buf[i+j];
            printf("%c", (c>=32&&c<127)?c:'.');
        }
        printf("|\n");
    }
}

// Auto-detect mesh dimensions from section count (sections = tiles * 2)
static void meshFromSections(int sections, int& meshW, int& meshH) {
    meshW = meshH = 0;
    if (sections < 2 || sections % 2 != 0) return;
    int tiles = sections / 2;
    int sq = (int)round(sqrt((double)tiles));
    if (sq * sq == tiles) { meshW = meshH = sq; return; }
    // Find closest-to-square rectangle
    int bestW = 1, bestH = tiles;
    for (int w = 2; w <= tiles; w++) {
        if (tiles % w == 0) {
            int h = tiles / w;
            if (abs(w-h) < abs(bestW-bestH)) { bestW=w; bestH=h; }
        }
    }
    meshW = bestW; meshH = bestH;
}

// Read sections count from a .256 tag's header at the given file offset
static int readSectionCount(FILE* f, long dataOffset) {
    uint8_t hdr[320];
    fseek(f, dataOffset, SEEK_SET);
    if (fread(hdr, 1, 320, f) != 320) return -1;
    // sections field is at byte offset +96, headerLength at +248
    int32_t hdrLen  = readBE32s(hdr, 248);
    int32_t sections = readBE32s(hdr, 96);
    if (hdrLen != 320) return -1;       // not a valid .256 header
    if (sections < 1 || sections > 2000) return -1;
    return (int)sections;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printf("Usage:\n");
        printf("  gor_dump <gorfile> <offset> [bytes]   -- hex dump\n");
        printf("  gor_dump <gorfile> scan <name>         -- find 4-char tag name\n");
        printf("  gor_dump <gorfile> list [type]         -- list index entries\n");
        printf("\n");
        printf("list type examples: .256 (default), mesh, all\n");
        return 1;
    }

    const char* path = argv[1];
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", path); return 1; }
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    printf("File: %s  (%.1f MB)\n\n", path, fileSize/1048576.0);

    // -------------------------------------------------------------------------
    // MODE: list
    // -------------------------------------------------------------------------
    if (strcmp(argv[2], "list") == 0) {
        // Filter type: ".256", "mesh", or "all"
        const char* filterType = ".256";
        bool showAll = false;
        if (argc >= 4) {
            if (strcmp(argv[3], "all") == 0) showAll = true;
            else filterType = argv[3];
        }

        // Read last 2MB where the index lives
        const long SCAN_SIZE = 2 * 1024 * 1024;
        long scanStart = fileSize - SCAN_SIZE;
        if (scanStart < 0) scanStart = 0;
        fseek(f, scanStart, SEEK_SET);
        std::vector<uint8_t> buf(fileSize - scanStart);
        size_t got = fread(buf.data(), 1, buf.size(), f);

        // Index entry layout (64 bytes):
        //   [+ 0] 32 bytes  description string
        //   [+32]  8 bytes  zeros
        //   [+40]  4 bytes  type (e.g. ".256")
        //   [+44]  4 bytes  name (e.g. "00tm")
        //   [+48]  4 bytes  unknown
        //   [+52]  4 bytes  flags
        //   [+56]  4 bytes  data offset (big-endian)
        //   [+60]  4 bytes  data length (big-endian)

        // We scan for type+name pairs. A valid entry has type at i, name at i+4,
        // data offset at i+16, data length at i+20.
        // Known Myth tag types:
        const char* TYPES[] = {
            ".256","mesh","amso","arti","bina","core","font","geom","inte",
            "ligh","lpgr","medi","meef","meli","mode","mons","obje","obpc",
            "part","phys","prgr","proj","reva","scen","soli","soun","stli",
            "temp","text","unit","wind", nullptr
        };

        if (showAll) {
            printf("%-32s  %-4s  %-4s  %10s  %10s\n",
                   "Description", "Type", "Name", "Offset", "Length");
            printf("%s\n", std::string(70, '-').c_str());
        } else if (strcmp(filterType, ".256") == 0) {
            printf("%-32s  %-4s  %10s  %10s  %8s  %s\n",
                   "Description", "Name", "Offset", "Length", "Sections", "Mesh");
            printf("%s\n", std::string(78, '-').c_str());
        } else {
            printf("%-32s  %-4s  %-4s  %10s  %10s\n",
                   "Description", "Type", "Name", "Offset", "Length");
            printf("%s\n", std::string(70, '-').c_str());
        }

        int count = 0;
        for (size_t i = 0; i + 64 <= got; i++) {
            // Check if buf[i..i+3] is a known type
            bool isKnownType = false;
            for (int t = 0; TYPES[t]; t++) {
                if (memcmp(buf.data()+i, TYPES[t], 4) == 0) { isKnownType=true; break; }
            }
            if (!isKnownType) continue;

            // Type is at offset +40 within a 64-byte entry, so entry starts at i-40
            if (i < 40) continue;
            size_t e = i - 40; // entry start

            char type[5] = {}; memcpy(type, buf.data()+i,   4);
            char name[5] = {}; memcpy(name, buf.data()+i+4, 4);

            uint32_t dataOff = readBE32(buf.data(), e+56);
            uint32_t dataLen = readBE32(buf.data(), e+60);

            // Validate
            if ((long)dataOff < 128 || (long)dataOff >= fileSize) continue;
            if (dataLen == 0 || (long)dataOff+(long)dataLen > fileSize) continue;

            // Description: null-terminated string at entry start
            char desc[33] = {};
            memcpy(desc, buf.data()+e, 32);
            desc[32] = 0;
            // Trim trailing nulls/spaces
            for (int k = 31; k >= 0; k--) {
                if (desc[k] == 0 || desc[k] == ' ') desc[k] = 0; else break;
            }

            // Apply filter
            if (!showAll && strcmp(type, filterType) != 0) continue;

            if (showAll || strcmp(filterType, ".256") != 0) {
                printf("%-32s  %-4s  %-4s  %10u  %10u\n",
                       desc, type, name, dataOff, dataLen);
            } else {
                // For .256 tags, also read section count from the header
                int secs = readSectionCount(f, (long)dataOff);
                char meshStr[32] = "?";
                if (secs > 0) {
                    int mw=0, mh=0;
                    meshFromSections(secs, mw, mh);
                    if (mw > 0)
                        snprintf(meshStr, sizeof(meshStr), "%dx%d (%d tiles)", mw, mh, secs/2);
                    else
                        snprintf(meshStr, sizeof(meshStr), "? (%d secs)", secs);
                }
                printf("%-32s  %-4s  %10u  %10u  %8d  %s\n",
                       desc, name, dataOff, dataLen, secs>0?secs:-1, meshStr);
            }
            count++;
        }

        printf("\n%d entries found.\n", count);
        fclose(f);
        return 0;
    }

    // -------------------------------------------------------------------------
    // MODE: scan
    // -------------------------------------------------------------------------
    if (strcmp(argv[2], "scan") == 0) {
        if (argc < 4) { fprintf(stderr, "scan requires a tag name\n"); return 1; }
        const char* tag = argv[3];
        printf("Scanning for '%.4s' in last 2MB...\n\n", tag);

        const long SCAN_SIZE = 2*1024*1024;
        long scanStart = fileSize - SCAN_SIZE;
        if (scanStart < 0) scanStart = 0;
        fseek(f, scanStart, SEEK_SET);
        std::vector<uint8_t> buf(fileSize - scanStart);
        size_t got = fread(buf.data(), 1, buf.size(), f);
        fclose(f);

        int found = 0;
        for (size_t i = 0; i+64 <= got; i++) {
            if (memcmp(buf.data()+i, tag, 4) != 0) continue;
            long absPos = scanStart+(long)i;
            long start = (long)i >= 48 ? (long)i-48 : 0;
            printf("Match at 0x%lX:\n", absPos);
            hexdump(buf.data()+start, 112, scanStart+start);
            printf("\n");
            if (++found >= 5) { printf("(stopped at 5 matches)\n"); break; }
        }
        if (!found) printf("Not found.\n");
        return 0;
    }

    // -------------------------------------------------------------------------
    // MODE: hex dump at offset
    // -------------------------------------------------------------------------
    long offset = atol(argv[2]);
    long length = argc >= 4 ? atol(argv[3]) : 320;

    printf("Hex dump: %ld bytes at offset %ld (0x%lX):\n\n", length, offset, offset);
    fseek(f, offset, SEEK_SET);
    std::vector<uint8_t> buf(length);
    size_t got = fread(buf.data(), 1, length, f);
    fclose(f);
    hexdump(buf.data(), got, offset);

    printf("\nAs big-endian int32s:\n");
    for (size_t i = 0; i+4 <= got && i < 512; i += 4) {
        uint32_t val; memcpy(&val, buf.data()+i, 4);
        int32_t sv = swap32s((int32_t)val);
        char str[5]; memcpy(str, buf.data()+i, 4); str[4]=0;
        for(int j=0;j<4;j++) if(str[j]<32||str[j]>126) str[j]='.';
        printf("  [%4zu / 0x%08lX]  0x%08X = %12d  '%s'\n",
               i, offset+(long)i, (uint32_t)sv, sv, str);
    }
    return 0;
}
