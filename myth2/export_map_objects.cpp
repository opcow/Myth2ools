// export_map_objects.cpp
// Export placed map objects and supporting assets from a Myth II mesh tag.
//
// For each scenery type (object class w0=6) that has a geom tag, emits:
//   assets/models/<tag>.obj   — geometry with UV coordinates
//   assets/models/<tag>.mtl   — material references
// Plus:
//   placement.json     — all scenery instance positions and facings
//
// Scenery types without a geom tag (sprites like trees, signs, farm) are
// listed in the output but skipped for 3D export.
//
// Usage:
//   export_map_objects <tags_folder> <out_folder> [terrain.obj] [--world-space] [--overwrite] [--animation-frame first|none|all]
//
// Example:
//   export_map_objects myth2_tags/small\ install out/le3e

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cctype>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <filesystem>

#include "png_writer.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Big-endian helpers
// ---------------------------------------------------------------------------

static uint32_t swap32(uint32_t n) {
    return ((n&0xFF000000u)>>24)|((n&0x00FF0000u)>>8)
          |((n&0x0000FF00u)<<8)|((n&0x000000FFu)<<24);
}
static uint16_t swap16(uint16_t n) { return (uint16_t)((n>>8)|(n<<8)); }

static uint32_t readBE32u(const uint8_t* b, size_t o) {
    uint32_t v; memcpy(&v, b+o, 4); return swap32(v);
}
static int32_t readBE32s(const uint8_t* b, size_t o) {
    return (int32_t)readBE32u(b, o);
}
static uint16_t readBE16u(const uint8_t* b, size_t o) {
    uint16_t v; memcpy(&v, b+o, 2); return swap16(v);
}
static int16_t readBE16s(const uint8_t* b, size_t o) {
    return (int16_t)readBE16u(b, o);
}

static std::string tagToString(uint32_t tag) {
    char s[5];
    s[0]=(char)((tag>>24)&0xFF);
    s[1]=(char)((tag>>16)&0xFF);
    s[2]=(char)((tag>> 8)&0xFF);
    s[3]=(char)( tag     &0xFF);
    s[4]=0;
    for (int i=0; i<4; i++) {
        unsigned char c=(unsigned char)s[i];
        if (c<32||c>126) s[i]='_';
    }
    return std::string(s,4);
}

