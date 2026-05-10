// extract_map.cpp
// Extract selected map assets from Myth II foundation/plugin tag files.
//
// First-pass outputs:
//   - raw mesh tag
//   - referenced terrain .256 tag and assembled color texture BMP
//   - referenced overhead/pregame .256 images where present
//   - map-name stli text
//   - manifest with discovered tag references
//
// Usage:
//   extract_map <tags_folder> <meshtag>
//
// Example:
//   extract_map myth2_tags 85gy

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>

#include "mesh_flags.h"
#include "png_writer.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

static uint32_t swap32(uint32_t n) {
    return ((n&0xFF000000u)>>24)|((n&0x00FF0000u)>>8)
          |((n&0x0000FF00u)<<8)|((n&0x000000FFu)<<24);
}
static uint16_t swap16(uint16_t n) { return (uint16_t)((n>>8)|(n<<8)); }
static uint32_t readBE32(const uint8_t* b, size_t o) { uint32_t v; memcpy(&v,b+o,4); return swap32(v); }
static int32_t  readBE32s(const uint8_t* b, size_t o) { return (int32_t)readBE32(b,o); }
static int16_t  readBE16s(const uint8_t* b, size_t o) { uint16_t v; memcpy(&v,b+o,2); return (int16_t)swap16(v); }

static std::string tagToString(uint32_t tag) {
    char s[5];
    s[0]=(char)((tag>>24)&0xFF);
    s[1]=(char)((tag>>16)&0xFF);
    s[2]=(char)((tag>> 8)&0xFF);
    s[3]=(char)( tag     &0xFF);
    s[4]=0;
    return std::string(s,4);
}

static bool isNullTag(uint32_t t) {
    return t==0 || t==0xFFFFFFFFu;
}

static std::vector<uint8_t> readFile(const std::string& path){
    FILE* f=fopen(path.c_str(),"rb");
    if(!f) return {};
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    if(sz<=0){ fclose(f); return {}; }
    std::vector<uint8_t> buf((size_t)sz);
    if(fread(buf.data(),1,(size_t)sz,f)!=(size_t)sz){ fclose(f); return {}; }
    fclose(f); return buf;
}

static bool writeFile(const std::string& path, const uint8_t* data, size_t len){
    FILE* f=fopen(path.c_str(),"wb");
    if(!f){ fprintf(stderr,"Cannot write: %s\n",path.c_str()); return false; }
    bool ok=(fwrite(data,1,len,f)==len);
    fclose(f);
    return ok;
}

static bool writeFile(const std::string& path, const std::vector<uint8_t>& data){
    return writeFile(path,data.data(),data.size());
}

static bool writeText(const std::string& path, const std::string& text){
    FILE* f=fopen(path.c_str(),"wb");
    if(!f){ fprintf(stderr,"Cannot write: %s\n",path.c_str()); return false; }
    bool ok=(fwrite(text.data(),1,text.size(),f)==text.size());
    fclose(f);
    return ok;
}

static void appendLE16(std::vector<uint8_t>& out, uint16_t v){
    out.push_back((uint8_t)(v & 0xFF));
    out.push_back((uint8_t)((v >> 8) & 0xFF));
}

static void appendLE32(std::vector<uint8_t>& out, uint32_t v){
    out.push_back((uint8_t)(v & 0xFF));
    out.push_back((uint8_t)((v >> 8) & 0xFF));
    out.push_back((uint8_t)((v >> 16) & 0xFF));
    out.push_back((uint8_t)((v >> 24) & 0xFF));
}

static uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t len){
    static uint32_t table[256];
    static bool ready = false;
    if(!ready){
        for(uint32_t i=0;i<256;i++){
            uint32_t c=i;
            for(int j=0;j<8;j++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i]=c;
        }
        ready=true;
    }
    crc = ~crc;
    for(size_t i=0;i<len;i++) crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

static bool readBMPToRGBA(const std::string& path, int& outW, int& outH,
                          std::vector<uint8_t>& rgba, bool blackTransparent){
    auto raw = readFile(path);
    if(raw.size() < 54 || raw[0] != 'B' || raw[1] != 'M') return false;
    uint32_t dataOff = (uint32_t)raw[10] | ((uint32_t)raw[11] << 8) | ((uint32_t)raw[12] << 16) | ((uint32_t)raw[13] << 24);
    int32_t w = (int32_t)((uint32_t)raw[18] | ((uint32_t)raw[19] << 8) | ((uint32_t)raw[20] << 16) | ((uint32_t)raw[21] << 24));
    int32_t h = (int32_t)((uint32_t)raw[22] | ((uint32_t)raw[23] << 8) | ((uint32_t)raw[24] << 16) | ((uint32_t)raw[25] << 24));
    uint16_t bits = (uint16_t)(raw[28] | ((uint16_t)raw[29] << 8));
    if(w <= 0 || h <= 0) return false;
    outW = w;
    outH = h;
    rgba.assign((size_t)w * h * 4, 0);

    if(bits == 24){
        int stride = (w * 3 + 3) & ~3;
        if(dataOff + (size_t)stride * h > raw.size()) return false;
        for(int row=0; row<h; row++){
            const uint8_t* src = raw.data() + dataOff + (size_t)(h - 1 - row) * stride;
            uint8_t* dst = rgba.data() + (size_t)row * w * 4;
            for(int x=0; x<w; x++){
                uint8_t b = src[(size_t)x * 3 + 0];
                uint8_t g = src[(size_t)x * 3 + 1];
                uint8_t r = src[(size_t)x * 3 + 2];
                dst[(size_t)x * 4 + 0] = r;
                dst[(size_t)x * 4 + 1] = g;
                dst[(size_t)x * 4 + 2] = b;
                dst[(size_t)x * 4 + 3] = (blackTransparent && r == 0 && g == 0 && b == 0) ? 0 : 255;
            }
        }
        return true;
    }

    if(bits == 4 || bits == 8){
        uint32_t dibSize = (uint32_t)raw[14] | ((uint32_t)raw[15] << 8) | ((uint32_t)raw[16] << 16) | ((uint32_t)raw[17] << 24);
        uint32_t palStart = 14 + dibSize;
        uint32_t paletteBytes = dataOff > palStart ? (dataOff - palStart) : 0;
        uint32_t paletteCount = paletteBytes / 4;
        int rawStride = (bits == 4) ? ((w + 1) / 2) : w;
        int stride = (rawStride + 3) & ~3;
        if(dataOff + (size_t)stride * h > raw.size()) return false;
        for(int row=0; row<h; row++){
            const uint8_t* src = raw.data() + dataOff + (size_t)(h - 1 - row) * stride;
            uint8_t* dst = rgba.data() + (size_t)row * w * 4;
            for(int x=0; x<w; x++){
                uint8_t idx = (bits == 4)
                    ? (uint8_t)((x & 1) ? (src[x / 2] & 0x0F) : (src[x / 2] >> 4))
                    : src[x];
                if(idx >= paletteCount) continue;
                const uint8_t* pe = raw.data() + palStart + (size_t)idx * 4;
                uint8_t b = pe[0], g = pe[1], r = pe[2];
                dst[(size_t)x * 4 + 0] = r;
                dst[(size_t)x * 4 + 1] = g;
                dst[(size_t)x * 4 + 2] = b;
                dst[(size_t)x * 4 + 3] = (blackTransparent && r == 0 && g == 0 && b == 0) ? 0 : 255;
            }
        }
        return true;
    }

    return false;
}

static bool encodePNG32ToMemory(const std::vector<uint8_t>& rgba, int w, int h, std::vector<uint8_t>& png){
    return myth2_png::encodeRGBA(png, rgba, w, h);
}

struct OraEntry {
    std::string name;
    std::vector<uint8_t> data;
    uint32_t crc = 0;
    uint32_t localOffset = 0;
};

static bool writeStoredZip(const std::string& path, std::vector<OraEntry>& entries){
    std::vector<uint8_t> out;
    out.reserve(1024);

    for(auto& e : entries){
        e.crc = crc32Update(0, e.data.data(), e.data.size());
        e.localOffset = (uint32_t)out.size();
        appendLE32(out, 0x04034B50u);
        appendLE16(out, 20);
        appendLE16(out, 0);
        appendLE16(out, 0);
        appendLE16(out, 0);
        appendLE16(out, 0);
        appendLE32(out, e.crc);
        appendLE32(out, (uint32_t)e.data.size());
        appendLE32(out, (uint32_t)e.data.size());
        appendLE16(out, (uint16_t)e.name.size());
        appendLE16(out, 0);
        out.insert(out.end(), e.name.begin(), e.name.end());
        out.insert(out.end(), e.data.begin(), e.data.end());
    }

    uint32_t centralOffset = (uint32_t)out.size();
    for(const auto& e : entries){
        appendLE32(out, 0x02014B50u);
        appendLE16(out, 20);
        appendLE16(out, 20);
        appendLE16(out, 0);
        appendLE16(out, 0);
        appendLE16(out, 0);
        appendLE16(out, 0);
        appendLE32(out, e.crc);
        appendLE32(out, (uint32_t)e.data.size());
        appendLE32(out, (uint32_t)e.data.size());
        appendLE16(out, (uint16_t)e.name.size());
        appendLE16(out, 0);
        appendLE16(out, 0);
        appendLE16(out, 0);
        appendLE16(out, 0);
        appendLE32(out, 0);
        appendLE32(out, e.localOffset);
        out.insert(out.end(), e.name.begin(), e.name.end());
    }
    uint32_t centralSize = (uint32_t)out.size() - centralOffset;
    appendLE32(out, 0x06054B50u);
    appendLE16(out, 0);
    appendLE16(out, 0);
    appendLE16(out, (uint16_t)entries.size());
    appendLE16(out, (uint16_t)entries.size());
    appendLE32(out, centralSize);
    appendLE32(out, centralOffset);
    appendLE16(out, 0);
    return writeFile(path, out);
}

static bool writeORA(const std::string& base){
    struct LayerSpec {
        const char* src;
        const char* zipName;
        const char* layerName;
        bool blackTransparent;
        bool visible;
    };
    const LayerSpec specs[] = {
        {"terrain/shadow.bmp",     "data/shadow.png",     "Shadow",      false, true},
        {"terrain/water.bmp",      "data/water.png",      "Water",       true,  true},
        {"terrain/water_mask.bmp", "data/water_mask.png", "Water Mask",  true,  true},
        {"terrain/passability.bmp","data/passability.png","Passability", true,  true},
        {"terrain/reflection.bmp", "data/reflection.png", "Reflection",  true,  true},
        {"terrain/animation.bmp",  "data/animation.png",  "Animation",   true,  true},
        {"terrain/terrain.bmp",    "data/terrain.png",    "Terrain",     false, true}
    };

    std::vector<OraEntry> entries;
    OraEntry mime;
    mime.name = "mimetype";
    const char* mt = "image/openraster";
    mime.data.assign(mt, mt + strlen(mt));
    entries.push_back(std::move(mime));

    int imageW = 0, imageH = 0;
    struct StackLayer { std::string zipName, layerName; };
    std::vector<StackLayer> stackLayers;

    for(const auto& spec : specs){
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        std::string srcPath = base + "/" + spec.src;
        if(!readBMPToRGBA(srcPath, w, h, rgba, spec.blackTransparent)) continue;
        if(imageW == 0 || imageH == 0){ imageW = w; imageH = h; }
        if(w != imageW || h != imageH) continue;

        std::vector<uint8_t> png;
        if(!encodePNG32ToMemory(rgba, w, h, png)) continue;

        OraEntry e;
        e.name = spec.zipName;
        e.data = std::move(png);
        entries.push_back(std::move(e));
        stackLayers.push_back({spec.zipName, spec.layerName});
    }

    if(imageW <= 0 || imageH <= 0 || stackLayers.empty()) return false;

    std::string stackXml;
    stackXml += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    stackXml += "<image version=\"0.0.1\" w=\"" + std::to_string(imageW) + "\" h=\"" + std::to_string(imageH) + "\">\n";
    stackXml += "  <stack name=\"root\">\n";
    for(const auto& layer : stackLayers){
        stackXml += "    <layer name=\"" + layer.layerName + "\" src=\"" + layer.zipName + "\" x=\"0\" y=\"0\" opacity=\"1.0\" visibility=\"visible\"/>\n";
    }
    stackXml += "  </stack>\n";
    stackXml += "</image>\n";

    OraEntry stack;
    stack.name = "stack.xml";
    stack.data.assign(stackXml.begin(), stackXml.end());
    entries.push_back(std::move(stack));

    return writeStoredZip(base + "/layers/map_layers.ora", entries);
}

static void makeDir(const std::string& p){
    if(p.empty()) return;
#ifdef _WIN32
    _mkdir(p.c_str());
#else
    mkdir(p.c_str(),0755);
#endif
}

static void makeDirs(const std::string& base){
    makeDir(base);
    makeDir(base+"/terrain");
    makeDir(base+"/screens");
    makeDir(base+"/sounds");
    makeDir(base+"/strings");
    makeDir(base+"/raw");
    makeDir(base+"/layers");
}

static bool copyIfExists(const std::string& src, const std::string& dst){
    auto data = readFile(src);
    if(data.empty()) return false;
    return writeFile(dst, data);
}

static bool writeLayerPNGIfExists(const std::string& src, const std::string& dst,
                                  bool blackTransparent)
{
    int w=0,h=0;
    std::vector<uint8_t> rgba;
    if(!readBMPToRGBA(src,w,h,rgba,blackTransparent)) return false;
    std::vector<uint8_t> png;
    if(!encodePNG32ToMemory(rgba,w,h,png)) return false;
    return writeFile(dst,png);
}

static void writeLayerBundle(const std::string& base){
    std::string manifest;
    manifest += "Myth II layered-friendly bundle\n";
    manifest += "================================\n\n";
    manifest += "Transparent PNG reference layers for image editors. Reimport uses the canonical files under terrain/, screens/, and strings/.\n\n";

    struct LayerItem {
        const char* src;
        const char* dst;
        const char* desc;
        bool image;
        bool blackTransparent;
    };
    const LayerItem items[] = {
        {"terrain/terrain.bmp",        "layers/01_terrain.png",      "Terrain texture",           true,  false},
        {"terrain/passability.bmp",    "layers/02_passability.png",  "Passability overlay",       true,  true},
        {"terrain/water.bmp",          "layers/03_water.png",        "Water/media reference only",true,  true},
        {"terrain/water_mask.bmp",     "layers/03_water_mask.png",   "Water OBJ texture mask",    true,  true},
        {"terrain/animation.bmp",      "layers/04_animation.png",    "Animated-water reference",  true,  true},
        {"terrain/shadow.bmp",         "layers/05_shadow.png",       "Terrain shadow map",        true,  false},
        {"terrain/reflection.bmp",     "layers/06_reflection.png",   "Reflection-area reference", true,  true},
        {"screens/overhead.bmp",       "layers/10_overhead.png",     "Overhead screen",           true,  false},
        {"screens/pregame.bmp",        "layers/11_pregame.png",      "Pregame screen",            true,  false},
        {"screens/postgame.bmp",       "layers/12_postgame.png",     "Postgame screen",           true,  false},
        {"strings/name.txt",           "layers/20_name.txt",         "Map name text",             false, false}
    };

    for(const auto& item : items){
        std::string src = base + "/" + item.src;
        std::string dst = base + "/" + item.dst;
        bool ok = item.image
            ? writeLayerPNGIfExists(src,dst,item.blackTransparent)
            : copyIfExists(src,dst);
        if(ok){
            manifest += std::string(item.dst) + "  - " + item.desc + "\n";
        }
    }

    writeText(base + "/layers/manifest.txt", manifest);
}

struct Myth2Header {
    uint16_t type=0, version=0, entryPointCount=0, tagCount=0;
    uint32_t flags=0, signature=0;
    bool isLocal=false;
    std::string name, url;
};

struct TagEntry {
    std::string sourceFile;
    std::string name;
    uint32_t groupTag=0, subgroupTag=0;
    uint32_t offset=0, size=0;
    uint16_t version=0;
};

static bool readMyth2Header(FILE* f, Myth2Header& h){
    uint8_t hdr[128];
    rewind(f);
    if(fread(hdr,1,128,f)!=128) return false;

    h.type=(uint16_t)((hdr[0]<<8)|hdr[1]);
    h.version=(uint16_t)((hdr[2]<<8)|hdr[3]);
    h.name=std::string((const char*)hdr+4, strnlen((const char*)hdr+4,32));
    h.url =std::string((const char*)hdr+36,strnlen((const char*)hdr+36,64));
    h.entryPointCount=(uint16_t)((hdr[100]<<8)|hdr[101]);
    h.tagCount=(uint16_t)((hdr[102]<<8)|hdr[103]);
    h.flags=readBE32(hdr,108);
    h.signature=readBE32(hdr,124);

    if(h.signature==0x646E6732u){ // dng2
        h.isLocal=false;
        return true;
    }
    if(readBE32(hdr,60)==0x6D746832u){ // mth2 local tag signature at tag header end
        h.isLocal=true;
        h.signature=0x6D746832u;
        h.entryPointCount=0;
        h.tagCount=1;
        return true;
    }
    return false;
}

static bool scanTagFile(const std::string& path, std::vector<TagEntry>& out){
    FILE* f=fopen(path.c_str(),"rb");
    if(!f) return false;

    Myth2Header h;
    if(!readMyth2Header(f,h)){ fclose(f); return false; }

    long fileLen=0;
    fseek(f,0,SEEK_END); fileLen=ftell(f);

    if(h.isLocal) rewind(f);
    else fseek(f,128L + (long)h.entryPointCount*112L, SEEK_SET);

    for(uint16_t i=0;i<h.tagCount;i++){
        uint8_t th[64];
        if(fread(th,1,64,f)!=64){ fclose(f); return false; }
        TagEntry e;
        e.sourceFile=path;
        e.name=std::string((const char*)th+4, strnlen((const char*)th+4,32));
        e.groupTag=readBE32(th,36);
        e.subgroupTag=readBE32(th,40);
        e.offset=readBE32(th,44);
        e.size=readBE32(th,48);
        e.version=(uint16_t)((th[56]<<8)|th[57]);
        if(h.isLocal) e.size=(uint32_t)std::max(0L,fileLen-64L);
        out.push_back(e);
    }

    fclose(f);
    return true;
}

static bool readTagData(const TagEntry& e, std::vector<uint8_t>& out){
    FILE* f=fopen(e.sourceFile.c_str(),"rb");
    if(!f) return false;
    if(fseek(f,(long)e.offset,SEEK_SET)!=0){ fclose(f); return false; }
    out.resize(e.size);
    bool ok=(e.size==0) || (fread(out.data(),1,e.size,f)==e.size);
    fclose(f);
    return ok;
}

struct MythColor { uint8_t red,fr,green,fg,blue,fb,flag,ff; };
struct Myth256Palette { int32_t colors; int32_t unk[7]; MythColor color[256]; };
static_assert(sizeof(Myth256Palette)==2080,"palette size");

static bool writeBMP8(const char* path, int w, int h,
                      const uint8_t* pal256rgb4, const std::vector<uint8_t>& px)
{
    FILE* f=fopen(path,"wb"); if(!f){fprintf(stderr,"Cannot create: %s\n",path);return false;}
    int stride=(w+3)&~3, dataOff=14+40+1024, fileSz=dataOff+stride*h;
    uint8_t fh[14]={'B','M',0,0,0,0,0,0,0,0,0,0,0,0};
    auto put32=[](uint8_t* b,int v){b[0]=v&0xFF;b[1]=(v>>8)&0xFF;b[2]=(v>>16)&0xFF;b[3]=(v>>24)&0xFF;};
    put32(fh+2,fileSz); put32(fh+10,dataOff); fwrite(fh,1,14,f);
    uint8_t ih[40]={};ih[0]=40;
    put32(ih+4,w); put32(ih+8,h); ih[12]=1; ih[14]=8; fwrite(ih,1,40,f);
    fwrite(pal256rgb4,1,1024,f);
    std::vector<uint8_t> row(stride,0);
    for(int y=h-1;y>=0;y--){memcpy(row.data(),px.data()+(size_t)y*w,w);fwrite(row.data(),1,stride,f);}
    fclose(f); return true;
}

static bool writeBMP4(const char* path, int w, int h,
                      const uint8_t* pal16bgra, const std::vector<uint8_t>& px)
{
    if((int)px.size() != w*h) return false;
    FILE* f=fopen(path,"wb"); if(!f){fprintf(stderr,"Cannot create: %s\n",path);return false;}
    int rawStride=(w+1)/2, stride=(rawStride+3)&~3, dataOff=14+40+64, fileSz=dataOff+stride*h;
    uint8_t fh[14]={'B','M',0,0,0,0,0,0,0,0,0,0,0,0};
    auto put32=[](uint8_t* b,int v){b[0]=v&0xFF;b[1]=(v>>8)&0xFF;b[2]=(v>>16)&0xFF;b[3]=(v>>24)&0xFF;};
    put32(fh+2,fileSz); put32(fh+10,dataOff); fwrite(fh,1,14,f);
    uint8_t ih[40]={}; ih[0]=40;
    put32(ih+4,w); put32(ih+8,h); ih[12]=1; ih[14]=4; put32(ih+32,16); fwrite(ih,1,40,f);
    fwrite(pal16bgra,1,64,f);
    std::vector<uint8_t> row((size_t)stride,0);
    for(int y=h-1;y>=0;y--){
        std::fill(row.begin(),row.end(),0);
        const uint8_t* src=px.data()+(size_t)y*w;
        for(int x=0;x<w;x++){
            uint8_t v=(uint8_t)(src[x]&0x0F);
            if((x&1)==0) row[(size_t)x/2]=(uint8_t)(v<<4);
            else row[(size_t)x/2]=(uint8_t)(row[(size_t)x/2]|v);
        }
        fwrite(row.data(),1,stride,f);
    }
    fclose(f); return true;
}

static bool writeBMP24(const char* path, int w, int h, const std::vector<uint8_t>& rgb)
{
    if((int)rgb.size() != w*h*3) return false;
    FILE* f=fopen(path,"wb"); if(!f){fprintf(stderr,"Cannot create: %s\n",path);return false;}
    int stride=(w*3+3)&~3, dataOff=14+40, fileSz=dataOff+stride*h;
    uint8_t fh[14]={'B','M',0,0,0,0,0,0,0,0,0,0,0,0};
    auto put32=[](uint8_t* b,int v){b[0]=v&0xFF;b[1]=(v>>8)&0xFF;b[2]=(v>>16)&0xFF;b[3]=(v>>24)&0xFF;};
    put32(fh+2,fileSz); put32(fh+10,dataOff); fwrite(fh,1,14,f);
    uint8_t ih[40]={}; ih[0]=40;
    put32(ih+4,w); put32(ih+8,h); ih[12]=1; ih[14]=24; fwrite(ih,1,40,f);
    std::vector<uint8_t> row((size_t)stride,0);
    for(int y=h-1;y>=0;y--){
        const uint8_t* src=rgb.data()+(size_t)y*w*3;
        for(int x=0;x<w;x++){
            row[(size_t)x*3+0]=src[(size_t)x*3+2];
            row[(size_t)x*3+1]=src[(size_t)x*3+1];
            row[(size_t)x*3+2]=src[(size_t)x*3+0];
        }
        fwrite(row.data(),1,stride,f);
    }
    fclose(f); return true;
}

static bool writeBMPMythPal(const char* path,int w,int h,
                            const Myth256Palette& pal,const std::vector<uint8_t>& px){
    uint8_t bp[1024];
    for(int i=0;i<256;i++){
        bp[i*4+0]=pal.color[i].blue;
        bp[i*4+1]=pal.color[i].green;
        bp[i*4+2]=pal.color[i].red;
        bp[i*4+3]=0;
    }
    return writeBMP8(path,w,h,bp,px);
}

static bool writeBMPGrey(const char* path,int w,int h,const std::vector<uint8_t>& px){
    uint8_t bp[1024];
    for(int i=0;i<256;i++){
        bp[i*4+0]=bp[i*4+1]=bp[i*4+2]=(uint8_t)i;
        bp[i*4+3]=0;
    }
    return writeBMP8(path,w,h,bp,px);
}

struct SecEntry { long absOffset; int32_t length; };

static bool readDot256FromData(const std::vector<uint8_t>& data,
                               int meshW, int meshH,
                               std::vector<SecEntry>& secs, Myth256Palette& pal)
{
    const size_t HDR=320, SECSZ=128, TEXHDR=52;
    if(data.size()<HDR+sizeof(Myth256Palette)){ fprintf(stderr,"  .256 too small\n"); return false; }
    const uint8_t* hdr=data.data();
    int32_t palOff  = readBE32s(hdr,68);
    int32_t sections= readBE32s(hdr,96);
    int32_t secOff  = readBE32s(hdr,100);
    int32_t hdrLen  = readBE32s(hdr,248);
    int32_t tagLen  = readBE32s(hdr,252);
    printf("  .256 header length: %d\n",hdrLen);
    printf("  .256 sections:      %d\n",sections);
    printf("  .256 tag length:    %d\n",tagLen);
    if(hdrLen!=320){ fprintf(stderr,"  Warning: unexpected .256 header len %d\n",hdrLen); }
    if(sections<1||sections>4000) return false;
    if((size_t)(HDR+palOff+sizeof(Myth256Palette))>data.size()) return false;
    memcpy(&pal, data.data()+HDR+palOff, sizeof(Myth256Palette));

    secs.resize(sections);
    size_t stPos=HDR+(size_t)secOff;
    for(int i=0;i<sections;i++){
        size_t ePos=stPos+(size_t)i*SECSZ;
        if(ePos+SECSZ>data.size()) return false;
        secs[i].absOffset=(long)(HDR+(long)readBE32(data.data(),ePos+64)+(long)TEXHDR);
        secs[i].length=readBE32s(data.data(),ePos+68);
    }
    return true;
}

static std::vector<uint8_t> assembleTilesFromData(const std::vector<uint8_t>& data,
                                                  const std::vector<SecEntry>& secs,
                                                  int meshW,int meshH,int secParity)
{
    int outW=meshW*256, outH=meshH*256;
    std::vector<uint8_t> out((size_t)outW*outH,0);
    for(int r=0;r<meshH;r++){
        for(int c=0;c<meshW;c++){
            int idx=2*(r*meshW+c)+secParity;
            if(idx<0||idx>=(int)secs.size()) continue;
            long src=secs[idx].absOffset;
            if(src<0||src+256L*256L>(long)data.size()) continue;
            for(int y=0;y<256;y++){
                const uint8_t* s=data.data()+src+(long)y*256;
                uint8_t* d=out.data()+(size_t)(r*256+y)*outW+(meshW-1-c)*256;
                for(int x=0;x<256;x++) d[255-x]=s[x];
            }
        }
    }
    return out;
}

static bool extractSingleImage256FromData(const std::vector<uint8_t>& data, const std::string& outPath){
    const size_t HDR=320, BITMAP_HEADER=48, BITMAP_REF_SIZE=128;
    if(data.size()<HDR+sizeof(Myth256Palette)) return false;
    int32_t bulkOff = readBE32s(data.data(),248);
    int32_t palOff = readBE32s(data.data(),68);
    int32_t bitmapCount = readBE32s(data.data(),96);
    int32_t bitmapRefsOff = readBE32s(data.data(),100);
    if(bulkOff<0 || palOff<0 || bitmapRefsOff<0 || bitmapCount<1 || bitmapCount>4096) return false;

    Myth256Palette pal;
    size_t palAbs = (size_t)bulkOff + (size_t)palOff;
    if(palAbs + sizeof(pal) > data.size()) return false;
    memcpy(&pal,data.data()+palAbs,sizeof(pal));

    int w=0, h=0;
    long imgAbs=-1;
    int32_t imgLen=0;
    size_t refBase = (size_t)bulkOff + (size_t)bitmapRefsOff;
    for(int bi=0; bi<bitmapCount; bi++){
        size_t ref = refBase + (size_t)bi * BITMAP_REF_SIZE;
        if(ref + BITMAP_REF_SIZE > data.size()) return false;
        int32_t imgDataOff = readBE32s(data.data(), ref + 64);
        int32_t imgDataLen = readBE32s(data.data(), ref + 68);
        int candidateW = (int)readBE16s(data.data(), ref + 76);
        int candidateH = (int)readBE16s(data.data(), ref + 78);
        if(candidateW<=0 || candidateW>4096 || candidateH<=0 || candidateH>4096) continue;
        long candidateImg = (long)bulkOff + (long)imgDataOff;
        if(imgDataLen <= 0 || candidateImg < 0 || candidateImg + imgDataLen > (long)data.size()) continue;
        w = candidateW;
        h = candidateH;
        imgAbs = candidateImg;
        imgLen = imgDataLen;
        break;
    }
    if(imgAbs<0 || imgLen<=0) return false;
    std::vector<uint8_t> px((size_t)w*h);
    bool decodedRows = false;
    if(imgLen >= (int32_t)(BITMAP_HEADER + h * 4 + w * h)) {
        std::vector<uint32_t> rowPtr((size_t)h + 1);
        for(int y=0; y<h; y++)
            rowPtr[(size_t)y] = readBE32(data.data(), (size_t)imgAbs + BITMAP_HEADER + (size_t)y * 4);
        uint32_t firstRowOffset = (uint32_t)(BITMAP_HEADER + h * 4);
        if(rowPtr[0] >= firstRowOffset) {
            uint32_t virtualBase = rowPtr[0] - firstRowOffset;
            rowPtr[(size_t)h] = virtualBase + (uint32_t)imgLen;
            decodedRows = true;
            for(int y=0; y<h && decodedRows; y++) {
                if(rowPtr[(size_t)y] < virtualBase || rowPtr[(size_t)y + 1] < rowPtr[(size_t)y]) {
                    decodedRows = false;
                    break;
                }
                size_t rowStart = (size_t)(rowPtr[(size_t)y] - virtualBase);
                size_t rowEnd = (size_t)(rowPtr[(size_t)y + 1] - virtualBase);
                if(rowStart > rowEnd || rowEnd > (size_t)imgLen) {
                    decodedRows = false;
                    break;
                }
                const uint8_t* row = data.data() + (size_t)imgAbs + rowStart;
                size_t rowLen = rowEnd - rowStart;
                if(rowLen == (size_t)w) {
                    memcpy(px.data() + (size_t)y * w, row, (size_t)w);
                    continue;
                }
                if(rowLen == (size_t)w * 2) {
                    uint8_t* dst = px.data() + (size_t)y * w;
                    for(int x=0; x<w; x++)
                        dst[x] = row[(size_t)x * 2 + 1];
                    continue;
                }
                if(rowLen < 4) continue;
                int spanCount = (int)readBE16s(row, 0);
                int pixelCount = (int)readBE16s(row, 2);
                size_t spanTableLen = 4 + (size_t)spanCount * 4;
                if(spanCount < 0 || pixelCount < 0 || spanTableLen + (size_t)pixelCount > rowLen) {
                    decodedRows = false;
                    break;
                }
                int summedPixels = 0;
                for(int si=0; si<spanCount; si++) {
                    int x0 = (int)readBE16s(row, 4 + (size_t)si * 4);
                    int x1 = (int)readBE16s(row, 6 + (size_t)si * 4);
                    if(x0 < 0 || x1 < x0 || x1 > w) {
                        decodedRows = false;
                        break;
                    }
                    summedPixels += x1 - x0;
                }
                if(!decodedRows || summedPixels != pixelCount) {
                    decodedRows = false;
                    break;
                }
                const uint8_t* rowPixels = row + spanTableLen;
                int pi = 0;
                for(int si=0; si<spanCount; si++) {
                    int x0 = (int)readBE16s(row, 4 + (size_t)si * 4);
                    int x1 = (int)readBE16s(row, 6 + (size_t)si * 4);
                    for(int x=x0; x<x1; x++)
                        px[(size_t)y * w + x] = rowPixels[pi++];
                }
            }
        }
    }
    if(!decodedRows) {
        long pixOff = imgAbs + 52;
        if(pixOff<0 || pixOff+(long)w*h>(long)data.size()) return false;
        memcpy(px.data(),data.data()+pixOff,(size_t)w*h);
    }
    return writeBMPMythPal(outPath.c_str(),w,h,pal,px);
}

static std::string sanitizeName(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for(char ch : s) {
        unsigned char c = (unsigned char)ch;
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.') {
            out.push_back((char)c);
        } else if(c == ' ' || c == '\t') {
            out.push_back('_');
        }
    }
    if(out.empty()) out = "unnamed";
    if(out.size() > 80) out.resize(80);
    return out;
}

