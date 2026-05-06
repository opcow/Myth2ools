// myth2_model.cpp
// Extract 3D scenery models and placement data from a Myth II mesh tag.
//
// For each scenery type (object class w0=6) that has a geom tag, emits:
//   models/<tag>.obj   — geometry with UV coordinates
//   models/<tag>.mtl   — material references
// Plus:
//   placement.json     — all scenery instance positions and facings
//
// Scenery types without a geom tag (sprites like trees, signs, farm) are
// listed in the output but skipped for 3D export.
//
// Usage:
//   myth2_model <tags_folder> <out_folder>
//
// Example:
//   myth2_model myth2_tags/small\ install out/le3e

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <wincodec.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#endif

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
#ifdef _WIN32
    IWICImagingFactory* factory = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    IStream* stream = nullptr;
    HGLOBAL hMem = nullptr;
    bool coInit = false;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) coInit = true;
    else if (hr != RPC_E_CHANGED_MODE) return false;

    auto cleanup = [&]() {
        if (props) props->Release();
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        if (factory) factory->Release();
        if (coInit) CoUninitialize();
    };

    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { cleanup(); return false; }
    hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr)) { cleanup(); return false; }
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) { cleanup(); return false; }
    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) { cleanup(); return false; }
    hr = encoder->CreateNewFrame(&frame, &props);
    if (FAILED(hr)) { cleanup(); return false; }
    hr = frame->Initialize(props);
    if (FAILED(hr)) { cleanup(); return false; }
    hr = frame->SetSize((UINT)w, (UINT)h);
    if (FAILED(hr)) { cleanup(); return false; }
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&fmt);
    if (FAILED(hr) || fmt != GUID_WICPixelFormat32bppBGRA) { cleanup(); return false; }

    std::vector<uint8_t> bgra((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        bgra[(size_t)i*4+0] = rgba[(size_t)i*4+2];
        bgra[(size_t)i*4+1] = rgba[(size_t)i*4+1];
        bgra[(size_t)i*4+2] = rgba[(size_t)i*4+0];
        bgra[(size_t)i*4+3] = rgba[(size_t)i*4+3];
    }
    hr = frame->WritePixels((UINT)h, (UINT)(w*4), (UINT)bgra.size(), bgra.data());
    if (FAILED(hr)) { cleanup(); return false; }
    hr = frame->Commit();
    if (FAILED(hr)) { cleanup(); return false; }
    hr = encoder->Commit();
    if (FAILED(hr)) { cleanup(); return false; }

    hr = GetHGlobalFromStream(stream, &hMem);
    if (FAILED(hr) || !hMem) { cleanup(); return false; }
    SIZE_T sz = GlobalSize(hMem);
    const void* mem = GlobalLock(hMem);
    if (!mem || sz == 0) { if (mem) GlobalUnlock(hMem); cleanup(); return false; }

    FILE* f = fopen(path.c_str(), "wb");
    bool ok = f && (fwrite(mem, 1, sz, f) == sz);
    if (f) fclose(f);
    GlobalUnlock(hMem);
    cleanup();
    return ok;
#else
    (void)path; (void)rgba; (void)w; (void)h;
    return false;
#endif
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
// Each image is preceded by a 52-byte bitmapinfo header:
//   [18] img_x  (int16 BE, width)
//   [20] img_y  (int16 BE, height)
//
// Palette: 2080 bytes at bulk_offset + palette_offset:
//   [0:4]   num_colors (int32 BE)
//   [32:]   color[256], each 8 bytes: r,fr,g,fg,b,fb,flag,ff

static bool extractDot256Texture(const std::vector<uint8_t>& d,
                                  int seqIndex,
                                  const std::string& outPng) {
    if (d.size() < 320) return false;
    const uint8_t* hdr = d.data();

    int32_t bulkOff    = readBE32s(hdr, 248);
    int32_t palOff     = readBE32s(hdr, 68);
    int32_t numSec     = readBE32s(hdr, 96);
    int32_t secOff     = readBE32s(hdr, 100);

    if (seqIndex < 0 || seqIndex >= numSec) return false;

    // Palette: 2080 bytes — at bulkOff + palOff, skip 32-byte palette header to reach color[0]
    size_t palAbs = (size_t)(bulkOff + palOff);
    if (palAbs + 2080 > d.size()) return false;
    const uint8_t* palData = d.data() + palAbs + 32;

    // sectioninfo for this sequence (128 bytes each)
    // [64] imagedata_offset (from bulkOff), [68] imagedata_length
    // [76] width (int16 BE), [78] height (int16 BE)
    size_t secBase = (size_t)(bulkOff + secOff);
    size_t secEntry = secBase + (size_t)seqIndex * 128;
    if (secEntry + 128 > d.size()) return false;
    int32_t imgDataOff = readBE32s(d.data(), secEntry + 64);
    int w = (int)readBE16s(d.data(), secEntry + 76);
    int h = (int)readBE16s(d.data(), secEntry + 78);
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;

    // Pixel data follows the 52-byte bitmapinfo header
    size_t pixAbs = (size_t)(bulkOff + imgDataOff) + 52;
    if (pixAbs + (size_t)w * h > d.size()) return false;
    const uint8_t* pix = d.data() + pixAbs;

    // Decode indexed pixels to RGBA using Myth palette (r,fr,g,fg,b,fb,flag,ff per entry)
    std::vector<uint8_t> rgba((size_t)w * h * 4);
    for (int i = 0; i < w * h; i++) {
        uint8_t idx = pix[i];
        const uint8_t* c = palData + (size_t)idx * 8;
        rgba[(size_t)i*4+0] = c[0]; // r
        rgba[(size_t)i*4+1] = c[2]; // g
        rgba[(size_t)i*4+2] = c[4]; // b
        rgba[(size_t)i*4+3] = 255;
    }

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
};

static bool parseMeshHeader(const std::vector<uint8_t>& d, MeshHeader& h) {
    if (d.size()<1024) return false;
    h.submeshW    = readBE16u(d.data(), 8);
    h.submeshH    = readBE16u(d.data(), 10);
    h.unitTypeCount   = readBE32u(d.data(), 0x24);
    h.unitTypeOffset  = readBE32u(d.data(), 0x28);
    h.instanceCount   = readBE32u(d.data(), 0x34);
    h.instanceOffset  = readBE32u(d.data(), 0x38);
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
//   [4:6]   type (int16)  — marker type enum (6 = _marker_model = scenery w/ geometry)
//   [6:8]   palette_index (int16)  — index into marker palette table
//   [8:10]  identifier (int16)
//   [10:12] minimum_difficulty_level (int16)
//   [12:16] position.x (int32, world_distance, WORLD_FRACTIONAL_BITS=9)
//   [16:20] position.y (int32, world_distance)
//   [20:24] position.z (int32, world_distance = height above datum)
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
// marker_data (64 bytes) — exact layout from markers.h / markers.cpp byte-swap data:
//   [0:4]   flags (uint32)
//   [4:6]   type (int16)  — marker type enum:
//             0=team, 1=scenery(collision), 3=monster, 5=effect, 6=_marker_model(placed 3D), 9=netgame, 11=local_ctrl
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
// Only markers with markerType==6 (_marker_model) are actual placed 3D scenery instances.
// Other types (1=collision marker, 3=monster with model ref) use the same struct but are not
// placed scenery.
struct ObjectInstance {
    int16_t  markerType=0;  // instance's own type: 6 = _marker_model (placed 3D scenery)
    uint16_t paletteIdx=0;  // index into marker palette table
    int32_t posX=0, posY=0, posZ=0;  // world_distance, /512 gives cell coordinate
    uint16_t yaw=0;  // 0..65535 = 0..360 degrees
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
        inst.posX       = readBE32s(d.data(), off+12);  // position.x
        inst.posY       = readBE32s(d.data(), off+16);  // position.y
        inst.posZ       = readBE32s(d.data(), off+20);  // position.z (height)
        inst.yaw        = readBE16u(d.data(), off+32);  // yaw angle
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

// ---------------------------------------------------------------------------
// OBJ / MTL export
// ---------------------------------------------------------------------------

static bool exportOBJ(const std::string& objPath, const std::string& mtlPath,
                      const std::string& typeTag, const Geometry& g) {
    // Only export surfaces with all valid vertex indices
    // Collect valid triangles
    struct Tri {
        int16_t mat;
        int16_t vi[3];
        float u[3], v[3];
    };
    std::vector<Tri> tris;
    for (const auto& s: g.surfaces) {
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
            const auto& mat = g.materials[i];
            mtl += "newmtl ";
            mtl += (mat.name.empty() ? "mat"+std::to_string(i) : mat.name);
            mtl += "\n";
            mtl += "Ka 1.0 1.0 1.0\n";
            mtl += "Kd 1.0 1.0 1.0\n";
            mtl += "Ks 0.0 0.0 0.0\n";
            mtl += "illum 1\n";
            if (!mat.texturePng.empty()) {
                mtl += "map_Kd ";
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

        // Subtract center, scale to cell units, mirror X to match map convention.
        // Emit as OBJ X/Y/Z = world X, height, world Y (Blender Z-up import).
        for (const auto& vtx: g.vertices) {
            float mx = -((vtx.x - g.cx) / WORLD_ONE);
            float my =   (vtx.y - g.cy) / WORLD_ONE;
            float mz =   (vtx.z - g.cz) / WORLD_ONE;
            char buf[80];
            snprintf(buf,sizeof(buf),"v %.6f %.6f %.6f\n", mx, mz, my);
            obj += buf;
        }
        obj += "\n";

        // UV coordinates: OBJ V is flipped relative to Myth II (Myth origin is top-left)
        for (const auto& tri: tris) {
            for (int c=0; c<3; c++) {
                char buf[80];
                snprintf(buf,sizeof(buf),"vt %.6f %.6f\n", tri.u[c], 1.0f - tri.v[c]);
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

struct PlacedInstance {
    const Geometry* geom;
    std::string typeTag;
    float cellX, cellY, cellZ;
    float facingRad;  // rotation around up axis
    float halfW, halfH; // half grid dimensions, for terrain-matching coordinate transform
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
                               const std::string& terrainObjPath = "") {
    // MTL: one entry per instance+material combination (typeTag_instIdx_matname).
    // Using per-instance names avoids any cross-instance material sharing in Blender.
    std::string mtl;
    for (size_t ii = 0; ii < instances.size(); ii++) {
        const PlacedInstance& inst = instances[ii];
        for (size_t mi = 0; mi < inst.geom->materials.size(); mi++) {
            const auto& mat = inst.geom->materials[mi];
            std::string qname = inst.typeTag + "_" + std::to_string(ii) + "_" +
                                (mat.name.empty() ? "mat" + std::to_string(mi) : mat.name);
            mtl += "newmtl " + qname + "\n";
            mtl += "Ka 1.0 1.0 1.0\nKd 1.0 1.0 1.0\nKs 0.0 0.0 0.0\nillum 1\n";
            if (!mat.texturePng.empty()) {
                mtl += "map_Kd ";
                mtl += fs::path(mat.texturePng).filename().string();
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
            float mx = -((vtx.x - inst.geom->cx) / WORLD_ONE);
            float my = (vtx.y - inst.geom->cy) / WORLD_ONE;
            float mz = (vtx.z - inst.geom->cz) / WORLD_ONE;
            // Rotate around Z (up) by facing angle
            float rx =  cosF * mx - sinF * my;
            float ry =  sinF * mx + cosF * my;
            float rz = mz;
            // Translate to placement position.
            // Terrain mesh uses vx = halfW - cellX, vz = cellY - halfH (see myth2_mesh.cpp).
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
                snprintf(buf, sizeof(buf), "vt %.6f %.6f\n", tri.u[c], 1.0f - tri.v[c]);
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

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

static void usage(const char* p) {
    fprintf(stderr,
        "Myth II 3D Model Extractor\n\n"
        "Usage:\n"
        "  %s <tags_folder> <out_folder> [terrain.obj]\n\n"
        "Arguments:\n"
        "  tags_folder   folder containing Myth II tag files (e.g. 'small install')\n"
        "  out_folder    extracted map folder (e.g. out/le3e, must contain raw/mesh_tag.bin)\n\n"
        "Output:\n"
        "  <out_folder>/models/<tag>.obj   per-type geometry\n"
        "  <out_folder>/models/<tag>.mtl   per-type material\n"
        "  <out_folder>/placement.json     all scenery instance placements\n",
        p);
}

int main(int argc, char* argv[]) {
    if (argc!=3 && argc!=4) { usage(argv[0]); return 1; }

    std::string tagsFolder   = argv[1];
    std::string outFolder    = argv[2];
    std::string terrainObjPath = argc==4 ? argv[3] : "";
    // trim trailing slashes
    while (!tagsFolder.empty() && (tagsFolder.back()=='/'||tagsFolder.back()=='\\'))
        tagsFolder.pop_back();
    while (!outFolder.empty() && (outFolder.back()=='/'||outFolder.back()=='\\'))
        outFolder.pop_back();

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

    printf("Myth II 3D Model Extractor\n==========================\n");
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

    // Also build a flat helper for scenery export (all type-6 palette entries in order)
    std::vector<const UnitType*> sceneryTypes;
    for (const auto& t: unitTypes)
        if (t.w0==6) sceneryTypes.push_back(&t);

    if (sceneryTypes.empty()) {
        printf("No scenery (w0=6) types found in this mesh.\n");
        return 0;
    }

    printf("Unit types: %u total, %u scenery\n",
           (unsigned)unitTypes.size(), (unsigned)sceneryTypes.size());

    // Scan tag files
    std::vector<TagEntry> tags;
    if (!fs::is_directory(tagsFolder)) {
        fprintf(stderr,"Not a directory: %s\n", tagsFolder.c_str()); return 1;
    }
    for (const auto& it: fs::directory_iterator(tagsFolder)) {
        if (!it.is_regular_file()) continue;
        scanTagFile(it.path().string(), tags);
    }
    if (tags.empty()) {
        fprintf(stderr,"No Myth II tag files found in %s\n", tagsFolder.c_str()); return 1;
    }
    printf("Tags loaded: %zu entries\n\n", tags.size());

    static const uint32_t GROUP_GEOM = 0x67656F6Du; // 'geom'
    static const uint32_t GROUP_MODE = 0x6d6f6465u; // 'mode'

    // No base offset needed — posX/posY are world_distance values (WORLD_FRACTIONAL_BITS=9).
    // Cell coordinate = posX >> 9 (integer part), sub-cell = posX & 511 (fraction).
    // Use float division for smooth placement: cellX = posX / WORLD_ONE.

    // Create output directories
    makeDirs(outFolder+"/models");

    // ---- Export 3D models per scenery type ----
    std::set<uint32_t> exportedTypes;
    std::map<uint32_t,Geometry> geomCache;  // typeTag -> loaded geometry
    int modelsExported=0, spritesSkipped=0;

    printf("%-8s  %-12s  %-7s  %-7s  %-7s  %s\n",
           "tag","geom_ref","verts","surfs","tris","result");
    printf("%s\n", std::string(68,'-').c_str());

    for (const auto* t: sceneryTypes) {
        std::string tagStr = tagToString(t->typeTag);
        if (exportedTypes.count(t->typeTag)) continue;
        exportedTypes.insert(t->typeTag);

        // Resolve geom via the mode tag: mode.geometry_tag -> geom subgroup.
        // Direct lookup by type_tag would find wrong geom tags from other maps
        // (e.g. 'corn' -> cornerWall_geom, 'oute' -> outerWallSep_geom).
        const TagEntry* modeEntry = findTag(tags, GROUP_MODE, t->typeTag);
        if (!modeEntry) {
            printf("%-8s  %-12s  %-7s  %-7s  %-7s  SPRITE (no mode tag)\n",
                   tagStr.c_str(), "-", "-", "-", "-");
            spritesSkipped++;
            continue;
        }
        std::vector<uint8_t> modeData;
        if (!readTagData(*modeEntry, modeData) || modeData.size() < 8) {
            printf("%-8s  %-12s  %-7s  %-7s  %-7s  ERROR reading mode\n",
                   tagStr.c_str(), "-", "-", "-", "-");
            continue;
        }
        // model_definition header: [4:8] geometry_tag (subgroup ID of the geom tag)
        uint32_t geomRefTag = readBE32u(modeData.data(), 4);
        std::string geomRefStr = tagToString(geomRefTag);

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

        // Extract textures from the .256 collection tag
        static const uint32_t GROUP_256 = 0x2E323536u; // '.256'
        const TagEntry* collEntry = findTag(tags, GROUP_256, g.collectionRefTag);
        if (collEntry) {
            std::vector<uint8_t> collData;
            if (readTagData(*collEntry, collData)) {
                std::string texDir = outFolder + "/models/textures";
                makeDirs(texDir);
                std::string collStr = tagToString(g.collectionRefTag);
                for (auto& mat: g.materials) {
                    std::string pngName = collStr + "_" + std::to_string(mat.sequenceIndex) + ".png";
                    std::string pngPath = texDir + "/" + pngName;
                    if (!fs::exists(pngPath))
                        extractDot256Texture(collData, mat.sequenceIndex, pngPath);
                    if (fs::exists(pngPath))
                        mat.texturePng = pngPath;
                }
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

        std::string objPath = outFolder+"/models/"+tagStr+".obj";
        std::string mtlPath = outFolder+"/models/"+tagStr+".mtl";

        bool ok = exportOBJ(objPath, mtlPath, tagStr, g);
        printf("  center: (%.1f, %.1f, %.1f)\n", g.cx, g.cy, g.cz);
        printf("%-8s  %-12s  %-7d  %-7d  %-7d  %s (coll: %s)\n",
               tagStr.c_str(), geomRefStr.c_str(),
               (int)g.vertices.size(), (int)g.surfaces.size(), validTris,
               ok ? "OK" : "FAILED",
               tagToString(g.collectionRefTag).c_str());
        if (ok) {
            modelsExported++;
            geomCache[t->typeTag] = std::move(g);
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
        // Only _marker_model (type 6) markers are actual placed 3D scenery instances.
        // Other types (1=collision marker, 3=monster ref) are not visual placements.
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

        // world_distance uses WORLD_FRACTIONAL_BITS=9 (WORLD_ONE=512).
        // posX and posY are the horizontal cell axes; posZ is height (matches terrain physical_height).
        float cellX = (float)inst.posX / WORLD_ONE;
        float cellY = (float)inst.posY / WORLD_ONE;
        float cellZ = (float)inst.posZ / WORLD_ONE;
        float facingDeg = (float)(((double)inst.yaw / 65536.0) * 360.0);

        printf("%-5zu  %-6d  %-8s  %-8.2f  %-8.2f  %-8.2f  %.1f\n",
               ii, (int)inst.paletteIdx, tagStr.c_str(),
               cellX, cellY, cellZ, facingDeg);

        if (!firstInst) json += ",\n";
        firstInst=false;

        json += "    {\"tag\": ";
        appendJsonString(json, tagStr);
        char buf[200];
        snprintf(buf,sizeof(buf),
                 ", \"x\": %.4f, \"y\": %.4f, \"z\": %.4f"
                 ", \"facing_deg\": %.4f"
                 ", \"pal_idx\": %d, \"marker_idx\": %zu}",
                 cellX, cellY, cellZ, facingDeg, (int)inst.paletteIdx, ii);
        json += buf;

        // Accumulate for combined OBJ if geom was loaded
        auto git = geomCache.find(t->typeTag);
        if (git != geomCache.end()) {
            PlacedInstance pi;
            pi.geom       = &git->second;
            pi.typeTag    = tagStr;
            pi.cellX      = cellX;
            pi.cellY      = cellY;
            pi.cellZ      = cellZ;
            pi.facingRad  = (facingDeg - 90.0f) * (float)(PI / 180.0);
            pi.halfW      = (float)(mh.submeshW * 32) * 0.5f;
            pi.halfH      = (float)(mh.submeshH * 32) * 0.5f;
            placedInstances.push_back(pi);
        }
    }
    printf("\n");

    json += "\n  ]\n}\n";

    std::string placePath=outFolder+"/placement.json";
    if (writeText(placePath, json))
        printf("Placement written: %s\n", placePath.c_str());

    // ---- Write combined map OBJ ----
    if (!placedInstances.empty()) {
        std::string combinedObj = outFolder+"/models/map_combined.obj";
        std::string combinedMtl = outFolder+"/models/map_combined.mtl";
        if (exportCombinedOBJ(combinedObj, combinedMtl, placedInstances, terrainObjPath))
            printf("Combined map:    %s (%zu instances%s)\n",
                   combinedObj.c_str(), placedInstances.size(),
                   terrainObjPath.empty() ? "" : " + terrain");
    }

    return 0;
}
