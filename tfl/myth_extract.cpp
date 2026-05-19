// myth_extract.cpp
// Extracts all editable assets from a Myth: The Fallen Lords multiplayer map.
//
// Usage:
//   myth_extract [--overwrite] <tags.gor> <meshtag>
//
//   --overwrite   Overwrite existing files.  Without this flag, any file that
//                 already exists is silently skipped so you can re-run extraction safely.
//
// Produces a folder named <meshtag>/ containing:
//
//   manifest.json                -- all mesh header fields, tag references, flags
//   terrain/
//     color.bmp                 -- color texture          (8-bit indexed)
//     shadow.bmp                 -- shadow/light map        (8-bit greyscale)
//     lit.bmp                    -- color * shadow         (24-bit RGB multiply)
//     height.bmp                 -- height map              (8-bit greyscale, normalised)
//     passability.bmp            -- passability             (24-bit RGB, color-coded)
//     water.bmp                  -- water map               (24-bit RGB, blue=water)
//     slope.bmp                  -- slope magnitude         (24-bit greyscale)
//     watermask.bmp              -- RLE water mask          (8-bit, white=water)
//     terrain_tag.bin            -- raw .256 tag data for reassembly
//
//   NOTE: The color blend table (256x256 palette-index lookup used for
//   8-bit alpha blending) is NOT extracted.  It is tightly coupled to the
//   palette and must be recomputed whenever a new texture is imported; see
//   putTexture.cpp for the generation algorithm.
//   screens/
//     <tag>_overhead.bmp         -- overhead map            (if present)
//     <tag>_pregame.bmp          -- pre-game screen         (if present)
//     <tag>_postgame.bmp         -- post-game screen        (if present)
//     overhead_tag.bin           -- raw .256 tag data       (if present)
//     pregame_tag.bin            -- raw .256 tag data       (if present)
//     postgame_tag.bin           -- raw .256 tag data       (if present)
//   items/
//     item_types.json            -- unit/scenery type roster
//     item_list.json             -- individual item placements (positions, teams, facing)
//   raw/
//     actions.bin                -- action script data, opaque, pass through unchanged
//     unknown3.bin               -- mystery trailing section, pass through unchanged
//     mesh_tag.bin               -- full raw mesh tag data for reference/reassembly
//   strings/                     -- only if STLI tags are present
//     name.txt                   -- map name
//     captions.txt               -- in-game captions
//     name_tag.bin               -- raw stli tag data
//     captions_tag.bin           -- raw stli tag data
//
// Passability color key:
//   Dark grey=Clear   Lt.blue=Human water   Blue=Giant water   Navy=Deep water
//   Yellow=Sloped     Orange=Steep slope    Green=Grass        Tan=Desert
//   Grey=Rocky        Teal=Marsh            White=Snow         Dk.green=Forest
//   Dark red=Walk impassable                Bright red=Fly impassable
//
// Height map: black=lowest point on map, white=highest.
//   manifest.json records height_min and height_max for exact reconstruction.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define F_OK 0
#define access _access
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Global overwrite flag — set by --overwrite command-line switch
// ---------------------------------------------------------------------------
static bool g_overwrite = false;