static std::string tagToFileStem(uint32_t tag) {
    std::string out;
    char hex[4];
    for (int i = 0; i < 4; i++) {
        unsigned char c = (unsigned char)((tag >> (24 - i * 8)) & 0xFF);
        bool illegal = c < 32 || c > 126 ||
                       c == '<' || c == '>' || c == ':' || c == '"' ||
                       c == '/' || c == '\\' || c == '|' || c == '?' || c == '*';
        if (illegal) {
            snprintf(hex, sizeof(hex), "_%02X", c);
            out += hex;
        } else {
            out += (char)c;
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out += '_';
    return out.empty() ? "tag" : out;
}

static uint32_t tagFromString(const std::string& s) {
    if (s.size()!=4) return 0;
    return ((uint32_t)(uint8_t)s[0]<<24)|((uint32_t)(uint8_t)s[1]<<16)
          |((uint32_t)(uint8_t)s[2]<<8) | (uint32_t)(uint8_t)s[3];
}

// ---------------------------------------------------------------------------
// File I/O
// ---------------------------------------------------------------------------

static std::vector<uint8_t> readFile(const std::string& path) {
    FILE* f=fopen(path.c_str(),"rb");
    if (!f) return {};
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    if (sz<=0) { fclose(f); return {}; }
    std::vector<uint8_t> buf((size_t)sz);
    if (fread(buf.data(),1,(size_t)sz,f)!=(size_t)sz) { fclose(f); return {}; }
    fclose(f); return buf;
}

static bool writeText(const std::string& path, const std::string& text) {
    FILE* f=fopen(path.c_str(),"wb");
    if (!f) { fprintf(stderr,"Cannot write: %s\n",path.c_str()); return false; }
    bool ok=(fwrite(text.data(),1,text.size(),f)==text.size());
    fclose(f); return ok;
}

static void makeDirs(const std::string& path) {
    fs::create_directories(path);
}

static bool writePNG(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) {
    return myth2_png::writeRGBA(path, rgba, w, h);
}

// ---------------------------------------------------------------------------
// .256 collection texture extraction
// ---------------------------------------------------------------------------
// Extracts a single palette-indexed bitmap frame from a Myth II .256 collection
// tag (COLORMAP type) and writes it as a PNG.
//
// The .256 header (320 bytes, all BE) layout used here:
//   [4]   type  (256 = COLORMAP, 12 = sprite)
//   [68]  palette_offset  (from bulk_offset)
//   [96]  num_sectioninfo
//   [100] sectioninfo_offset  (from bulk_offset)
//   [256] bulk_offset  (from start of tag data)
//
// Each sectioninfo is 128 bytes:
//   [64] imagedata_offset  (from bulk_offset)
//   [68] imagedata_length
//
// Each image is preceded by a 48-byte bitmapinfo header plus one row pointer
// per output row:
//   [18] img_x  (int16 BE, width)
//   [20] img_y  (int16 BE, height)
//
// Palette: 2080 bytes at bulk_offset + palette_offset:
//   [0:4]   num_colors (int32 BE)
//   [32:]   color[256], each 8 bytes: r,fr,g,fg,b,fb,flag,ff

// Returns the number of views in a sequence, or 0 on error.
static int dot256SequenceViewCount(const std::vector<uint8_t>& d, int seqIndex) {
    if (d.size() < 320) return 0;
    int32_t bulkOff  = readBE32s(d.data(), 248);
    int32_t seqCount = readBE32s(d.data(), 128);
    int32_t seqOff   = readBE32s(d.data(), 132);
    if (seqIndex < 0 || seqIndex >= seqCount) return 0;
    size_t seqRefEntry = (size_t)(bulkOff + seqOff) + (size_t)seqIndex * 128;
    if (seqRefEntry + 68 > d.size()) return 0;
    int32_t seqDataOff = readBE32s(d.data(), seqRefEntry + 64);
    size_t seqDataAbs = (size_t)(bulkOff + seqDataOff);
    if (seqDataAbs + 12 > d.size()) return 0;
    int16_t nv = readBE16s(d.data(), seqDataAbs + 8);
    return nv > 0 ? nv : 1;
}

static int dot256SequenceCount(const std::vector<uint8_t>& d) {
    if (d.size() < 320) return 0;
    int32_t seqCount = readBE32s(d.data(), 128);
    return seqCount > 0 && seqCount < 4096 ? seqCount : 0;
}

static std::string dot256SequenceName(const std::vector<uint8_t>& d, int seqIndex) {
    if (d.size() < 320) return "";
    int32_t bulkOff  = readBE32s(d.data(), 248);
    int32_t seqCount = readBE32s(d.data(), 128);
    int32_t seqOff   = readBE32s(d.data(), 132);
    if (seqIndex < 0 || seqIndex >= seqCount) return "";
    size_t seqRefEntry = (size_t)(bulkOff + seqOff) + (size_t)seqIndex * 128;
    if (seqRefEntry + 32 > d.size()) return "";
    return std::string((const char*)d.data() + seqRefEntry,
                       strnlen((const char*)d.data() + seqRefEntry, 32));
}

// Extract one view from a sequence in a .256 collection tag.
// Follows: sequence_reference -> sequence_data -> sequence_frame_data ->
//          bitmap_instance_indexes[viewIndex] -> bitmap_instance_data -> bitmap_reference -> pixels
static bool extractDot256Texture(const std::vector<uint8_t>& d,
                                  int seqIndex,
                                  int viewIndex,
                                  const std::string& outPng,
                                  int* outW = nullptr,
                                  int* outH = nullptr,
                                  bool chromaBlueTransparent = false,
                                  bool forceOpaqueNonBlue = false) {
    if (d.size() < 320) return false;

    int32_t bulkOff        = readBE32s(d.data(), 248);
    int32_t palOff         = readBE32s(d.data(), 68);   // color_tables_offset
    int32_t bitmapCount    = readBE32s(d.data(), 96);
    int32_t bitmapRefsOff  = readBE32s(d.data(), 100);
    int32_t bitmapInstCount= readBE32s(d.data(), 112);
    int32_t bitmapInstsOff = readBE32s(d.data(), 116);
    int32_t seqCount       = readBE32s(d.data(), 128);
    int32_t seqRefsOff     = readBE32s(d.data(), 132);

    if (seqIndex < 0 || seqIndex >= seqCount) return false;

    // Palette: at bulkOff + palOff, skip 32-byte color_table header to reach color[0]
    size_t palAbs = (size_t)(bulkOff + palOff);
    if (palAbs + 2080 > d.size()) return false;
    const uint8_t* palData = d.data() + palAbs + 32;

    // sequence_reference[seqIndex]: 128 bytes, data offset at +64
    size_t seqRefEntry = (size_t)(bulkOff + seqRefsOff) + (size_t)seqIndex * 128;
    if (seqRefEntry + 68 > d.size()) return false;
    int32_t seqDataOff = readBE32s(d.data(), seqRefEntry + 64);

    // sequence_data: 64 bytes, number_of_views at +8, frames_per_view at +10
    size_t seqDataAbs = (size_t)(bulkOff + seqDataOff);
    if (seqDataAbs + 64 > d.size()) return false;
    int16_t numViews   = readBE16s(d.data(), seqDataAbs + 8);
    if (numViews < 1) numViews = 1;
    if (viewIndex < 0 || viewIndex >= numViews) return false;

    // sequence_frame_data[0]: 46 bytes immediately after sequence_data,
    // followed by bitmap_instance_indexes[numViews] (2 bytes each)
    size_t biiBase = seqDataAbs + 64 + 46;
    if (biiBase + (size_t)numViews * 2 > d.size()) return false;
    int16_t bii = readBE16s(d.data(), biiBase + (size_t)viewIndex * 2);

    if (bii < 0 || bii >= bitmapInstCount) return false;

    // bitmap_instance_data[bii]: 64 bytes, bitmap_index at +28
    size_t biInstEntry = (size_t)(bulkOff + bitmapInstsOff) + (size_t)bii * 64;
    if (biInstEntry + 30 > d.size()) return false;
    int16_t bi = readBE16s(d.data(), biInstEntry + 28);

    if (bi < 0 || bi >= bitmapCount) return false;

    // bitmap_reference[bi]: 128 bytes, data offset at +64, width at +76, height at +78
    size_t bitmapRefEntry = (size_t)(bulkOff + bitmapRefsOff) + (size_t)bi * 128;
    if (bitmapRefEntry + 128 > d.size()) return false;
    int32_t imgDataOff = readBE32s(d.data(), bitmapRefEntry + 64);
    int32_t imgDataLen = readBE32s(d.data(), bitmapRefEntry + 68);
    int w = (int)readBE16s(d.data(), bitmapRefEntry + 76);
    int h = (int)readBE16s(d.data(), bitmapRefEntry + 78);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;

    size_t imgAbs = (size_t)(bulkOff + imgDataOff);
    if (imgDataLen <= 48 || imgAbs + (size_t)imgDataLen > d.size()) return false;
    uint32_t collectionUserData = readBE32u(d.data(), 8);
    uint16_t bitmapFlags = (uint16_t)readBE16s(d.data(), imgAbs + 6);
    constexpr uint32_t COLLECTION_IS_COLOR_MAP = 0x00000100u;
    constexpr uint16_t BITMAP_TRANSPARENCY_ENCODED_1BIT = 0x0004;
    constexpr uint16_t BITMAP_TRANSPARENCY_ENCODED_4BIT = 0x0010;
    constexpr uint16_t BITMAP_NO_ROW_ADDRESS_TABLE = 0x0020;
    constexpr uint16_t BITMAP_WORD_ALIGNED_ENCODING = 0x0040;
    constexpr uint16_t BITMAP_IS_OVERLAY = 0x0800;
    bool isColorMap = (collectionUserData & COLLECTION_IS_COLOR_MAP) != 0;
    bool isCompressed = (bitmapFlags & BITMAP_TRANSPARENCY_ENCODED_1BIT) != 0;
    bool has4BitAlpha = (bitmapFlags & BITMAP_TRANSPARENCY_ENCODED_4BIT) != 0;
    bool hasRowAddressTable = (bitmapFlags & BITMAP_NO_ROW_ADDRESS_TABLE) == 0;
    bool wordAlignedRows = (bitmapFlags & BITMAP_WORD_ALIGNED_ENCODING) != 0;
    bool isOverlay = (bitmapFlags & BITMAP_IS_OVERLAY) != 0;

    // Decode indexed pixels to RGBA using Myth palette (rgb_color: word r, g, b, flags each).
    std::vector<uint8_t> indexes((size_t)w * h, 0);
    std::vector<uint8_t> alpha((size_t)w * h, 0);
    auto alphaForIndex = [&](uint8_t idx) -> uint8_t {
        const uint8_t* c = palData + (size_t)idx * 8;
        if (isOverlay) return std::max(c[0], std::max(c[2], c[4]));
        return (idx > 0 || isColorMap) ? 255 : 0;
    };
    auto decodeAlpha4 = [](uint8_t a4) -> uint8_t {
        return (uint8_t)((15 - (a4 & 15)) * 17);
    };

    size_t bitmapDataStart = imgAbs + 52;
    if (hasRowAddressTable) bitmapDataStart += (size_t)std::max(0, h - 1) * 4;
    size_t bitmapDataEnd = imgAbs + (size_t)imgDataLen;
    if (bitmapDataStart > bitmapDataEnd) return false;
    const uint8_t* bitmapData = d.data() + bitmapDataStart;
    size_t bitmapDataLen = bitmapDataEnd - bitmapDataStart;

    if (isCompressed) {
        size_t cursor = 0;
        for (int y = 0; y < h; y++) {
            if (cursor + 4 > bitmapDataLen) return false;
            int spanCount = (int)readBE16s(bitmapData, cursor + 0);
            int pixelCount = (int)readBE16s(bitmapData, cursor + 2);
            if (spanCount < 0 || spanCount > w || pixelCount < 0 || pixelCount > w) return false;
            size_t spanTable = cursor + 4;
            size_t pixelStart = spanTable + (size_t)spanCount * 4;
            size_t pixelUnit = has4BitAlpha ? 2 : 1;
            size_t pixelEnd = pixelStart + (size_t)pixelCount * pixelUnit;
            if (pixelEnd > bitmapDataLen) return false;

            int col = 0;
            int pi = 0;
            for (int si = 0; si < spanCount; si++) {
                int x0 = (int)readBE16s(bitmapData, spanTable + (size_t)si * 4);
                int x1 = (int)readBE16s(bitmapData, spanTable + (size_t)si * 4 + 2);
                if (x0 < col || x1 < x0 || x1 > w) return false;
                for (int x = x0; x < x1; x++) {
                    size_t dst = (size_t)y * w + x;
                    if (has4BitAlpha) {
                        uint8_t a4 = bitmapData[pixelStart + (size_t)pi * 2 + 0];
                        uint8_t idx = bitmapData[pixelStart + (size_t)pi * 2 + 1];
                        indexes[dst] = idx;
                        alpha[dst] = decodeAlpha4(a4);
                    } else {
                        uint8_t idx = bitmapData[pixelStart + (size_t)pi];
                        indexes[dst] = idx;
                        alpha[dst] = alphaForIndex(idx);
                    }
                    pi++;
                }
                col = x1;
            }
            cursor = pixelEnd;
            if (wordAlignedRows && (cursor & 1)) cursor++;
        }
    } else if (has4BitAlpha) {
        size_t need = (size_t)w * h * 2;
        if (need > bitmapDataLen) return false;
        for (int y = 0; y < h; y++) {
            const uint8_t* row = bitmapData + (size_t)y * w * 2;
            for (int x = 0; x < w; x++) {
                size_t dst = (size_t)y * w + x;
                uint8_t a4 = row[(size_t)x * 2 + 0];
                indexes[dst] = row[(size_t)x * 2 + 1];
                alpha[dst] = decodeAlpha4(a4);
            }
        }
    } else {
        size_t need = (size_t)w * h;
        if (need > bitmapDataLen) return false;
        memcpy(indexes.data(), bitmapData, need);
        for (int i = 0; i < w * h; i++) {
            alpha[(size_t)i] = alphaForIndex(indexes[(size_t)i]);
        }
    }

    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        uint8_t idx = indexes[(size_t)i];
        const uint8_t* c = palData + (size_t)idx * 8;
        uint8_t a = alpha[(size_t)i];
        uint8_t r = a ? c[0] : 0;
        uint8_t g = a ? c[2] : 0;
        uint8_t b = a ? c[4] : 0;
        if (chromaBlueTransparent && r == 0 && g == 0 && b == 255) {
            a = 0;
            r = g = b = 0;
        } else if (forceOpaqueNonBlue && a != 0) {
            a = 255;
        }
        rgba[(size_t)i*4+0] = r;
        rgba[(size_t)i*4+1] = g;
        rgba[(size_t)i*4+2] = b;
        rgba[(size_t)i*4+3] = a;
    }

    if (outW) *outW = w;
    if (outH) *outH = h;
    return writePNG(outPng, rgba, w, h);
}

// ---------------------------------------------------------------------------
// Tag catalog
// ---------------------------------------------------------------------------

struct TagEntry {
    std::string sourceFile;
    std::string name;
    uint32_t groupTag=0, subgroupTag=0;
    uint32_t offset=0, size=0;
};

static bool readTagData(const TagEntry& e, std::vector<uint8_t>& out);

struct FileHeader {
    uint16_t entryPointCount=0;
    uint16_t tagCount=0;
    bool isLocal=false;
};

static bool readFileHeader(FILE* f, FileHeader& h) {
    uint8_t hdr[128];
    rewind(f);
    if (fread(hdr,1,128,f)!=128) return false;
    h.entryPointCount=(uint16_t)((hdr[100]<<8)|hdr[101]);
    h.tagCount=(uint16_t)((hdr[102]<<8)|hdr[103]);
    uint32_t sig=readBE32u(hdr,124);
    if (sig==0x646E6732u) { h.isLocal=false; return true; }  // dng2
    if (readBE32u(hdr,60)==0x6D746832u) {                     // mth2 local
        h.isLocal=true; h.entryPointCount=0; h.tagCount=1; return true;
    }
    return false;
}

static void scanTagFile(const std::string& path, std::vector<TagEntry>& out) {
    FILE* f=fopen(path.c_str(),"rb");
    if (!f) return;
    FileHeader h;
    if (!readFileHeader(f,h)) { fclose(f); return; }
    long fileLen=0;
    fseek(f,0,SEEK_END); fileLen=ftell(f);
    if (h.isLocal) rewind(f);
    else fseek(f, 128L+(long)h.entryPointCount*112L, SEEK_SET);
    for (uint16_t i=0; i<h.tagCount; i++) {
        uint8_t th[64];
        if (fread(th,1,64,f)!=64) break;
        TagEntry e;
        e.sourceFile=path;
        e.name=std::string((const char*)th+4, strnlen((const char*)th+4,32));
        e.groupTag=readBE32u(th,36);
        e.subgroupTag=readBE32u(th,40);
        e.offset=readBE32u(th,44);
        e.size=readBE32u(th,48);
        if (h.isLocal) e.size=(uint32_t)std::max(0L,fileLen-64L);
        out.push_back(e);
    }
    fclose(f);
}

static const TagEntry* findTag(const std::vector<TagEntry>& tags, uint32_t group, uint32_t subgroup) {
    for (const auto& e: tags)
        if (e.groupTag==group && e.subgroupTag==subgroup) return &e;
    return nullptr;
}

static std::string normalizedTextureName(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    auto trim = [](std::string& v) {
        while (!v.empty() && std::isspace((unsigned char)v.front()))
            v.erase(v.begin());
        while (!v.empty() && std::isspace((unsigned char)v.back()))
            v.pop_back();
    };
    trim(s);
    for (const char* suffix : {" textures", " texture"}) {
        size_t n = strlen(suffix);
        if (s.size() >= n && s.compare(s.size() - n, n, suffix) == 0) {
            s.resize(s.size() - n);
            trim(s);
            break;
        }
    }
    return s;
}

static const TagEntry* findTextureCollection(const std::vector<TagEntry>& tags,
                                             uint32_t collectionRefTag) {
    static const uint32_t GROUP_CORE = 0x636F7265u; // 'core'
    static const uint32_t GROUP_256  = 0x2E323536u; // '.256'

    const TagEntry* coreEntry = findTag(tags, GROUP_CORE, collectionRefTag);
    if (coreEntry) {
        std::vector<uint8_t> coreData;
        if (readTagData(*coreEntry, coreData) && coreData.size() >= 4) {
            uint32_t collectionTag = readBE32u(coreData.data(), 0);
            const TagEntry* direct = findTag(tags, GROUP_256, collectionTag);
            if (direct) return direct;
        }

        std::string wanted = normalizedTextureName(coreEntry->name);
        if (!wanted.empty()) {
            for (const auto& e: tags) {
                if (e.groupTag == GROUP_256 &&
                    normalizedTextureName(e.name) == wanted) {
                    return &e;
                }
            }
        }
    }

    return findTag(tags, GROUP_256, collectionRefTag);
}

static bool readTagData(const TagEntry& e, std::vector<uint8_t>& out) {
    FILE* f=fopen(e.sourceFile.c_str(),"rb");
    if (!f) return false;
    if (fseek(f,(long)e.offset,SEEK_SET)!=0) { fclose(f); return false; }
    out.resize(e.size);
    bool ok=(e.size==0)||(fread(out.data(),1,e.size,f)==e.size);
    fclose(f); return ok;
}

// ---------------------------------------------------------------------------
// Mesh header parsing
// ---------------------------------------------------------------------------

struct MeshHeader {
    uint16_t submeshW=0, submeshH=0;
    uint32_t unitTypeCount=0, unitTypeOffset=0;
    uint32_t instanceCount=0, instanceOffset=0;
    uint32_t connectorTag=0;
    uint32_t connectorCount=0, connectorOffset=0, connectorSize=0;
};

static bool parseMeshHeader(const std::vector<uint8_t>& d, MeshHeader& h) {
    if (d.size()<1024) return false;
    h.submeshW    = readBE16u(d.data(), 8);
    h.submeshH    = readBE16u(d.data(), 10);
    h.unitTypeCount   = readBE32u(d.data(), 0x24);
    h.unitTypeOffset  = readBE32u(d.data(), 0x28);
    h.instanceCount   = readBE32u(d.data(), 0x34);
    h.instanceOffset  = readBE32u(d.data(), 0x38);
    h.connectorTag    = readBE32u(d.data(), 72);
    h.connectorCount  = readBE32u(d.data(), 0x11C);
    h.connectorOffset = readBE32u(d.data(), 0x120);
    h.connectorSize   = readBE32u(d.data(), 0x124);
    return h.submeshW>0 && h.submeshH>0;
}

// ---------------------------------------------------------------------------
// Unit type record (32 bytes each)
// ---------------------------------------------------------------------------

struct UnitType {
    uint16_t w0=0;        // object class: 6=scenery
    uint32_t typeTag=0;   // 4-char tag id
    uint16_t instanceCount=0;
    uint16_t typeIndex=0;
};

static bool readUnitTypes(const std::vector<uint8_t>& d, const MeshHeader& h,
                          std::vector<UnitType>& out) {
    size_t base=1024+(size_t)h.unitTypeOffset;
    for (uint32_t i=0; i<h.unitTypeCount; i++) {
        size_t off=base+(size_t)i*32;
        if (off+32>d.size()) return false;
        UnitType t;
        t.w0          = readBE16u(d.data(), off+0);
        t.typeTag     = readBE32u(d.data(), off+4);
        t.instanceCount= readBE16u(d.data(), off+28);
        t.typeIndex   = readBE16u(d.data(), off+30);
        out.push_back(t);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Object instance record (64 bytes each)
// ---------------------------------------------------------------------------

// marker_data (64 bytes) — exact layout from markers.h / markers.cpp byte-swap data:
//   [0:4]   flags (uint32)
//   [4:6]   type (int16)  — marker type enum:
//             0=team, 1=scenery, 3=monster, 5=sound/effect, 6=_marker_model(placed 3D),
//             9=projectile, 10=local projectile group, 11=_marker_model_animation
//   [6:8]   palette_index (int16)  — index into marker palette table
//   [8:10]  identifier (int16)
//   [10:12] minimum_difficulty_level (int16)
//   [12:16] position.x (int32, world_distance, WORLD_FRACTIONAL_BITS=9)
//   [16:20] position.y (int32, world_distance)
//   [20:24] position.z (int32, world_distance = height above datum, matches cell.physical_height)
//   [24:30] velocity (3 × int16)
//   [30:32] height above ground (int16)
//   [32:34] yaw (uint16, 0..65535 = full circle)
//   [34:36] pitch (uint16)
//   [36:52] user_data[16]
//   [52:54] roll (uint16)
//   [54:56] unused
//   [56:60] render_chain ptr (zeroed on disk)
//   [60:62] data_index (int16)
//   [62:64] data_identifier (int16)
//
// Markers with markerType==6 (_marker_model) are direct placed 3D model instances.
// Markers with markerType==11 (_marker_model_animation) reference an anim tag whose
// frames point at mode/model tags.
struct ObjectInstance {
    int16_t  markerType=0;  // instance's own type: 6 = _marker_model (placed 3D scenery)
    uint16_t paletteIdx=0;  // index into marker palette table
    uint16_t identifier=0;
    int32_t posX=0, posY=0, posZ=0;  // world_distance, /512 gives cell coordinate
    uint16_t yaw=0;  // 0..65535 = 0..360 degrees
    uint16_t pitch=0;
    uint16_t roll=0;
    uint8_t userData[16]={};  // marker-type-specific data at [36:52]
    uint8_t permutationIndex=0;  // user_data[1]: 0-based permutation/variant index
};

static bool readInstances(const std::vector<uint8_t>& d, const MeshHeader& h,
                          std::vector<ObjectInstance>& out) {
    size_t base=1024+(size_t)h.instanceOffset;
    for (uint32_t i=0; i<h.instanceCount; i++) {
        size_t off=base+(size_t)i*64;
        if (off+64>d.size()) return false;
        ObjectInstance inst;
        inst.markerType = readBE16s(d.data(), off+4);   // marker type
        inst.paletteIdx = readBE16u(d.data(), off+6);   // palette_index
        inst.identifier = readBE16u(d.data(), off+8);    // marker identifier
        inst.posX       = readBE32s(d.data(), off+12);  // position.x
        inst.posY       = readBE32s(d.data(), off+16);  // position.y
        inst.posZ       = readBE32s(d.data(), off+20);  // position.z (height)
        inst.yaw        = readBE16u(d.data(), off+32);  // yaw angle
        inst.pitch      = readBE16u(d.data(), off+34);  // pitch angle / marker-type state
        inst.roll       = readBE16u(d.data(), off+52);  // roll angle
        memcpy(inst.userData, d.data()+off+36, 16);     // user_data[16]
        inst.permutationIndex = inst.userData[1];        // 0-based permutation index
        out.push_back(inst);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Geometry tag parsing (geom, group 0x67656F6D)
// ---------------------------------------------------------------------------
// geometry_definition header is 128 bytes.
// All offset fields within it are relative to data_off (which equals 128).
// So absolute offset into geom data = 128 + field_value.

struct GeomVertex {
    float x, y, z;
};

struct GeomCorner {
    float u, v;
    int16_t vertexIndex;
};

struct GeomSurface {
    int16_t materialIndex;
    GeomCorner corners[3];
};

struct GeomMaterial {
    std::string name;
    int16_t sequenceIndex = 0;  // index into .256 collection sectioninfo array
    std::string texturePng;     // path to extracted PNG (filled after extraction)
};

struct Geometry {
    uint32_t collectionRefTag=0;  // .256 texture collection tag
    float cx=0, cy=0, cz=0;      // center (local origin) from geom header [16:22]
    std::vector<GeomMaterial> materials;
    std::vector<GeomVertex> vertices;
    std::vector<GeomSurface> surfaces;
};

struct ModelOrigin {
    float x=0, y=0, z=0;
};

struct AnimationFrame {
    uint32_t modelTag = 0;
    int16_t permutationIndex = 0;
};

struct AnimationDef {
    uint32_t animTag = 0;
    int16_t ticksPerFrame = 0;
    std::vector<AnimationFrame> frames;
};

static bool parseAnimation(const std::vector<uint8_t>& d, uint32_t animTag, AnimationDef& a) {
    if (d.size() < 1024) return false;
    int16_t frameCount = readBE16s(d.data(), 4);
    if (frameCount < 0 || frameCount > 31) return false;
    if (8 + (size_t)frameCount * 16 > d.size()) return false;

    a.animTag = animTag;
    a.ticksPerFrame = readBE16s(d.data(), 6);
    a.frames.clear();
    a.frames.reserve(frameCount);
    for (int i = 0; i < frameCount; i++) {
        size_t o = 8 + (size_t)i * 16;
        AnimationFrame f;
        f.modelTag = readBE32u(d.data(), o + 4);
        f.permutationIndex = readBE16s(d.data(), o + 10);
        a.frames.push_back(f);
    }
    return true;
}

static bool parseGeometry(const std::vector<uint8_t>& d, Geometry& g) {
    if (d.size()<128) return false;
    const uint8_t* hdr=d.data();

    g.collectionRefTag = readBE32u(hdr, 4);
    int matCount   = (int)readBE16u(hdr, 8);
    int vtxCount   = (int)readBE16u(hdr, 10);
    int srfCount   = (int)readBE16u(hdr, 12);
    // center: short_world_point3d at [16:22] — local origin in geometry space
    g.cx = (float)readBE16s(hdr, 16);
    g.cy = (float)readBE16s(hdr, 18);
    g.cz = (float)readBE16s(hdr, 20);

    uint32_t dataOff      = 128;
    uint32_t matRelOff    = readBE32u(hdr, 28);
    uint32_t vtxRelOff    = readBE32u(hdr, 40);
    uint32_t srfRelOff    = readBE32u(hdr, 52);

    size_t matAbs = dataOff + matRelOff;
    size_t vtxAbs = dataOff + vtxRelOff;
    size_t srfAbs = dataOff + srfRelOff;

    // Materials: 64 bytes each, name at [0:32], sequence_index at [32:34]
    if (matAbs + (size_t)matCount*64 > d.size()) return false;
    g.materials.resize(matCount);
    for (int i=0; i<matCount; i++) {
        size_t o=matAbs+(size_t)i*64;
        g.materials[i].name=std::string((const char*)d.data()+o,
                                        strnlen((const char*)d.data()+o,32));
        g.materials[i].sequenceIndex = readBE16s(d.data(), o+32);
    }

    // Vertices: 6 bytes each — x,y,z as int16
    if (vtxAbs + (size_t)vtxCount*6 > d.size()) return false;
    g.vertices.resize(vtxCount);
    for (int i=0; i<vtxCount; i++) {
        size_t o=vtxAbs+(size_t)i*6;
        g.vertices[i].x = (float)readBE16s(d.data(), o+0);
        g.vertices[i].y = (float)readBE16s(d.data(), o+2);
        g.vertices[i].z = (float)readBE16s(d.data(), o+4);
    }

    // Surfaces: 64 bytes each
    // [0:2]   flags
    // [2:8]   normal (3 × int16)
    // [8:12]  d (int32, plane equation distance)
    // [12:24] bounds (short_world_rectangle3d: 6 × int16)
    // [24:26] transparency (uint16)
    // [26:28] material_index (int16)
    // [28:52] corners[3], each 8 bytes:
    //           [+0] u (uint16, /65536 = float UV)
    //           [+2] v (uint16, /65536 = float UV)
    //           [+4] vertex_index (int16)
    //           [+6] encoded_normal (uint16)
    // [52:54] next_neg
    // [54:56] next_par
    // [56:58] next_pos
    if (srfAbs + (size_t)srfCount*64 > d.size()) return false;
    g.surfaces.reserve(srfCount);
    for (int i=0; i<srfCount; i++) {
        size_t o=srfAbs+(size_t)i*64;
        GeomSurface s;
        s.materialIndex = readBE16s(d.data(), o+26);
        for (int c=0; c<3; c++) {
            size_t co=o+28+(size_t)c*8;
            float u = (float)readBE16u(d.data(), co+0) / 65536.0f;
            float v = (float)readBE16u(d.data(), co+2) / 65536.0f;
            int16_t vi = readBE16s(d.data(), co+4);
            s.corners[c].u = u;
            s.corners[c].v = v;
            s.corners[c].vertexIndex = vi;
        }
        g.surfaces.push_back(s);
    }

    return true;
}

// WORLD_ONE: raw vertex units per mesh cell (WORLD_FRACTIONAL_BITS=9 → 1<<9=512).
static constexpr float WORLD_ONE = 512.0f;

struct WorldTransform {
    float cellX, cellY, cellZ;
    float facingRad;
    float halfW, halfH;
};

// ---------------------------------------------------------------------------
// OBJ / MTL export
// ---------------------------------------------------------------------------

static bool exportOBJ(const std::string& objPath, const std::string& mtlPath,
                      const std::string& typeTag, const Geometry& g,
                      const WorldTransform* wt = nullptr,
                      const std::vector<bool>* hiddenMaterials = nullptr,
                      const ModelOrigin* originOverride = nullptr) {
    // Only export surfaces with all valid vertex indices
    // Collect valid triangles
    struct Tri {
        int16_t mat;
        int16_t vi[3];
        float u[3], v[3];
    };
    std::vector<Tri> tris;
    for (const auto& s: g.surfaces) {
        if (hiddenMaterials &&
            s.materialIndex >= 0 &&
            s.materialIndex < (int)hiddenMaterials->size() &&
            (*hiddenMaterials)[s.materialIndex]) {
            continue;
        }
        bool valid=true;
        for (int c=0; c<3; c++) {
            int16_t vi=s.corners[c].vertexIndex;
            if (vi<0 || vi>=(int16_t)g.vertices.size()) { valid=false; break; }
        }
        if (!valid) continue;
        Tri t;
        t.mat=s.materialIndex;
        for (int c=0; c<3; c++) {
            t.vi[c]=s.corners[c].vertexIndex;
            t.u[c]=s.corners[c].u;
            t.v[c]=s.corners[c].v;
        }
        tris.push_back(t);
    }

    if (tris.empty()) {
        fprintf(stderr,"  No valid triangles for %s\n", typeTag.c_str());
        return false;
    }

    // Write MTL
    {
        std::string mtl;
        for (int i=0; i<(int)g.materials.size(); i++) {
            if (hiddenMaterials &&
                i < (int)hiddenMaterials->size() &&
                (*hiddenMaterials)[i]) {
                continue;
            }
            const auto& mat = g.materials[i];
            mtl += "newmtl ";
            mtl += (mat.name.empty() ? "mat"+std::to_string(i) : mat.name);
            mtl += "\n";
            mtl += "Ka 1.0 1.0 1.0\n";
            mtl += "Kd 1.0 1.0 1.0\n";
            mtl += "Ks 0.0 0.0 0.0\n";
            mtl += "illum 1\n";
            if (!mat.texturePng.empty()) {
                mtl += "map_Kd textures/";
                mtl += fs::path(mat.texturePng).filename().string();
                mtl += "\n";
            }
            mtl += "\n";
        }
        writeText(mtlPath, mtl);
    }

    // Write OBJ
    {
        std::string obj;
        obj += "# Myth II scenery: ";
        obj += typeTag;
        obj += "\nmtllib ";
        obj += fs::path(mtlPath).filename().string();
        obj += "\n\n";

        ModelOrigin origin = originOverride ? *originOverride : ModelOrigin{g.cx, g.cy, g.cz};
        for (const auto& vtx: g.vertices) {
            float mx = -((vtx.x - origin.x) / WORLD_ONE);
            float my =   (vtx.y - origin.y) / WORLD_ONE;
            float mz =   (vtx.z - origin.z) / WORLD_ONE;
            float ox, oy, oz;
            if (wt) {
                float cosF = std::cos(wt->facingRad);
                float sinF = std::sin(wt->facingRad);
                float rx =  cosF * mx - sinF * my;
                float ry =  sinF * mx + cosF * my;
                ox = rx + (wt->halfW - wt->cellX);
                oy = ry + (wt->cellY - wt->halfH);
                oz = mz + wt->cellZ;
            } else {
                ox = mx; oy = my; oz = mz;
            }
            char buf[80];
            snprintf(buf, sizeof(buf), "v %.6f %.6f %.6f\n", ox, oz, oy);
            obj += buf;
        }
        obj += "\n";

        // UV coordinates: OBJ V is flipped relative to Myth II (Myth origin is top-left)
        for (const auto& tri: tris) {
            for (int c=0; c<3; c++) {
                char buf[80];
                snprintf(buf,sizeof(buf),"vt %.6f %.6f\n", tri.v[c], 1.0f - tri.u[c]);
                obj += buf;
            }
        }
        obj += "\n";

        // Group by material
        std::string lastMat;
        int uvIdx=1;
        for (const auto& tri: tris) {
            std::string matName;
            if (tri.mat>=0 && tri.mat<(int)g.materials.size())
                matName = g.materials[tri.mat].name.empty()
                        ? "mat"+std::to_string(tri.mat)
                        : g.materials[tri.mat].name;
            else matName = "unknown";

            if (matName != lastMat) {
                obj += "usemtl "; obj += matName; obj += "\n";
                lastMat = matName;
            }

            // OBJ face: vertex_index/uv_index (1-based)
            // Flip winding: Myth uses clockwise, OBJ default is counter-clockwise
            char buf[120];
            snprintf(buf,sizeof(buf),"f %d/%d %d/%d %d/%d\n",
                     (int)tri.vi[2]+1, uvIdx+2,
                     (int)tri.vi[1]+1, uvIdx+1,
                     (int)tri.vi[0]+1, uvIdx+0);
            obj += buf;
            uvIdx += 3;
        }

        writeText(objPath, obj);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Combined map OBJ export
// ---------------------------------------------------------------------------

static constexpr double PI = 3.14159265358979323846;

enum class AnimationFrameMode {
    First,
    None,
    All
};

struct PlacedInstance {
    const Geometry* geom;
    std::string typeTag;
    ModelOrigin origin;
    float cellX, cellY, cellZ;
    float facingRad;  // rotation around up axis
    float halfW, halfH; // half grid dimensions, for terrain-matching coordinate transform
    std::vector<std::string> materialTexturePngs; // per-material texture paths (permutation-specific)
    std::vector<bool> hiddenMaterials; // per-material 0xFF permutation slots
};

// Append a terrain OBJ (displacement.obj) into the combined OBJ string,
// re-indexing all face indices by the current vBase/vtBase/vnBase offsets.
static void appendTerrainOBJ(const std::string& terrainObjPath,
                              std::string& obj,
                              int vBase, int vtBase, int vnBase) {
    FILE* f = fopen(terrainObjPath.c_str(), "r");
    if (!f) {
        fprintf(stderr, "Warning: could not open terrain OBJ: %s\n", terrainObjPath.c_str());
        return;
    }

    // Collect mtllib reference from terrain file to include in combined header
    // (already handled by caller; here we just emit geometry)
    obj += "o terrain\ng terrain\n";

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        // Strip trailing newline
        while (!s.empty() && (s.back()=='\n'||s.back()=='\r')) s.pop_back();

        if (s.substr(0,2)=="v " || s.substr(0,3)=="vt " || s.substr(0,3)=="vn ") {
            obj += s; obj += '\n';
        } else if (s.substr(0,7)=="usemtl ") {
            obj += s; obj += '\n';
        } else if (s.substr(0,2)=="f ") {
            // Re-index: each token after "f " is v/vt/vn or v//vn or v
            obj += "f";
            const char* p = s.c_str() + 2;
            while (*p) {
                while (*p==' ') ++p;
                if (!*p) break;
                // parse v[/vt[/vn]]
                char* end;
                long v = strtol(p, &end, 10);  p = end;
                long vt = 0, vn = 0;
                if (*p=='/') {
                    ++p;
                    if (*p!='/') { vt = strtol(p, &end, 10); p = end; }
                    if (*p=='/') { ++p; vn = strtol(p, &end, 10); p = end; }
                }
                char tok[64];
                if (vn && vt)
                    snprintf(tok,sizeof(tok)," %ld/%ld/%ld", v+vBase-1, vt+vtBase-1, vn+vnBase-1);
                else if (vt)
                    snprintf(tok,sizeof(tok)," %ld/%ld", v+vBase-1, vt+vtBase-1);
                else if (vn)
                    snprintf(tok,sizeof(tok)," %ld//%ld", v+vBase-1, vn+vnBase-1);
                else
                    snprintf(tok,sizeof(tok)," %ld", v+vBase-1);
                obj += tok;
            }
            obj += '\n';
        }
        // skip comments, mtllib, o, g lines from terrain file
    }
    fclose(f);
    obj += '\n';
}

static bool exportCombinedOBJ(const std::string& objPath,
                               const std::string& mtlPath,
                               const std::vector<PlacedInstance>& instances,
                               const std::string& terrainObjPath = "",
                               const std::string& texturePrefix = "textures/") {
    // MTL: one entry per instance+material combination (typeTag_instIdx_matname).
    // Using per-instance names avoids any cross-instance material sharing in Blender.
    std::string mtl;
    for (size_t ii = 0; ii < instances.size(); ii++) {
        const PlacedInstance& inst = instances[ii];
        for (size_t mi = 0; mi < inst.geom->materials.size(); mi++) {
            if (mi < inst.hiddenMaterials.size() && inst.hiddenMaterials[mi])
                continue;
            const auto& mat = inst.geom->materials[mi];
            std::string qname = inst.typeTag + "_" + std::to_string(ii) + "_" +
                                (mat.name.empty() ? "mat" + std::to_string(mi) : mat.name);
            mtl += "newmtl " + qname + "\n";
            mtl += "Ka 1.0 1.0 1.0\nKd 1.0 1.0 1.0\nKs 0.0 0.0 0.0\nillum 1\n";
            std::string tex;
            if (mi < inst.materialTexturePngs.size() && !inst.materialTexturePngs[mi].empty())
                tex = inst.materialTexturePngs[mi];
            else if (!mat.texturePng.empty())
                tex = mat.texturePng;
            if (!tex.empty()) {
                mtl += "map_Kd " + texturePrefix;
                mtl += fs::path(tex).filename().string();
                mtl += "\n";
            }
            mtl += "\n";
        }
    }
    writeText(mtlPath, mtl);

    std::string obj;
    obj += "# Myth II combined map scenery + terrain\nmtllib ";
    obj += fs::path(mtlPath).filename().string();
    if (!terrainObjPath.empty()) {
        // Also reference the terrain MTL (assumed to be displacement.mtl alongside displacement.obj)
        fs::path tp(terrainObjPath);
        std::string terrainMtl = tp.stem().string() + ".mtl";
        obj += "\nmtllib " + terrainMtl;
    }
    obj += "\n\n";

    int vBase = 1;  // 1-based OBJ vertex counter
    int uvBase = 1;

    for (size_t instIdx = 0; instIdx < instances.size(); instIdx++) {
        const PlacedInstance& inst = instances[instIdx];
        const Geometry& g = *inst.geom;

        // Collect valid triangles
        struct Tri { int16_t mat; int16_t vi[3]; float u[3], v[3]; };
        std::vector<Tri> tris;
        for (const auto& s: g.surfaces) {
            if (s.materialIndex >= 0 &&
                s.materialIndex < (int)inst.hiddenMaterials.size() &&
                inst.hiddenMaterials[s.materialIndex]) {
                continue;
            }
            bool valid = true;
            for (int c = 0; c < 3; c++) {
                int16_t vi = s.corners[c].vertexIndex;
                if (vi < 0 || vi >= (int16_t)g.vertices.size()) { valid = false; break; }
            }
            if (!valid) continue;
            Tri t; t.mat = s.materialIndex;
            for (int c = 0; c < 3; c++) {
                t.vi[c] = s.corners[c].vertexIndex;
                t.u[c]  = s.corners[c].u;
                t.v[c]  = s.corners[c].v;
            }
            tris.push_back(t);
        }
        if (tris.empty()) continue;

        // Transform vertices: model space -> world cell space
        // Model: X right, Y forward (into screen), Z up (raw world units)
        // OBJ:   X right, Y up, Z toward viewer (Blender Z-up import)
        // Placement: cellX=right, cellY=into screen, cellZ=up (same axes as model)
        // facing rotates around Z (up) axis in model space -> around Y in OBJ space
        float cosF = std::cos(inst.facingRad);
        float sinF = std::sin(inst.facingRad);

        char buf[128];
        // Named object + group per instance so Blender keeps each mesh separate
        std::string instName = inst.typeTag + "_" + std::to_string(instIdx);
        obj += "o " + instName + "\ng " + instName + "\n";
        for (const auto& vtx: g.vertices) {
            // Scale from world units to cell units
            // Subtract center to get vertex relative to local origin, then mirror X
            float mx = -((vtx.x - inst.origin.x) / WORLD_ONE);
            float my = (vtx.y - inst.origin.y) / WORLD_ONE;
            float mz = (vtx.z - inst.origin.z) / WORLD_ONE;
            // Rotate around Z (up) by facing angle
            float rx =  cosF * mx - sinF * my;
            float ry =  sinF * mx + cosF * my;
            float rz = mz;
            // Translate to placement position.
            // Terrain mesh uses vx = halfW - cellX, vz = cellY - halfH (see mesh.cpp).
            // Match that convention so models align with the terrain.
            float wx = rx + (inst.halfW - inst.cellX);
            float wy = ry + (inst.cellY - inst.halfH);
            float wz = rz + inst.cellZ;
            // Match terrain OBJ convention: X=halfW-cellX, Z=cellY-halfH, Y=height
            snprintf(buf, sizeof(buf), "v %.6f %.6f %.6f\n", wx, wz, wy);
            obj += buf;
        }

        for (const auto& tri: tris) {
            for (int c = 0; c < 3; c++) {
                snprintf(buf, sizeof(buf), "vt %.6f %.6f\n", tri.v[c], 1.0f - tri.u[c]);
                obj += buf;
            }
        }

        std::string lastMat;
        int uvIdx = uvBase;
        for (const auto& tri: tris) {
            std::string matName;
            if (tri.mat >= 0 && tri.mat < (int)g.materials.size())
                matName = inst.typeTag + "_" + std::to_string(instIdx) + "_" +
                          (g.materials[tri.mat].name.empty()
                           ? "mat" + std::to_string(tri.mat)
                           : g.materials[tri.mat].name);
            else matName = inst.typeTag + "_" + std::to_string(instIdx) + "_unknown";

            if (matName != lastMat) {
                obj += "usemtl " + matName + "\n";
                lastMat = matName;
            }
            snprintf(buf, sizeof(buf), "f %d/%d %d/%d %d/%d\n",
                     vBase + (int)tri.vi[2], uvIdx+2,
                     vBase + (int)tri.vi[1], uvIdx+1,
                     vBase + (int)tri.vi[0], uvIdx+0);
            obj += buf;
            uvIdx += 3;
        }

        vBase  += (int)g.vertices.size();
        uvBase += (int)tris.size() * 3;
        obj += "\n";
    }

    if (!terrainObjPath.empty())
        appendTerrainOBJ(terrainObjPath, obj, vBase, uvBase, 1);

    return writeText(objPath, obj);
}

// ---------------------------------------------------------------------------
// JSON placement output
// ---------------------------------------------------------------------------

static void appendJsonString(std::string& s, const std::string& val) {
    s += '"';
    for (char c: val) {
        if (c=='"') s += "\\\"";
        else if (c=='\\') s += "\\\\";
        else s += c;
    }
    s += '"';
}

static std::string lowerString(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return s;
}

static std::string safeStem(std::string s) {
    for (char& c : s) {
        unsigned char ch = (unsigned char)c;
        if (ch < 32 || ch > 126 ||
            c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            c = '_';
        }
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '.'))
        s.back() = '_';
    return s.empty() ? "sound" : s;
}

static void putLE16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back((uint8_t)(v & 0xFF));
    out.push_back((uint8_t)((v >> 8) & 0xFF));
}

static void putLE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back((uint8_t)(v & 0xFF));
    out.push_back((uint8_t)((v >> 8) & 0xFF));
    out.push_back((uint8_t)((v >> 16) & 0xFF));
    out.push_back((uint8_t)((v >> 24) & 0xFF));
}

static bool writeBinary(const std::string& path, const std::vector<uint8_t>& data) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { fprintf(stderr, "Cannot write: %s\n", path.c_str()); return false; }
    bool ok = data.empty() || fwrite(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    return ok;
}

static bool writeWavPCM16(const std::string& path,
                          const std::vector<int16_t>& samples,
                          uint16_t channels,
                          uint32_t sampleRate) {
    if (channels == 0 || sampleRate == 0) return false;
    uint32_t dataSize = (uint32_t)(samples.size() * sizeof(int16_t));
    uint32_t byteRate = sampleRate * channels * 2;
    uint16_t blockAlign = (uint16_t)(channels * 2);

    std::vector<uint8_t> out;
    out.reserve(44 + dataSize);
    out.insert(out.end(), {'R','I','F','F'});
    putLE32(out, 36 + dataSize);
    out.insert(out.end(), {'W','A','V','E','f','m','t',' '});
    putLE32(out, 16);
    putLE16(out, 1);
    putLE16(out, channels);
    putLE32(out, sampleRate);
    putLE32(out, byteRate);
    putLE16(out, blockAlign);
    putLE16(out, 16);
    out.insert(out.end(), {'d','a','t','a'});
    putLE32(out, dataSize);
    for (int16_t s : samples)
        putLE16(out, (uint16_t)s);
    return writeBinary(path, out);
}

static int clamp16(int v) {
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return v;
}

static void decompressAppleIMAPacket(int16_t state,
                                     const uint8_t* encoded,
                                     int16_t* dst) {
    static const int indexTab[16] = {
        -1,-1,-1,-1, 2, 4, 6, 8,
        -1,-1,-1,-1, 2, 4, 6, 8
    };
    static const int stepTab[89] = {
        7, 8, 9, 10, 11, 12, 13, 14,
        16, 17, 19, 21, 23, 25, 28,
        31, 34, 37, 41, 45, 50, 55,
        60, 66, 73, 80, 88, 97, 107,
        118, 130, 143, 157, 173, 190, 209,
        230, 253, 279, 307, 337, 371, 408,
        449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552,
        1707, 1878, 2066, 2272, 2499, 2749, 3024,
        3327, 3660, 4026, 4428, 4871, 5358, 5894,
        6484, 7132, 7845, 8630, 9493, 10442, 11487,
        12635, 13899, 15289, 16818, 18500, 20350,
        22385, 24623, 27086, 29794, 32767
    };

    int index = state & 0x7F;
    if (index < 0) index = 0;
    if (index > 88) index = 88;
    int pred = (int)state & ~0x7F;
    int step = stepTab[index];

    for (int sample = 0; sample < 64; sample++) {
        int byte = encoded[sample >> 1];
        int code = (sample & 1) ? ((byte >> 4) & 0xF) : (byte & 0xF);
        int diff = step >> 3;
        if (code & 4) diff += step;
        if (code & 2) diff += step >> 1;
        if (code & 1) diff += step >> 2;
        if (code & 8) diff = -diff;

        pred = clamp16(pred + diff);
        dst[sample] = (int16_t)pred;
        index += indexTab[code];
        if (index < 0) index = 0;
        if (index > 88) index = 88;
        step = stepTab[index];
    }
}

struct SoundPermutationExport {
    std::string name;
    std::string relativePath;
};

static bool exportSoundTagWavs(const std::vector<TagEntry>& tags,
                               uint32_t soundTag,
                               const std::string& outFolder,
                               bool overwrite,
                               std::vector<SoundPermutationExport>& outPerms) {
    static const uint32_t GROUP_SOUN = 0x736F756Eu; // 'soun'
    outPerms.clear();

    const TagEntry* e = findTag(tags, GROUP_SOUN, soundTag);
    if (!e) return false;

    std::vector<uint8_t> d;
    if (!readTagData(*e, d) || d.size() < 64) return false;

    int32_t soundOffset = readBE32s(d.data(), 20);
    int32_t numPerms = readBE32s(d.data(), 36);
    int32_t permOffset = readBE32s(d.data(), 40);
    int32_t permSize = readBE32s(d.data(), 44);
    if (numPerms <= 0 || numPerms > 5) return false;
    if (permSize != numPerms * 32) return false;
    if (permOffset < 0 || soundOffset < 0) return false;
    if ((size_t)permOffset + (size_t)numPerms * 32 > d.size()) return false;
    if ((size_t)soundOffset + (size_t)numPerms * 32 > d.size()) return false;

    makeDirs(outFolder + "/assets/sounds/wav");
    std::string tagStem = tagToFileStem(soundTag);

    size_t sampleOffset = (size_t)soundOffset + (size_t)numPerms * 32;
    for (int i = 0; i < numPerms; i++) {
        size_t permAbs = (size_t)permOffset + (size_t)i * 32;
        size_t hdrAbs = (size_t)soundOffset + (size_t)i * 32;
        std::string permName((const char*)d.data() + permAbs + 6,
                             strnlen((const char*)d.data() + permAbs + 6, 26));

        uint32_t sampleFlags = readBE32u(d.data(), hdrAbs + 0);
        uint16_t bits = readBE16u(d.data(), hdrAbs + 4);
        uint16_t physicalMinusOne = readBE16u(d.data(), hdrAbs + 6);
        uint16_t channels = readBE16u(d.data(), hdrAbs + 8);
        uint32_t sampleRate = readBE32u(d.data(), hdrAbs + 12) >> 16;
        uint32_t sampleCount = readBE32u(d.data(), hdrAbs + 16);

        size_t storedSize = 0;
        if (sampleFlags & 1)
            storedSize = (size_t)sampleCount * 34;
        else
            storedSize = (size_t)sampleCount << physicalMinusOne;
        if (sampleOffset + storedSize > d.size()) return false;

        std::string wavName = tagStem + "_" + std::to_string(i);
        std::string pretty = safeStem(permName);
        if (!pretty.empty())
            wavName += "_" + pretty;
        wavName += ".wav";
        std::string relPath = "assets/sounds/wav/" + wavName;
        std::string wavPath = outFolder + "/" + relPath;

        if (overwrite || !fs::exists(wavPath)) {
            bool ok = false;
            if ((sampleFlags & 1) && bits == 16 && channels >= 1 && channels <= 2) {
                std::vector<int16_t> pcm;
                if (channels == 1) {
                    pcm.resize((size_t)sampleCount * 64);
                    for (uint32_t p = 0; p < sampleCount; p++) {
                        size_t packet = sampleOffset + (size_t)p * 34;
                        int16_t state = readBE16s(d.data(), packet);
                        decompressAppleIMAPacket(state, d.data() + packet + 2, pcm.data() + (size_t)p * 64);
                    }
                } else {
                    uint32_t framePackets = sampleCount / 2;
                    pcm.resize((size_t)framePackets * 64 * 2);
                    int16_t left[64], right[64];
                    for (uint32_t p = 0; p < framePackets; p++) {
                        size_t lPacket = sampleOffset + (size_t)(p * 2) * 34;
                        size_t rPacket = lPacket + 34;
                        decompressAppleIMAPacket(readBE16s(d.data(), lPacket), d.data() + lPacket + 2, left);
                        decompressAppleIMAPacket(readBE16s(d.data(), rPacket), d.data() + rPacket + 2, right);
                        for (int s = 0; s < 64; s++) {
                            pcm[((size_t)p * 64 + s) * 2 + 0] = left[s];
                            pcm[((size_t)p * 64 + s) * 2 + 1] = right[s];
                        }
                    }
                }
                ok = writeWavPCM16(wavPath, pcm, channels, sampleRate);
            } else if (!(sampleFlags & 1) && bits == 16 && channels >= 1 && channels <= 2) {
                std::vector<int16_t> pcm(sampleCount);
                for (uint32_t s = 0; s < sampleCount; s++)
                    pcm[s] = readBE16s(d.data(), sampleOffset + (size_t)s * 2);
                ok = writeWavPCM16(wavPath, pcm, channels, sampleRate);
            }
            if (!ok) {
                sampleOffset += storedSize;
                continue;
            }
        }

        outPerms.push_back({permName, relPath});
        sampleOffset += storedSize;
    }

    return !outPerms.empty();
}

struct UnitSpriteDef {
    bool textured = false;
    std::string tagStr;
    std::string collectionTag;
    std::string sequenceName;
    std::vector<std::string> texturePngs;
    int sequenceIndex = 0;
    int viewCount = 1;
    int width = 32;
    int height = 48;
};

static int scoreUnitSequenceName(const std::string& name) {
    std::string n = lowerString(name);
    int score = 0;
    if (n.find("stand_all") != std::string::npos || n.find("stand all") != std::string::npos) score = 120;
    else if (n.rfind("stand1", 0) == 0 || n.rfind("stand 1", 0) == 0) score = 110;
    else if (n.rfind("stand", 0) == 0) score = 100;
    else if (n.find("idle") != std::string::npos) score = 95;
    else if (n.find("sit") != std::string::npos) score = 90;
    else if (n.find("glide") != std::string::npos) score = 85;
    else if (n.find("stand") != std::string::npos) score = 80;
    else if (n.find("hold") != std::string::npos) score = 70;
    else if (n.find("flight") != std::string::npos) score = 60;
    else score = 10;

    for (const char* bad : {"death", "dead", "body", "head", "leg", "arm",
                            "bits", "piece", "flinch", "attack", "throw",
                            "run", "walk", "transition", "trans"}) {
        if (n.find(bad) != std::string::npos) score -= 35;
    }
    return score;
}

static UnitSpriteDef resolveUnitSprite(const std::string& outFolder,
                                       const std::vector<TagEntry>& tags,
                                       uint32_t unitTag,
                                       bool overwriteTextures) {
    static const uint32_t GROUP_UNIT = 0x756E6974u; // 'unit'
    static const uint32_t GROUP_MONS = 0x6D6F6E73u; // 'mons'

    UnitSpriteDef def;
    def.tagStr = tagToString(unitTag);

    uint32_t monsterTag = unitTag;
    const TagEntry* unitEntry = findTag(tags, GROUP_UNIT, unitTag);
    if (unitEntry) {
        std::vector<uint8_t> unitData;
        if (readTagData(*unitEntry, unitData) && unitData.size() >= 4)
            monsterTag = readBE32u(unitData.data(), 0);
    }

    const TagEntry* monsEntry = findTag(tags, GROUP_MONS, monsterTag);
    if (!monsEntry) return def;

    std::vector<uint8_t> monsData;
    if (!readTagData(*monsEntry, monsData) || monsData.size() < 8) return def;

    uint32_t collectionRef = readBE32u(monsData.data(), 4);
    const TagEntry* collEntry = findTextureCollection(tags, collectionRef);
    if (!collEntry) return def;

    std::vector<uint8_t> collData;
    if (!readTagData(*collEntry, collData)) return def;

    int seqCount = dot256SequenceCount(collData);
    if (seqCount <= 0) return def;

    int bestSeq = 0;
    int bestScore = -100000;
    for (int si = 0; si < seqCount; si++) {
        int views = dot256SequenceViewCount(collData, si);
        if (views <= 0) continue;
        std::string name = dot256SequenceName(collData, si);
        int score = scoreUnitSequenceName(name);
        if (score > bestScore) {
            bestScore = score;
            bestSeq = si;
        }
    }

    int viewCount = dot256SequenceViewCount(collData, bestSeq);
    if (viewCount <= 0) return def;

    std::string texDir = outFolder + "/assets/sprites/textures";
    makeDirs(texDir);
    std::string unitStem = tagToFileStem(unitTag);
    std::string collStem = tagToFileStem(collEntry->subgroupTag);

    def.collectionTag = tagToString(collEntry->subgroupTag);
    def.sequenceName = dot256SequenceName(collData, bestSeq);
    def.sequenceIndex = bestSeq;
    def.viewCount = viewCount;
    def.texturePngs.resize((size_t)viewCount);

    bool anyViewsOk = false;
    for (int vi = 0; vi < viewCount; vi++) {
        std::string pngName = unitStem + "_unit_" + collStem + "_"
                            + std::to_string(bestSeq) + "_" + std::to_string(vi) + ".png";
        std::string pngPath = texDir + "/" + pngName;
        int w = 0, h = 0;
        if (overwriteTextures || !fs::exists(pngPath)) {
            if (!extractDot256Texture(collData, bestSeq, vi, pngPath, &w, &h)) {
                continue;
            }
        } else if (vi == 0) {
            // Keep a sane default scale when reusing an existing PNG.
            w = 32;
            h = 48;
        }
        if (!fs::exists(pngPath))
            continue;
        if (!anyViewsOk) {
            def.width = w > 0 ? w : 32;
            def.height = h > 0 ? h : 48;
        }
        def.texturePngs[(size_t)vi] = pngPath;
        anyViewsOk = true;
    }

    def.textured = anyViewsOk;
    return def;
}

static bool exportUnitPlaceholders(const std::string& outFolder,
                                   const MeshHeader& mh,
                                   const std::vector<ObjectInstance>& instances,
                                   const std::map<int16_t, std::vector<const UnitType*>>& typeRelIdx,
                                   const std::vector<TagEntry>& tags,
                                   bool overwriteTextures) {
    auto itTR = typeRelIdx.find(3);
    if (itTR == typeRelIdx.end() || itTR->second.empty())
        return false;

    const float halfW = (float)(mh.submeshW * 32) * 0.5f;
    const float halfH = (float)(mh.submeshH * 32) * 0.5f;

    std::string objPath = outFolder + "/assets/sprites/units.obj";
    std::string mtlPath = outFolder + "/assets/sprites/units.mtl";
    std::string jsonPath = outFolder + "/assets/sprites/units.json";

    std::string obj;
    obj += "# Myth II unit/monster sprites\n";
    obj += "mtllib units.mtl\n\n";

    std::string json;
    json += "{\n  \"units\": [\n";

    std::map<uint32_t, UnitSpriteDef> spriteDefs;
    for (const auto* t: itTR->second) {
        if (t->w0 != 3) continue;
        spriteDefs[t->typeTag] = resolveUnitSprite(outFolder, tags, t->typeTag, overwriteTextures);
    }

    std::string mtl;
    mtl += "newmtl unit_placeholder\n";
    mtl += "Ka 0.1 0.1 0.1\n";
    mtl += "Kd 0.9 0.1 0.1\n";
    mtl += "Ks 0.0 0.0 0.0\n";
    mtl += "illum 1\n\n";
    for (const auto& kv: spriteDefs) {
        const UnitSpriteDef& def = kv.second;
        if (!def.textured) continue;
        for (int vi = 0; vi < def.viewCount; vi++) {
            if (def.texturePngs[(size_t)vi].empty()) continue;
            std::string matName = tagToFileStem(kv.first) + "_unit_" + std::to_string(vi) + "_sprite";
            mtl += "newmtl " + matName + "\n";
            mtl += "Ka 1.0 1.0 1.0\n";
            mtl += "Kd 1.0 1.0 1.0\n";
            mtl += "Ks 0.0 0.0 0.0\n";
            mtl += "d 1.0\n";
            mtl += "illum 1\n";
            mtl += "map_Kd textures/" + fs::path(def.texturePngs[(size_t)vi]).filename().string() + "\n";
            mtl += "\n";
        }
    }
    writeText(mtlPath, mtl);

    auto findAvailableUnitView = [](const UnitSpriteDef& def, int preferred) -> int {
        if (def.viewCount <= 0) return -1;
        preferred %= def.viewCount;
        if (preferred < 0) preferred += def.viewCount;
        if (!def.texturePngs[(size_t)preferred].empty())
            return preferred;
        for (int radius = 1; radius < def.viewCount; radius++) {
            int a = (preferred + radius) % def.viewCount;
            if (!def.texturePngs[(size_t)a].empty())
                return a;
            int b = preferred - radius;
            while (b < 0) b += def.viewCount;
            if (!def.texturePngs[(size_t)b].empty())
                return b;
        }
        return -1;
    };

    int vBase = 1;
    int vtBase = 1;
    int unitCount = 0;
    int texturedCount = 0;
    for (size_t ii = 0; ii < instances.size(); ii++) {
        const auto& inst = instances[ii];
        if (inst.markerType != 3) continue;

        const auto& sameTypeEntries = itTR->second;
        if ((size_t)inst.paletteIdx >= sameTypeEntries.size()) continue;
        const UnitType* t = sameTypeEntries[inst.paletteIdx];
        if (t->w0 != 3) continue;

        std::string tagStr = tagToString(t->typeTag);
        float cellX = (float)inst.posX / WORLD_ONE;
        float cellY = (float)inst.posY / WORLD_ONE;
        float cellZ = (float)inst.posZ / WORLD_ONE;
        float facingDeg = (float)(((double)inst.yaw / 65536.0) * 360.0);
        float displayFacingDeg = std::fmod(180.0f - facingDeg, 360.0f);
        if (displayFacingDeg < 0.0f) displayFacingDeg += 360.0f;

        float wx = halfW - cellX;
        float wy = cellY - halfH;
        float wz = cellZ;

        char name[128];
        snprintf(name, sizeof(name), "%s_%zu", tagToFileStem(t->typeTag).c_str(), ii);
        obj += "o "; obj += name; obj += "\n";
        obj += "g "; obj += name; obj += "\n";

        auto dit = spriteDefs.find(t->typeTag);
        const UnitSpriteDef* def = dit == spriteDefs.end() ? nullptr : &dit->second;
        bool textured = def && def->textured && def->viewCount > 0;
        int viewIndex = 0;
        if (textured) {
            viewIndex = findAvailableUnitView(*def,
                (int)std::floor(((double)displayFacingDeg / 360.0) * def->viewCount + 0.5));
            if (viewIndex < 0) textured = false;
        }
        if (textured) {
            float cardW = std::max(0.25f, (float)def->width / 64.0f);
            float cardH = std::max(0.25f, (float)def->height / 64.0f);
            const float r = cardW * 0.5f;
            const float h = cardH;
            float facingRad = (displayFacingDeg - 90.0f) * (float)(PI / 180.0);
            float cosF = std::cos(facingRad);
            float sinF = std::sin(facingRad);
            auto emitVertex = [&](float lx, float ly, float lz) {
                float rx = cosF * lx - sinF * ly;
                float ry = sinF * lx + cosF * ly;
                char buf[128];
                snprintf(buf, sizeof(buf), "v %.6f %.6f %.6f\n", wx + rx, wz + lz, wy + ry);
                obj += buf;
            };

            emitVertex(-r, 0.0f, 0.0f);
            emitVertex( r, 0.0f, 0.0f);
            emitVertex( r, 0.0f, h);
            emitVertex(-r, 0.0f, h);

            obj += "usemtl " + tagToFileStem(t->typeTag) + "_unit_"
                 + std::to_string(viewIndex) + "_sprite\n";
            obj += "vt 0.000000 0.000000\nvt 1.000000 0.000000\n";
            obj += "vt 1.000000 1.000000\nvt 0.000000 1.000000\n";

            char fbuf[128];
            snprintf(fbuf, sizeof(fbuf),
                     "f %d/%d %d/%d %d/%d %d/%d\n\n",
                     vBase + 1, vtBase + 1,
                     vBase + 2, vtBase + 2,
                     vBase + 3, vtBase + 3,
                     vBase + 0, vtBase + 0);
            obj += fbuf;
            vBase += 4;
            vtBase += 4;
            texturedCount++;
        } else {
            obj += "usemtl unit_placeholder\n";
            float facingRad = (displayFacingDeg - 90.0f) * (float)(PI / 180.0);
            float cosF = std::cos(facingRad);
            float sinF = std::sin(facingRad);

            // Simple standing marker: a small diamond footprint plus a facing spike.
            const float r = 0.35f;
            const float h = 1.8f;
            auto emitVertex = [&](float lx, float ly, float lz) {
                float rx = cosF * lx - sinF * ly;
                float ry = sinF * lx + cosF * ly;
                char buf[128];
                snprintf(buf, sizeof(buf), "v %.6f %.6f %.6f\n", wx + rx, wz + lz, wy + ry);
                obj += buf;
            };

            emitVertex(0.0f, 0.0f, h);      // top
            emitVertex(-r, 0.0f, 0.0f);     // left
            emitVertex(0.0f, r, 0.0f);      // front
            emitVertex(r, 0.0f, 0.0f);      // right
            emitVertex(0.0f, -r, 0.0f);     // back
            emitVertex(0.0f, r * 1.9f, h * 0.55f); // facing spike

            char fbuf[256];
            snprintf(fbuf, sizeof(fbuf),
                     "f %d %d %d\nf %d %d %d\nf %d %d %d\nf %d %d %d\nf %d %d %d\n\n",
                     vBase, vBase+1, vBase+2,
                     vBase, vBase+2, vBase+3,
                     vBase, vBase+3, vBase+4,
                     vBase, vBase+4, vBase+1,
                     vBase, vBase+2, vBase+5);
            obj += fbuf;
            vBase += 6;
        }

        if (unitCount) json += ",\n";
        json += "    {\"tag\": ";
        appendJsonString(json, tagStr);
        char jbuf[256];
        snprintf(jbuf, sizeof(jbuf),
                 ", \"x\": %.4f, \"y\": %.4f, \"z\": %.4f"
                 ", \"facing_deg\": %.4f, \"display_facing_deg\": %.4f, \"sequence\": %d"
                 ", \"view\": %d, \"textured\": %s"
                 ", \"pal_idx\": %d, \"marker_idx\": %zu, \"identifier\": %u}",
                 cellX, cellY, cellZ, facingDeg,
                 displayFacingDeg,
                 textured ? def->sequenceIndex : -1,
                 textured ? viewIndex : -1,
                 textured ? "true" : "false",
                 (int)inst.paletteIdx, ii, (unsigned)inst.identifier);
        json += jbuf;
        unitCount++;
    }

    json += "\n  ]\n}\n";

    if (unitCount == 0)
        return false;

    bool okObj = writeText(objPath, obj);
    bool okJson = writeText(jsonPath, json);
    if (okObj)
        printf("Unit sprites:      %s (%d units, %d textured)\n",
               objPath.c_str(), unitCount, texturedCount);
    if (okJson)
        printf("Unit placement:    %s\n", jsonPath.c_str());
    return okObj && okJson;
}

static bool exportSoundPlaceholders(const std::string& outFolder,
                                    const MeshHeader& mh,
                                    const std::vector<ObjectInstance>& instances,
                                    const std::map<int16_t, std::vector<const UnitType*>>& typeRelIdx,
                                    const std::vector<TagEntry>& tags,
                                    bool overwriteSounds) {
    auto itTR = typeRelIdx.find(5);
    if (itTR == typeRelIdx.end() || itTR->second.empty())
        return false;

    const float halfW = (float)(mh.submeshW * 32) * 0.5f;
    const float halfH = (float)(mh.submeshH * 32) * 0.5f;

    std::string objPath = outFolder + "/assets/sounds/sounds.obj";
    std::string mtlPath = outFolder + "/assets/sounds/sounds.mtl";
    std::string jsonPath = outFolder + "/assets/sounds/sounds.json";

    std::string mtl;
    mtl += "newmtl sound_placeholder\n";
    mtl += "Ka 0.1 0.1 0.1\n";
    mtl += "Kd 0.1 0.45 1.0\n";
    mtl += "Ks 0.0 0.0 0.0\n";
    mtl += "illum 1\n\n";
    writeText(mtlPath, mtl);

    std::string obj;
    obj += "# Myth II sound placeholders\n";
    obj += "mtllib sounds.mtl\n\n";

    std::string json;
    json += "{\n  \"sounds\": [\n";

    int vBase = 1;
    int soundCount = 0;
    int extractedSoundCount = 0;
    std::map<uint32_t, std::vector<SoundPermutationExport>> extractedByTag;
    for (size_t ii = 0; ii < instances.size(); ii++) {
        const auto& inst = instances[ii];
        if (inst.markerType != 5) continue;

        const auto& sameTypeEntries = itTR->second;
        if ((size_t)inst.paletteIdx >= sameTypeEntries.size()) continue;
        const UnitType* t = sameTypeEntries[inst.paletteIdx];
        if (t->w0 != 5) continue;

        std::string tagStr = tagToString(t->typeTag);
        float cellX = (float)inst.posX / WORLD_ONE;
        float cellY = (float)inst.posY / WORLD_ONE;
        float cellZ = (float)inst.posZ / WORLD_ONE;
        float facingDeg = (float)(((double)inst.yaw / 65536.0) * 360.0);

        float wx = halfW - cellX;
        float wy = cellY - halfH;
        float wz = cellZ;

        auto exIt = extractedByTag.find(t->typeTag);
        if (exIt == extractedByTag.end()) {
            std::vector<SoundPermutationExport> perms;
            exportSoundTagWavs(tags, t->typeTag, outFolder, overwriteSounds, perms);
            exIt = extractedByTag.emplace(t->typeTag, std::move(perms)).first;
            extractedSoundCount += (int)exIt->second.size();
        }

        char name[128];
        snprintf(name, sizeof(name), "%s_%zu", tagToFileStem(t->typeTag).c_str(), ii);
        obj += "o "; obj += name; obj += "\n";
        obj += "g "; obj += name; obj += "\n";
        obj += "usemtl sound_placeholder\n";

        // Simple speaker/ripple marker: upright pole plus two ground diamonds.
        const float r1 = 0.35f;
        const float r2 = 0.65f;
        const float h = 1.2f;
        auto emitVertex = [&](float lx, float ly, float lz) {
            char buf[128];
            snprintf(buf, sizeof(buf), "v %.6f %.6f %.6f\n", wx + lx, wz + lz, wy + ly);
            obj += buf;
        };

        emitVertex(0.0f, 0.0f, 0.0f);
        emitVertex(0.0f, 0.0f, h);
        emitVertex(-r1, 0.0f, 0.0f);
        emitVertex(0.0f, r1, 0.0f);
        emitVertex(r1, 0.0f, 0.0f);
        emitVertex(0.0f, -r1, 0.0f);
        emitVertex(-r2, 0.0f, 0.0f);
        emitVertex(0.0f, r2, 0.0f);
        emitVertex(r2, 0.0f, 0.0f);
        emitVertex(0.0f, -r2, 0.0f);

        char lbuf[256];
        snprintf(lbuf, sizeof(lbuf),
                 "l %d %d\nl %d %d %d %d %d\nl %d %d %d %d %d\n\n",
                 vBase, vBase + 1,
                 vBase + 2, vBase + 3, vBase + 4, vBase + 5, vBase + 2,
                 vBase + 6, vBase + 7, vBase + 8, vBase + 9, vBase + 6);
        obj += lbuf;
        vBase += 10;

        if (soundCount) json += ",\n";
        json += "    {\"tag\": ";
        appendJsonString(json, tagStr);
        char jbuf[256];
        snprintf(jbuf, sizeof(jbuf),
                 ", \"x\": %.4f, \"y\": %.4f, \"z\": %.4f"
                 ", \"obj_x\": %.4f, \"obj_y\": %.4f, \"obj_z\": %.4f"
                 ", \"facing_deg\": %.4f, \"pal_idx\": %d, \"marker_idx\": %zu, \"identifier\": %u",
                 cellX, cellY, cellZ, wx, wz, wy, facingDeg, (int)inst.paletteIdx, ii,
                 (unsigned)inst.identifier);
        json += jbuf;
        json += ", \"audio\": [";
        for (size_t ai = 0; ai < exIt->second.size(); ai++) {
            if (ai) json += ", ";
            json += "{\"name\": ";
            appendJsonString(json, exIt->second[ai].name);
            json += ", \"path\": ";
            appendJsonString(json, exIt->second[ai].relativePath);
            json += "}";
        }
        json += "]}";
        soundCount++;
    }

    json += "\n  ]\n}\n";

    if (soundCount == 0)
        return false;

    bool okObj = writeText(objPath, obj);
    bool okJson = writeText(jsonPath, json);
    if (okObj)
        printf("Sound placeholders: %s (%d sounds)\n", objPath.c_str(), soundCount);
    if (okJson)
        printf("Sound placement:    %s\n", jsonPath.c_str());
    if (extractedSoundCount > 0)
        printf("Sound WAVs:         %s/assets/sounds/wav (%d permutations)\n",
               outFolder.c_str(), extractedSoundCount);
    return okObj && okJson;
}

struct ScenerySpriteDef {
    bool textured = false;
    std::string tagStr;
    std::string texturePng;
    int sequenceIndex = 0;
    int width = 32;
    int height = 32;
};

struct ConnectorDefinition {
    uint32_t collectionRefTag = 0;
    int16_t normalSequenceIndex = -1;
    int16_t damagedSequenceIndex = -1;
};

struct FencePostPoint {
    uint16_t identifier = 0;
    size_t markerIdx = 0;
    float cellX = 0.0f;
    float cellY = 0.0f;
    float cellZ = 0.0f;
};

static bool readConnectorDefinition(const std::vector<TagEntry>& tags,
                                    uint32_t connectorTag,
                                    ConnectorDefinition& def) {
    static const uint32_t GROUP_CONN = 0x636F6E6Eu; // 'conn'
    const TagEntry* entry = findTag(tags, GROUP_CONN, connectorTag);
    if (!entry) return false;
    std::vector<uint8_t> data;
    if (!readTagData(*entry, data) || data.size() < 16) return false;
    def.collectionRefTag = readBE32u(data.data(), 4);
    def.normalSequenceIndex = readBE16s(data.data(), 8);
    def.damagedSequenceIndex = readBE16s(data.data(), 14);
    return true;
}

static bool exportFenceConnectors(const std::string& outFolder,
                                  const MeshHeader& mh,
                                  const std::vector<uint8_t>& meshData,
                                  const std::vector<ObjectInstance>& instances,
                                  const std::map<int16_t, std::vector<const UnitType*>>& typeRelIdx,
                                  const std::vector<TagEntry>& tags) {
    if (mh.connectorCount == 0 || mh.connectorSize == 0) return false;
    if (mh.connectorOffset >= meshData.size()) return false;
    if (mh.connectorTag == 0 || mh.connectorTag == 0xFFFFFFFFu) return false;

    size_t connectorsAbs = 1024 + (size_t)mh.connectorOffset;
    if (connectorsAbs >= meshData.size()) return false;
    if (connectorsAbs + (size_t)mh.connectorSize > meshData.size()) return false;
    if (mh.connectorCount == 0 || (mh.connectorSize % mh.connectorCount) != 0) return false;
    const size_t recordSize = (size_t)(mh.connectorSize / mh.connectorCount);
    if (recordSize < 4 || (recordSize % 2) != 0) return false;

    auto itTR = typeRelIdx.find(1);
    if (itTR == typeRelIdx.end() || itTR->second.empty()) return false;

    const uint32_t fencePostTag = tagFromString("mufp");
    std::map<uint16_t, FencePostPoint> postsByIdentifier;
    for (size_t ii = 0; ii < instances.size(); ii++) {
        const auto& inst = instances[ii];
        if (inst.markerType != 1) continue;
        if ((size_t)inst.paletteIdx >= itTR->second.size()) continue;
        const UnitType* t = itTR->second[inst.paletteIdx];
        if (!t || t->w0 != 1 || t->typeTag != fencePostTag) continue;
        FencePostPoint p;
        p.identifier = inst.identifier;
        p.markerIdx = ii;
        p.cellX = (float)inst.posX / WORLD_ONE;
        p.cellY = (float)inst.posY / WORLD_ONE;
        p.cellZ = (float)inst.posZ / WORLD_ONE;
        postsByIdentifier[p.identifier] = p;
    }
    if (postsByIdentifier.empty()) return false;

    ConnectorDefinition connDef;
    bool haveConnDef = readConnectorDefinition(tags, mh.connectorTag, connDef);

    const float halfW = (float)(mh.submeshW * 32) * 0.5f;
    const float halfH = (float)(mh.submeshH * 32) * 0.5f;
    const float fenceHeight = 0.72f;
    const float minSegmentLength = 0.001f;

    std::string objPath = outFolder + "/assets/terrain/fences.obj";
    std::string mtlPath = outFolder + "/assets/terrain/fences.mtl";
    std::string jsonPath = outFolder + "/assets/terrain/fences.json";
    std::string texturePath;
    bool textured = false;
    int texW = 0, texH = 0;
    if (haveConnDef && connDef.collectionRefTag != 0 && connDef.collectionRefTag != 0xFFFFFFFFu &&
        connDef.normalSequenceIndex >= 0) {
        const TagEntry* collEntry = findTextureCollection(tags, connDef.collectionRefTag);
        if (collEntry) {
            std::vector<uint8_t> collData;
            if (readTagData(*collEntry, collData)) {
                std::string texDir = outFolder + "/assets/terrain/textures";
                makeDirs(texDir);
                texturePath = texDir + "/" + tagToFileStem(mh.connectorTag) + "_fence.png";
                textured = extractDot256Texture(collData, connDef.normalSequenceIndex, 0, texturePath,
                                                &texW, &texH,
                                                true,  // pure blue becomes transparent
                                                true); // non-blue fence pixels stay opaque
            }
        }
    }

    std::string mtl;
    mtl += "newmtl fence_placeholder\n";
    mtl += "Ka 0.10 0.10 0.10\n";
    mtl += textured ? "Kd 1.00 1.00 1.00\n" : "Kd 0.72 0.78 0.80\n";
    mtl += "Ks 0.00 0.00 0.00\n";
    mtl += "d 1.00\n";
    mtl += "illum 1\n\n";
    if (textured) {
        mtl += "map_Kd textures/" + fs::path(texturePath).filename().string() + "\n";
        mtl += "map_d textures/" + fs::path(texturePath).filename().string() + "\n\n";
    }
    if (!writeText(mtlPath, mtl))
        return false;

    std::string obj;
    obj += "# Myth II fence connector spans\n";
    obj += "mtllib fences.mtl\n\n";

    std::string json;
    json += "{\n";
    json += "  \"connector_tag\": ";
    appendJsonString(json, tagToString(mh.connectorTag));
    if (haveConnDef) {
        json += ",\n  \"collection_tag\": ";
        appendJsonString(json, tagToString(connDef.collectionRefTag));
        char metaBuf[128];
        snprintf(metaBuf, sizeof(metaBuf),
                 ",\n  \"normal_sequence\": %d,\n  \"damaged_sequence\": %d",
                 (int)connDef.normalSequenceIndex, (int)connDef.damagedSequenceIndex);
        json += metaBuf;
    }
    json += ",\n  \"fences\": [\n";

    int vBase = 1;
    int vtBase = 1;
    int fenceCount = 0;
    int segmentCount = 0;
    for (uint32_t ci = 0; ci < mh.connectorCount; ci++) {
        size_t recOff = connectorsAbs + (size_t)ci * recordSize;
        std::vector<FencePostPoint> chain;
        for (size_t wi = 0; wi < recordSize / 2; wi++) {
            uint16_t identifier = readBE16u(meshData.data(), recOff + wi * 2);
            auto pit = postsByIdentifier.find(identifier);
            if (pit == postsByIdentifier.end())
                break;
            if (!chain.empty() && chain.back().identifier == identifier)
                continue;
            chain.push_back(pit->second);
        }
        if (chain.size() < 2)
            continue;

        char name[64];
        snprintf(name, sizeof(name), "fence_%u", (unsigned)ci);
        obj += "o ";
        obj += name;
        obj += "\n";
        obj += "g ";
        obj += name;
        obj += "\n";
        obj += "usemtl fence_placeholder\n";

        for (size_t pi = 0; pi + 1 < chain.size(); pi++) {
            const auto& a = chain[pi];
            const auto& b = chain[pi + 1];
            float ax = halfW - a.cellX;
            float ay = a.cellY - halfH;
            float az0 = a.cellZ + 0.05f;
            float az1 = az0 + fenceHeight;
            float bx = halfW - b.cellX;
            float by = b.cellY - halfH;
            float bz0 = b.cellZ + 0.05f;
            float bz1 = bz0 + fenceHeight;
            float dx = bx - ax;
            float dy = by - ay;
            float segLen = std::sqrt(dx * dx + dy * dy);
            if (segLen < minSegmentLength)
                continue;
            float u1 = textured && texW > 0 ? (segLen / ((float)texW / 64.0f)) : 1.0f;

            char buf[768];
            snprintf(buf, sizeof(buf),
                     "v %.6f %.6f %.6f\n"
                     "v %.6f %.6f %.6f\n"
                     "v %.6f %.6f %.6f\n"
                     "v %.6f %.6f %.6f\n"
                     "vt 0.000000 0.000000\n"
                     "vt %.6f 0.000000\n"
                     "vt %.6f 1.000000\n"
                     "vt 0.000000 1.000000\n"
                     "f %d/%d %d/%d %d/%d %d/%d\n",
                     ax, az0, ay,
                     bx, bz0, by,
                     bx, bz1, by,
                     ax, az1, ay,
                     u1, u1,
                     vBase, vtBase, vBase + 1, vtBase + 1,
                     vBase + 2, vtBase + 2, vBase + 3, vtBase + 3);
            obj += buf;
            vBase += 4;
            vtBase += 4;
            segmentCount++;
        }
        obj += "\n";

        if (fenceCount) json += ",\n";
        char headerBuf[128];
        snprintf(headerBuf, sizeof(headerBuf),
                 "    {\"connector_idx\": %u, \"post_count\": %zu, \"posts\": [",
                 (unsigned)ci, chain.size());
        json += headerBuf;
        for (size_t pi = 0; pi < chain.size(); pi++) {
            if (pi) json += ", ";
            const auto& p = chain[pi];
            char postBuf[256];
            snprintf(postBuf, sizeof(postBuf),
                     "{\"identifier\": %u, \"marker_idx\": %zu, \"x\": %.4f, \"y\": %.4f, \"z\": %.4f}",
                     (unsigned)p.identifier, p.markerIdx, p.cellX, p.cellY, p.cellZ);
            json += postBuf;
        }
        json += "]}";
        fenceCount++;
    }

    json += "\n  ]\n}\n";
    if (segmentCount == 0)
        return false;

    bool okObj = writeText(objPath, obj);
    bool okJson = writeText(jsonPath, json);
    if (okObj)
        printf("Fence connectors: %s (%d fences, %d segments)\n",
               objPath.c_str(), fenceCount, segmentCount);
    if (okJson)
        printf("Fence metadata:   %s\n", jsonPath.c_str());
    return okObj && okJson;
}

static ScenerySpriteDef resolveScenerySprite(const std::string& outFolder,
                                             const std::vector<TagEntry>& tags,
                                             uint32_t typeTag,
                                             bool overwriteTextures) {
    static const uint32_t GROUP_SCEN = 0x7363656Eu; // 'scen'

    ScenerySpriteDef def;
    def.tagStr = tagToString(typeTag);

    const TagEntry* scenEntry = findTag(tags, GROUP_SCEN, typeTag);
    if (!scenEntry) return def;

    std::vector<uint8_t> scenData;
    if (!readTagData(*scenEntry, scenData) || scenData.size() < 0x2A) return def;

    uint32_t collectionRef = readBE32u(scenData.data(), 4);
    int16_t sequenceIndex = readBE16s(scenData.data(), 0x28);
    if (sequenceIndex < 0) return def;

    const TagEntry* collEntry = findTextureCollection(tags, collectionRef);
    if (!collEntry) {
        // Some scenery definitions carry a direct object/collection reference here.
        collectionRef = readBE32u(scenData.data(), 8);
        collEntry = findTextureCollection(tags, collectionRef);
    }
    if (!collEntry) return def;

    std::vector<uint8_t> collData;
    if (!readTagData(*collEntry, collData)) return def;

    std::string texDir = outFolder + "/assets/sprites/textures";
    makeDirs(texDir);
    std::string pngName = tagToFileStem(typeTag) + "_sprite.png";
    std::string pngPath = texDir + "/" + pngName;

    int w = 0, h = 0;
    if (overwriteTextures || !fs::exists(pngPath)) {
        if (!extractDot256Texture(collData, sequenceIndex, 0, pngPath, &w, &h))
            return def;
    } else {
        // Re-extract to learn the dimensions when the PNG already exists would be wasteful.
        // Use a conservative default scale for existing textures.
        w = 32;
        h = 48;
    }

    def.textured = fs::exists(pngPath);
    def.texturePng = pngPath;
    def.sequenceIndex = sequenceIndex;
    def.width = w > 0 ? w : 32;
    def.height = h > 0 ? h : 48;
    return def;
}

static bool exportSceneryPlaceholders(const std::string& outFolder,
                                      const MeshHeader& mh,
                                      const std::vector<ObjectInstance>& instances,
                                      const std::map<int16_t, std::vector<const UnitType*>>& typeRelIdx,
                                      const std::vector<TagEntry>& tags,
                                      bool overwriteTextures) {
    auto itTR = typeRelIdx.find(1);
    if (itTR == typeRelIdx.end() || itTR->second.empty())
        return false;

    const float halfW = (float)(mh.submeshW * 32) * 0.5f;
    const float halfH = (float)(mh.submeshH * 32) * 0.5f;

    std::string objPath = outFolder + "/assets/sprites/scenery.obj";
    std::string mtlPath = outFolder + "/assets/sprites/scenery.mtl";
    std::string jsonPath = outFolder + "/assets/sprites/scenery.json";

    std::string obj;
    obj += "# Myth II sprite scenery billboards\n";
    obj += "mtllib scenery.mtl\n\n";

    std::string json;
    json += "{\n  \"scenery\": [\n";

    std::map<uint32_t, ScenerySpriteDef> spriteDefs;
    std::set<std::string> materialNames;
    for (const auto* t: itTR->second) {
        if (t->w0 != 1) continue;
        ScenerySpriteDef def = resolveScenerySprite(outFolder, tags, t->typeTag, overwriteTextures);
        if (def.tagStr.empty()) def.tagStr = tagToString(t->typeTag);
        spriteDefs[t->typeTag] = std::move(def);
    }

    std::string mtl;
    mtl += "newmtl scenery_placeholder\n";
    mtl += "Ka 0.1 0.1 0.1\n";
    mtl += "Kd 0.25 0.75 0.25\n";
    mtl += "Ks 0.0 0.0 0.0\n";
    mtl += "illum 1\n\n";

    for (const auto& kv: spriteDefs) {
        const ScenerySpriteDef& def = kv.second;
        if (!def.textured) continue;
        std::string matName = tagToFileStem(kv.first) + "_sprite";
        if (!materialNames.insert(matName).second) continue;
        mtl += "newmtl " + matName + "\n";
        mtl += "Ka 1.0 1.0 1.0\n";
        mtl += "Kd 1.0 1.0 1.0\n";
        mtl += "Ks 0.0 0.0 0.0\n";
        mtl += "d 1.0\n";
        mtl += "illum 1\n";
        mtl += "map_Kd textures/" + fs::path(def.texturePng).filename().string() + "\n";
        mtl += "\n";
    }
    writeText(mtlPath, mtl);

    int vBase = 1;
    int vtBase = 1;
    int sceneryCount = 0;
    for (size_t ii = 0; ii < instances.size(); ii++) {
        const auto& inst = instances[ii];
        if (inst.markerType != 1) continue;

        const auto& sameTypeEntries = itTR->second;
        if ((size_t)inst.paletteIdx >= sameTypeEntries.size()) continue;
        const UnitType* t = sameTypeEntries[inst.paletteIdx];
        if (t->w0 != 1) continue;

        std::string tagStr = tagToString(t->typeTag);
        float cellX = (float)inst.posX / WORLD_ONE;
        float cellY = (float)inst.posY / WORLD_ONE;
        float cellZ = (float)inst.posZ / WORLD_ONE;
        float facingDeg = (float)(((double)inst.yaw / 65536.0) * 360.0);
        float facingRad = (facingDeg - 90.0f) * (float)(PI / 180.0);
        float cosF = std::cos(facingRad);
        float sinF = std::sin(facingRad);

        float wx = halfW - cellX;
        float wy = cellY - halfH;
        float wz = cellZ;

        char name[128];
        snprintf(name, sizeof(name), "%s_%zu", tagToFileStem(t->typeTag).c_str(), ii);
        obj += "o "; obj += name; obj += "\n";
        obj += "g "; obj += name; obj += "\n";
        auto dit = spriteDefs.find(t->typeTag);
        const ScenerySpriteDef* def = dit == spriteDefs.end() ? nullptr : &dit->second;
        bool textured = def && def->textured;
        obj += "usemtl ";
        obj += textured ? tagToFileStem(t->typeTag) + "_sprite" : "scenery_placeholder";
        obj += "\n";

        float cardW = 0.44f;
        float cardH = 0.8f;
        if (textured) {
            cardW = std::max(0.20f, (float)def->width / 64.0f);
            cardH = std::max(0.20f, (float)def->height / 64.0f);
        }
        const float r = cardW * 0.5f;
        const float h = cardH;
        auto emitVertex = [&](float lx, float ly, float lz) {
            float rx = cosF * lx - sinF * ly;
            float ry = sinF * lx + cosF * ly;
            char buf[128];
            snprintf(buf, sizeof(buf), "v %.6f %.6f %.6f\n", wx + rx, wz + lz, wy + ry);
            obj += buf;
        };

        emitVertex(-r, 0.0f, 0.0f);
        emitVertex(r, 0.0f, 0.0f);
        emitVertex(r, 0.0f, h);
        emitVertex(-r, 0.0f, h);
        emitVertex(0.0f, -r, 0.0f);
        emitVertex(0.0f, r, 0.0f);
        emitVertex(0.0f, r, h);
        emitVertex(0.0f, -r, h);

        if (textured) {
            obj += "vt 0.000000 0.000000\nvt 1.000000 0.000000\n";
            obj += "vt 1.000000 1.000000\nvt 0.000000 1.000000\n";
            obj += "vt 0.000000 0.000000\nvt 1.000000 0.000000\n";
            obj += "vt 1.000000 1.000000\nvt 0.000000 1.000000\n";
            char fbuf[192];
            snprintf(fbuf, sizeof(fbuf),
                     "f %d/%d %d/%d %d/%d %d/%d\n"
                     "f %d/%d %d/%d %d/%d %d/%d\n\n",
                     vBase, vtBase, vBase+1, vtBase+1, vBase+2, vtBase+2, vBase+3, vtBase+3,
                     vBase+4, vtBase+4, vBase+5, vtBase+5, vBase+6, vtBase+6, vBase+7, vtBase+7);
            obj += fbuf;
            vtBase += 8;
        } else {
            char fbuf[128];
            snprintf(fbuf, sizeof(fbuf), "f %d %d %d %d\nf %d %d %d %d\n\n",
                     vBase, vBase+1, vBase+2, vBase+3,
                     vBase+4, vBase+5, vBase+6, vBase+7);
            obj += fbuf;
        }
        vBase += 8;

        if (sceneryCount) json += ",\n";
        json += "    {\"tag\": ";
        appendJsonString(json, tagStr);
        char jbuf[256];
        snprintf(jbuf, sizeof(jbuf),
                 ", \"x\": %.4f, \"y\": %.4f, \"z\": %.4f"
                  ", \"facing_deg\": %.4f, \"sequence\": %d"
                 ", \"textured\": %s, \"pal_idx\": %d, \"marker_idx\": %zu, \"identifier\": %u"
                 ", \"pitch_raw\": %u, \"roll_raw\": %u}",
                 cellX, cellY, cellZ, facingDeg,
                 textured ? def->sequenceIndex : -1,
                 textured ? "true" : "false",
                 (int)inst.paletteIdx, ii, (unsigned)inst.identifier,
                 (unsigned)inst.pitch, (unsigned)inst.roll);
        json += jbuf;
        sceneryCount++;
    }

    json += "\n  ]\n}\n";

    if (sceneryCount == 0)
        return false;

    bool okObj = writeText(objPath, obj);
    bool okJson = writeText(jsonPath, json);
    if (okObj)
        printf("Sprite scenery billboards:  %s (%d markers)\n", objPath.c_str(), sceneryCount);
    if (okJson)
        printf("Sprite scenery placement:    %s\n", jsonPath.c_str());
    return okObj && okJson;
}

static bool exportProjectilePlaceholders(const std::string& outFolder,
                                         const MeshHeader& mh,
                                         const std::vector<ObjectInstance>& instances,
                                         const std::map<int16_t, std::vector<const UnitType*>>& typeRelIdx) {
    auto itTR = typeRelIdx.find(9);
    if (itTR == typeRelIdx.end() || itTR->second.empty())
        return false;

    const float halfW = (float)(mh.submeshW * 32) * 0.5f;
    const float halfH = (float)(mh.submeshH * 32) * 0.5f;

    std::string objPath = outFolder + "/assets/models/projectiles.obj";
    std::string mtlPath = outFolder + "/assets/models/projectiles.mtl";
    std::string jsonPath = outFolder + "/assets/models/projectiles.json";

    std::string mtl;
    mtl += "newmtl projectile_placeholder\n";
    mtl += "Ka 0.1 0.1 0.1\n";
    mtl += "Kd 1.0 0.75 0.1\n";
    mtl += "Ks 0.0 0.0 0.0\n";
    mtl += "illum 1\n\n";
    writeText(mtlPath, mtl);

    std::string obj;
    obj += "# Myth II projectile placeholders\n";
    obj += "mtllib projectiles.mtl\n\n";

    std::string json;
    json += "{\n  \"projectiles\": [\n";

    int vBase = 1;
    int projectileCount = 0;
    for (size_t ii = 0; ii < instances.size(); ii++) {
        const auto& inst = instances[ii];
        if (inst.markerType != 9) continue;

        const auto& sameTypeEntries = itTR->second;
        if ((size_t)inst.paletteIdx >= sameTypeEntries.size()) continue;
        const UnitType* t = sameTypeEntries[inst.paletteIdx];
        if (t->w0 != 9) continue;

        std::string tagStr = tagToString(t->typeTag);
        float cellX = (float)inst.posX / WORLD_ONE;
        float cellY = (float)inst.posY / WORLD_ONE;
        float cellZ = (float)inst.posZ / WORLD_ONE;
        float facingDeg = (float)(((double)inst.yaw / 65536.0) * 360.0);
        float facingRad = (facingDeg - 90.0f) * (float)(PI / 180.0);
        float cosF = std::cos(facingRad);
        float sinF = std::sin(facingRad);

        float wx = halfW - cellX;
        float wy = cellY - halfH;
        float wz = cellZ;

        char name[128];
        snprintf(name, sizeof(name), "%s_%zu", tagToFileStem(t->typeTag).c_str(), ii);
        obj += "o "; obj += name; obj += "\n";
        obj += "g "; obj += name; obj += "\n";
        obj += "usemtl projectile_placeholder\n";

        // Small arrow marker in the facing direction.
        auto emitVertex = [&](float lx, float ly, float lz) {
            float rx = cosF * lx - sinF * ly;
            float ry = sinF * lx + cosF * ly;
            char buf[128];
            snprintf(buf, sizeof(buf), "v %.6f %.6f %.6f\n", wx + rx, wz + lz, wy + ry);
            obj += buf;
        };

        emitVertex(0.0f, -0.35f, 0.0f);
        emitVertex(0.0f, 0.45f, 0.0f);
        emitVertex(-0.18f, 0.20f, 0.0f);
        emitVertex(0.18f, 0.20f, 0.0f);
        emitVertex(0.0f, 0.0f, 0.45f);

        char lbuf[128];
        snprintf(lbuf, sizeof(lbuf),
                 "l %d %d\nl %d %d\nl %d %d\nl %d %d\nl %d %d\n\n",
                 vBase, vBase+1,
                 vBase+1, vBase+2,
                 vBase+1, vBase+3,
                 vBase, vBase+4,
                 vBase+1, vBase+4);
        obj += lbuf;
        vBase += 5;

        if (projectileCount) json += ",\n";
        json += "    {\"tag\": ";
        appendJsonString(json, tagStr);
        char jbuf[256];
        snprintf(jbuf, sizeof(jbuf),
                 ", \"x\": %.4f, \"y\": %.4f, \"z\": %.4f"
                 ", \"facing_deg\": %.4f, \"pal_idx\": %d, \"marker_idx\": %zu, \"identifier\": %u}",
                 cellX, cellY, cellZ, facingDeg, (int)inst.paletteIdx, ii,
                 (unsigned)inst.identifier);
        json += jbuf;
        projectileCount++;
    }

    json += "\n  ]\n}\n";

    if (projectileCount == 0)
        return false;

    bool okObj = writeText(objPath, obj);
    bool okJson = writeText(jsonPath, json);
    if (okObj)
        printf("Projectile placeholders: %s (%d projectiles)\n", objPath.c_str(), projectileCount);
    if (okJson)
        printf("Projectile placement:    %s\n", jsonPath.c_str());
    return okObj && okJson;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void usage(const char* p) {
    fprintf(stderr,
        "Myth II Map Object Exporter\n\n"
        "Usage:\n"
        "  %s <tags_folder|plugin_file> <out_folder> [terrain.obj] [--world-space] [--overwrite]\n"
        "     [--animation-frame first|none|all]\n\n"
        "Arguments:\n"
        "  tags_folder|plugin_file\n"
        "               folder containing Myth II tag files, or a single plugin/tag file\n"
        "  out_folder     extracted map folder (e.g. out/le3e, must contain raw/mesh_tag.bin)\n"
        "  terrain.obj    terrain OBJ to inline into map_combined.obj\n"
        "                 (auto-detected from assets/terrain/displacement.obj if present)\n"
        "  --world-space  per-instance OBJs use world (map) coordinates instead of local origin\n\n"
        "  --overwrite    regenerate existing asset texture/audio files\n\n"
        "  --animation-frame first|none|all\n"
        "                 animation frames to include in map_combined.obj (default: first)\n\n"
        "Output:\n"
        "  <out_folder>/assets/models/<tag>.obj      per-type geometry (local origin)\n"
        "  <out_folder>/assets/models/<tag>.mtl\n"
        "  <out_folder>/assets/models/<tag>_<N>.obj  per-instance geometry\n"
        "  <out_folder>/assets/models/<tag>_<N>.mtl\n"
        "  <out_folder>/assets/models/<anim>_<N>_frame##_*.obj  model-animation frames\n"
        "  <out_folder>/assets/models/animations.json  model-animation frame manifest\n"
        "  <out_folder>/placement.json        all scenery instance placements\n",
        p);
}

int main(int argc, char* argv[]) {
    if (argc < 3) { usage(argv[0]); return 1; }

    std::string tagsFolder   = argv[1];
    std::string outFolder    = argv[2];
    std::string terrainObjPath;
    bool worldSpace = false;
    bool overwriteTextures = false;
    AnimationFrameMode animationFrameMode = AnimationFrameMode::First;

    for (int i = 3; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--world-space") worldSpace = true;
        else if (a == "--overwrite") overwriteTextures = true;
        else if (a == "--animation-frame") {
            if (i + 1 >= argc) { usage(argv[0]); return 1; }
            std::string v = argv[++i];
            if (v == "first") animationFrameMode = AnimationFrameMode::First;
            else if (v == "none") animationFrameMode = AnimationFrameMode::None;
            else if (v == "all") animationFrameMode = AnimationFrameMode::All;
            else { usage(argv[0]); return 1; }
        }
        else if (a.rfind("--animation-frame=", 0) == 0) {
            std::string v = a.substr(strlen("--animation-frame="));
            if (v == "first") animationFrameMode = AnimationFrameMode::First;
            else if (v == "none") animationFrameMode = AnimationFrameMode::None;
            else if (v == "all") animationFrameMode = AnimationFrameMode::All;
            else { usage(argv[0]); return 1; }
        }
        else if (terrainObjPath.empty()) terrainObjPath = a;
        else { usage(argv[0]); return 1; }
    }
    // trim trailing slashes
    while (!tagsFolder.empty() && (tagsFolder.back()=='/'||tagsFolder.back()=='\\'))
        tagsFolder.pop_back();
    while (!outFolder.empty() && (outFolder.back()=='/'||outFolder.back()=='\\'))
        outFolder.pop_back();

    // Auto-detect terrain OBJ from assets/terrain/ if not specified on command line
    if (terrainObjPath.empty()) {
        std::string candidate = outFolder + "/assets/terrain/displacement.obj";
        if (fs::exists(candidate))
            terrainObjPath = candidate;
    }

    // Load mesh tag
    std::string meshPath = outFolder+"/raw/mesh_tag.bin";
    auto meshData = readFile(meshPath);
    if (meshData.empty()) {
        fprintf(stderr,"Cannot read mesh: %s\n", meshPath.c_str());
        return 1;
    }

    MeshHeader mh;
    if (!parseMeshHeader(meshData, mh)) {
        fprintf(stderr,"Failed to parse mesh header\n"); return 1;
    }

    printf("Myth II Map Object Exporter\n===========================\n");
    printf("Tags:    %s\n", tagsFolder.c_str());
    printf("Output:  %s\n", outFolder.c_str());
    printf("Submesh: %dx%d (%dx%d cells)\n\n",
           mh.submeshW, mh.submeshH, mh.submeshW*32, mh.submeshH*32);

    // Read unit type table
    std::vector<UnitType> unitTypes;
    if (!readUnitTypes(meshData, mh, unitTypes)) {
        fprintf(stderr,"Failed to read unit type table\n"); return 1;
    }

    // Read instance table
    std::vector<ObjectInstance> instances;
    if (!readInstances(meshData, mh, instances)) {
        fprintf(stderr,"Failed to read instance table\n"); return 1;
    }

    // The on-disk marker.palette_index is type-relative: it counts how many palette
    // entries of the same type (marker_data.type) precede the referenced entry.
    // This matches the Myth II engine's save/load format (confirmed in TMeshForm.cpp).
    // To resolve: for marker with (markerType=T, palette_index=N),
    // find the Nth palette entry where palette_entry.w0 == T.
    //
    // Build per-marker-type ordered list: markerTypeRelIdx[T][N] -> &UnitType
    std::map<int16_t, std::vector<const UnitType*>> typeRelIdx;
    for (const auto& t: unitTypes)
        typeRelIdx[(int16_t)t.w0].push_back(&t);

    // Also build flat helpers for direct model and model-animation palette entries.
    std::vector<const UnitType*> sceneryTypes;
    for (const auto& t: unitTypes)
        if (t.w0==6) sceneryTypes.push_back(&t);
    std::vector<const UnitType*> animationTypes;
    for (const auto& t: unitTypes)
        if (t.w0==11) animationTypes.push_back(&t);
    std::vector<const UnitType*> unitMarkerTypes;
    for (const auto& t: unitTypes)
        if (t.w0==3) unitMarkerTypes.push_back(&t);
    std::vector<const UnitType*> soundMarkerTypes;
    for (const auto& t: unitTypes)
        if (t.w0==5) soundMarkerTypes.push_back(&t);
    std::vector<const UnitType*> spriteSceneryTypes;
    for (const auto& t: unitTypes)
        if (t.w0==1) spriteSceneryTypes.push_back(&t);
    std::vector<const UnitType*> projectileTypes;
    for (const auto& t: unitTypes)
        if (t.w0==9) projectileTypes.push_back(&t);

    if (sceneryTypes.empty() && animationTypes.empty() && unitMarkerTypes.empty() && soundMarkerTypes.empty() && spriteSceneryTypes.empty() && projectileTypes.empty()) {
        printf("No direct model (w0=6), model animation (w0=11), sprite scenery (w0=1), unit (w0=3), sound (w0=5), or projectile (w0=9) types found in this mesh.\n");
        return 0;
    }

    printf("Unit types: %u total, %u direct models, %u model animations, %u sprite scenery, %u units, %u sounds, %u projectiles\n",
           (unsigned)unitTypes.size(), (unsigned)sceneryTypes.size(),
           (unsigned)animationTypes.size(), (unsigned)spriteSceneryTypes.size(),
           (unsigned)unitMarkerTypes.size(),
           (unsigned)soundMarkerTypes.size(), (unsigned)projectileTypes.size());

    // Scan tag files
    std::vector<TagEntry> tags;
    if (fs::is_regular_file(tagsFolder)) {
        scanTagFile(tagsFolder, tags);
    } else if (fs::is_directory(tagsFolder)) {
        for (const auto& it: fs::directory_iterator(tagsFolder)) {
            if (!it.is_regular_file()) continue;
            scanTagFile(it.path().string(), tags);
        }
    } else {
        fprintf(stderr,"Not a directory or file: %s\n", tagsFolder.c_str());
        return 1;
    }
    if (tags.empty()) {
        fprintf(stderr,"No Myth II tag files found in %s\n", tagsFolder.c_str()); return 1;
    }
    printf("Tags loaded: %zu entries\n\n", tags.size());

    static const uint32_t GROUP_GEOM = 0x67656F6Du; // 'geom'
    static const uint32_t GROUP_MODE = 0x6d6f6465u; // 'mode'
    static const uint32_t GROUP_ANIM = 0x616e696du; // 'anim'

    std::map<uint32_t, AnimationDef> animCache;
    std::set<uint32_t> modelTypeTags;
    for (const auto* t: sceneryTypes)
        modelTypeTags.insert(t->typeTag);

    for (const auto* t: animationTypes) {
        const TagEntry* animEntry = findTag(tags, GROUP_ANIM, t->typeTag);
        if (!animEntry) {
            printf("Animation %s: missing anim tag\n", tagToString(t->typeTag).c_str());
            continue;
        }
        std::vector<uint8_t> animData;
        AnimationDef anim;
        if (!readTagData(*animEntry, animData) || !parseAnimation(animData, t->typeTag, anim)) {
            printf("Animation %s: failed to parse anim tag\n", tagToString(t->typeTag).c_str());
            continue;
        }
        printf("Animation %s: %zu frames, %d ticks/frame\n",
               tagToString(t->typeTag).c_str(), anim.frames.size(), (int)anim.ticksPerFrame);
        for (const auto& frame: anim.frames)
            if (frame.modelTag)
                modelTypeTags.insert(frame.modelTag);
        animCache[t->typeTag] = std::move(anim);
    }

    // No base offset needed — posX/posY are world_distance values (WORLD_FRACTIONAL_BITS=9).
    // Cell coordinate = posX >> 9 (integer part), sub-cell = posX & 511 (fraction).
    // Use float division for smooth placement: cellX = posX / WORLD_ONE.

    // Create output directories
    makeDirs(outFolder + "/assets/models");
    makeDirs(outFolder + "/assets/terrain");
    makeDirs(outFolder + "/assets/sprites");
    makeDirs(outFolder + "/assets/sounds");

    // ---- Export 3D models per scenery type ----
    std::set<uint32_t> exportedTypes;
    std::map<uint32_t,Geometry> geomCache;         // typeTag -> loaded geometry
    std::map<uint32_t,std::vector<uint8_t>> collCache; // typeTag -> .256 collection data
    // permCache: typeTag -> permutations table [permIndex][matIndex] = view index (0xFF = not rendered)
    std::map<uint32_t,std::vector<std::vector<uint8_t>>> permCache;
    int modelsExported=0, spritesSkipped=0;

    printf("%-8s  %-12s  %-7s  %-7s  %-7s  %s\n",
           "tag","geom_ref","verts","surfs","tris","result");
    printf("%s\n", std::string(68,'-').c_str());

    for (uint32_t typeTag: modelTypeTags) {
        std::string tagStr = tagToString(typeTag);
        std::string tagFile = tagToFileStem(typeTag);
        if (exportedTypes.count(typeTag)) continue;
        exportedTypes.insert(typeTag);

        // Resolve geom via the mode tag: mode.geometry_tag -> geom subgroup.
        // Direct lookup by type_tag would find wrong geom tags from other maps
        // (e.g. 'corn' -> cornerWall_geom, 'oute' -> outerWallSep_geom).
        const TagEntry* modeEntry = findTag(tags, GROUP_MODE, typeTag);
        if (!modeEntry) {
            printf("%-8s  %-12s  %-7s  %-7s  %-7s  SPRITE (no mode tag)\n",
                   tagStr.c_str(), "-", "-", "-", "-");
            spritesSkipped++;
            continue;
        }
        std::vector<uint8_t> modeData;
        if (!readTagData(*modeEntry, modeData) || modeData.size() < 64) {
            printf("%-8s  %-12s  %-7s  %-7s  %-7s  ERROR reading mode\n",
                   tagStr.c_str(), "-", "-", "-", "-");
            continue;
        }
        // model_definition header (64 bytes): [4:8] geometry_tag, [12:14] permutation_count,
        // [28:32] permutations_offset (relative to data start at byte 64)
        uint32_t geomRefTag = readBE32u(modeData.data(), 4);
        std::string geomRefStr = tagToString(geomRefTag);

        // Parse model_permutation_data table from mode tag
        {
            int16_t permCount = readBE16s(modeData.data(), 12);
            int32_t permRelOff = readBE32s(modeData.data(), 28);
            std::vector<std::vector<uint8_t>> perms;
            if (permCount > 0 && permRelOff >= 0) {
                size_t permBase = 64 + (size_t)permRelOff;
                if (permBase <= modeData.size()) {
                    for (int pi = 0; pi < permCount; pi++) {
                        size_t pe = permBase + (size_t)pi * 64;
                        if (pe + 34 > modeData.size()) break;
                        // permutations[32] starts at byte 2 within the 64-byte struct
                        std::vector<uint8_t> matViews(32);
                        memcpy(matViews.data(), modeData.data() + pe + 2, 32);
                        perms.push_back(std::move(matViews));
                    }
                }
            }
            if (!perms.empty())
                permCache[typeTag] = std::move(perms);
        }

        const TagEntry* geomEntry = findTag(tags, GROUP_GEOM, geomRefTag);
        if (!geomEntry) {
            printf("%-8s  %-12s  %-7s  %-7s  %-7s  SPRITE (no geom tag)\n",
                   tagStr.c_str(), geomRefStr.c_str(), "-", "-", "-");
            spritesSkipped++;
            continue;
        }

        std::vector<uint8_t> geomData;
        if (!readTagData(*geomEntry, geomData)) {
            printf("%-8s  %-12s  %-7s  %-7s  %-7s  ERROR reading geom\n",
                   tagStr.c_str(), geomRefStr.c_str(), "-", "-", "-");
            continue;
        }

        Geometry g;
        if (!parseGeometry(geomData, g)) {
            printf("%-8s  %-12s  %-7s  %-7s  %-7s  ERROR parsing geom\n",
                   tagStr.c_str(), geomRefStr.c_str(), "-", "-", "-");
            continue;
        }

        // Geometry stores a texture collection reference. Some maps point this
        // at a core texture-reference tag rather than directly at the .256 tag.
        const uint32_t originalCollectionRef = g.collectionRefTag;
        const TagEntry* collEntry = findTextureCollection(tags, originalCollectionRef);
        if (collEntry) {
            g.collectionRefTag = collEntry->subgroupTag;
            std::vector<uint8_t> collData;
            if (readTagData(*collEntry, collData)) {
                std::string texDir = outFolder + "/assets/models/textures";
                makeDirs(texDir);
                std::string collStr = tagToFileStem(g.collectionRefTag);
                for (auto& mat: g.materials) {
                    int numViews = dot256SequenceViewCount(collData, mat.sequenceIndex);
                    for (int vi = 0; vi < numViews; vi++) {
                        std::string pngName = collStr + "_" + std::to_string(mat.sequenceIndex)
                                            + "_" + std::to_string(vi) + ".png";
                        std::string pngPath = texDir + "/" + pngName;
                        if (overwriteTextures || !fs::exists(pngPath))
                            extractDot256Texture(collData, mat.sequenceIndex, vi, pngPath);
                        if (vi == 0 && fs::exists(pngPath))
                            mat.texturePng = pngPath;
                    }
                }
                collCache[typeTag] = std::move(collData);
            }
        }

        // Count valid tris
        int validTris=0;
        for (const auto& s: g.surfaces) {
            bool ok=true;
            for (int c=0; c<3; c++) {
                int16_t vi=s.corners[c].vertexIndex;
                if (vi<0||vi>=(int16_t)g.vertices.size()) { ok=false; break; }
            }
            if (ok) validTris++;
        }

        std::string objPath = outFolder+"/assets/models/"+tagFile+".obj";
        std::string mtlPath = outFolder+"/assets/models/"+tagFile+".mtl";

        bool ok = exportOBJ(objPath, mtlPath, tagStr, g);
        std::string collNote = tagToString(g.collectionRefTag);
        if (g.collectionRefTag != originalCollectionRef)
            collNote += " via " + tagToString(originalCollectionRef);
        printf("%-8s  %-12s  %-7d  %-7d  %-7d  %s (coll: %s)\n",
               tagStr.c_str(), geomRefStr.c_str(),
               (int)g.vertices.size(), (int)g.surfaces.size(), validTris,
               ok ? "OK" : "FAILED",
               collNote.c_str());
        if (ok) {
            modelsExported++;
            geomCache[typeTag] = std::move(g);
        }
    }

    printf("\n%d models exported, %d sprite types skipped\n\n",
           modelsExported, spritesSkipped);

    // ---- Write placement.json + build combined instance list ----
    std::string json;
    json += "{\n";
    json += "  \"instances\": [\n";

    std::vector<PlacedInstance> placedInstances;
    bool firstInst=true;

    printf("%-5s  %-6s  %-8s  %-8s  %-8s  %-8s  %s\n",
           "inst","pal","tag","x","y","z","facing");
    printf("%s\n", std::string(60,'-').c_str());

    for (size_t ii=0; ii<instances.size(); ii++) {
        const auto& inst = instances[ii];
        // Direct model placements. Model-animation placements are handled below
        // after the static instance list is closed.
        if (inst.markerType != 6) continue;

        // Resolve type-relative palette_index -> UnitType
        // palette_index is the Nth entry in the palette where entry.w0 == markerType
        auto itTR = typeRelIdx.find(inst.markerType);
        if (itTR == typeRelIdx.end()) continue;
        const auto& sameTypeEntries = itTR->second;
        if ((size_t)inst.paletteIdx >= sameTypeEntries.size()) continue;
        const UnitType* t = sameTypeEntries[inst.paletteIdx];
        if (t->w0 != 6) continue;  // should always be true since markerType==6

        std::string tagStr=tagToString(t->typeTag);
        std::string tagFile=tagToFileStem(t->typeTag);

        // world_distance uses WORLD_FRACTIONAL_BITS=9 (WORLD_ONE=512).
        // posX and posY are the horizontal cell axes; posZ is height (matches terrain physical_height).
        float cellX = (float)inst.posX / WORLD_ONE;
        float cellY = (float)inst.posY / WORLD_ONE;
        float cellZ = (float)inst.posZ / WORLD_ONE;
        float facingDeg = (float)(((double)inst.yaw / 65536.0) * 360.0);

        printf("%-5zu  %-6d  %-8s  %-8.2f  %-8.2f  %-8.2f  %.1f  perm=%d\n",
               ii, (int)inst.paletteIdx, tagStr.c_str(),
               cellX, cellY, cellZ, facingDeg, (int)inst.permutationIndex);

        if (!firstInst) json += ",\n";
        firstInst=false;

        json += "    {\"tag\": ";
        appendJsonString(json, tagStr);
        char buf[256];
        snprintf(buf,sizeof(buf),
                 ", \"x\": %.4f, \"y\": %.4f, \"z\": %.4f"
                 ", \"facing_deg\": %.4f"
                 ", \"permutation\": %d"
                 ", \"pal_idx\": %d, \"marker_idx\": %zu, \"identifier\": %u"
                 ", \"pitch_raw\": %u, \"roll_raw\": %u}",
                 cellX, cellY, cellZ, facingDeg,
                 (int)inst.permutationIndex, (int)inst.paletteIdx, ii,
                 (unsigned)inst.identifier,
                 (unsigned)inst.pitch, (unsigned)inst.roll);
        json += buf;

        // Accumulate for combined OBJ if geom was loaded
        auto git = geomCache.find(t->typeTag);
        if (git != geomCache.end()) {
            const Geometry& g = git->second;
            std::vector<std::string> permTexPngs;
            std::vector<bool> hiddenMaterials;

            // Emit per-instance OBJ+MTL with permutation-specific textures
            auto cit = collCache.find(t->typeTag);
            if (cit != collCache.end()) {
                const std::vector<uint8_t>& collData = cit->second;
                std::string collStr = tagToFileStem(g.collectionRefTag);
                std::string texDir = outFolder + "/assets/models/textures";
                int perm = (int)inst.permutationIndex;

                std::string instObjPath = outFolder + "/assets/models/" + tagFile + "_" + std::to_string(ii) + ".obj";
                std::string instMtlPath = outFolder + "/assets/models/" + tagFile + "_" + std::to_string(ii) + ".mtl";

                // Build a copy of the geometry with per-permutation texture paths.
                // Look up view index per material from the mode tag permutation table.
                // permutations[matIndex] = view index; 0xFF = material not rendered.
                Geometry instGeom = g;
                auto pit = permCache.find(t->typeTag);
                const std::vector<uint8_t>* matViews = nullptr;
                if (pit != permCache.end() && perm < (int)pit->second.size())
                    matViews = &pit->second[perm];

                for (int mi = 0; mi < (int)instGeom.materials.size(); mi++) {
                    auto& mat = instGeom.materials[mi];
                    bool hidden = matViews && mi < (int)matViews->size() && (*matViews)[mi] == 0xFF;
                    hiddenMaterials.push_back(hidden);
                    if (hidden) {
                        mat.texturePng.clear();
                        permTexPngs.push_back("");
                        continue;
                    }
                    int vi = 0;
                    if (matViews && mi < (int)matViews->size())
                        vi = (int)(*matViews)[mi];
                    int nv = dot256SequenceViewCount(collData, mat.sequenceIndex);
                    if (nv > 0) vi %= nv;
                    else vi = 0;
                    std::string pngName = collStr + "_" + std::to_string(mat.sequenceIndex)
                                        + "_" + std::to_string(vi) + ".png";
                    std::string pngPath = texDir + "/" + pngName;
                    mat.texturePng = fs::exists(pngPath) ? pngPath : "";
                    permTexPngs.push_back(mat.texturePng);
                }
                WorldTransform wt;
                wt.cellX     = cellX;
                wt.cellY     = cellY;
                wt.cellZ     = cellZ;
                wt.facingRad = (facingDeg - 90.0f) * (float)(PI / 180.0);
                wt.halfW     = (float)(mh.submeshW * 32) * 0.5f;
                wt.halfH     = (float)(mh.submeshH * 32) * 0.5f;
                exportOBJ(instObjPath, instMtlPath, tagStr, instGeom,
                          worldSpace ? &wt : nullptr, &hiddenMaterials);
            }

            PlacedInstance pi;
            pi.geom       = &git->second;
            pi.typeTag    = tagStr;
            pi.origin     = ModelOrigin{git->second.cx, git->second.cy, git->second.cz};
            pi.cellX      = cellX;
            pi.cellY      = cellY;
            pi.cellZ      = cellZ;
            pi.facingRad  = (facingDeg - 90.0f) * (float)(PI / 180.0);
            pi.halfW      = (float)(mh.submeshW * 32) * 0.5f;
            pi.halfH      = (float)(mh.submeshH * 32) * 0.5f;
            pi.materialTexturePngs = permTexPngs;
            pi.hiddenMaterials = hiddenMaterials;
            placedInstances.push_back(pi);
        }
    }
    printf("\n");

    json += "\n  ],\n";
    json += "  \"animations\": [\n";

    std::string animationsJson;
    animationsJson += "{\n";
    animationsJson += "  \"map_width_cells\": " + std::to_string(mh.submeshW * 32) + ",\n";
    animationsJson += "  \"map_height_cells\": " + std::to_string(mh.submeshH * 32) + ",\n";
    animationsJson += "  \"coordinate_space\": ";
    appendJsonString(animationsJson, worldSpace ? "world" : "local");
    animationsJson += ",\n";
    animationsJson += "  \"animations\": [\n";

    bool firstAnim = true;
    bool firstAnimManifest = true;
    for (size_t ii = 0; ii < instances.size(); ii++) {
        const auto& inst = instances[ii];
        if (inst.markerType != 11) continue;

        auto itTR = typeRelIdx.find(inst.markerType);
        if (itTR == typeRelIdx.end()) continue;
        const auto& sameTypeEntries = itTR->second;
        if ((size_t)inst.paletteIdx >= sameTypeEntries.size()) continue;
        const UnitType* t = sameTypeEntries[inst.paletteIdx];
        if (t->w0 != 11) continue;

        auto ait = animCache.find(t->typeTag);
        if (ait == animCache.end()) continue;
        const AnimationDef& anim = ait->second;

        std::string animStr = tagToString(t->typeTag);
        std::string animFile = tagToFileStem(t->typeTag);
        float cellX = (float)inst.posX / WORLD_ONE;
        float cellY = (float)inst.posY / WORLD_ONE;
        float cellZ = (float)inst.posZ / WORLD_ONE;
        float facingDeg = (float)(((double)inst.yaw / 65536.0) * 360.0);

        printf("anim %-5zu  %-6d  %-8s  %-8.2f  %-8.2f  %-8.2f  %.1f  frames=%zu\n",
               ii, (int)inst.paletteIdx, animStr.c_str(),
               cellX, cellY, cellZ, facingDeg, anim.frames.size());

        if (!firstAnim) json += ",\n";
        firstAnim = false;
        json += "    {\"tag\": ";
        appendJsonString(json, animStr);
        char buf[256];
        snprintf(buf, sizeof(buf),
                 ", \"x\": %.4f, \"y\": %.4f, \"z\": %.4f"
                 ", \"facing_deg\": %.4f, \"pal_idx\": %d, \"marker_idx\": %zu, \"identifier\": %u"
                 ", \"pitch_raw\": %u, \"roll_raw\": %u"
                 ", \"ticks_per_frame\": %d, \"frames\": [",
                 cellX, cellY, cellZ, facingDeg, (int)inst.paletteIdx, ii,
                 (unsigned)inst.identifier,
                 (unsigned)inst.pitch, (unsigned)inst.roll,
                 (int)anim.ticksPerFrame);
        json += buf;

        if (!firstAnimManifest) animationsJson += ",\n";
        firstAnimManifest = false;
        animationsJson += "    {\"tag\": ";
        appendJsonString(animationsJson, animStr);
        snprintf(buf, sizeof(buf),
                 ", \"x\": %.4f, \"y\": %.4f, \"z\": %.4f"
                 ", \"facing_deg\": %.4f, \"pal_idx\": %d, \"marker_idx\": %zu, \"identifier\": %u"
                 ", \"pitch_raw\": %u, \"roll_raw\": %u"
                 ", \"frame_duration_ticks\": %d, \"frames\": [",
                 cellX, cellY, cellZ, facingDeg, (int)inst.paletteIdx, ii,
                 (unsigned)inst.identifier,
                 (unsigned)inst.pitch, (unsigned)inst.roll,
                 (int)anim.ticksPerFrame);
        animationsJson += buf;

        WorldTransform wt;
        wt.cellX     = cellX;
        wt.cellY     = cellY;
        wt.cellZ     = cellZ;
        wt.facingRad = (facingDeg - 90.0f) * (float)(PI / 180.0);
        wt.halfW     = (float)(mh.submeshW * 32) * 0.5f;
        wt.halfH     = (float)(mh.submeshH * 32) * 0.5f;

        ModelOrigin animOrigin;
        bool haveAnimOrigin = false;
        if (!anim.frames.empty()) {
            auto firstGeom = geomCache.find(anim.frames[0].modelTag);
            if (firstGeom != geomCache.end()) {
                animOrigin = ModelOrigin{firstGeom->second.cx, firstGeom->second.cy, firstGeom->second.cz};
                haveAnimOrigin = true;
            }
        }

        for (size_t fi = 0; fi < anim.frames.size(); fi++) {
            const AnimationFrame& frame = anim.frames[fi];
            std::string frameTagStr = tagToString(frame.modelTag);
            std::string frameTagFile = tagToFileStem(frame.modelTag);
            std::string stem = animFile + "_" + std::to_string(ii) + "_frame"
                             + (fi < 10 ? "0" : "") + std::to_string(fi)
                             + "_" + frameTagFile;

            if (fi) json += ", ";
            json += "{\"model\": ";
            appendJsonString(json, frameTagStr);
            snprintf(buf, sizeof(buf), ", \"permutation\": %d}", (int)frame.permutationIndex);
            json += buf;

            if (fi) animationsJson += ", ";
            animationsJson += "{\"frame\": ";
            animationsJson += std::to_string(fi);
            animationsJson += ", \"model\": ";
            appendJsonString(animationsJson, frameTagStr);
            snprintf(buf, sizeof(buf), ", \"permutation\": %d, \"obj\": ", (int)frame.permutationIndex);
            animationsJson += buf;
            appendJsonString(animationsJson, stem + ".obj");
            animationsJson += ", \"mtl\": ";
            appendJsonString(animationsJson, stem + ".mtl");
            animationsJson += "}";

            auto git = geomCache.find(frame.modelTag);
            if (git == geomCache.end()) continue;

            Geometry frameGeom = git->second;
            std::vector<std::string> permTexPngs;
            std::vector<bool> hiddenMaterials;
            auto cit = collCache.find(frame.modelTag);
            auto pit = permCache.find(frame.modelTag);
            const std::vector<uint8_t>* matViews = nullptr;
            if (pit != permCache.end() &&
                frame.permutationIndex >= 0 &&
                frame.permutationIndex < (int)pit->second.size()) {
                matViews = &pit->second[(size_t)frame.permutationIndex];
            }
            for (int mi = 0; mi < (int)frameGeom.materials.size(); mi++) {
                auto& mat = frameGeom.materials[mi];
                bool hidden = matViews && mi < (int)matViews->size() && (*matViews)[mi] == 0xFF;
                hiddenMaterials.push_back(hidden);
                if (hidden) {
                    mat.texturePng.clear();
                    permTexPngs.push_back("");
                    continue;
                }
                if (cit != collCache.end()) {
                    int vi = 0;
                    if (matViews && mi < (int)matViews->size())
                        vi = (int)(*matViews)[mi];
                    int nv = dot256SequenceViewCount(cit->second, mat.sequenceIndex);
                    if (nv > 0) vi %= nv;
                    else vi = 0;
                    std::string pngName = tagToFileStem(frameGeom.collectionRefTag) + "_"
                                        + std::to_string(mat.sequenceIndex) + "_"
                                        + std::to_string(vi) + ".png";
                    std::string pngPath = outFolder + "/assets/models/textures/" + pngName;
                    mat.texturePng = fs::exists(pngPath) ? pngPath : "";
                }
                permTexPngs.push_back(mat.texturePng);
            }

            exportOBJ(outFolder + "/assets/models/" + stem + ".obj",
                      outFolder + "/assets/models/" + stem + ".mtl",
                      animStr + "_" + frameTagStr,
                      frameGeom,
                      worldSpace ? &wt : nullptr,
                      &hiddenMaterials,
                      haveAnimOrigin ? &animOrigin : nullptr);

            bool includeInCombined =
                animationFrameMode == AnimationFrameMode::All ||
                (animationFrameMode == AnimationFrameMode::First && fi == 0);
            if (includeInCombined) {
                PlacedInstance pi;
                pi.geom       = &git->second;
                pi.typeTag    = animStr + "_" + frameTagStr;
                pi.origin     = haveAnimOrigin ? animOrigin : ModelOrigin{git->second.cx, git->second.cy, git->second.cz};
                pi.cellX      = cellX;
                pi.cellY      = cellY;
                pi.cellZ      = cellZ;
                pi.facingRad  = wt.facingRad;
                pi.halfW      = wt.halfW;
                pi.halfH      = wt.halfH;
                pi.materialTexturePngs = permTexPngs;
                pi.hiddenMaterials = hiddenMaterials;
                placedInstances.push_back(pi);
            }
        }
        json += "]}";
        animationsJson += "]}";
    }

    json += "\n  ]\n}\n";
    animationsJson += "\n  ]\n}\n";

    std::string placePath=outFolder+"/placement.json";
    if (writeText(placePath, json))
        printf("Placement written: %s\n", placePath.c_str());

    if (!firstAnimManifest) {
        std::string animPath = outFolder + "/assets/models/animations.json";
        if (writeText(animPath, animationsJson))
            printf("Animations written: %s\n", animPath.c_str());
    }

    exportUnitPlaceholders(outFolder, mh, instances, typeRelIdx, tags, overwriteTextures);
    exportSoundPlaceholders(outFolder, mh, instances, typeRelIdx, tags, overwriteTextures);
    exportSceneryPlaceholders(outFolder, mh, instances, typeRelIdx, tags, overwriteTextures);
    exportProjectilePlaceholders(outFolder, mh, instances, typeRelIdx);
    exportFenceConnectors(outFolder, mh, meshData, instances, typeRelIdx, tags);

    // ---- Write combined map OBJ ----
    if (!placedInstances.empty()) {
        std::string combinedObj = outFolder+"/assets/terrain/map_combined.obj";
        std::string combinedMtl = outFolder+"/assets/terrain/map_combined.mtl";
        if (exportCombinedOBJ(combinedObj, combinedMtl, placedInstances, terrainObjPath, "../models/textures/"))
            printf("Combined map:    %s (%zu instances%s)\n",
                   combinedObj.c_str(), placedInstances.size(),
                   terrainObjPath.empty() ? "" : " + terrain");
    }


    return 0;
}
