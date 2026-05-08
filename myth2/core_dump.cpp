// core_dump.cpp
// Dump a Myth II core tag (collection reference) in human-readable form.
//
// Usage:
//   core_dump <file> list
//   core_dump <file> <id>

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

static uint16_t readBE16(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t readBE32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int16_t readBE16s(const uint8_t* p) {
    return (int16_t)readBE16(p);
}

static std::string tagToString(uint32_t tag) {
    char s[5];
    s[0] = (char)((tag >> 24) & 0xFF);
    s[1] = (char)((tag >> 16) & 0xFF);
    s[2] = (char)((tag >> 8) & 0xFF);
    s[3] = (char)(tag & 0xFF);
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
    h.url = readFixedString(f, 64);
    h.entryPointCount = readBE16(f);
    h.tagCount = readBE16(f);
    h.checksum = readBE32(f);
    h.flags = readBE32(f);
    h.size = readBE32(f);
    h.headerChecksum = readBE32(f);
    h.unused0 = readBE32(f);
    h.signature = readBE32(f);

    if (h.signature == 0x646E6732u) {
        h.isLocal = false;
        return true;
    }

    fseek(f, 0x3C, SEEK_SET);
    uint32_t sig = readBE32(f);
    if (sig == 0x6D746832u) {
        h.isLocal = true;
        h.signature = sig;
        h.entryPointCount = 0;
        h.tagCount = 1;
        return true;
    }
    return false;
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

        if (h.isLocal) th.size = (uint32_t)std::max(0L, len - 64L);
        out.push_back(th);
    }
    return true;
}

static double shortFixed(int16_t v) { return (double)v / 256.0; }

static void printTagOrNone(const char* label, uint32_t tag) {
    std::string s = tagToString(tag);
    if (s == "____") printf("%-32s (none)\n", label);
    else printf("%-32s %s\n", label, s.c_str());
}

static void dumpCollectionReference(const uint8_t* p, size_t size) {
    if (size < 384) {
        fprintf(stderr, "Collection reference too small: %zu bytes\n", size);
        return;
    }

    printf("Collection Reference\n");
    printf("====================\n");
    printTagOrNone("collection_tag", readBE32(p + 0));
    printf("%-32s %d\n", "number_of_permutations", (int)readBE16s(p + 4));
    printf("%-32s %d\n", "tint_fraction_raw", (int)readBE16s(p + 6));
    printf("%-32s %.6f\n", "tint_fraction", shortFixed(readBE16s(p + 6)));
    printf("%-32s %d, %d, %d, %d\n", "tint_color_rgba",
           (int)readBE16s(p + 328), (int)readBE16s(p + 330),
           (int)readBE16s(p + 332), (int)readBE16s(p + 334));

    printf("\nPermutation Colors\n");
    printf("------------------\n");
    for (int perm = 0; perm < 5; perm++) {
        printf("perm[%d]:\n", perm);
        for (int hue = 0; hue < 8; hue++) {
            size_t o = 8 + (perm * 8 + hue) * 8;
            printf("  hue[%d] = (%d, %d, %d, %d)\n",
                   hue,
                   (int)readBE16s(p + o + 0),
                   (int)readBE16s(p + o + 2),
                   (int)readBE16s(p + o + 4),
                   (int)readBE16s(p + o + 6));
        }
    }
}

static void usage(const char* p) {
    fprintf(stderr,
        "Myth II Core Dumper\n\n"
        "Usage:\n"
        "  %s <file> list\n"
        "  %s <file> <id>\n\n"
        "Arguments:\n"
        "  file   Myth II foundation/plugin/local tag file\n"
        "  list   List all core tags in the file\n"
        "  id     4-character core tag id (for example wate, wagr, wamu)\n",
        p, p);
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        usage(argv[0]);
        return 1;
    }

    const char* path = argv[1];
    std::string id = argv[2];
    bool listOnly = (id == "list");
    if (!listOnly && id.size() != 4) {
        fprintf(stderr, "Tag id must be exactly 4 characters\n");
        return 1;
    }

    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open: %s\n", path);
        return 1;
    }

    Myth2Header h;
    if (!readHeader(f, h)) {
        fprintf(stderr, "Not a recognized Myth II dng2/mth2 tag file: %s\n", path);
        fclose(f);
        return 1;
    }

    std::vector<Myth2TagHeader> tags;
    if (!readTagHeaders(f, h, tags)) {
        fprintf(stderr, "Failed to parse tag directory\n");
        fclose(f);
        return 1;
    }

    if (listOnly) {
        printf("Myth II Core Dumper\n");
        printf("===================\n");
        printf("File:          %s\n", path);
        printf("Core tags:\n\n");
        printf("%-32s  %-4s  %-10s  %-10s  %-7s\n",
               "Name", "ID", "Offset", "Length", "Ver");
        printf("%s\n", std::string(72, '-').c_str());
        int count = 0;
        for (const auto& t : tags) {
            if (t.groupTag != 0x636F7265u) continue; // 'core'
            printf("%-32.32s  %-4s  %10u  %10u  %7u\n",
                   t.name.c_str(),
                   tagToString(t.subgroupTag).c_str(),
                   t.offset,
                   t.size,
                   (unsigned)t.version);
            count++;
        }
        printf("\n%d core tags listed.\n", count);
        fclose(f);
        return 0;
    }

    uint32_t targetId =
        ((uint32_t)(uint8_t)id[0] << 24) |
        ((uint32_t)(uint8_t)id[1] << 16) |
        ((uint32_t)(uint8_t)id[2] << 8) |
        (uint32_t)(uint8_t)id[3];

    const Myth2TagHeader* found = nullptr;
    for (const auto& t : tags) {
        if (t.groupTag == 0x636F7265u && t.subgroupTag == targetId) { // 'core'
            found = &t;
            break;
        }
    }
    if (!found) {
        fprintf(stderr, "core tag '%s' not found in %s\n", id.c_str(), path);
        fclose(f);
        return 1;
    }

    long dataOffset = (long)found->offset;
    if (!h.isLocal) {
        uint8_t hdr[64];
        fseek(f, dataOffset, SEEK_SET);
        if (fread(hdr, 1, 64, f) == 64) {
            if (readBE32(hdr + 60) == 0x6D746832u) dataOffset += 64;
        } else {
            fprintf(stderr, "Failed to read tag header at offset %u\n", found->offset);
            fclose(f);
            return 1;
        }
    } else {
        dataOffset = 64;
    }

    std::vector<uint8_t> data(found->size);
    fseek(f, dataOffset, SEEK_SET);
    if (fread(data.data(), 1, found->size, f) != found->size) {
        fprintf(stderr, "Failed to read core payload\n");
        fclose(f);
        return 1;
    }
    fclose(f);

    printf("Myth II Core Dumper\n");
    printf("===================\n");
    printf("File:          %s\n", path);
    printf("Tag name:      %s\n", found->name.c_str());
    printf("Tag type/id:   %s %s\n", tagToString(found->groupTag).c_str(), tagToString(found->subgroupTag).c_str());
    printf("Tag offset:    %u\n", found->offset);
    printf("Payload size:  %u\n\n", found->size);

    dumpCollectionReference(data.data(), data.size());
    return 0;
}