static std::string fixedString(const uint8_t* p, size_t n) {
    size_t len = 0;
    while(len < n && p[len] != 0) len++;
    return std::string((const char*)p, len);
}

static std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for(char ch : s) {
        unsigned char c = (unsigned char)ch;
        switch(c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if(c < 32) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
                break;
        }
    }
    return out;
}

static bool decodeBitmap256FromData(const std::vector<uint8_t>& data, int bi,
                                    Myth256Palette& pal, int& w, int& h,
                                    std::vector<uint8_t>& px,
                                    std::string* bitmapName = nullptr) {
    const size_t HDR=320, BITMAP_META_SIZE=52, BITMAP_REF_SIZE=128;
    if(data.size()<HDR+sizeof(Myth256Palette)) return false;
    int32_t bulkOff = readBE32s(data.data(),248);
    int32_t palOff = readBE32s(data.data(),68);
    int32_t bitmapCount = readBE32s(data.data(),96);
    int32_t bitmapRefsOff = readBE32s(data.data(),100);
    if(bulkOff<0 || palOff<0 || bitmapRefsOff<0 || bitmapCount<1 || bitmapCount>4096) return false;
    if(bi < 0 || bi >= bitmapCount) return false;

    size_t palAbs = (size_t)bulkOff + (size_t)palOff;
    if(palAbs + sizeof(pal) > data.size()) return false;
    memcpy(&pal,data.data()+palAbs,sizeof(pal));

    size_t ref = (size_t)bulkOff + (size_t)bitmapRefsOff + (size_t)bi * BITMAP_REF_SIZE;
    if(ref + BITMAP_REF_SIZE > data.size()) return false;
    if(bitmapName) *bitmapName = fixedString(data.data() + ref, 64);
    int32_t imgDataOff = readBE32s(data.data(), ref + 64);
    int32_t imgDataLen = readBE32s(data.data(), ref + 68);
    w = (int)readBE16s(data.data(), ref + 76);
    h = (int)readBE16s(data.data(), ref + 78);
    if(w<=0 || w>4096 || h<=0 || h>4096) return false;
    long imgAbs = (long)bulkOff + (long)imgDataOff;
    if(imgDataLen <= 0 || imgAbs < 0 || imgAbs + imgDataLen > (long)data.size()) return false;

    px.assign((size_t)w*h, 0);
    uint16_t flags = (uint16_t)readBE16s(data.data(), (size_t)imgAbs + 6);
    bool compressed1Bit = (flags & 0x0004u) != 0;
    bool compressed4Bit = (flags & 0x0010u) != 0;
    bool noRowAddressTable = (flags & 0x0020u) != 0;

    if(compressed1Bit || compressed4Bit) {
        size_t addressTableSize = noRowAddressTable ? 0 : (size_t)std::max(0, h - 1) * 4u;
        size_t streamStart = (size_t)imgAbs + BITMAP_META_SIZE + addressTableSize;
        size_t streamEnd = (size_t)imgAbs + (size_t)imgDataLen;
        size_t pos = streamStart;
        for(int y=0; y<h; y++) {
            if(pos + 4 > streamEnd) return false;
            int spanCount = (int)readBE16s(data.data(), pos);
            int pixelCount = (int)readBE16s(data.data(), pos + 2);
            if(spanCount < 0 || pixelCount < 0 || spanCount > w || pixelCount > w) return false;
            size_t spanTable = pos + 4;
            size_t pixelStart = spanTable + (size_t)spanCount * 4u;
            size_t pixelBytes = (size_t)pixelCount * (compressed4Bit ? 2u : 1u);
            if(pixelStart + pixelBytes > streamEnd) return false;
            int pi = 0;
            for(int si=0; si<spanCount; si++) {
                int x0 = (int)readBE16s(data.data(), spanTable + (size_t)si * 4u);
                int x1 = (int)readBE16s(data.data(), spanTable + (size_t)si * 4u + 2u);
                if(x0 < 0 || x1 < x0 || x1 > w) return false;
                for(int x=x0; x<x1; x++) {
                    size_t p = pixelStart + (size_t)pi * (compressed4Bit ? 2u : 1u);
                    px[(size_t)y * w + x] = compressed4Bit ? data[p + 1] : data[p];
                    pi++;
                }
            }
            if(pi != pixelCount) return false;
            pos = pixelStart + pixelBytes;
            if(flags & 0x0040u) pos = (pos + 1u) & ~1u;
        }
        return true;
    }

    // Uncompressed layout matches extractSingleImage256FromData: a 48-byte
    // bitmap header followed by h row pointers (BE32 each), then the pixel/
    // span data. The on-disk row pointers are runtime virtual addresses; the
    // engine (re)normalises them by subtracting (rowPtr[0] - firstRowOffset).
    const size_t BITMAP_HEADER = 48;
    bool decodedRows = false;
    if(imgDataLen >= (int32_t)(BITMAP_HEADER + h * 4 + w * h)) {
        std::vector<uint32_t> rowPtr((size_t)h + 1);
        for(int y=0; y<h; y++)
            rowPtr[(size_t)y] = readBE32(data.data(), (size_t)imgAbs + BITMAP_HEADER + (size_t)y * 4);
        uint32_t firstRowOffset = (uint32_t)(BITMAP_HEADER + h * 4);
        if(rowPtr[0] >= firstRowOffset) {
            uint32_t virtualBase = rowPtr[0] - firstRowOffset;
            rowPtr[(size_t)h] = virtualBase + (uint32_t)imgDataLen;
            decodedRows = true;
            for(int y=0; y<h && decodedRows; y++) {
                if(rowPtr[(size_t)y] < virtualBase || rowPtr[(size_t)y + 1] < rowPtr[(size_t)y]) {
                    decodedRows = false;
                    break;
                }
                size_t rowStart = (size_t)(rowPtr[(size_t)y] - virtualBase);
                size_t rowEnd = (size_t)(rowPtr[(size_t)y + 1] - virtualBase);
                if(rowStart > rowEnd || rowEnd > (size_t)imgDataLen) {
                    decodedRows = false;
                    break;
                }
                const uint8_t* row = data.data() + (size_t)imgAbs + rowStart;
                size_t rowLen = rowEnd - rowStart;
                if(rowLen == (size_t)w) {
                    memcpy(px.data() + (size_t)y * w, row, (size_t)w);
                    continue;
                }
                if(rowLen == (size_t)w * 2) {
                    uint8_t* dst = px.data() + (size_t)y * w;
                    for(int x=0; x<w; x++) dst[x] = row[(size_t)x * 2 + 1];
                    continue;
                }
                if(rowLen < 4) continue;
                int spanCount = (int)readBE16s(row, 0);
                int pixelCount = (int)readBE16s(row, 2);
                size_t spanTableLen = 4 + (size_t)spanCount * 4;
                if(spanCount < 0 || pixelCount < 0 || spanTableLen + (size_t)pixelCount > rowLen) {
                    decodedRows = false;
                    break;
                }
                int summedPixels = 0;
                for(int si=0; si<spanCount; si++) {
                    int x0 = (int)readBE16s(row, 4 + (size_t)si * 4);
                    int x1 = (int)readBE16s(row, 6 + (size_t)si * 4);
                    if(x0 < 0 || x1 < x0 || x1 > w) {
                        decodedRows = false;
                        break;
                    }
                    summedPixels += x1 - x0;
                }
                if(!decodedRows || summedPixels != pixelCount) {
                    decodedRows = false;
                    break;
                }
                const uint8_t* rowPixels = row + spanTableLen;
                int pi = 0;
                for(int si=0; si<spanCount; si++) {
                    int x0 = (int)readBE16s(row, 4 + (size_t)si * 4);
                    int x1 = (int)readBE16s(row, 6 + (size_t)si * 4);
                    for(int x=x0; x<x1; x++) px[(size_t)y * w + x] = rowPixels[pi++];
                }
            }
        }
    }
    if(!decodedRows) {
        // Pixels follow the 48-byte header AND the h*4-byte row offset table.
        long pixOff = imgAbs + (long)(BITMAP_HEADER + h * 4);
        if(pixOff<0 || pixOff+(long)w*h>(long)data.size()) return false;
        memcpy(px.data(),data.data()+pixOff,(size_t)w*h);
    }
    return true;
}

