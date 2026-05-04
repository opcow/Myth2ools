// myth2_dump.cpp
// Minimal Myth II tag-file dumper for plugin/local files.
//
// Supports:
//   - dng2 plugin/foundation style files
//   - mth2 local single-tag files
//
// Usage:
//   myth2_dump <file>
//   myth2_dump <file> list [type|all]
//   myth2_dump <file> entrypoints
//
// Examples:
//   myth2_dump "Behind the Library v2"
//   myth2_dump "Behind the Library v2" list mesh
//   myth2_dump "Behind the Library v2" list all
//   myth2_dump "Behind the Library v2" entrypoints

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

static uint16_t readBE16(FILE* f) {
    int a = fgetc(f), b = fgetc(f);
    if (a == EOF || b == EOF) return 0;
    return (uint16_t)(((uint16_t)a << 8) | (uint16_t)b);
}

static uint32_t readBE32(FILE* f) {
    int a = fgetc(f), b = fgetc(f), c = fgetc(f), d = fgetc(f);
    if (a == EOF || b == EOF || c == EOF || d == EOF) return 0;
    return ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
}

static std::string tagToString(uint32_t tag) {
    char s[5];
    s[0] = (char)((tag >> 24) & 0xFF);
    s[1] = (char)((tag >> 16) & 0xFF);
    s[2] = (char)((tag >>  8) & 0xFF);
    s[3] = (char)( tag        & 0xFF);
    s[4] = 0;
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 32 || c > 126) s[i] = '_';
    }
    return std::string(s, 4);
}

static std::string readFixedString(FILE* f, size_t len) {
    std::vector<char> buf(len);
    if (len && fread(buf.data(), 1, len, f) != len) return "";
    size_t n = 0;
    while (n < len && buf[n] != '\0') n++;
    return std::string(buf.data(), n);
}

static long fileLength(FILE* f) {
    long cur = ftell(f);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, cur, SEEK_SET);
    return len;
}

struct Myth2Header {
    uint16_t type = 0;
    uint16_t version = 0;
    std::string name;
    std::string url;
    uint16_t entryPointCount = 0;
    uint16_t tagCount = 0;
    uint32_t checksum = 0;
    uint32_t flags = 0;
    uint32_t size = 0;
    uint32_t headerChecksum = 0;
    uint32_t unused0 = 0;
    uint32_t signature = 0;
    bool isLocal = false;
};

struct Myth2EntryPoint {
    uint32_t subgroupTag = 0;
    uint32_t teamCount = 0;
    uint32_t unused0 = 0;
    uint32_t userData = 0;
    std::string name;
    std::string printedName;
};

struct Myth2TagHeader {
    uint16_t identifier = 0;
    uint8_t flags = 0;
    uint8_t type = 0;
    std::string name;
    uint32_t groupTag = 0;
    uint32_t subgroupTag = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
    uint32_t userData = 0;
    uint16_t version = 0;
    uint8_t foundationIndex = 0;
    uint8_t ownerIndex = 0;
    uint32_t signature = 0;
};

static bool readHeader(FILE* f, Myth2Header& h) {
    rewind(f);
    h.type = readBE16(f);
    h.version = readBE16(f);
    h.name = readFixedString(f, 32);
    h.url  = readFixedString(f, 64);
    h.entryPointCount = readBE16(f);
    h.tagCount = readBE16(f);
    h.checksum = readBE32(f);
    h.flags = readBE32(f);
    h.size = readBE32(f);
    h.headerChecksum = readBE32(f);
    h.unused0 = readBE32(f);
    h.signature = readBE32(f);

    if (h.signature == 0x646E6732u) { // 'dng2'
        h.isLocal = false;
        return true;
    }

    fseek(f, 0x3C, SEEK_SET);
    uint32_t sig = readBE32(f);
    if (sig == 0x6D746832u) { // 'mth2'
        h.isLocal = true;
        h.signature = sig;
        h.entryPointCount = 0;
        h.tagCount = 1;
        return true;
    }
    return false;
}

static bool readEntryPoints(FILE* f, const Myth2Header& h, std::vector<Myth2EntryPoint>& out) {
    out.clear();
    if (h.isLocal || h.entryPointCount == 0) return true;

    fseek(f, 128, SEEK_SET);
    out.reserve(h.entryPointCount);
    for (uint16_t i = 0; i < h.entryPointCount; i++) {
        Myth2EntryPoint ep;
        ep.subgroupTag = readBE32(f);
        ep.teamCount = readBE32(f);
        ep.unused0 = readBE32(f);
        ep.userData = readBE32(f);
        ep.name = readFixedString(f, 32);
        ep.printedName = readFixedString(f, 64);
        out.push_back(ep);
    }
    return true;
}