// Returns true if the path should be skipped (file already exists and
// g_overwrite is false).  Prints a notice when skipping.
static bool skipExisting(const char* path)
{
    if (g_overwrite) return false;
    if (access(path, F_OK) == 0) {
        printf("  Skipping (exists): %s\n", path);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Endian helpers  (Myth TFL is big-endian PowerPC)
// ---------------------------------------------------------------------------
static uint32_t swap32(uint32_t n) {
    return ((n&0xFF000000u)>>24)|((n&0x00FF0000u)>>8)
          |((n&0x0000FF00u)<<8)|((n&0x000000FFu)<<24);
}
static int32_t  swap32s(int32_t n)  { return (int32_t)swap32((uint32_t)n); }
static uint16_t swap16(uint16_t n)  { return (uint16_t)((n>>8)|(n<<8)); }
static int16_t  swap16s(int16_t n)  { return (int16_t)swap16((uint16_t)n); }
static uint32_t readBE32(const uint8_t* b, size_t o)  { uint32_t v; memcpy(&v,b+o,4); return swap32(v); }
static int32_t  readBE32s(const uint8_t* b, size_t o) { return (int32_t)readBE32(b,o); }
static int16_t  readBE16s(const uint8_t* b, size_t o) { uint16_t v; memcpy(&v,b+o,2); return (int16_t)swap16(v); }
static uint16_t readBE16u(const uint8_t* b, size_t o) { uint16_t v; memcpy(&v,b+o,2); return swap16(v); }

static std::string dirOf(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return s==std::string::npos ? "" : path.substr(0,s+1);
}
static void makeDir(const std::string& p) {
    if(p.empty()) return;
    std::string q=p;
    while(!q.empty()&&(q.back()=='/'||q.back()=='\\')) q.pop_back();
    if(q.empty()) return;
#ifdef _WIN32
    _mkdir(q.c_str());
#else
    mkdir(q.c_str(),0755);
#endif
}
static void makeDirs(const std::string& base, const std::initializer_list<const char*>& subs) {
    makeDir(base);
    for(auto s:subs) makeDir(base+"/"+s);
}

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
struct MythColor { uint8_t red,fr,green,fg,blue,fb,flag,ff; };
struct Myth256Palette { int32_t colors; int32_t unk[7]; MythColor color[256]; };
static_assert(sizeof(Myth256Palette)==2080,"palette size");

// ---------------------------------------------------------------------------
// Mesh structures
// ---------------------------------------------------------------------------
struct GridPoint {
    int16_t height;
    uint8_t slope_tri0, slope_tri1;
    uint8_t water;
    uint8_t passability;
    uint8_t unk1, unk2;
};

// All fields parsed from the 1024-byte mesh header (plug prefix stripped)
struct MeshInfo {
    // Tag IDs – 4 chars + null terminator
    char dot256Tag[5];       // terrain texture (.256)
    char waterType1[5];      // media effect 1 (medi)
    char waterType2[5];      // media effect 2 (medi)
    char lighting[5];        // lighting (meli)
    char particleEffect[5];  // weather particles (part)
    char ambientSound[5];    // global ambient (amso)
    char nameIDTag[5];       // map name string list (stli)
    char captionsTag[5];     // in-game captions (stli)
    char windEffects[5];     // wind (wind), usually FFFF
    char preScreen[5];       // pre-game screen (.256)
    char postScreen[5];      // post-game screen (.256)
    char overheadMap[5];     // overhead map (.256)
    char nextLevel[5];       // campaign next level (mesh) – null for MP
    char narrationSound[5];  // narration voice (soun)  – null for MP

    // Flags and config
    int16_t meshWidth;       // tiles wide
    int16_t meshHeight;      // tiles tall
    int16_t netFlags;        // 0x02 bit = multiplayer
    int16_t gameTypeFlags;   // bitmask of available game types
    int32_t maxTeams;

    // Section layout (all offsets are from the start of grid data,
    // which is meshOffset bytes from the start of the tag data)
    int32_t meshOffset;          // = 1024 (header size)
    int32_t meshLength;          // grid data byte count
    int32_t tagLength;           // total tag data length

    int32_t itemTypeCount;
    int32_t itemTypeOffset;      // from grid data start
    int32_t itemTypeLength;

    int32_t itemCount;
    int32_t itemOffset;
    int32_t itemLength;

    int32_t actionCount;
    int32_t actionOffset;
    int32_t actionLength;

    int32_t waterMaskOffset;
    int32_t waterMaskLength;

    int32_t unknown3Offset;
    int32_t unknown3Length;

    // Height range (computed after reading grid)
    int16_t heightMin, heightMax;

    // Raw header bytes preserved for round-trip of unknown fields
    uint8_t rawHeader[1024];
};

// ---------------------------------------------------------------------------
// BMP writers
// ---------------------------------------------------------------------------
static bool writeBMP8(const char* path, int w, int h,
                      const uint8_t* pal256rgb4, const std::vector<uint8_t>& px)
{
    if(skipExisting(path)) return true;
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
static bool writeBMP24(const char* path, int w, int h, const std::vector<uint8_t>& px)
{
    if(skipExisting(path)) return true;
    FILE* f=fopen(path,"wb"); if(!f){fprintf(stderr,"Cannot create: %s\n",path);return false;}
    int stride=(w*3+3)&~3, dataOff=54, fileSz=dataOff+stride*h;
    uint8_t fh[14]={'B','M',0,0,0,0,0,0,0,0,0,0,0,0};
    auto put32=[](uint8_t* b,int v){b[0]=v&0xFF;b[1]=(v>>8)&0xFF;b[2]=(v>>16)&0xFF;b[3]=(v>>24)&0xFF;};
    put32(fh+2,fileSz); put32(fh+10,dataOff); fwrite(fh,1,14,f);
    uint8_t ih[40]={};ih[0]=40;
    put32(ih+4,w); put32(ih+8,h); ih[12]=1; ih[14]=24; fwrite(ih,1,40,f);
    std::vector<uint8_t> row(stride,0);
    for(int y=h-1;y>=0;y--){
        const uint8_t* src=px.data()+(size_t)y*w*3;
        for(int x=0;x<w;x++){row[x*3]=src[x*3+2];row[x*3+1]=src[x*3+1];row[x*3+2]=src[x*3];}
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
    for(int i=0;i<256;i++){bp[i*4]=bp[i*4+1]=bp[i*4+2]=(uint8_t)i;bp[i*4+3]=0;}
    return writeBMP8(path,w,h,bp,px);
}

// ---------------------------------------------------------------------------
// Find a tag in a .gor by scanning the trailing index.
// plugHeader layout (64 bytes):
//   [0..31]  description
//   [32..35] type code
//   [36..39] name code
//   [40..47] Q1/Q2 (unknown flags)
//   [48..51] tagOffset
//   [52..55] tagLength
//   [56..63] Q4/Q5 (unknown)
// If outPlugHdr != nullptr, the 64-byte record is copied there.
// ---------------------------------------------------------------------------
static bool findTagInGor(FILE* gor, long fileSize,
                         const char* type4, const char* name4,
                         long& outOff, long& outLen,
                         uint8_t outPlugHdr[64]=nullptr)
{
    const long SCAN=2*1024*1024;
    long start=fileSize-SCAN; if(start<0) start=0;
    fseek(gor,start,SEEK_SET);
    std::vector<uint8_t> buf(fileSize-start);
    size_t got=fread(buf.data(),1,buf.size(),gor);
    for(size_t i=0;i+64<=got;i++){
        if(memcmp(buf.data()+i,   type4,4)!=0) continue;
        if(memcmp(buf.data()+i+4, name4,4)!=0) continue;
        if(i<40) continue;
        size_t e=i-40;
        // type is at plug header offset 32, so record starts at i-32 = e+8
        uint32_t off=readBE32(buf.data(),e+56), len=readBE32(buf.data(),e+60);
        if((long)off<128||(long)off>=fileSize) continue;
        if(!len||(long)off+(long)len>fileSize) continue;
        outOff=(long)off; outLen=(long)len;
        if(outPlugHdr && i>=32 && i-32+64<=got)
            memcpy(outPlugHdr, buf.data()+i-32, 64);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Save a 64-byte plug header to a .hdr sidecar file
// ---------------------------------------------------------------------------
static bool savePlugHdr(const uint8_t hdr[64], const char* path)
{
    if(skipExisting(path)) return false;  // skipped, not saved
    FILE* f=fopen(path,"wb"); if(!f){fprintf(stderr,"Cannot create: %s\n",path);return false;}
    fwrite(hdr,1,64,f); fclose(f); return true;
}

// ---------------------------------------------------------------------------
// Save a raw blob from an open file
// ---------------------------------------------------------------------------
static bool saveRawBlob(FILE* src, long offset, long length, const char* outPath)
{
    if(length<=0) return false;
    if(skipExisting(outPath)) return false;  // skipped, not saved
    FILE* out=fopen(outPath,"wb");
    if(!out){fprintf(stderr,"Cannot create: %s\n",outPath);return false;}
    fseek(src,offset,SEEK_SET);
    const size_t BUF=65536;
    std::vector<uint8_t> buf(BUF);
    long rem=length;
    while(rem>0){
        size_t n=(size_t)(rem<(long)BUF?rem:(long)BUF);
        size_t r=fread(buf.data(),1,n,src);
        if(!r) break;
        fwrite(buf.data(),1,r,out);
        rem-=(long)r;
    }
    fclose(out);
    return true;
}

// ---------------------------------------------------------------------------
// Tag validity
// ---------------------------------------------------------------------------
static bool isNullTag(const char* n4) {
    const uint8_t* n=(const uint8_t*)n4;
    if(n[0]==0&&n[1]==0&&n[2]==0&&n[3]==0) return true;
    if(n[0]==0xFF&&n[1]==0xFF&&n[2]==0xFF&&n[3]==0xFF) return true;
    return false;
}

// Format a tag4 for JSON: null if empty/null, else "xxxx"
static std::string jsonTag(const char* b4) {
    if(isNullTag(b4)) return "null";
    char buf[8];
    // Escape any non-printable chars
    bool ok=true;
    for(int i=0;i<4;i++) if((uint8_t)b4[i]<0x20||(uint8_t)b4[i]>0x7E){ok=false;break;}
    if(ok){ snprintf(buf,sizeof(buf),"\"%.4s\"",b4); }
    else  { snprintf(buf,sizeof(buf),"\"????\""); }
    return buf;
}
static std::string hexStr(const uint8_t* b, int len) {
    std::string s; s.reserve(len*2);
    const char* h="0123456789abcdef";
    for(int i=0;i<len;i++){s+=h[b[i]>>4];s+=h[b[i]&0xF];}
    return s;
}

// ---------------------------------------------------------------------------
// Read mesh header + grid data
// ---------------------------------------------------------------------------
static bool readMeshInfo(FILE* f, long tagOffset,
                         MeshInfo& info, std::vector<GridPoint>& grid)
{
    fseek(f,tagOffset,SEEK_SET);
    if(fread(info.rawHeader,1,1024,f)!=1024){
        fprintf(stderr,"  Error: failed to read mesh header\n"); return false;
    }
    const uint8_t* h=info.rawHeader;

    // Tag IDs
    auto copyTag=[](char dst[5], const uint8_t* src){ memcpy(dst,src,4); dst[4]=0; };
    copyTag(info.dot256Tag,      h+0);
    copyTag(info.waterType1,     h+4);
    // meshWidth/meshHeight at 8,10
    info.meshWidth  = readBE16s(h, 8);
    info.meshHeight = readBE16s(h,10);
    info.meshLength = readBE32s(h,16);
    info.meshOffset = readBE32s(h,24);
    info.tagLength  = readBE32s(h,28);

    info.itemTypeCount  = readBE32s(h,36);
    info.itemTypeOffset = readBE32s(h,40);
    info.itemTypeLength = readBE32s(h,44);

    info.itemCount  = readBE32s(h,52);
    info.itemOffset = readBE32s(h,56);
    info.itemLength = readBE32s(h,60);

    copyTag(info.lighting,       h+68);
    copyTag(info.waterType2,     h+72);
    info.netFlags      = readBE16s(h,76);
    info.gameTypeFlags = readBE16s(h,78);
    copyTag(info.particleEffect, h+80);
    info.maxTeams      = readBE32s(h,84);

    copyTag(info.ambientSound,   h+124);
    info.actionCount  = readBE32s(h,128);
    info.actionOffset = readBE32s(h,132);
    info.actionLength = readBE32s(h,136);

    copyTag(info.nameIDTag,      h+140);
    copyTag(info.postScreen,     h+144);
    copyTag(info.preScreen,      h+148);
    copyTag(info.overheadMap,    h+152);
    copyTag(info.nextLevel,      h+156);
    copyTag(info.narrationSound, h+256);  // sounTAG at struct offset 320 -> hdr 256

    info.waterMaskOffset = readBE32s(h,192);
    info.waterMaskLength = readBE32s(h,196);
    info.unknown3Offset  = readBE32s(h,204);
    info.unknown3Length  = readBE32s(h,208);

    copyTag(info.windEffects,    h+228);
    copyTag(info.captionsTag,    h+252);

    // Sanity
    if(info.meshWidth<1||info.meshWidth>64||info.meshHeight<1||info.meshHeight>64){
        fprintf(stderr,"  Error: implausible mesh dimensions %dx%d\n",
                info.meshWidth,info.meshHeight); return false;
    }

    // Grid data
    int gridW=info.meshWidth*16, gridH=info.meshHeight*16;
    int total=gridW*gridH;
    fseek(f, tagOffset+(long)info.meshOffset, SEEK_SET);
    grid.resize(total);
    int16_t hmin=32767, hmax=-32768;
    for(int i=0;i<total;i++){
        uint8_t e[8];
        if(fread(e,1,8,f)!=8){fprintf(stderr,"  Error: short read at grid point %d\n",i);return false;}
        grid[i].height     = readBE16s(e,0);
        grid[i].slope_tri0 = e[2]; grid[i].slope_tri1=e[3];
        grid[i].water      = e[4]; grid[i].passability=e[5];
        grid[i].unk1=e[6]; grid[i].unk2=e[7];
        if(grid[i].height<hmin) hmin=grid[i].height;
        if(grid[i].height>hmax) hmax=grid[i].height;
    }
    info.heightMin=hmin; info.heightMax=hmax;
    return true;
}

// ---------------------------------------------------------------------------
// .256 section table + palette
// ---------------------------------------------------------------------------
struct SecEntry { long absOffset; int32_t length; };

static bool readDot256(FILE* f, long tagOffset,
                       int meshW, int meshH,
                       std::vector<SecEntry>& secs, Myth256Palette& pal)
{
    const size_t HDR=320, SECSZ=128, TEXHDR=52;
    fseek(f,tagOffset,SEEK_SET);
    uint8_t hdr[HDR];
    if(fread(hdr,1,HDR,f)!=HDR){fprintf(stderr,"  Error: .256 header\n");return false;}
    int32_t palOff  = readBE32s(hdr,68);
    int32_t sections= readBE32s(hdr,96);
    int32_t secOff  = readBE32s(hdr,100);
    int32_t hdrLen  = readBE32s(hdr,248);
    int32_t tagLen  = readBE32s(hdr,252);
    printf("  Header length:   %d\n",hdrLen);
    printf("  Sections:        %d  (expected %d)\n",sections,meshW*meshH*2);
    printf("  Tag data length: %d bytes\n",tagLen);
    if(sections<2||sections>4000){fprintf(stderr,"  Error: bad section count %d\n",sections);return false;}
    long stPos=tagOffset+(long)HDR+(long)secOff;
    fseek(f,stPos,SEEK_SET);
    secs.resize(sections);
    for(int i=0;i<sections;i++){
        uint8_t e[SECSZ];
        if(fread(e,1,SECSZ,f)!=SECSZ){fprintf(stderr,"  Error: section entry %d\n",i);return false;}
        secs[i].absOffset=tagOffset+(long)HDR+(long)readBE32(e,64)+(long)TEXHDR;
        secs[i].length=readBE32s(e,68);
    }
    fseek(f,tagOffset+(long)HDR+(long)palOff,SEEK_SET);
    if(fread(&pal,1,sizeof(pal),f)!=sizeof(pal)){fprintf(stderr,"  Error: palette\n");return false;}
    return true;
}

static std::vector<uint8_t> assembleTiles(FILE* f,
                                          const std::vector<SecEntry>& secs,
                                          int meshW, int meshH, int secOff)
{
    int totalW=meshW*256, totalH=meshH*256;
    int sections=(int)secs.size(), meshSize=meshW*meshH;
    std::vector<uint8_t> px((size_t)totalW*totalH,0);
    size_t mark=0;
    for(int y=0;y<2*meshSize-meshW;y+=2*meshW){
        for(int line=0;line<256;line++){
            for(int x=2*meshW-2;x>=0;x-=2){
                int idx=x+y+secOff;
                if(idx<0||idx>=sections){mark+=256;continue;}
                fseek(f,secs[idx].absOffset+(long)(line*256),SEEK_SET);
                uint8_t row[256]; memset(row,0,256);
                fread(row,1,256,f);
                for(int a=0,b=255;a<b;a++,b--){uint8_t t=row[a];row[a]=row[b];row[b]=t;}
                memcpy(px.data()+mark,row,256); mark+=256;
            }
        }
    }
    return px;
}

// ---------------------------------------------------------------------------
// Derived maps from grid data
// ---------------------------------------------------------------------------
static const uint8_t PASS_COLOURS[16][3]={
    {0x20,0x20,0x20},{0x40,0x80,0xFF},{0x20,0x60,0xCC},{0x00,0x00,0xAA},
    {0xDD,0xDD,0x00},{0xFF,0x88,0x00},{0x20,0xAA,0x20},{0xCC,0xAA,0x66},
    {0x88,0x88,0x88},{0x20,0x99,0x88},{0xFF,0xFF,0xFF},{0x10,0x66,0x10},
    {0xFF,0x00,0xFF},{0xAA,0x00,0xFF},{0xCC,0x00,0x00},{0xFF,0x00,0x00},
};

static std::vector<uint8_t> buildPassabilityMap(
    const std::vector<GridPoint>& grid, int gW, int gH, int tW, int tH)
{
    int bW=tW/gW, bH=tH/gH;
    std::vector<uint8_t> out((size_t)tW*tH*3,0);
    for(int row=0;row<gH;row++) for(int col=0;col<gW;col++){
        const GridPoint& p=grid[row*gW+(gW-1-col)];
        uint8_t p0=p.passability&0x0F, p1=(p.passability>>4)&0x0F;
        for(int dy=0;dy<bH;dy++) for(int dx=0;dx<bW;dx++){
            uint8_t pass=(dx+dy<bW)?p0:p1;
            int o=((row*bH+dy)*tW+(col*bW+dx))*3;
            out[o]=PASS_COLOURS[pass][0]; out[o+1]=PASS_COLOURS[pass][1]; out[o+2]=PASS_COLOURS[pass][2];
        }
    }
    return out;
}

static std::vector<uint8_t> buildWaterMap(
    const std::vector<GridPoint>& grid, int gW, int gH, int tW, int tH)
{
    int bW=tW/gW, bH=tH/gH;
    std::vector<uint8_t> out((size_t)tW*tH*3,0);
    for(int row=0;row<gH;row++) for(int col=0;col<gW;col++){
        uint8_t w=grid[row*gW+(gW-1-col)].water;
        if(!w) continue;
        uint8_t blue=(uint8_t)(128+(int)w*127/255);
        uint8_t green=(uint8_t)(w*80/255);
        for(int dy=0;dy<bH;dy++) for(int dx=0;dx<bW;dx++){
            int o=((row*bH+dy)*tW+(col*bW+dx))*3;
            out[o]=0; out[o+1]=green; out[o+2]=blue;
        }
    }
    return out;
}

static std::vector<uint8_t> buildSlopeMap(
    const std::vector<GridPoint>& grid, int gW, int gH, int tW, int tH)
{
    int bW=tW/gW, bH=tH/gH;
    std::vector<uint8_t> out((size_t)tW*tH*3,0);
    for(int row=0;row<gH;row++) for(int col=0;col<gW;col++){
        const GridPoint& p=grid[row*gW+(gW-1-col)];
        for(int dy=0;dy<bH;dy++) for(int dx=0;dx<bW;dx++){
            uint8_t s=(dx+dy<bW)?p.slope_tri0:p.slope_tri1;
            int o=((row*bH+dy)*tW+(col*bW+dx))*3;
            out[o]=out[o+1]=out[o+2]=s;
        }
    }
    return out;
}

// Height map: normalise to 0-255.  If min==max, output flat grey.
static std::vector<uint8_t> buildHeightMap(
    const std::vector<GridPoint>& grid, int gW, int gH, int tW, int tH,
    int16_t hmin, int16_t hmax)
{
    int bW=tW/gW, bH=tH/gH;
    std::vector<uint8_t> out((size_t)tW*tH,128);
    int range=(int)hmax-(int)hmin;
    for(int row=0;row<gH;row++) for(int col=0;col<gW;col++){
        int16_t h=grid[row*gW+(gW-1-col)].height;
        uint8_t v=(range>0)?(uint8_t)(((int)h-(int)hmin)*255/range):128;
        for(int dy=0;dy<bH;dy++) for(int dx=0;dx<bW;dx++)
            out[(row*bH+dy)*tW+(col*bW+dx)]=v;
    }
    return out;
}

static std::vector<uint8_t> multiplyComposite(
    const std::vector<uint8_t>& cidx, const std::vector<uint8_t>& shadow,
    const Myth256Palette& pal, int w, int h)
{
    std::vector<uint8_t> out(w*h*3);
    for(int i=0;i<w*h;i++){
        uint8_t idx=cidx[i], s=shadow[i];
        out[i*3+0]=(uint8_t)((pal.color[idx].red  *s)/255);
        out[i*3+1]=(uint8_t)((pal.color[idx].green*s)/255);
        out[i*3+2]=(uint8_t)((pal.color[idx].blue *s)/255);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Decode water mask (RLE span list)
// ---------------------------------------------------------------------------
static std::vector<uint8_t> decodeWaterMask(FILE* f, long tagOffset,
                                             int32_t meshOff,
                                             int32_t wmOff, int32_t wmLen,
                                             int tW, int tH)
{
    std::vector<uint8_t> out((size_t)tW*tH,0);
    if(wmOff<=0||wmLen<=0) return out;
    long pos=tagOffset+(long)meshOff+(long)wmOff;
    fseek(f,pos,SEEK_SET);
    std::vector<uint8_t> raw(wmLen);
    if(fread(raw.data(),1,wmLen,f)!=(size_t)wmLen) return out;
    int nS=wmLen/2, y=0, k=0;
    while(k<nS&&y<tH-1){
        uint16_t v=(uint16_t)((raw[k*2]<<8)|raw[k*2+1]);
        if(v==(uint16_t)tW){y++;k++;}
        else{
            if(k+1>=nS) break;
            uint16_t x1=(uint16_t)((raw[(k+1)*2]<<8)|raw[(k+1)*2+1]);
            for(int x=(int)v;x<=(int)x1&&x<tW;x++)
                out[(size_t)((tW-1-x)+y*tW)]=255;
            k+=2;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Read color blend table from a .256 tag (for reference / analysis only).
//
// The blend table is a 256x256 palette-index lookup: entry [i][j] is the
// palette index whose color is closest to the average of palette colors i
// and j.  It is used by the engine for 8-bit alpha-blending effects.
//
// The table is NOT extracted during normal map extraction because it must be
// recomputed from the palette whenever a new texture is imported.  The
// putTexture.cpp insertion code is responsible for generating it.
//
// Key properties confirmed by inspection of Bungie-shipped maps:
//   - The table is symmetric: table[i][j] == table[j][i]
//   - The diagonal is identity: table[i][i] == i
//   - Row 0 and column 0 are identity: table[0][x]==x, table[y][0]==y
//     (palette entry 0 acts as a "pass-through" color in blending)
//   - Interior values reflect the palette at the time the table was
//     generated (before final palette quantisation), so they will not
//     exactly match a nearest-color search against the stored palette.
// ---------------------------------------------------------------------------
static bool readColorBlendTable(FILE* f, long dot256Offset,
                                 std::vector<uint8_t>& tableOut, int& rowsOut)
{
    uint8_t ci[12];
    fseek(f,(long)(dot256Offset+160),SEEK_SET);
    if(fread(ci,1,12,f)!=12) return false;
    int32_t cmaps=readBE32s(ci,0), cmOff=readBE32s(ci,4), cmLen=readBE32s(ci,8);
    if(cmaps<=0||cmLen<=0) return false;
    long pos=dot256Offset+320L+(long)cmOff;
    tableOut.resize((size_t)cmLen);
    fseek(f,pos,SEEK_SET);
    if(fread(tableOut.data(),1,(size_t)cmLen,f)!=(size_t)cmLen) return false;
    rowsOut=cmLen/256;
    return rowsOut>0;
}

// ---------------------------------------------------------------------------
// Extract a single-image .256 tag (overhead map, pre/post-game screens)
// ---------------------------------------------------------------------------
static bool extractSingleImage256(FILE* f, long artSize,
                                   const char* name4, const char* outPath,
                                   long* outTagOffset=nullptr,
                                   long* outTagLength=nullptr,
                                   uint8_t outPlugHdr[64]=nullptr)
{
    long tagOff=0, tagLen=0;
    if(!findTagInGor(f,artSize,".256",name4,tagOff,tagLen,outPlugHdr)){
        fprintf(stderr,"  Warning: tag '%.4s' not found\n",name4); return false;
    }
    if(outTagOffset) *outTagOffset=tagOff;
    if(outTagLength) *outTagLength=tagLen;

    // Reject texture atlases (many sections)
    uint8_t hdr[320]; fseek(f,tagOff,SEEK_SET);
    if(fread(hdr,1,320,f)!=320) return false;
    int32_t sections=readBE32s(hdr,96);
    if(sections>1){
        printf("  Skipping: '%.4s' is a texture atlas (%d sections).\n",name4,sections);
        return false;
    }

    // Width/height at tag+2812/2814 (standalone: 2876/2878 = 2812+64)
    uint8_t dim[4]; fseek(f,tagOff+2812,SEEK_SET);
    if(fread(dim,1,4,f)!=4){ fprintf(stderr,"  Error: dim read\n"); return false; }
    int w=(int)(int16_t)((dim[0]<<8)|dim[1]);
    int h=(int)(int16_t)((dim[2]<<8)|dim[3]);

    // Fall back to fixed 377x190 if dimensions look wrong (pre/post-game screens
    // store dims differently on some variants)
    if(w<=0||w>4096||h<=0||h>4096){ w=377; h=190; }

    printf("  Found '%.4s' at offset %ld, dims %dx%d\n",name4,tagOff,w,h);

    Myth256Palette pal;
    fseek(f,tagOff+320,SEEK_SET);
    if(fread(&pal,1,sizeof(pal),f)!=sizeof(pal)) return false;

    // Pixel data: tag+2912+4*h (variable image) or tag+4008 (fixed 377x190)
    long pixOff;
    if(w==377&&h==190) pixOff=tagOff+4008;
    else               pixOff=tagOff+2912+4L*h;

    std::vector<uint8_t> px((size_t)w*h);
    fseek(f,pixOff,SEEK_SET);
    if(fread(px.data(),1,(size_t)w*h,f)!=(size_t)w*h){
        fprintf(stderr,"  Warning: short pixel read\n"); return false;
    }
    return writeBMPMythPal(outPath,w,h,pal,px);
}

// ---------------------------------------------------------------------------
// Extract STLI tag as text
// ---------------------------------------------------------------------------
static bool extractStli(FILE* f, long fileSize,
                        const char* name4,
                        const char* textPath, const char* binPath,
                        const char* hdrPath=nullptr)
{
    long tagOff=0, tagLen=0;
    uint8_t plugHdr[64]={};
    if(!findTagInGor(f,fileSize,"stli",name4,tagOff,tagLen,plugHdr)){
        fprintf(stderr,"  Warning: stli tag '%.4s' not found\n",name4); return false;
    }
    printf("  stli '%.4s' at offset %ld, length %ld\n",name4,tagOff,tagLen);
    // Save raw binary + plug header sidecar
    saveRawBlob(f,tagOff,tagLen,binPath);
    if(hdrPath) savePlugHdr(plugHdr,hdrPath);
    // Parse text: strings are Mac CR (0x0D) delimited
    if(skipExisting(textPath)) return true;
    std::vector<uint8_t> raw((size_t)tagLen);
    fseek(f,tagOff,SEEK_SET);
    fread(raw.data(),1,(size_t)tagLen,f);
    FILE* tf=fopen(textPath,"w");
    if(!tf){fprintf(stderr,"Cannot create: %s\n",textPath);return false;}
    for(size_t i=0;i<raw.size();i++){
        uint8_t c=raw[i];
        if(c==0x0D) fputc('\n',tf);
        else if(c>=0x20&&c<=0x7E) fputc(c,tf);
        // skip non-printable / binary header bytes silently
    }
    fclose(tf);
    printf("  Wrote: %s\n",textPath);
    return true;
}

// ---------------------------------------------------------------------------
// Item type names
// ---------------------------------------------------------------------------
static const char* itemTypeName(int16_t t){
    switch(t){
        case 0: return "camera_object";
        case 1: return "scenery";
        case 3: return "unit";
        case 5: return "ambient_sound";
        case 6: return "model";
        case 9: return "physics_object";
        case 0xA: return "special_effect";
        default: return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Extract item type list → JSON
// ---------------------------------------------------------------------------
static bool extractItemTypes(FILE* f, long tagOffset,
                              const MeshInfo& info, const char* outPath)
{
    if(info.itemTypeCount<=0){
        printf("  No item types.\n"); return true;
    }
    long pos=tagOffset+(long)info.meshOffset+(long)info.itemTypeOffset;
    fseek(f,pos,SEEK_SET);
    if(skipExisting(outPath)) return true;
    FILE* out=fopen(outPath,"w");
    if(!out){fprintf(stderr,"Cannot create: %s\n",outPath);return false;}
    fprintf(out,"[\n");
    for(int i=0;i<info.itemTypeCount;i++){
        uint8_t e[32];
        if(fread(e,1,32,f)!=32){fprintf(stderr,"  Error: short read item type %d\n",i);break;}
        int16_t itype  = readBE16s(e,0);
        int16_t flags  = readBE16s(e,2);
        char    id[5]  = {(char)e[4],(char)e[5],(char)e[6],(char)e[7],0};
        int16_t team   = readBE16s(e,8);
        int16_t max1   = readBE16s(e,26);
        int16_t max2   = readBE16s(e,28);
        int16_t ref    = readBE16s(e,30);
        fprintf(out,"  {\n");
        fprintf(out,"    \"index\": %d,\n",i);
        fprintf(out,"    \"item_type\": %d,\n",(int)itype);
        fprintf(out,"    \"item_type_name\": \"%s\",\n",itemTypeName(itype));
        fprintf(out,"    \"flags\": \"0x%04x\",\n",(unsigned)(uint16_t)flags);
        fprintf(out,"    \"item_id\": %s,\n",jsonTag(id).c_str());
        fprintf(out,"    \"team\": %d,\n",(int)team);
        fprintf(out,"    \"count_present\": %d,\n",(int)max1);
        fprintf(out,"    \"count_max\": %d,\n",(int)max2);
        fprintf(out,"    \"ref\": %d\n",(int)ref);
        fprintf(out,"  }%s\n",i<info.itemTypeCount-1?",":"");
    }
    fprintf(out,"]\n");
    fclose(out);
    printf("  Wrote %d item types: %s\n",info.itemTypeCount,outPath);
    return true;
}

// ---------------------------------------------------------------------------
// Extract item placement list → JSON
// ---------------------------------------------------------------------------
static bool extractItemList(FILE* f, long tagOffset,
                             const MeshInfo& info, const char* outPath)
{
    if(info.itemCount<=0){
        printf("  No items placed.\n"); return true;
    }
    long pos=tagOffset+(long)info.meshOffset+(long)info.itemOffset;
    fseek(f,pos,SEEK_SET);
    if(skipExisting(outPath)) return true;
    FILE* out=fopen(outPath,"w");
    if(!out){fprintf(stderr,"Cannot create: %s\n",outPath);return false;}
    fprintf(out,"[\n");
    for(int i=0;i<info.itemCount;i++){
        uint8_t e[64];
        if(fread(e,1,64,f)!=64){fprintf(stderr,"  Error: short read item %d\n",i);break;}
        int16_t initial = readBE16s(e, 2);
        int16_t itype   = readBE16s(e, 4);
        int16_t iref    = readBE16s(e, 6);
        int16_t id1     = readBE16s(e, 8);
        int32_t posX    = readBE32s(e,12);
        int32_t posY    = readBE32s(e,16);
        int32_t posZ    = readBE32s(e,20);
        int16_t facing  = readBE16s(e,32);
        int16_t id2     = readBE16s(e,60);
        int16_t id3     = readBE16s(e,62);
        // Position: 1/1024th of a mesh point from NE corner
        // Convert to floating-point mesh coordinates for readability
        double mx=(double)posX/1024.0, my=(double)posY/1024.0, mz=(double)posZ/1024.0;
        // Facing: 1/65536th of a circle, 0 = north (assumed)
        double deg=(double)(uint16_t)facing*360.0/65536.0;
        fprintf(out,"  {\n");
        fprintf(out,"    \"index\": %d,\n",i);
        fprintf(out,"    \"item_type\": %d,\n",(int)itype);
        fprintf(out,"    \"item_type_name\": \"%s\",\n",itemTypeName(itype));
        fprintf(out,"    \"type_ref\": %d,\n",(int)iref);
        fprintf(out,"    \"id\": %d,\n",(int)(uint16_t)id1);
        fprintf(out,"    \"initial\": %s,\n",initial==0?"true":"false");
        fprintf(out,"    \"tradable\": %s,\n",initial!=0?"true":"false");
        fprintf(out,"    \"pos_raw\": [%d, %d, %d],\n",posX,posY,posZ);
        fprintf(out,"    \"pos_mesh\": [%.3f, %.3f, %.3f],\n",mx,my,mz);
        fprintf(out,"    \"facing_raw\": %d,\n",(int)(uint16_t)facing);
        fprintf(out,"    \"facing_deg\": %.2f,\n",deg);
        fprintf(out,"    \"id2\": %d,\n",(int)(uint16_t)id2);
        fprintf(out,"    \"id3\": %d\n",(int)(uint16_t)id3);
        fprintf(out,"  }%s\n",i<info.itemCount-1?",":"");
    }
    fprintf(out,"]\n");
    fclose(out);
    printf("  Wrote %d items: %s\n",info.itemCount,outPath);
    return true;
}

// ---------------------------------------------------------------------------
// Game type flag names
// ---------------------------------------------------------------------------
static const struct { uint16_t flag; const char* name; } GAME_TYPES[]={
    {0x0001,"body_count"},{0x0002,"steal_the_bacon"},{0x0004,"last_man_on_hill"},
    {0x0008,"scavenger_hunt"},{0x0010,"flag_rally"},{0x0020,"capture_the_flag"},
    {0x0040,"balls_on_parade"},{0x0080,"territories"},{0x0100,"captures"},
    {0x0200,"king_of_the_hill"},{0x0400,"rugby"},{0x0800,"none"},
};
static const int N_GAME_TYPES=12;

// ---------------------------------------------------------------------------
// Write manifest.json
// ---------------------------------------------------------------------------
static bool writeManifest(const char* outPath, const char* meshTag,
                          const MeshInfo& info)
{
    if(skipExisting(outPath)) return true;
    FILE* f=fopen(outPath,"w");
    if(!f){fprintf(stderr,"Cannot create: %s\n",outPath);return false;}

    bool isMP=(info.netFlags&0x0002)!=0;
    uint16_t gf=(uint16_t)info.gameTypeFlags;

    fprintf(f,"{\n");
    fprintf(f,"  \"mesh_tag\": \"%.4s\",\n",meshTag);
    fprintf(f,"  \"is_multiplayer\": %s,\n",isMP?"true":"false");
    fprintf(f,"  \"net_flags\": \"0x%04x\",\n",(unsigned)(uint16_t)info.netFlags);

    // Game types array
    fprintf(f,"  \"game_types\": [");
    bool first=true;
    for(int i=0;i<N_GAME_TYPES;i++){
        if(gf&GAME_TYPES[i].flag){
            if(!first) fprintf(f,", ");
            fprintf(f,"\"%s\"",GAME_TYPES[i].name);
            first=false;
        }
    }
    fprintf(f,"],\n");
    fprintf(f,"  \"game_type_flags\": \"0x%04x\",\n",(unsigned)gf);
    fprintf(f,"  \"max_teams\": %d,\n",(int)info.maxTeams);

    fprintf(f,"\n  \"mesh_dimensions\": { \"width\": %d, \"height\": %d },\n",
            (int)info.meshWidth,(int)info.meshHeight);

    fprintf(f,"\n  \"referenced_tags\": {\n");
    fprintf(f,"    \"terrain_256\": %s,\n",  jsonTag(info.dot256Tag).c_str());
    fprintf(f,"    \"water_type_1\": %s,\n", jsonTag(info.waterType1).c_str());
    fprintf(f,"    \"water_type_2\": %s,\n", jsonTag(info.waterType2).c_str());
    fprintf(f,"    \"lighting\": %s,\n",     jsonTag(info.lighting).c_str());
    fprintf(f,"    \"particle_effect\": %s,\n",jsonTag(info.particleEffect).c_str());
    fprintf(f,"    \"ambient_sound\": %s,\n",jsonTag(info.ambientSound).c_str());
    fprintf(f,"    \"wind_effects\": %s,\n", jsonTag(info.windEffects).c_str());
    fprintf(f,"    \"map_name_stli\": %s,\n",jsonTag(info.nameIDTag).c_str());
    fprintf(f,"    \"captions_stli\": %s,\n",jsonTag(info.captionsTag).c_str());
    fprintf(f,"    \"pre_screen_256\": %s,\n",jsonTag(info.preScreen).c_str());
    fprintf(f,"    \"post_screen_256\": %s,\n",jsonTag(info.postScreen).c_str());
    fprintf(f,"    \"overhead_map_256\": %s,\n",jsonTag(info.overheadMap).c_str());
    fprintf(f,"    \"next_level_mesh\": %s,\n",jsonTag(info.nextLevel).c_str());
    fprintf(f,"    \"narration_sound\": %s\n",jsonTag(info.narrationSound).c_str());
    fprintf(f,"  },\n");

    fprintf(f,"\n  \"height_map\": {\n");
    fprintf(f,"    \"min\": %d,\n",(int)info.heightMin);
    fprintf(f,"    \"max\": %d\n",(int)info.heightMax);
    fprintf(f,"  },\n");

    fprintf(f,"\n  \"section_layout\": {\n");
    fprintf(f,"    \"mesh_header_size\": %d,\n",(int)info.meshOffset);
    fprintf(f,"    \"mesh_length\": %d,\n",(int)info.meshLength);
    fprintf(f,"    \"tag_length\": %d,\n",(int)info.tagLength);
    fprintf(f,"    \"item_type_count\": %d,\n",(int)info.itemTypeCount);
    fprintf(f,"    \"item_type_offset\": %d,\n",(int)info.itemTypeOffset);
    fprintf(f,"    \"item_type_length\": %d,\n",(int)info.itemTypeLength);
    fprintf(f,"    \"item_count\": %d,\n",(int)info.itemCount);
    fprintf(f,"    \"item_offset\": %d,\n",(int)info.itemOffset);
    fprintf(f,"    \"item_length\": %d,\n",(int)info.itemLength);
    fprintf(f,"    \"action_count\": %d,\n",(int)info.actionCount);
    fprintf(f,"    \"action_offset\": %d,\n",(int)info.actionOffset);
    fprintf(f,"    \"action_length\": %d,\n",(int)info.actionLength);
    fprintf(f,"    \"water_mask_offset\": %d,\n",(int)info.waterMaskOffset);
    fprintf(f,"    \"water_mask_length\": %d,\n",(int)info.waterMaskLength);
    fprintf(f,"    \"unknown3_offset\": %d,\n",(int)info.unknown3Offset);
    fprintf(f,"    \"unknown3_length\": %d\n",(int)info.unknown3Length);
    fprintf(f,"  }\n");

    fprintf(f,"}\n");
    fclose(f);
    printf("  Wrote: %s\n",outPath);
    return true;
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------
static void usage(const char* p){
    fprintf(stderr,
        "Myth TFL Map Asset Extractor\n\n"
        "Usage:  %s [--overwrite] <tags.gor> <meshtag>\n\n"
        "  --overwrite  Overwrite existing output files (default: skip them)\n"
        "  tags.gor   path to tags.gor (artsound.gor must be in the same directory)\n"
        "  meshtag    4-character mesh tag name (e.g. sega, 00tm, land)\n\n"
        "Creates <meshtag>/ with terrain/, screens/, items/, raw/, strings/ subfolders.\n"
        "See source header for full output file list.\n",p);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    int argBase=1;
    while (argBase < argc && argv[argBase][0] == '-') {
        if (strcmp(argv[argBase], "--overwrite") == 0) {
            g_overwrite = true;
            argBase++;
        } else {
            fprintf(stderr, "Error: unknown option: %s\n", argv[argBase]);
            usage(argv[0]);
            return 1;
        }
    }

    if(argc-argBase!=2){usage(argv[0]);return 1;}
    const char* tagsPath=argv[argBase];
    const char* meshTag =argv[argBase+1];
    if(strlen(meshTag)!=4){fprintf(stderr,"Error: tag must be exactly 4 characters\n");return 1;}

    std::string tagsDir   =dirOf(tagsPath);
    std::string artPath   =tagsDir+"artsound.gor";
    std::string tag4(meshTag,4);

    // Output directory layout
    std::string base     =tag4;
    std::string terrDir  =base+"/terrain";
    std::string scrDir   =base+"/screens";
    std::string itemDir  =base+"/items";
    std::string rawDir   =base+"/raw";
    std::string strDir   =base+"/strings";

    auto tp=[&](const std::string& s)->std::string{ return terrDir+"/"+s; };
    auto sp=[&](const std::string& s)->std::string{ return scrDir +"/"+s; };
    auto ip=[&](const std::string& s)->std::string{ return itemDir+"/"+s; };
    auto rp=[&](const std::string& s)->std::string{ return rawDir +"/"+s; };
    auto stP=[&](const std::string& s)->std::string{ return strDir+"/"+s; };

    printf("Myth TFL Map Asset Extractor\n============================\n");
    printf("tags.gor:  %s\n",tagsPath);
    printf("artsound:  %s\n",artPath.c_str());
    printf("Mesh tag:  %.4s\n\n",meshTag);

    // ------------------------------------------------------------------
    // Step 1: find and read mesh tag
    // ------------------------------------------------------------------
    FILE* tagsGor=fopen(tagsPath,"rb");
    if(!tagsGor){fprintf(stderr,"Cannot open: %s\n",tagsPath);return 1;}
    fseek(tagsGor,0,SEEK_END); long tagsSize=ftell(tagsGor);

    long meshOff=0, meshLen=0;
    uint8_t meshPlugHdr[64]={};
    printf("Step 1: Finding mesh tag '%.4s' in tags.gor...\n",meshTag);
    if(!findTagInGor(tagsGor,tagsSize,"mesh",meshTag,meshOff,meshLen,meshPlugHdr)){
        fprintf(stderr,"  Error: mesh tag '%.4s' not found\n",meshTag);
        fclose(tagsGor); return 1;
    }
    printf("  Found at offset %ld, length %ld\n",meshOff,meshLen);

    MeshInfo info; memset(&info,0,sizeof(info));
    std::vector<GridPoint> grid;
    printf("Step 1b: Reading mesh header and grid data...\n");
    if(!readMeshInfo(tagsGor,meshOff,info,grid)){fclose(tagsGor);return 1;}

    int gridW=info.meshWidth*16, gridH=info.meshHeight*16;
    int texW =info.meshWidth*256, texH=info.meshHeight*256;

    printf("  Terrain .256:  '%.4s'\n",info.dot256Tag);
    printf("  Dimensions:    %dx%d tiles  (%dx%d grid, %dx%d texture)\n",
           info.meshWidth,info.meshHeight,gridW,gridH,texW,texH);
    printf("  Net flags:     0x%04x  (%s)\n",(unsigned)(uint16_t)info.netFlags,
           (info.netFlags&0x0002)?"MULTIPLAYER":"single-player");
    printf("  Game types:    0x%04x\n",(unsigned)(uint16_t)info.gameTypeFlags);
    printf("  Max teams:     %d\n",(int)info.maxTeams);
    printf("  Item types:    %d\n",(int)info.itemTypeCount);
    printf("  Items placed:  %d\n",(int)info.itemCount);
    printf("  Actions:       %d\n",(int)info.actionCount);
    printf("  Water mask:    offset %d, length %d\n",info.waterMaskOffset,info.waterMaskLength);
    printf("  Overhead map:  '%.4s'%s\n",info.overheadMap, isNullTag(info.overheadMap)?" (none)":"");
    printf("  Pre-game:      '%.4s'%s\n",info.preScreen,   isNullTag(info.preScreen)?  " (none)":"");
    printf("  Post-game:     '%.4s'%s\n",info.postScreen,  isNullTag(info.postScreen)? " (none)":"");
    printf("  Map name stli: '%.4s'%s\n",info.nameIDTag,   isNullTag(info.nameIDTag)?  " (none)":"");
    printf("  Captions stli: '%.4s'%s\n\n",info.captionsTag,isNullTag(info.captionsTag)?" (none)":"");

    if(!(info.netFlags&0x0002))
        printf("  WARNING: This does not appear to be a multiplayer map (netFlags=0x%04x).\n"
               "  Extraction will continue but reassembly targets multiplayer maps only.\n\n",
               (unsigned)(uint16_t)info.netFlags);

    // ------------------------------------------------------------------
    // Step 1c: decode water mask (while tagsGor is still open)
    // ------------------------------------------------------------------
    printf("Step 1c: Decoding water mask...\n");
    std::vector<uint8_t> waterMaskPx=decodeWaterMask(tagsGor,meshOff,
        info.meshOffset,info.waterMaskOffset,info.waterMaskLength,texW,texH);
    printf("  %s\n\n",info.waterMaskLength>0?"OK":"Not present.");

    // ------------------------------------------------------------------
    // Step 1d: save raw mesh tag and binary blobs
    // ------------------------------------------------------------------
    makeDirs(base,{"terrain","screens","items","raw","strings"});

    printf("Step 1d: Saving raw mesh tag and section blobs...\n");
    if(saveRawBlob(tagsGor,meshOff,meshLen,rp("mesh_tag.bin").c_str()))
        printf("  Saved: raw/mesh_tag.bin (%ld bytes)\n",meshLen);
    if(savePlugHdr(meshPlugHdr, rp("mesh_plug.hdr").c_str()))
        printf("  Saved: raw/mesh_plug.hdr\n");

    if(info.actionLength>0){
        long apos=meshOff+(long)info.meshOffset+(long)info.actionOffset;
        if(saveRawBlob(tagsGor,apos,info.actionLength,rp("actions.bin").c_str()))
            printf("  Saved: raw/actions.bin (%d bytes)\n",info.actionLength);
    } else {
        printf("  No action data.\n");
    }
    if(info.unknown3Length>0){
        long upos=meshOff+(long)info.meshOffset+(long)info.unknown3Offset;
        if(saveRawBlob(tagsGor,upos,info.unknown3Length,rp("unknown3.bin").c_str()))
            printf("  Saved: raw/unknown3.bin (%d bytes)\n",info.unknown3Length);
    } else {
        printf("  No unknown3 section.\n");
    }
    printf("\n");

    // ------------------------------------------------------------------
    // Step 1e: item types and item list
    // ------------------------------------------------------------------
    printf("Step 1e: Extracting item type list...\n");
    extractItemTypes(tagsGor,meshOff,info,ip("item_types.json").c_str());
    printf("\n");

    printf("Step 1f: Extracting item placement list...\n");
    extractItemList(tagsGor,meshOff,info,ip("item_list.json").c_str());
    printf("\n");

    fclose(tagsGor);

    // ------------------------------------------------------------------
    // Step 2: open artsound.gor and extract all .256 assets
    // ------------------------------------------------------------------
    FILE* art=fopen(artPath.c_str(),"rb");
    if(!art){
        fprintf(stderr,"Step 2: Cannot open artsound.gor: %s\n"
                       "  Ensure it is in the same directory as tags.gor\n",artPath.c_str());
        return 1;
    }
    fseek(art,0,SEEK_END); long artSize=ftell(art);

    // ------------------------------------------------------------------
    // Step 2a: find terrain .256 tag
    // ------------------------------------------------------------------
    long dot256Off=0, dot256Len=0;
    uint8_t dot256PlugHdr[64]={};
    printf("Step 2a: Finding terrain .256 tag '%.4s' in artsound.gor...\n",info.dot256Tag);
    if(!findTagInGor(art,artSize,".256",info.dot256Tag,dot256Off,dot256Len,dot256PlugHdr)){
        fprintf(stderr,"  Error: terrain .256 tag not found\n"); fclose(art); return 1;
    }
    printf("  Found at offset %ld, length %ld\n\n",dot256Off,dot256Len);

    // ------------------------------------------------------------------
    // Step 2b: read .256 structure
    // ------------------------------------------------------------------
    printf("Step 2b: Reading .256 tag structure...\n\n");
    std::vector<SecEntry> secs;
    Myth256Palette pal;
    if(!readDot256(art,dot256Off,info.meshWidth,info.meshHeight,secs,pal)){fclose(art);return 1;}

    // Save raw terrain tag binary + plug header sidecar
    if(saveRawBlob(art,dot256Off,dot256Len,tp("terrain_tag.bin").c_str()))
        printf("  Saved: terrain/terrain_tag.bin\n");
    if(savePlugHdr(dot256PlugHdr, tp("terrain_plug.hdr").c_str()))
        printf("  Saved: terrain/terrain_plug.hdr\n");
    printf("\n");

    // ------------------------------------------------------------------
    // Step 3: color texture
    // ------------------------------------------------------------------
    printf("Step 3: Assembling color texture (%dx%d)...\n",texW,texH);
    std::vector<uint8_t> colorPx=assembleTiles(art,secs,info.meshWidth,info.meshHeight,0);
    std::string texPath=tp("color.bmp");
    if(!writeBMPMythPal(texPath.c_str(),texW,texH,pal,colorPx)){fclose(art);return 1;}
    printf("  Done: %s\n\n",texPath.c_str());

    // ------------------------------------------------------------------
    // Step 4: shadow map
    // ------------------------------------------------------------------
    printf("Step 4: Assembling shadow map (%dx%d)...\n",texW,texH);
    std::vector<uint8_t> shadowPx=assembleTiles(art,secs,info.meshWidth,info.meshHeight,1);
    std::string shadowPath=tp("shadow.bmp");
    if(!writeBMPGrey(shadowPath.c_str(),texW,texH,shadowPx)){fclose(art);return 1;}
    printf("  Done: %s\n\n",shadowPath.c_str());

    // ------------------------------------------------------------------
    // Step 5: screen images (overhead, pre-game, post-game)
    // ------------------------------------------------------------------
    printf("Step 5: Extracting screen images...\n");

    if(!isNullTag(info.overheadMap)){
        long oTagOff=0, oTagLen=0; uint8_t oPH[64]={};
        printf("  Overhead map '%.4s'...\n",info.overheadMap);
        if(extractSingleImage256(art,artSize,info.overheadMap,
                                 sp("overhead.bmp").c_str(),&oTagOff,&oTagLen,oPH)){
            if(saveRawBlob(art,oTagOff,oTagLen,sp("overhead_tag.bin").c_str()))
                printf("  Saved: screens/overhead_tag.bin\n");
            if(savePlugHdr(oPH, sp("overhead_plug.hdr").c_str()))
                printf("  Saved: screens/overhead_plug.hdr\n");
        }
    } else printf("  No overhead map.\n");

    if(!isNullTag(info.preScreen)){
        long pTagOff=0, pTagLen=0; uint8_t pPH[64]={};
        printf("  Pre-game screen '%.4s'...\n",info.preScreen);
        if(extractSingleImage256(art,artSize,info.preScreen,
                                 sp("pregame.bmp").c_str(),&pTagOff,&pTagLen,pPH)){
            if(saveRawBlob(art,pTagOff,pTagLen,sp("pregame_tag.bin").c_str()))
                printf("  Saved: screens/pregame_tag.bin\n");
            if(savePlugHdr(pPH, sp("pregame_plug.hdr").c_str()))
                printf("  Saved: screens/pregame_plug.hdr\n");
        }
    } else printf("  No pre-game screen.\n");

    if(!isNullTag(info.postScreen)){
        long qTagOff=0, qTagLen=0; uint8_t qPH[64]={};
        printf("  Post-game screen '%.4s'...\n",info.postScreen);
        if(extractSingleImage256(art,artSize,info.postScreen,
                                 sp("postgame.bmp").c_str(),&qTagOff,&qTagLen,qPH)){
            if(saveRawBlob(art,qTagOff,qTagLen,sp("postgame_tag.bin").c_str()))
                printf("  Saved: screens/postgame_tag.bin\n");
            if(savePlugHdr(qPH, sp("postgame_plug.hdr").c_str()))
                printf("  Saved: screens/postgame_plug.hdr\n");
        }
    } else printf("  No post-game screen.\n");
    printf("\n");

    fclose(art);

    // ------------------------------------------------------------------
    // Steps 6-10: pure grid-derived maps (no file I/O beyond output BMPs)
    // ------------------------------------------------------------------

    // Step 6: multiply composite
    printf("Step 6: Compositing color * shadow...\n");
    std::vector<uint8_t> litPx=multiplyComposite(colorPx,shadowPx,pal,texW,texH);
    if(!writeBMP24(tp("lit.bmp").c_str(),texW,texH,litPx)) return 1;
    printf("  Done: terrain/lit.bmp\n\n");

    // Step 7: height map
    printf("Step 7: Building height map (min=%d, max=%d)...\n",
           (int)info.heightMin,(int)info.heightMax);
    std::vector<uint8_t> heightPx=buildHeightMap(grid,gridW,gridH,texW,texH,
                                                  info.heightMin,info.heightMax);
    if(!writeBMPGrey(tp("height.bmp").c_str(),texW,texH,heightPx)) return 1;
    printf("  Done: terrain/height.bmp\n\n");

    // Step 8: passability map
    printf("Step 8: Building passability map...\n");
    std::vector<uint8_t> passPx=buildPassabilityMap(grid,gridW,gridH,texW,texH);
    if(!writeBMP24(tp("passability.bmp").c_str(),texW,texH,passPx)) return 1;
    printf("  Done\n\n");

    // Step 9: water map
    printf("Step 9: Building water map...\n");
    std::vector<uint8_t> waterPx=buildWaterMap(grid,gridW,gridH,texW,texH);
    if(!writeBMP24(tp("water.bmp").c_str(),texW,texH,waterPx)) return 1;
    printf("  Done\n\n");

    // Step 10: slope map
    printf("Step 10: Building slope map...\n");
    std::vector<uint8_t> slopePx=buildSlopeMap(grid,gridW,gridH,texW,texH);
    if(!writeBMP24(tp("slope.bmp").c_str(),texW,texH,slopePx)) return 1;
    printf("  Done\n\n");

    // Step 11: water mask
    printf("Step 11: Writing water mask...\n");
    if(!writeBMPGrey(tp("watermask.bmp").c_str(),texW,texH,waterMaskPx)) return 1;
    printf("  Done\n\n");

    // ------------------------------------------------------------------
    // Step 12: STLI string extraction
    // ------------------------------------------------------------------
    // Re-open tags.gor for STLI (already closed above)
    FILE* tagsGor2=fopen(tagsPath,"rb");
    if(tagsGor2){
        fseek(tagsGor2,0,SEEK_END); long tSz=ftell(tagsGor2);
        bool hadStli=false;
        printf("Step 12: Extracting map name strings...\n");
        if(!isNullTag(info.nameIDTag)){
            extractStli(tagsGor2,tSz,info.nameIDTag,
                        stP("name.txt").c_str(), stP("name_tag.bin").c_str(),
                        stP("name_plug.hdr").c_str());
            hadStli=true;
        } else printf("  No map name stli tag.\n");
        if(!isNullTag(info.captionsTag)){
            extractStli(tagsGor2,tSz,info.captionsTag,
                        stP("captions.txt").c_str(), stP("captions_tag.bin").c_str(),
                        stP("captions_plug.hdr").c_str());
            hadStli=true;
        } else printf("  No captions stli tag.\n");
        if(!hadStli) printf("  No string tags for this map.\n");
        printf("\n");
        fclose(tagsGor2);
    }

    // ------------------------------------------------------------------
    // Step 13: manifest.json
    // ------------------------------------------------------------------
    printf("Step 13: Writing manifest.json...\n");
    writeManifest((base+"/manifest.json").c_str(), meshTag, info);
    printf("\n");

    // ------------------------------------------------------------------
    // Summary
    // ------------------------------------------------------------------
    printf("Done!  Output folder: %s/\n\n", base.c_str());
    printf("  manifest.json\n");
    printf("  terrain/color.bmp            (color texture)\n");
    printf("  terrain/shadow.bmp            (shadow/light)\n");
    printf("  terrain/lit.bmp               (multiply composite)\n");
    printf("  terrain/height.bmp            (height, min=%d max=%d)\n",
           (int)info.heightMin,(int)info.heightMax);
    printf("  terrain/passability.bmp       (passability)\n");
    printf("  terrain/water.bmp             (water map)\n");
    printf("  terrain/slope.bmp             (slope map)\n");
    printf("  terrain/watermask.bmp         (water mask)\n");
    printf("  terrain/terrain_tag.bin       (raw .256 tag for reassembly)\n");
    printf("  terrain/terrain_plug.hdr      (plug header sidecar)\n");
    if(!isNullTag(info.overheadMap)) printf("  screens/overhead.bmp\n");
    if(!isNullTag(info.preScreen))   printf("  screens/pregame.bmp\n");
    if(!isNullTag(info.postScreen))  printf("  screens/postgame.bmp\n");
    printf("  items/item_types.json         (%d types)\n",(int)info.itemTypeCount);
    printf("  items/item_list.json          (%d items)\n",(int)info.itemCount);
    printf("  raw/mesh_tag.bin              (full raw mesh tag)\n");
    printf("  raw/mesh_plug.hdr             (plug header sidecar)\n");
    if(info.actionLength>0)  printf("  raw/actions.bin               (%d bytes)\n",info.actionLength);
    if(info.unknown3Length>0)printf("  raw/unknown3.bin              (%d bytes)\n",info.unknown3Length);
    printf("\n");
    printf("Passability key:\n");
    printf("  Dark grey=Clear  Lt.blue=Human water  Blue=Giant water  Navy=Deep water\n");
    printf("  Yellow=Sloped    Orange=Steep slope   Green=Grass       Tan=Desert\n");
    printf("  Grey=Rocky       Teal=Marsh           White=Snow        Dk.green=Forest\n");
    printf("  Dark red=Walk impassable              Bright red=Fly impassable\n");
    return 0;
}