static bool extractCollection256FromData(const std::vector<uint8_t>& data,
                                         const std::string& outDir,
                                         const char* label) {
    const size_t HDR=320, BITMAP_REF_SIZE=128, BITMAP_INSTANCE_SIZE=64, SEQUENCE_REF_SIZE=128;
    if(data.size()<HDR+sizeof(Myth256Palette)) return false;
    int32_t bulkOff = readBE32s(data.data(),248);
    int32_t bitmapCount = readBE32s(data.data(),96);
    int32_t bitmapRefsOff = readBE32s(data.data(),100);
    int32_t bitmapInstanceCount = readBE32s(data.data(),112);
    int32_t bitmapInstancesOff = readBE32s(data.data(),116);
    int32_t sequenceCount = readBE32s(data.data(),128);
    int32_t sequenceRefsOff = readBE32s(data.data(),132);
    if(bulkOff<0 || bitmapCount<1 || bitmapCount>4096 || bitmapRefsOff<0) return false;

    fs::create_directories(outDir);
    std::vector<std::string> bitmapFiles((size_t)bitmapCount);
    int written = 0;
    for(int bi=0; bi<bitmapCount; bi++) {
        Myth256Palette pal;
        int w=0, h=0;
        std::vector<uint8_t> px;
        std::string name;
        if(!decodeBitmap256FromData(data, bi, pal, w, h, px, &name)) continue;
        char prefix[32];
        std::snprintf(prefix, sizeof(prefix), "bitmap_%03d_", bi);
        std::string file = std::string(prefix) + sanitizeName(name) + ".bmp";
        if(writeBMPMythPal((outDir + "/" + file).c_str(), w, h, pal, px)) {
            bitmapFiles[(size_t)bi] = file;
            written++;
        }
    }

    std::string meta;
    meta += "{\n";
    meta += "  \"label\": \"" + jsonEscape(label ? label : "collection") + "\",\n";
    meta += "  \"bitmap_count\": " + std::to_string(bitmapCount) + ",\n";
    meta += "  \"bitmap_instance_count\": " + std::to_string(std::max(0, bitmapInstanceCount)) + ",\n";
    meta += "  \"sequence_count\": " + std::to_string(std::max(0, sequenceCount)) + ",\n";
    meta += "  \"bitmaps\": [\n";
    for(int bi=0; bi<bitmapCount; bi++) {
        size_t ref = (size_t)bulkOff + (size_t)bitmapRefsOff + (size_t)bi * BITMAP_REF_SIZE;
        std::string name;
        int w=0, h=0;
        if(ref + BITMAP_REF_SIZE <= data.size()) {
            name = fixedString(data.data() + ref, 64);
            w = (int)readBE16s(data.data(), ref + 76);
            h = (int)readBE16s(data.data(), ref + 78);
        }
        meta += "    { \"index\": " + std::to_string(bi) +
                ", \"name\": \"" + jsonEscape(name) +
                "\", \"width\": " + std::to_string(w) +
                ", \"height\": " + std::to_string(h) +
                ", \"file\": \"" + jsonEscape(bitmapFiles[(size_t)bi]) + "\" }";
        meta += (bi + 1 == bitmapCount) ? "\n" : ",\n";
    }
    meta += "  ],\n";
    meta += "  \"bitmap_instances\": [\n";
    int instLimit = 0;
    if(bitmapInstanceCount > 0 && bitmapInstanceCount < 4096 && bitmapInstancesOff >= 0) instLimit = bitmapInstanceCount;
    for(int ii=0; ii<instLimit; ii++) {
        size_t inst = (size_t)bulkOff + (size_t)bitmapInstancesOff + (size_t)ii * BITMAP_INSTANCE_SIZE;
        if(inst + BITMAP_INSTANCE_SIZE > data.size()) { instLimit = ii; break; }
        int bitmapIndex = readBE16s(data.data(), inst + 28);
        meta += "    { \"index\": " + std::to_string(ii) +
                ", \"bitmap_index\": " + std::to_string(bitmapIndex) +
                ", \"reg_x\": " + std::to_string(readBE16s(data.data(), inst + 12)) +
                ", \"reg_y\": " + std::to_string(readBE16s(data.data(), inst + 14)) +
                ", \"key_x\": " + std::to_string(readBE16s(data.data(), inst + 24)) +
                ", \"key_y\": " + std::to_string(readBE16s(data.data(), inst + 26)) + " }";
        meta += (ii + 1 == instLimit) ? "\n" : ",\n";
    }
    meta += "  ],\n";
    meta += "  \"sequences\": [\n";
    int seqLimit = 0;
    if(sequenceCount > 0 && sequenceCount < 4096 && sequenceRefsOff >= 0) seqLimit = sequenceCount;
    for(int si=0; si<seqLimit; si++) {
        size_t seqRef = (size_t)bulkOff + (size_t)sequenceRefsOff + (size_t)si * SEQUENCE_REF_SIZE;
        if(seqRef + SEQUENCE_REF_SIZE > data.size()) { seqLimit = si; break; }
        std::string name = fixedString(data.data() + seqRef, 64);
        int32_t seqOff = readBE32s(data.data(), seqRef + 64);
        int32_t seqSize = readBE32s(data.data(), seqRef + 68);
        size_t seq = (size_t)bulkOff + (size_t)seqOff;
        int views = 0, frames = 0, ticks = 0;
        if(seqOff >= 0 && seqSize >= 64 && seq + 64 <= data.size()) {
            views = readBE16s(data.data(), seq + 8);
            frames = readBE16s(data.data(), seq + 10);
            ticks = readBE16s(data.data(), seq + 12);
        }
        meta += "    { \"index\": " + std::to_string(si) +
                ", \"name\": \"" + jsonEscape(name) +
                "\", \"views\": " + std::to_string(views) +
                ", \"frames_per_view\": " + std::to_string(frames) +
                ", \"ticks_per_frame\": " + std::to_string(ticks) +
                ", \"frames\": [";
        bool frameOk = views > 0 && views < 128 && frames > 0 && frames < 4096 &&
                       seqOff >= 0 && seqSize >= 64 && seq + 64 <= data.size();
        if(frameOk) {
            size_t framePos = seq + 64;
            for(int fi=0; fi<frames; fi++) {
                if(framePos + 46 + (size_t)views * 2 > data.size()) break;
                if(fi > 0) meta += ", ";
                meta += "{ \"index\": " + std::to_string(fi) + ", \"views\": [";
                size_t viewPos = framePos + 46;
                for(int vi=0; vi<views; vi++) {
                    if(vi > 0) meta += ", ";
                    meta += std::to_string(readBE16s(data.data(), viewPos + (size_t)vi * 2));
                }
                meta += "] }";
                framePos = viewPos + (size_t)views * 2;
            }
        }
        meta += "] }";
        meta += (si + 1 == seqLimit) ? "\n" : ",\n";
    }
    meta += "  ]\n";
    meta += "}\n";
    writeText(outDir + "/collection.json", meta);
    printf("Extracted %s collection: %d/%d bitmaps -> %s\n",
           label ? label : ".256", written, bitmapCount, outDir.c_str());
    return written > 0;
}