static bool readTagHeaders(FILE* f, const Myth2Header& h, std::vector<Myth2TagHeader>& out) {
    out.clear();
    long len = fileLength(f);

    if (h.isLocal) rewind(f);
    else fseek(f, 128L + (long)h.entryPointCount * 112L, SEEK_SET);

    out.reserve(h.tagCount);
    for (uint16_t i = 0; i < h.tagCount; i++) {
        Myth2TagHeader th;
        th.identifier = readBE16(f);
        int c = fgetc(f);
        if (c == EOF) return false;
        th.flags = (uint8_t)c;
        c = fgetc(f);
        if (c == EOF) return false;
        th.type = (uint8_t)c;
        th.name = readFixedString(f, 32);
        th.groupTag = readBE32(f);
        th.subgroupTag = readBE32(f);
        th.offset = readBE32(f);
        th.size = readBE32(f);
        th.userData = readBE32(f);
        th.version = readBE16(f);
        c = fgetc(f);
        if (c == EOF) return false;
        th.foundationIndex = (uint8_t)c;
        c = fgetc(f);
        if (c == EOF) return false;
        th.ownerIndex = (uint8_t)c;
        th.signature = readBE32(f);

        if (h.isLocal) {
            th.size = (uint32_t)std::max(0L, len - 64L);
        }

        out.push_back(th);
    }
    return true;
}

static const char* fileTypeName(uint16_t t, bool isLocal) {
    if (isLocal) return "local";
    switch (t) {
        case 0: return "local";
        case 1: return "plugin";
        case 2: return "patch";
        case 3: return "foundation";
        case 4: return "cache";
        default: return "unknown";
    }
}

static void printSummary(const Myth2Header& h, long fileSize) {
    printf("Myth II Tag Dumper\n==================\n");
    printf("File type:   %s\n", fileTypeName(h.type, h.isLocal));
    printf("Signature:   %s\n", tagToString(h.signature).c_str());
    printf("File size:   %ld bytes\n", fileSize);
    printf("Name:        %s\n", h.name.empty() ? "(none)" : h.name.c_str());
    printf("URL:         %s\n", h.url.empty() ? "(none)" : h.url.c_str());
    printf("Tags:        %u\n", (unsigned)h.tagCount);
    printf("Entry points:%u\n", (unsigned)h.entryPointCount);
    printf("Version:     %u\n", (unsigned)h.version);
    printf("Flags:       0x%08X\n", h.flags);
    printf("Checksum:    0x%08X\n", h.checksum);
    printf("Header CRC:  0x%08X\n", h.headerChecksum);
    printf("\n");
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  %s <file>\n"
            "  %s <file> list [type|all]\n"
            "  %s <file> entrypoints\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    const char* path = argv[1];
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open: %s\n", path);
        return 1;
    }

    long len = fileLength(f);
    Myth2Header h;
    if (!readHeader(f, h)) {
        fprintf(stderr, "Not a recognized Myth II dng2/mth2 tag file: %s\n", path);
        fclose(f);
        return 1;
    }

    printSummary(h, len);

    std::vector<Myth2EntryPoint> eps;
    std::vector<Myth2TagHeader> tags;
    if (!readEntryPoints(f, h, eps) || !readTagHeaders(f, h, tags)) {
        fprintf(stderr, "Failed to parse directory\n");
        fclose(f);
        return 1;
    }

    if (argc == 2) {
        if (!tags.empty()) {
            printf("First tags:\n");
            printf("%-32s  %-4s  %-4s  %10s  %10s  %7s\n",
                   "Name", "Type", "ID", "Offset", "Length", "Ver");
            printf("%s\n", std::string(78, '-').c_str());
            size_t n = std::min<size_t>(tags.size(), 12);
            for (size_t i = 0; i < n; i++) {
                const Myth2TagHeader& t = tags[i];
                printf("%-32.32s  %-4s  %-4s  %10u  %10u  %7u\n",
                       t.name.c_str(),
                       tagToString(t.groupTag).c_str(),
                       tagToString(t.subgroupTag).c_str(),
                       t.offset, t.size, (unsigned)t.version);
            }
        }
        fclose(f);
        return 0;
    }

    if (strcmp(argv[2], "entrypoints") == 0) {
        printf("Entry points:\n");
        printf("%-4s  %10s  %-32s  %s\n", "ID", "Teams", "Name", "Printed name");
        printf("%s\n", std::string(96, '-').c_str());
        for (size_t i = 0; i < eps.size(); i++) {
            const Myth2EntryPoint& ep = eps[i];
            printf("%-4s  %10u  %-32.32s  %s\n",
                   tagToString(ep.subgroupTag).c_str(),
                   ep.teamCount,
                   ep.name.c_str(),
                   ep.printedName.c_str());
        }
        fclose(f);
        return 0;
    }

    if (strcmp(argv[2], "list") == 0) {
        const char* filter = (argc >= 4) ? argv[3] : "all";
        bool showAll = strcmp(filter, "all") == 0;

        printf("%-32s  %-4s  %-4s  %10s  %10s  %7s  %10s\n",
               "Name", "Type", "ID", "Offset", "Length", "Ver", "Tag sig");
        printf("%s\n", std::string(92, '-').c_str());

        int count = 0;
        for (size_t i = 0; i < tags.size(); i++) {
            const Myth2TagHeader& t = tags[i];
            std::string type = tagToString(t.groupTag);
            if (!showAll && type != std::string(filter, std::min<size_t>(4, strlen(filter)))) continue;

            printf("%-32.32s  %-4s  %-4s  %10u  %10u  %7u  %10s\n",
                   t.name.c_str(),
                   type.c_str(),
                   tagToString(t.subgroupTag).c_str(),
                   t.offset, t.size, (unsigned)t.version,
                   tagToString(t.signature).c_str());
            count++;
        }
        printf("\n%d entries listed.\n", count);
        fclose(f);
        return 0;
    }

    fprintf(stderr, "Unknown mode: %s\n", argv[2]);
    fclose(f);
    return 1;
}