struct MeshRefs {
    uint32_t landscapeTag=0;
    uint32_t mediaTag=0;
    int submeshW=0, submeshH=0;
    uint32_t meshOffset=0, meshSize=0;
    uint32_t dataOffset=0, dataSize=0;
    uint32_t mapDescStli=0;
    uint32_t postgameTag=0;
    uint32_t pregameTag=0;
    uint32_t overheadTag=0;
    uint32_t globalAmbientSoundTag=0;
    uint32_t narrationSoundTag=0;
    uint32_t winAmbientSoundTag=0;
    uint32_t lossAmbientSoundTag=0;
    uint32_t pregameStorylineTag=0;
    uint32_t storylineTag2=0;
    uint32_t storylineTag3=0;
    uint32_t storylineTag4=0;
};

static const uint8_t TERRAIN_TYPE_COLORS[16][3] = {
    {0x00,0x00,0xFF}, // media dwarf
    {0x00,0x00,0xB0}, // media human
    {0x00,0x00,0x80}, // media giant
    {0x00,0x00,0x40}, // media deep
    {0x80,0x00,0x00}, // sloped
    {0xFF,0x00,0x00}, // steep
    {0x00,0x00,0x00}, // grass
    {0xFF,0xFF,0x00}, // desert
    {0x20,0x20,0x20}, // rocky
    {0x80,0x40,0x00}, // marsh
    {0xA0,0xA0,0xA0}, // snow
    {0x20,0x70,0x20}, // forest
    {0xFF,0x00,0xFF}, // loathing special
    {0x00,0x00,0x00}, // unused
    {0x00,0x80,0x00}, // walking impassable
    {0x00,0xFF,0x00}  // flying impassable
};

static void makeTerrainTypeBMPPalette(uint8_t pal[64]){
    for(int i=0;i<16;i++){
        pal[(size_t)i*4+0]=TERRAIN_TYPE_COLORS[i][2];
        pal[(size_t)i*4+1]=TERRAIN_TYPE_COLORS[i][1];
        pal[(size_t)i*4+2]=TERRAIN_TYPE_COLORS[i][0];
        pal[(size_t)i*4+3]=0;
    }
}

static void makeBinaryBMPPalette(uint8_t pal[64], uint8_t r, uint8_t g, uint8_t b){
    memset(pal,0,64);
    pal[4]=b;
    pal[5]=g;
    pal[6]=r;
}

static int getCellTriType(uint16_t flags, int which){
    return which ? (flags & 0x0F) : ((flags >> 4) & 0x0F);
}

static bool isTriMedia(uint16_t flags, int which){
    return (flags & (which ? MYTH2_MESH_CELL_TRIANGLE1_IS_MEDIA_FLAG : MYTH2_MESH_CELL_TRIANGLE0_IS_MEDIA_FLAG)) != 0;
}

static void setRGB(std::vector<uint8_t>& img,int w,int x,int y,const uint8_t rgb[3]){
    size_t off=((size_t)y*w+x)*3;
    img[off+0]=rgb[0];
    img[off+1]=rgb[1];
    img[off+2]=rgb[2];
}

static void exportMeshMaps(const std::vector<uint8_t>& meshData, int submeshW, int submeshH,
                            const std::string& waterBmp, const std::string& passBmp,
                            const std::string& waterMaskBmp,
                            const std::string& animationBmp, const std::string& reflectionBmp)
{
    const int cellW=submeshW*32, cellH=submeshH*32, pixPerCell=8;
    const int outW=cellW*pixPerCell, outH=cellH*pixPerCell;
    std::vector<uint8_t> water((size_t)outW*outH,6);
    std::vector<uint8_t> waterMask((size_t)outW*outH,0);
    std::vector<uint8_t> pass((size_t)outW*outH,0);
    std::vector<uint8_t> reflection((size_t)outW*outH,0);
    std::vector<uint8_t> animation((size_t)outW*outH,0);
    uint8_t terrainTypePalette[64], waterMaskPalette[64], animationPalette[64], reflectionPalette[64];
    makeTerrainTypeBMPPalette(terrainTypePalette);
    makeBinaryBMPPalette(waterMaskPalette,0x80,0xC8,0xFF);
    makeBinaryBMPPalette(animationPalette,0xFF,0xFF,0xFF);
    makeBinaryBMPPalette(reflectionPalette,0xFF,0x80,0x00);
    for(int cy=0;cy<cellH;cy++){
        for(int cx=0;cx<cellW;cx++){
            size_t off=1024u + (size_t)(cy*cellW+cx)*12u;
            if(off+12>meshData.size()) continue;
            uint16_t flags=(uint16_t)readBE16s(meshData.data(),off+4);
            int t0=getCellTriType(flags,0), t1=getCellTriType(flags,1);
            bool w0=isTriMedia(flags,0), w1=isTriMedia(flags,1);
            bool animated=(flags & MYTH2_MESH_VERTEX_IS_ANIMATED_MEDIA_FLAG) != 0;
            bool reflective=(flags & MYTH2_MESH_CELL_HAS_REFLECTION_FLAG) != 0;
            for(int py=0;py<pixPerCell;py++){
                for(int px=0;px<pixPerCell;px++){
                    float u=((float)px+0.5f)/(float)pixPerCell;
                    float v=((float)py+0.5f)/(float)pixPerCell;
                    int tri=0;
                    if(((cx^cy)&1)!=0) tri=(u+v<1.0f)?0:1;
                    else tri=(u<v)?0:1;
                    int type=(tri==0)?t0:t1;
                    bool wet=(tri==0)?w0:w1;
                    int ox=(cellW-1-cx)*pixPerCell + (pixPerCell-1-px);
                    int oy=cy*pixPerCell+py;
                    pass[(size_t)oy*outW+ox]=(uint8_t)(type&15);
                    if(wet && type>=0 && type<4){
                        water[(size_t)oy*outW+ox]=(uint8_t)type;
                        waterMask[(size_t)oy*outW+ox]=1;
                    }
                    if(wet && animated){
                        animation[(size_t)oy*outW+ox]=1;
                    }
                    if(reflective){
                        reflection[(size_t)oy*outW+ox]=1;
                    }
                }
            }
        }
    }
    if(writeBMP4(waterBmp.c_str(),outW,outH,terrainTypePalette,water)) printf("Extracted water.bmp from mesh\n");
    if(writeBMP4(passBmp.c_str(),outW,outH,terrainTypePalette,pass)) printf("Extracted passability.bmp from mesh\n");
    if(writeBMP4(waterMaskBmp.c_str(),outW,outH,waterMaskPalette,waterMask)) printf("Extracted water_mask.bmp from mesh\n");
    if(writeBMP4(reflectionBmp.c_str(),outW,outH,reflectionPalette,reflection)) printf("Extracted reflection.bmp from mesh\n");
    if(writeBMP4(animationBmp.c_str(),outW,outH,animationPalette,animation)) printf("Extracted animation.bmp from mesh\n");
}

// Sound (.soun) tag extraction. Mirrors export_map_objects.cpp's
// exportSoundTagWavs but operates on raw tag data rather than the TagEntry
// catalogue, so we can use it directly inside extract_map's screen-side
// extraction (narration, win-ambient, loss-ambient).
static bool writeWavPCM16(const std::string& path,
                          const std::vector<int16_t>& samples,
                          uint16_t channels,
                          uint32_t sampleRate){
    if(channels == 0 || sampleRate == 0) return false;
    uint32_t dataSize = (uint32_t)(samples.size() * sizeof(int16_t));
    uint32_t byteRate = sampleRate * channels * 2;
    uint16_t blockAlign = (uint16_t)(channels * 2);

    std::vector<uint8_t> out;
    out.reserve(44 + dataSize);
    out.insert(out.end(), {'R','I','F','F'});
    appendLE32(out, 36 + dataSize);
    out.insert(out.end(), {'W','A','V','E','f','m','t',' '});
    appendLE32(out, 16);
    appendLE16(out, 1);
    appendLE16(out, channels);
    appendLE32(out, sampleRate);
    appendLE32(out, byteRate);
    appendLE16(out, blockAlign);
    appendLE16(out, 16);
    out.insert(out.end(), {'d','a','t','a'});
    appendLE32(out, dataSize);
    for(int16_t s : samples) appendLE16(out, (uint16_t)s);
    return writeFile(path, out);
}

static int clamp16(int v){
    if(v < -32768) return -32768;
    if(v > 32767) return 32767;
    return v;
}

static void decompressAppleIMAPacket(int16_t state, const uint8_t* encoded, int16_t* dst){
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
    if(index < 0) index = 0;
    if(index > 88) index = 88;
    int pred = (int)state & ~0x7F;
    int step = stepTab[index];

    for(int sample = 0; sample < 64; sample++){
        int byte = encoded[sample >> 1];
        int code = (sample & 1) ? ((byte >> 4) & 0xF) : (byte & 0xF);
        int diff = step >> 3;
        if(code & 4) diff += step;
        if(code & 2) diff += step >> 1;
        if(code & 1) diff += step >> 2;
        if(code & 8) diff = -diff;

        pred = clamp16(pred + diff);
        dst[sample] = (int16_t)pred;
        index += indexTab[code];
        if(index < 0) index = 0;
        if(index > 88) index = 88;
        step = stepTab[index];
    }
}

// Decodes a Myth II 'soun' tag's permutations into one WAV per permutation.
// Sound permutation table: numPerms × 32-byte permutation header followed by
// numPerms × 32-byte sample headers, then sample data. See
// export_map_objects.cpp:exportSoundTagWavs for the canonical implementation.
static int extractSoundTagWavs(const std::vector<uint8_t>& d,
                               const std::string& outDir,
                               const std::string& stem){
    if(d.size() < 64) return 0;
    int32_t soundOffset = readBE32s(d.data(), 20);
    int32_t numPerms    = readBE32s(d.data(), 36);
    int32_t permOffset  = readBE32s(d.data(), 40);
    int32_t permSize    = readBE32s(d.data(), 44);
    if(numPerms <= 0 || numPerms > 5) return 0;
    if(permSize != numPerms * 32) return 0;
    if(permOffset < 0 || soundOffset < 0) return 0;
    if((size_t)permOffset + (size_t)numPerms * 32 > d.size()) return 0;
    if((size_t)soundOffset + (size_t)numPerms * 32 > d.size()) return 0;

    fs::create_directories(outDir);
    int written = 0;
    size_t sampleOffset = (size_t)soundOffset + (size_t)numPerms * 32;
    for(int i = 0; i < numPerms; i++){
        size_t permAbs = (size_t)permOffset + (size_t)i * 32;
        size_t hdrAbs  = (size_t)soundOffset + (size_t)i * 32;
        std::string permName((const char*)d.data() + permAbs + 6,
                             strnlen((const char*)d.data() + permAbs + 6, 26));

        uint32_t sampleFlags     = readBE32(d.data(), hdrAbs + 0);
        uint16_t bits            = (uint16_t)readBE16s(d.data(), hdrAbs + 4);
        uint16_t physicalMinusOne= (uint16_t)readBE16s(d.data(), hdrAbs + 6);
        uint16_t channels        = (uint16_t)readBE16s(d.data(), hdrAbs + 8);
        uint32_t sampleRate      = readBE32(d.data(), hdrAbs + 12) >> 16;
        uint32_t sampleCount     = readBE32(d.data(), hdrAbs + 16);

        size_t storedSize = (sampleFlags & 1)
            ? (size_t)sampleCount * 34
            : (size_t)sampleCount << physicalMinusOne;
        if(sampleOffset + storedSize > d.size()) return written;

        std::string wavName = stem + "_" + std::to_string(i);
        std::string pretty;
        for(char c : permName){
            unsigned char ch = (unsigned char)c;
            if(ch < 32 || ch > 126 ||
               c == '<' || c == '>' || c == ':' || c == '"' ||
               c == '/' || c == '\\' || c == '|' || c == '?' || c == '*')
                pretty.push_back('_');
            else
                pretty.push_back(c);
        }
        while(!pretty.empty() && (pretty.back() == ' ' || pretty.back() == '.'))
            pretty.back() = '_';
        if(!pretty.empty()) wavName += "_" + pretty;
        wavName += ".wav";
        std::string wavPath = outDir + "/" + wavName;

        bool ok = false;
        if((sampleFlags & 1) && bits == 16 && channels >= 1 && channels <= 2){
            std::vector<int16_t> pcm;
            if(channels == 1){
                pcm.resize((size_t)sampleCount * 64);
                for(uint32_t p = 0; p < sampleCount; p++){
                    size_t packet = sampleOffset + (size_t)p * 34;
                    int16_t state = readBE16s(d.data(), packet);
                    decompressAppleIMAPacket(state, d.data() + packet + 2, pcm.data() + (size_t)p * 64);
                }
            } else {
                uint32_t framePackets = sampleCount / 2;
                pcm.resize((size_t)framePackets * 64 * 2);
                int16_t left[64], right[64];
                for(uint32_t p = 0; p < framePackets; p++){
                    size_t lPacket = sampleOffset + (size_t)(p * 2) * 34;
                    size_t rPacket = lPacket + 34;
                    decompressAppleIMAPacket(readBE16s(d.data(), lPacket),     d.data() + lPacket + 2, left);
                    decompressAppleIMAPacket(readBE16s(d.data(), rPacket),     d.data() + rPacket + 2, right);
                    for(int s = 0; s < 64; s++){
                        pcm[((size_t)p * 64 + s) * 2 + 0] = left[s];
                        pcm[((size_t)p * 64 + s) * 2 + 1] = right[s];
                    }
                }
            }
            ok = writeWavPCM16(wavPath, pcm, channels, sampleRate);
        } else if(!(sampleFlags & 1) && bits == 16 && channels >= 1 && channels <= 2){
            std::vector<int16_t> pcm(sampleCount);
            for(uint32_t s = 0; s < sampleCount; s++)
                pcm[s] = readBE16s(d.data(), sampleOffset + (size_t)s * 2);
            ok = writeWavPCM16(wavPath, pcm, channels, sampleRate);
        }
        if(ok) written++;
        sampleOffset += storedSize;
    }
    return written;
}

static bool parseMeshRefs(const std::vector<uint8_t>& meshData, MeshRefs& m){
    if(meshData.size()<1024) return false;
    m.landscapeTag = readBE32(meshData.data(),0);
    m.mediaTag     = readBE32(meshData.data(),4);
    m.submeshW     = readBE16s(meshData.data(),8);
    m.submeshH     = readBE16s(meshData.data(),10);
    m.meshOffset   = readBE32(meshData.data(),12);
    m.meshSize     = readBE32(meshData.data(),16);
    m.dataOffset   = readBE32(meshData.data(),24);
    m.dataSize     = readBE32(meshData.data(),28);
    m.mapDescStli  = readBE32(meshData.data(),140);
    m.postgameTag  = readBE32(meshData.data(),144);
    m.pregameTag   = readBE32(meshData.data(),148);
    m.overheadTag  = readBE32(meshData.data(),152);
    m.globalAmbientSoundTag = readBE32(meshData.data(),0x7C);
    m.narrationSoundTag     = readBE32(meshData.data(),0x100);
    m.winAmbientSoundTag    = readBE32(meshData.data(),0x104);
    m.lossAmbientSoundTag   = readBE32(meshData.data(),0x108);
    m.pregameStorylineTag   = readBE32(meshData.data(),0xB0);
    m.storylineTag2         = readBE32(meshData.data(),0xB4);
    m.storylineTag3         = readBE32(meshData.data(),0xB8);
    m.storylineTag4         = readBE32(meshData.data(),0xBC);
    return true;
}

static std::string stliToText(const std::vector<uint8_t>& raw){
    std::string s;
    s.reserve(raw.size()+8);
    for(size_t i=0;i<raw.size();i++){
        unsigned char c=raw[i];
        if(c=='\r') s.push_back('\n');
        else if(c>=0x20 || c=='\t') s.push_back((char)c);
    }
    return s;
}

static const TagEntry* findTag(const std::vector<TagEntry>& tags, uint32_t groupTag, uint32_t id){
    for(const auto& e:tags) if(e.groupTag==groupTag && e.subgroupTag==id) return &e;
    return nullptr;
}

static void usage(const char* p){
    fprintf(stderr,
        "Myth II Map Extractor\n\n"
        "Usage:\n"
        "  %s <tags_folder> <meshtag> [output_folder] [--ora]\n"
        "  %s <tags_folder> <meshtag> --out <output_folder> [--ora]\n\n"
        "  tags_folder  folder containing Myth II files like small install, large install,\n"
        "               international small install, etc.\n"
        "  meshtag      4-character mesh tag (e.g. 85gy, 85gi, 08li)\n"
        "  output_folder folder to write extracted files into (default: meshtag)\n"
        "  --ora        also emit layers/map_layers.ora from the extracted terrain layers\n",
        p,p);
}

int main(int argc, char* argv[]){
    if(argc<3){ usage(argv[0]); return 1; }
    std::string folder=argv[1];
    std::string meshTag=argv[2];
    std::string outputFolder;
    bool emitOra = false;
    for(int i=3;i<argc;i++){
        std::string a = argv[i];
        if(a == "--ora") emitOra = true;
        else if(a == "--out") {
            if(i+1>=argc){
                fprintf(stderr,"Missing output folder after %s\n",a.c_str());
                return 1;
            }
            if(!outputFolder.empty()){
                fprintf(stderr,"Output folder specified more than once\n");
                return 1;
            }
            outputFolder = argv[++i];
        }
        else if(!a.empty() && a[0] == '-') {
            fprintf(stderr,"Unknown option: %s\n",a.c_str());
            return 1;
        }
        else if(outputFolder.empty()) {
            outputFolder = a;
        }
        else {
            fprintf(stderr,"Unexpected argument: %s\n",a.c_str());
            return 1;
        }
    }
    if(meshTag.size()!=4){ fprintf(stderr,"Error: meshtag must be 4 chars\n"); return 1; }
    if(outputFolder.empty()) outputFolder = meshTag;

    std::vector<TagEntry> tags;
    for(const auto& it: fs::directory_iterator(folder)){
        if(!it.is_regular_file()) continue;
        scanTagFile(it.path().string(), tags);
    }
    if(tags.empty()){ fprintf(stderr,"No Myth II tag files found in %s\n",folder.c_str()); return 1; }

    uint32_t meshId=((uint32_t)(uint8_t)meshTag[0]<<24)|((uint32_t)(uint8_t)meshTag[1]<<16)
                   |((uint32_t)(uint8_t)meshTag[2]<<8)|((uint32_t)(uint8_t)meshTag[3]);

    const TagEntry* meshEntry=findTag(tags,0x6D657368u,meshId); // mesh
    if(!meshEntry){ fprintf(stderr,"Mesh tag not found: %s\n",meshTag.c_str()); return 1; }

    printf("Myth II Map Extractor\n====================\n");
    printf("Tags folder: %s\n",folder.c_str());
    printf("Mesh tag:    %s\n",meshTag.c_str());
    printf("Output:      %s\n",outputFolder.c_str());
    printf("Mesh source: %s\n\n",meshEntry->sourceFile.c_str());

    std::vector<uint8_t> meshData;
    if(!readTagData(*meshEntry,meshData)){ fprintf(stderr,"Failed to read mesh data\n"); return 1; }

    MeshRefs refs;
    if(!parseMeshRefs(meshData,refs)){ fprintf(stderr,"Failed to parse mesh header\n"); return 1; }

    std::string base=outputFolder;
    makeDirs(base);

    writeFile(base+"/raw/mesh_tag.bin", meshData);

    printf("Submesh dimensions: %d x %d\n",refs.submeshW,refs.submeshH);
    printf("Landscape tag:      %s\n",tagToString(refs.landscapeTag).c_str());
    printf("Media tag:          %s\n",tagToString(refs.mediaTag).c_str());
    printf("Map name stli:      %s\n",tagToString(refs.mapDescStli).c_str());
    printf("Pregame .256:       %s\n",tagToString(refs.pregameTag).c_str());
    printf("Overhead .256:      %s\n",tagToString(refs.overheadTag).c_str());
    printf("Postgame .256:      %s\n\n",tagToString(refs.postgameTag).c_str());

    exportMeshMaps(meshData,refs.submeshW,refs.submeshH,
                   base+"/terrain/water.bmp",
                   base+"/terrain/passability.bmp",
                   base+"/terrain/water_mask.bmp",
                   base+"/terrain/animation.bmp",
                   base+"/terrain/reflection.bmp");

    if(!isNullTag(refs.mapDescStli)){
        const TagEntry* stli=findTag(tags,0x73746C69u,refs.mapDescStli); // stli
        if(stli){
            std::vector<uint8_t> raw;
            if(readTagData(*stli,raw)){
                writeFile(base+"/strings/name_tag.bin",raw);
                std::string txt=stliToText(raw);
                writeText(base+"/strings/name.txt",txt);
                printf("Extracted name stli from %s\n",stli->sourceFile.c_str());
            }
        }
    }

    const TagEntry* terrain=findTag(tags,0x2E323536u,refs.landscapeTag); // .256
    if(terrain){
        std::vector<uint8_t> terrData;
        if(readTagData(*terrain,terrData)){
            writeFile(base+"/terrain/terrain_tag.bin",terrData);
            std::vector<SecEntry> secs;
            Myth256Palette pal;
            if(readDot256FromData(terrData,refs.submeshW,refs.submeshH,secs,pal)){
                std::vector<uint8_t> colorPx=assembleTilesFromData(terrData,secs,refs.submeshW,refs.submeshH,0);
                if(writeBMPMythPal((base+"/terrain/terrain.bmp").c_str(),refs.submeshW*256,refs.submeshH*256,pal,colorPx))
                    printf("Extracted terrain terrain.bmp from %s\n",terrain->sourceFile.c_str());
                std::vector<uint8_t> shadowPx=assembleTilesFromData(terrData,secs,refs.submeshW,refs.submeshH,1);
                if(writeBMPGrey((base+"/terrain/shadow.bmp").c_str(),refs.submeshW*256,refs.submeshH*256,shadowPx))
                    printf("Extracted terrain shadow.bmp from %s\n",terrain->sourceFile.c_str());
            }
        }
    }

    auto extractScreen=[&](uint32_t id, const std::string& outBmp, const std::string& outBin, const char* label){
        if(isNullTag(id)) return;
        const TagEntry* e=findTag(tags,0x2E323536u,id);
        if(!e) return;
        std::vector<uint8_t> data;
        if(!readTagData(*e,data)) return;
        writeFile(outBin,data);
        if(extractSingleImage256FromData(data,outBmp))
            printf("Extracted %s from %s\n",label,e->sourceFile.c_str());
        extractCollection256FromData(data, base + "/screens/" + std::string(label) + "_collection", label);
    };

    extractScreen(refs.overheadTag, base+"/screens/overhead.bmp", base+"/screens/overhead_tag.bin", "overhead");
    extractScreen(refs.pregameTag,  base+"/screens/pregame.bmp",  base+"/screens/pregame_tag.bin",  "pregame");
    extractScreen(refs.postgameTag, base+"/screens/postgame.bmp", base+"/screens/postgame_tag.bin", "postgame");

    auto extractSound = [&](uint32_t id, const std::string& outBin, const std::string& wavDir,
                            const std::string& wavStem, const char* label){
        if(isNullTag(id)) return;
        const TagEntry* e = findTag(tags, 0x736F756Eu, id); // 'soun'
        if(!e) return;
        std::vector<uint8_t> data;
        if(!readTagData(*e, data) || data.empty()) return;
        writeFile(outBin, data);
        int wavs = extractSoundTagWavs(data, wavDir, wavStem);
        printf("Extracted %s sound (%s) from %s -> %d wav(s)\n",
               label, tagToString(id).c_str(), e->sourceFile.c_str(), wavs);
    };

    // Per-map sound tags. The mesh format reserves win_ambient_sound_tag and
    // loss_ambient_sound_tag for postgame audio, but Bungie's stock vanilla
    // maps leave both null — postgame audio in Myth II is game-wide rather
    // than per-map, so the extractor only fires for narration in practice.
    // The win/loss slots are still wired so custom maps that do use them
    // round-trip correctly.
    extractSound(refs.narrationSoundTag,
                 base + "/sounds/narration_tag.bin",
                 base + "/sounds", "narration", "narration");
    extractSound(refs.winAmbientSoundTag,
                 base + "/sounds/win_ambient_tag.bin",
                 base + "/sounds", "win_ambient", "win ambient");
    extractSound(refs.lossAmbientSoundTag,
                 base + "/sounds/loss_ambient_tag.bin",
                 base + "/sounds", "loss_ambient", "loss ambient");

    // 'text' group tags (group=0x74657874): scrolling-prologue / storyline
    // narration text. The mesh references a primary pregame_storyline_tag
    // plus three additional slots (storyline_string_tags_2/3/4) per
    // mesh_tag.py from reference_source/mythextract.
    auto extractText = [&](uint32_t id, const std::string& outBin, const std::string& outTxt,
                           const char* label){
        if(isNullTag(id)) return;
        const TagEntry* e = findTag(tags, 0x74657874u, id); // 'text'
        if(!e) return;
        std::vector<uint8_t> data;
        if(!readTagData(*e, data) || data.empty()) return;
        writeFile(outBin, data);
        // text tags are CR-separated narration text — same shape as stli, so
        // stliToText already filters non-printables and converts CR→LF.
        std::string txt = stliToText(data);
        writeText(outTxt, txt);
        printf("Extracted %s text (%s) from %s -> %zu chars\n",
               label, tagToString(id).c_str(), e->sourceFile.c_str(), txt.size());
    };

    extractText(refs.pregameStorylineTag,
                base + "/strings/pregame_storyline_tag.bin",
                base + "/strings/pregame_storyline.txt",
                "pregame storyline");
    extractText(refs.storylineTag2,
                base + "/strings/storyline_2_tag.bin",
                base + "/strings/storyline_2.txt", "storyline 2");
    extractText(refs.storylineTag3,
                base + "/strings/storyline_3_tag.bin",
                base + "/strings/storyline_3.txt", "storyline 3");
    extractText(refs.storylineTag4,
                base + "/strings/storyline_4_tag.bin",
                base + "/strings/storyline_4.txt", "storyline 4");

    writeLayerBundle(base);
    if(emitOra){
        if(writeORA(base)) printf("Wrote layered OpenRaster: %s\n", (base + "/layers/map_layers.ora").c_str());
        else fprintf(stderr, "Warning: failed to write %s\n", (base + "/layers/map_layers.ora").c_str());
    }

    FILE* mf=fopen((base+"/manifest.json").c_str(),"wb");
    if(mf){
        fprintf(mf,"{\n");
        fprintf(mf,"  \"mesh_tag\": \"%s\",\n",meshTag.c_str());
        fprintf(mf,"  \"mesh_source\": \"%s\",\n",meshEntry->sourceFile.c_str());
        fprintf(mf,"  \"submesh_dimensions\": { \"width\": %d, \"height\": %d },\n",refs.submeshW,refs.submeshH);
        fprintf(mf,"  \"referenced_tags\": {\n");
        fprintf(mf,"    \"landscape_256\": %s,\n",isNullTag(refs.landscapeTag)?"null":("\""+tagToString(refs.landscapeTag)+"\"").c_str());
        fprintf(mf,"    \"media_tag\": %s,\n",isNullTag(refs.mediaTag)?"null":("\""+tagToString(refs.mediaTag)+"\"").c_str());
        fprintf(mf,"    \"map_name_stli\": %s,\n",isNullTag(refs.mapDescStli)?"null":("\""+tagToString(refs.mapDescStli)+"\"").c_str());
        fprintf(mf,"    \"pregame_256\": %s,\n",isNullTag(refs.pregameTag)?"null":("\""+tagToString(refs.pregameTag)+"\"").c_str());
        fprintf(mf,"    \"overhead_256\": %s,\n",isNullTag(refs.overheadTag)?"null":("\""+tagToString(refs.overheadTag)+"\"").c_str());
        fprintf(mf,"    \"postgame_256\": %s,\n",isNullTag(refs.postgameTag)?"null":("\""+tagToString(refs.postgameTag)+"\"").c_str());
        fprintf(mf,"    \"pregame_storyline_text\": %s,\n",isNullTag(refs.pregameStorylineTag)?"null":("\""+tagToString(refs.pregameStorylineTag)+"\"").c_str());
        fprintf(mf,"    \"storyline_2_text\": %s,\n",isNullTag(refs.storylineTag2)?"null":("\""+tagToString(refs.storylineTag2)+"\"").c_str());
        fprintf(mf,"    \"storyline_3_text\": %s,\n",isNullTag(refs.storylineTag3)?"null":("\""+tagToString(refs.storylineTag3)+"\"").c_str());
        fprintf(mf,"    \"storyline_4_text\": %s,\n",isNullTag(refs.storylineTag4)?"null":("\""+tagToString(refs.storylineTag4)+"\"").c_str());
        fprintf(mf,"    \"narration_sound\": %s,\n",isNullTag(refs.narrationSoundTag)?"null":("\""+tagToString(refs.narrationSoundTag)+"\"").c_str());
        fprintf(mf,"    \"win_ambient_sound\": %s,\n",isNullTag(refs.winAmbientSoundTag)?"null":("\""+tagToString(refs.winAmbientSoundTag)+"\"").c_str());
        fprintf(mf,"    \"loss_ambient_sound\": %s\n",isNullTag(refs.lossAmbientSoundTag)?"null":("\""+tagToString(refs.lossAmbientSoundTag)+"\"").c_str());
        fprintf(mf,"  },\n");
        fprintf(mf,"  \"mesh_layout\": {\n");
        fprintf(mf,"    \"mesh_offset\": %u,\n",refs.meshOffset);
        fprintf(mf,"    \"mesh_size\": %u,\n",refs.meshSize);
        fprintf(mf,"    \"data_offset\": %u,\n",refs.dataOffset);
        fprintf(mf,"    \"data_size\": %u\n",refs.dataSize);
        fprintf(mf,"  }\n");
        fprintf(mf,"}\n");
        fclose(mf);
    }

    printf("\nDone. Output folder: %s/\n",base.c_str());
    return 0;
}
