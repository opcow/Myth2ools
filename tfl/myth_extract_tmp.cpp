// myth_extract.cpp
// Extracts texture maps from Myth: The Fallen Lords.
//
// Usage:
//   myth_extract <tags.gor> <meshtag> [output.bmp]
//
// Arguments:
//   tags.gor   -- path to tags.gor (artsound.gor must be in the same directory)
//   meshtag    -- 4-character mesh tag name (e.g. sega, 00tm, balo)
//   output.bmp -- optional output filename stem (default: <meshtag>.bmp)
//
// Produces output files:
//   <meshtag>.bmp              -- color texture map    (8-bit indexed)
//   <meshtag>_shadow.bmp       -- shadow/light map      (8-bit greyscale)
//   <meshtag>_lit.bmp          -- color * shadow       (24-bit RGB, multiply blend)
//   <meshtag>_passability.bmp  -- passability map       (24-bit RGB, color-coded)
//   <meshtag>_water.bmp        -- water map             (24-bit RGB, blue=water)
//   <meshtag>_slope.bmp        -- slope magnitude map   (8-bit greyscale)
//   <meshtag>_watermask.bmp    -- RLE water mask        (8-bit, white=water)
//   <meshtag>_clut.bmp         -- color LUT            (256xN, palette-indexed)
//   <meshtag>_overhead.bmp     -- overhead map          (if present)
//   <meshtag>_pregame.bmp      -- pre-game screen       (if present)
//   <meshtag>_postgame.bmp     -- post-game screen      (if present)
//
// Passability color key:
//   Black       = Clear (0)          Dark blue   = Human depth water (1)
//   Blue        = Giant depth water (2) Navy      = Deep water (3)
//   Yellow      = Sloped (4)         Orange      = Steep slope (5)
//   Green       = Grass (6)          Tan         = Desert (7)
//   Grey        = Rocky (8)          Teal        = Marsh (9)
//   White       = Snow (A)           Dark green  = Forest (B)
//   Red         = Walking impass (E) Bright red  = Flying impass (F)
//
// Water map:
//   Black = dry land,  Blue = water (any depth)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

// ---------------------------------------------------------------------------
// Endian helpers
// ---------------------------------------------------------------------------
static uint32_t swap32(uint32_t n) {
    return ((n & 0xFF000000u) >> 24) | ((n & 0x00FF0000u) >>  8)
         | ((n & 0x0000FF00u) <<  8) | ((n & 0x000000FFu) << 24);
}
static int32_t  swap32s(int32_t n)  { return (int32_t)swap32((uint32_t)n); }
static uint16_t swap16(uint16_t n)  { return (uint16_t)((n >> 8) | (n << 8)); }
static uint32_t readBE32(const uint8_t* b, size_t o) { uint32_t v; memcpy(&v,b+o,4); return swap32(v); }
static int32_t  readBE32s(const uint8_t* b, size_t o) { return (int32_t)readBE32(b,o); }
static int16_t  readBE16s(const uint8_t* b, size_t o) { uint16_t v; memcpy(&v,b+o,2); return (int16_t)swap16(v); }

static std::string dirOf(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? "" : path.substr(0, slash + 1);
}

static void makeDir(const std::string& path) {
    if(path.empty()) return;
    std::string p = path;
    while(!p.empty() && (p.back()=='/' || p.back()=='\\')) p.pop_back();
    if(p.empty()) return;
#ifdef _WIN32
    _mkdir(p.c_str());
#else
    mkdir(p.c_str(), 0755);
#endif
}

// ---------------------------------------------------------------------------
// Myth palette
// ---------------------------------------------------------------------------
struct MythColor { uint8_t red, fr, green, fg, blue, fb, flag, ff; };
struct Myth256Palette {
    int32_t   colors;
    int32_t   unk[7];
    MythColor color[256];
};
static_assert(sizeof(Myth256Palette) == 2080, "palette size mismatch");

// ---------------------------------------------------------------------------
// BMP writers
// ---------------------------------------------------------------------------
static bool writeBMP8(const char* path, int w, int h,
                      const uint8_t* pal256, const std::vector<uint8_t>& px)
{
    FILE* f = fopen(path,"wb"); if(!f){fprintf(stderr,"Cannot create: %s\n",path);return false;}
    int stride=(w+3)&~3, dataOff=14+40+1024, fileSz=dataOff+stride*h;
    uint8_t fh[14]={'B','M'};
    fh[2]=fileSz&0xFF;fh[3]=(fileSz>>8)&0xFF;fh[4]=(fileSz>>16)&0xFF;fh[5]=(fileSz>>24)&0xFF;
    fh[10]=dataOff&0xFF;fh[11]=(dataOff>>8)&0xFF;fh[12]=(dataOff>>16)&0xFF;fh[13]=(dataOff>>24)&0xFF;
    fwrite(fh,1,14,f);
    uint8_t ih[40]={};ih[0]=40;
    ih[4]=w&0xFF;ih[5]=(w>>8)&0xFF;ih[6]=(w>>16)&0xFF;ih[7]=(w>>24)&0xFF;
    ih[8]=h&0xFF;ih[9]=(h>>8)&0xFF;ih[10]=(h>>16)&0xFF;ih[11]=(h>>24)&0xFF;
    ih[12]=1;ih[14]=8; fwrite(ih,1,40,f); fwrite(pal256,1,1024,f);
    std::vector<uint8_t> row(stride,0);
    for(int y=h-1;y>=0;y--){memcpy(row.data(),px.data()+(size_t)y*w,w);fwrite(row.data(),1,stride,f);}
    fclose(f); return true;
}

static bool writeBMP24(const char* path, int w, int h, const std::vector<uint8_t>& px)
{
    FILE* f = fopen(path,"wb"); if(!f){fprintf(stderr,"Cannot create: %s\n",path);return false;}
    int stride=(w*3+3)&~3, dataOff=14+40, fileSz=dataOff+stride*h;
    uint8_t fh[14]={'B','M'};
    fh[2]=fileSz&0xFF;fh[3]=(fileSz>>8)&0xFF;fh[4]=(fileSz>>16)&0xFF;fh[5]=(fileSz>>24)&0xFF;
    fh[10]=dataOff&0xFF;fh[11]=(dataOff>>8)&0xFF;fh[12]=(dataOff>>16)&0xFF;fh[13]=(dataOff>>24)&0xFF;
    fwrite(fh,1,14,f);
    uint8_t ih[40]={};ih[0]=40;
    ih[4]=w&0xFF;ih[5]=(w>>8)&0xFF;ih[6]=(w>>16)&0xFF;ih[7]=(w>>24)&0xFF;
    ih[8]=h&0xFF;ih[9]=(h>>8)&0xFF;ih[10]=(h>>16)&0xFF;ih[11]=(h>>24)&0xFF;
    ih[12]=1;ih[14]=24; fwrite(ih,1,40,f);
    std::vector<uint8_t> row(stride,0);
    for(int y=h-1;y>=0;y--){
        const uint8_t* src=px.data()+(size_t)y*w*3;
        for(int x=0;x<w;x++){row[x*3+0]=src[x*3+2];row[x*3+1]=src[x*3+1];row[x*3+2]=src[x*3+0];}
        fwrite(row.data(),1,stride,f);
    }
    fclose(f); return true;
}

static bool writeBMPMythPal(const char* path,int w,int h,const Myth256Palette& pal,const std::vector<uint8_t>& px){
    uint8_t bp[1024];
    for(int i=0;i<256;i++){bp[i*4+0]=pal.color[i].blue;bp[i*4+1]=pal.color[i].green;bp[i*4+2]=pal.color[i].red;bp[i*4+3]=0;}
    return writeBMP8(path,w,h,bp,px);
}
static bool writeBMPGrey(const char* path,int w,int h,const std::vector<uint8_t>& px){
    uint8_t bp[1024];
    for(int i=0;i<256;i++){bp[i*4+0]=bp[i*4+1]=bp[i*4+2]=(uint8_t)i;bp[i*4+3]=0;}
    return writeBMP8(path,w,h,bp,px);
}

// ---------------------------------------------------------------------------
// Multiply composite (Photoshop multiply blend mode)
// ---------------------------------------------------------------------------
static std::vector<uint8_t> multiplyComposite(
    const std::vector<uint8_t>& colorIdx,
    const std::vector<uint8_t>& shadowPx,
    const Myth256Palette& pal, int w, int h)
{
    std::vector<uint8_t> out(w*h*3);
    for(int i=0;i<w*h;i++){
        uint8_t idx=colorIdx[i], s=shadowPx[i];
        out[i*3+0]=(uint8_t)((pal.color[idx].red   * s)/255);
        out[i*3+1]=(uint8_t)((pal.color[idx].green * s)/255);
        out[i*3+2]=(uint8_t)((pal.color[idx].blue  * s)/255);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Find a tag in a .gor index (last 2MB)
// ---------------------------------------------------------------------------
static bool findTagInGor(FILE* gor, long fileSize,
                         const char* type4, const char* name4,
                         long& outOffset, long& outLength)
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
        uint32_t off=readBE32(buf.data(),e+56),len=readBE32(buf.data(),e+60);
        if((long)off<128||(long)off>=fileSize) continue;
        if(!len||(long)off+(long)len>fileSize) continue;
        outOffset=(long)off; outLength=(long)len; return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Mesh grid point
// ---------------------------------------------------------------------------
// Each grid point sits at the corner shared by up to four grid cells.  Myth
// splits each cell along one diagonal into two triangles:
//
//   col   col+1
//    +-----+   row
//    |A   /|
//    |   / |
//    |  /  |
//    | /   |
//    |/   B|
//    +-----+   row+1
//
// Triangle A = upper-left  (verts: [row][col], [row][col+1], [row+1][col])
// Triangle B = lower-right (verts: [row][col+1], [row+1][col+1], [row+1][col])
//
// The slope and passability bytes at each grid point describe the two triangles
// that share this point as their "primary" vertex (i.e. the cell to the lower-
// right of this point): tri0 = triangle A, tri1 = triangle B.
//
// passability byte [5]: lower nibble = tri0 (A) terrain type,
//                       upper nibble = tri1 (B) terrain type.
struct GridPoint {
    int16_t  height;
    // Precomputed slope magnitude for each triangle in this grid cell.
    // Both correlate with height-gradient magnitude (Pearson r~0.8) but
    // neither encodes direction -- direction is derived at runtime from
    // neighbour heights.  Scale: approximately mag/11, quantised 0-255.
    // Visual comparison with the passability and shadow maps on 58cm is
    // consistent with this interpretation, but not yet conclusive.
    uint8_t  slope_tri0;   // slope magnitude, triangle A (upper-left)
    uint8_t  slope_tri1;   // slope magnitude, triangle B (lower-right)
    uint8_t  water;        // 0=dry, non-zero=water type
    uint8_t  passability;  // lo nibble = tri A terrain type, hi nibble = tri B
    uint8_t  unk1, unk2;
};

// ---------------------------------------------------------------------------
// Read mesh header + grid data from tags.gor
// ---------------------------------------------------------------------------
static bool readMeshGrid(FILE* tagsGor, long tagOffset,
                         char dot256Name[5], int& meshW, int& meshH,
                         int& gridW, int& gridH,
                         std::vector<GridPoint>& grid,
                         int32_t& outMeshOffset,
                         int32_t& outWaterMaskOffset,
                         int32_t& outWaterMaskLength,
                         char overheadName[5],
                         char preScreenName[5],
                         char postScreenName[5])
{
    uint8_t hdr[1024];
    fseek(tagsGor, tagOffset, SEEK_SET);
    if(fread(hdr,1,1024,tagsGor)!=1024){
        fprintf(stderr,"  Error: failed to read mesh header\n"); return false;
    }
    memcpy(dot256Name, hdr, 4); dot256Name[4]=0;
    meshW = (int)readBE16s(hdr, 8);
    meshH = (int)readBE16s(hdr,10);
    int32_t meshOffset    = readBE32s(hdr, 24);
    outMeshOffset         = meshOffset;
    outWaterMaskOffset    = readBE32s(hdr, 192);
    outWaterMaskLength    = readBE32s(hdr, 196);

    // Screen tag IDs from mesh header (struct offsets minus 64-byte plug-in prefix):
    //   postScreen  at struct 208 -> hdr[144]
    //   preScreen   at struct 212 -> hdr[148]
    //   overheadMap at struct 216 -> hdr[152]
    memcpy(postScreenName, hdr+144, 4); postScreenName[4] = 0;
    memcpy(preScreenName,  hdr+148, 4); preScreenName[4]  = 0;
    memcpy(overheadName,   hdr+152, 4); overheadName[4]   = 0;

    gridW = meshW * 16;
    gridH = meshH * 16;
    int totalPts = gridW * gridH;

    if(meshW<1||meshW>64||meshH<1||meshH>64){
        fprintf(stderr,"  Error: implausible mesh dimensions\n"); return false;
    }

    fseek(tagsGor, tagOffset+(long)meshOffset, SEEK_SET);
    grid.resize(totalPts);
    for(int i=0;i<totalPts;i++){
        uint8_t e[8];
        if(fread(e,1,8,tagsGor)!=8){
            fprintf(stderr,"  Error: short read at grid point %d\n",i); return false;
        }
        grid[i].height      = readBE16s(e,0);
        grid[i].slope_tri0  = e[2];
        grid[i].slope_tri1  = e[3];
        grid[i].water       = e[4];
        grid[i].passability = e[5];
        grid[i].unk1=e[6]; grid[i].unk2=e[7];
    }
    return true;
}

// ---------------------------------------------------------------------------
// Section entry
// ---------------------------------------------------------------------------
struct SecEntry { long absOffset; int32_t length; };

static bool readDot256(FILE* artsound, long tagOffset,
                       int meshW, int meshH,
                       std::vector<SecEntry>& secs, Myth256Palette& pal)
{
    const size_t HDR_SIZE=320, SEC_SIZE=128, TEX_HDR=52;
    fseek(artsound,tagOffset,SEEK_SET);
    uint8_t hdr[HDR_SIZE];
    if(fread(hdr,1,HDR_SIZE,artsound)!=HDR_SIZE){fprintf(stderr,"  Error: failed to read _256Header\n");return false;}
    int32_t palettes      =readBE32s(hdr, 64);
    int32_t paletteOffset =readBE32s(hdr, 68);
    int32_t paletteLength =readBE32s(hdr, 72);
    int32_t sections      =readBE32s(hdr, 96);
    int32_t secInfoOffset =readBE32s(hdr,100);
    int32_t secInfoLength =readBE32s(hdr,104);
    int32_t headerLength  =readBE32s(hdr,248);
    int32_t tagLength     =readBE32s(hdr,252);
    printf("  Header length:   %d\n",headerLength);
    printf("  Palettes:        %d  (offset +%d, length %d)\n",palettes,paletteOffset,paletteLength);
    printf("  Sections:        %d  (offset +%d, length %d)\n",sections,secInfoOffset,secInfoLength);
    printf("  Tag data length: %d bytes\n",tagLength);
    if(headerLength!=320) fprintf(stderr,"  Warning: unexpected header length %d\n",headerLength);
    if(sections<2||sections>2000){fprintf(stderr,"  Error: implausible section count %d\n",sections);return false;}
    int expectedSections=meshW*meshH*2;
    if(sections==expectedSections) printf("  Mesh:            %d x %d tiles (%d sections) OK\n\n",meshW,meshH,sections);
    else printf("  Warning: sections=%d but %dx%dx2=%d expected.\n\n",sections,meshW,meshH,expectedSections);
    long secTablePos=tagOffset+(long)HDR_SIZE+(long)secInfoOffset;
    fseek(artsound,secTablePos,SEEK_SET);
    secs.resize(sections);
    for(int i=0;i<sections;i++){
        uint8_t entry[SEC_SIZE];
        if(fread(entry,1,SEC_SIZE,artsound)!=SEC_SIZE){fprintf(stderr,"  Error: failed reading section entry %d\n",i);return false;}
        uint32_t rawOff=readBE32(entry,64);
        secs[i].absOffset=tagOffset+(long)HDR_SIZE+(long)rawOff+(long)TEX_HDR;
        secs[i].length=readBE32s(entry,68);
    }
    fseek(artsound,tagOffset+(long)HDR_SIZE+(long)paletteOffset,SEEK_SET);
    if(fread(&pal,1,sizeof(pal),artsound)!=sizeof(pal)){fprintf(stderr,"  Error: failed reading palette\n");return false;}
    return true;
}

static std::vector<uint8_t> assembleTiles(FILE* artsound,
                                          const std::vector<SecEntry>& secs,
                                          int meshW, int meshH, int sectionOffset)
{
    int totalW=meshW*256, totalH=meshH*256;
    int sections=(int)secs.size(), meshSize=meshW*meshH;
    std::vector<uint8_t> pixels((size_t)totalW*totalH,0);
    size_t mark=0;
    for(int y=0;y<2*meshSize-meshW;y+=2*meshW){
        for(int line=0;line<256;line++){
            for(int x=2*meshW-2;x>=0;x-=2){
                int idx=x+y+sectionOffset;
                if(idx<0||idx>=sections){mark+=256;continue;}
                fseek(artsound,secs[idx].absOffset+(long)(line*256),SEEK_SET);
                uint8_t scanline[256];
                if(fread(scanline,1,256,artsound)!=256) memset(scanline,0,256);
                for(int a=0,b=255;a<b;a++,b--){uint8_t t=scanline[a];scanline[a]=scanline[b];scanline[b]=t;}
                memcpy(pixels.data()+mark,scanline,256); mark+=256;
            }
        }
    }
    return pixels;
}

// ---------------------------------------------------------------------------
// Passability color table (RGB per terrain type 0-F)
// ---------------------------------------------------------------------------
static const uint8_t PASS_COLOURS[16][3] = {
    {0x20,0x20,0x20},  // 0 Clear
    {0x40,0x80,0xFF},  // 1 Human depth water
    {0x20,0x60,0xCC},  // 2 Giant depth water
    {0x00,0x00,0xAA},  // 3 Deep water
    {0xDD,0xDD,0x00},  // 4 Sloped
    {0xFF,0x88,0x00},  // 5 Steep slope
    {0x20,0xAA,0x20},  // 6 Grass
    {0xCC,0xAA,0x66},  // 7 Desert
    {0x88,0x88,0x88},  // 8 Rocky
    {0x20,0x99,0x88},  // 9 Marsh
    {0xFF,0xFF,0xFF},  // A Snow
    {0x10,0x66,0x10},  // B Forest
    {0xFF,0x00,0xFF},  // C Unknown
    {0xAA,0x00,0xFF},  // D Unknown
    {0xCC,0x00,0x00},  // E Walking impassable
    {0xFF,0x00,0x00},  // F Flying impassable
};

// ---------------------------------------------------------------------------
// Build passability map
//
// Each grid point covers a 16x16 pixel block at texture resolution.
// Each block is split diagonally to show both triangle nibbles.
// Grid data is stored right-to-left in X, so we mirror the column index.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> buildPassabilityMap(
    const std::vector<GridPoint>& grid, int gridW, int gridH,
    int texW, int texH)
{
    int blockW = texW / gridW;   // = 16
    int blockH = texH / gridH;   // = 16
    std::vector<uint8_t> out((size_t)texW * texH * 3, 0);

    for(int row=0; row<gridH; row++){
        for(int col=0; col<gridW; col++){
            // Mirror column: grid is stored right-to-left, texture is left-to-right
            const GridPoint& p = grid[row*gridW + (gridW-1-col)];
            uint8_t pass0 = p.passability & 0x0F;         // lower nibble: triangle A
            uint8_t pass1 = (p.passability >> 4) & 0x0F;  // upper nibble: triangle B

            int px0 = col * blockW;
            int py0 = row * blockH;
            for(int dy=0; dy<blockH; dy++){
                for(int dx=0; dx<blockW; dx++){
                    // Split block diagonally: upper-left = pass0, lower-right = pass1
                    uint8_t pass = (dx + dy < blockW) ? pass0 : pass1;
                    const uint8_t* c = PASS_COLOURS[pass];
                    int outIdx = ((py0+dy)*texW + (px0+dx)) * 3;
                    out[outIdx+0] = c[0];
                    out[outIdx+1] = c[1];
                    out[outIdx+2] = c[2];
                }
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Decode water mask
//
// The mesh tag contains a run-length encoded binary mask at full texture
// resolution.  The RLE stream is an array of big-endian uint16 values:
//   - value == texW  -> advance to the next scanline
//   - otherwise      -> two values (startX, endX) fill pixels startX..endX
//                       with 255 (white = water)
// X is stored right-to-left so we mirror on output, matching the other maps.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> decodeWaterMask(FILE* tagsGor, long tagOffset,
                                             int32_t meshOffset,
                                             int32_t waterMaskOffset,
                                             int32_t waterMaskLength,
                                             int texW, int texH)
{
    std::vector<uint8_t> out((size_t)texW * texH, 0);
    if(waterMaskOffset <= 0 || waterMaskLength <= 0) return out;

    long pos = tagOffset + (long)meshOffset + (long)waterMaskOffset;
    fseek(tagsGor, pos, SEEK_SET);
    std::vector<uint8_t> raw(waterMaskLength);
    if(fread(raw.data(), 1, waterMaskLength, tagsGor) != (size_t)waterMaskLength){
        fprintf(stderr,"  Warning: short read on water mask\n"); return out;
    }

    int numShorts = waterMaskLength / 2;
    int y = 0, k = 0;
    while(k < numShorts && y < texH - 1){
        uint16_t v = (uint16_t)((raw[k*2] << 8) | raw[k*2+1]);
        if(v == (uint16_t)texW){
            y++; k++;
        } else {
            if(k+1 >= numShorts) break;
            uint16_t x0 = v;
            uint16_t x1 = (uint16_t)((raw[(k+1)*2] << 8) | raw[(k+1)*2+1]);
            for(int x = (int)x0; x <= (int)x1 && x < texW; x++)
                out[(size_t)((texW-1-x) + y*texW)] = 255;
            k += 2;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Build water map
//
// Grid water byte: 0=dry, non-zero=water.
// Same 16x16 block scaling and column mirroring as passability map.
// Output: black=dry, blue=water.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> buildWaterMap(
    const std::vector<GridPoint>& grid, int gridW, int gridH,
    int texW, int texH)
{
    int blockW = texW / gridW;
    int blockH = texH / gridH;
    std::vector<uint8_t> out((size_t)texW * texH * 3, 0);

    for(int row=0; row<gridH; row++){
        for(int col=0; col<gridW; col++){
            // Mirror column: grid is stored right-to-left
            uint8_t w = grid[row*gridW + (gridW-1-col)].water;
            if(w == 0) continue;

            // Scale water value into a visible blue range (128-255)
            uint8_t blue  = (uint8_t)(128 + (int)w * 127 / 255);
            uint8_t green = (uint8_t)(w * 80 / 255);

            int px0 = col * blockW;
            int py0 = row * blockH;
            for(int dy=0; dy<blockH; dy++){
                for(int dx=0; dx<blockW; dx++){
                    int outIdx = ((py0+dy)*texW + (px0+dx)) * 3;
                    out[outIdx+0] = 0;
                    out[outIdx+1] = green;
                    out[outIdx+2] = blue;
                }
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Build slope map
//
// Same 16x16 block layout and diagonal split as the passability map.
// Greyscale: black = flat, white = steepest.
// ---------------------------------------------------------------------------
static std::vector<uint8_t> buildSlopeMap(
    const std::vector<GridPoint>& grid, int gridW, int gridH,
    int texW, int texH)
{
    int blockW = texW / gridW;   // = 16
    int blockH = texH / gridH;   // = 16
    std::vector<uint8_t> out((size_t)texW * texH * 3, 0);

    for(int row=0; row<gridH; row++){
        for(int col=0; col<gridW; col++){
            // Mirror column: grid is stored right-to-left, texture is left-to-right
            const GridPoint& p = grid[row*gridW + (gridW-1-col)];

            int px0 = col * blockW;
            int py0 = row * blockH;
            for(int dy=0; dy<blockH; dy++){
                for(int dx=0; dx<blockW; dx++){
                    bool triA = (dx + dy < blockW);
                    uint8_t s = triA ? p.slope_tri0 : p.slope_tri1;
                    int outIdx = ((py0+dy)*texW + (px0+dx)) * 3;
                    out[outIdx+0] = out[outIdx+1] = out[outIdx+2] = s;
                }
            }
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Tag validity check
// ---------------------------------------------------------------------------
static bool isNullTag(const char* name4) {
    const uint8_t* n = (const uint8_t*)name4;
    if(n[0]==0 && n[1]==0 && n[2]==0 && n[3]==0) return true;
    if(n[0]==0xFF && n[1]==0xFF && n[2]==0xFF && n[3]==0xFF) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Extract overhead map from artsound.gor
//
// The overhead map is a .256 tag containing a single top-down bitmap of the
// level.  Width and height are stored at tag-relative offsets 2812/2814
// (standalone offsets 2876/2878 minus the 64-byte plug-in prefix).
// A row-offset table precedes the pixel data; the table has one uint32 per
// row, so the pixel data starts at offset 2912 + 4*h (standalone: 2976+4*h).
// ---------------------------------------------------------------------------
static bool extractOverheadMap(FILE* art, long artSize,
                                const char* name4, const char* outPath)
{
    long tagOff=0, tagLen=0;
    if(!findTagInGor(art, artSize, ".256", name4, tagOff, tagLen)){
        fprintf(stderr,"  Warning: overhead map tag '%.4s' not found\n", name4);
        return false;
    }
    printf("  Found at offset %ld\n", tagOff);

    // Validate: overhead maps are single-image .256 files (0 or 1 sections).
    // Terrain texture atlases have many sections; reject those.
    {
        uint8_t hdr[320];
        fseek(art, tagOff, SEEK_SET);
        if(fread(hdr,1,320,art)!=320){ fprintf(stderr,"  Error: header read\n"); return false; }
        int32_t sections = readBE32s(hdr, 96);
        if(sections > 1){
            printf("  Skipping: tag '%.4s' is a texture atlas (%d sections), not an overhead map.\n",
                   name4, sections);
            return false;
        }
    }

    // Width and height stored as big-endian int16 at tag+2812, tag+2814
    uint8_t dim[4];
    fseek(art, tagOff+2812, SEEK_SET);
    if(fread(dim,1,4,art)!=4){ fprintf(stderr,"  Error: short read on dims\n"); return false; }
    int w = (int)(int16_t)((dim[0]<<8)|dim[1]);
    int h = (int)(int16_t)((dim[2]<<8)|dim[3]);
    if(w<=0||w>4096||h<=0||h>4096){
        fprintf(stderr,"  Warning: implausible overhead dimensions %dx%d\n",w,h); return false;
    }
    printf("  Dimensions: %dx%d\n", w, h);

    // Palette at tag+320 (HDR_SIZE); standalone: 384 = 64+320
    Myth256Palette pal;
    fseek(art, tagOff+320, SEEK_SET);
    if(fread(&pal,1,sizeof(pal),art)!=sizeof(pal)){ fprintf(stderr,"  Error: palette read\n"); return false; }

    // Row-offset table (h uint32s) followed by pixel data
    // Pixel start: tag + 2912 + 4*h  (standalone: 2976 + 4*h)
    long pixOff = tagOff + 2912 + 4L*h;
    std::vector<uint8_t> pixels((size_t)w*h);
    fseek(art, pixOff, SEEK_SET);
    if(fread(pixels.data(),1,(size_t)w*h,art)!=(size_t)w*h){
        fprintf(stderr,"  Warning: short read on overhead pixels\n"); return false;
    }

    printf("  Writing: %s\n", outPath);
    return writeBMPMythPal(outPath, w, h, pal, pixels);
}

// ---------------------------------------------------------------------------
// Extract pre-game or post-game screen from artsound.gor
//
// These are fixed-size 377x190 .256 bitmaps.
// Palette at tag+320 (standalone: 384), pixels at tag+4008 (standalone: 4072).
// Single-image bitmaps have 0 sections; texture atlases have many -- we use
// the sections count to reject tags that are clearly not screen images.
// ---------------------------------------------------------------------------
static bool extractScreenBmp(FILE* art, long artSize,
                               const char* name4, const char* outPath)
{
    long tagOff=0, tagLen=0;
    if(!findTagInGor(art, artSize, ".256", name4, tagOff, tagLen)){
        fprintf(stderr,"  Warning: screen tag '%.4s' not found\n", name4);
        return false;
    }
    printf("  Found at offset %ld\n", tagOff);

    // Validate: single-image .256 files (pre/post-game screens) have 0 or 1 sections.
    // Terrain texture atlases have meshW*meshH*2 sections.  If sections > 1,
    // this tag is not a screen image -- the mesh header field likely doesn't
    // apply to this map type.
    {
        uint8_t hdr[320];
        fseek(art, tagOff, SEEK_SET);
        if(fread(hdr,1,320,art)!=320){ fprintf(stderr,"  Error: header read\n"); return false; }
        int32_t sections = readBE32s(hdr, 96);
        if(sections > 1){
            printf("  Skipping: tag '%.4s' is a texture atlas (%d sections), not a screen image.\n",
                   name4, sections);
            return false;
        }
    }

    const int w = 377, h = 190;

    Myth256Palette pal;
    fseek(art, tagOff+320, SEEK_SET);
    if(fread(&pal,1,sizeof(pal),art)!=sizeof(pal)){ fprintf(stderr,"  Error: palette read\n"); return false; }

    // Pixel data at tag+4008 (standalone: 4072 = 64+4008)
    std::vector<uint8_t> pixels((size_t)w*h);
    fseek(art, tagOff+4008, SEEK_SET);
    if(fread(pixels.data(),1,(size_t)w*h,art)!=(size_t)w*h){
        fprintf(stderr,"  Warning: short read on screen pixels\n"); return false;
    }

    printf("  Writing: %s\n", outPath);
    return writeBMPMythPal(outPath, w, h, pal, pixels);
}

// ---------------------------------------------------------------------------
// Extract CLUT from the terrain .256 tag
//
// The CLUT is a 256xN look-up table used for lighting and blending effects.
// colorMaps/colorMapOffset/colorMapLength are at .256 hdr+160/164/168
// (struct offsets 224/228/232 minus 64-byte plug-in prefix).
// We render it as a 256xN 8-bit BMP indexed by the terrain palette, so each
// cell shows the actual resulting color for that input/modifier combination.
// ---------------------------------------------------------------------------
static bool extractClut(FILE* art, long dot256Offset,
                         const Myth256Palette& pal, const char* outPath)
{
    const size_t HDR_SIZE = 320;
    uint8_t clutInfo[12];
    fseek(art, (long)(dot256Offset+160), SEEK_SET);
    if(fread(clutInfo,1,12,art)!=12){ fprintf(stderr,"  Error: CLUT header read\n"); return false; }

    int32_t colorMaps      = readBE32s(clutInfo, 0);
    int32_t colorMapOffset = readBE32s(clutInfo, 4);
    int32_t colorMapLength = readBE32s(clutInfo, 8);

    printf("  CLUT: %d map(s), offset +%d, length %d bytes\n",
           colorMaps, colorMapOffset, colorMapLength);

    if(colorMaps <= 0 || colorMapLength <= 0){
        printf("  No CLUT present in this tag.\n"); return false;
    }

    long clutPos = dot256Offset + (long)HDR_SIZE + (long)colorMapOffset;
    std::vector<uint8_t> clut((size_t)colorMapLength);
    fseek(art, clutPos, SEEK_SET);
    if(fread(clut.data(),1,(size_t)colorMapLength,art)!=(size_t)colorMapLength){
        fprintf(stderr,"  Warning: short read on CLUT data\n"); return false;
    }

    // Render as 256 x (colorMapLength/256) image using the terrain palette.
    // Each pixel value is a palette index; applying the palette makes each cell
    // show the actual blended color.
    int clutW = 256, clutH = colorMapLength / 256;
    if(clutH <= 0){ fprintf(stderr,"  Warning: CLUT too small to render\n"); return false; }

    printf("  Writing CLUT (%dx%d): %s\n", clutW, clutH, outPath);
    return writeBMPMythPal(outPath, clutW, clutH, pal, clut);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
static void usage(const char* prog) {
    fprintf(stderr,
        "Myth: The Fallen Lords -- Texture Extractor\n\n"
        "Usage:\n"
        "  %s <tags.gor> <meshtag> [output.bmp]\n\n"
        "  tags.gor   Path to tags.gor. artsound.gor must be in same directory.\n"
        "  meshtag    4-character mesh tag name (e.g. sega, 00tm, balo, land)\n"
        "  output.bmp Output filename stem (default: <meshtag>.bmp)\n\n"
        "Outputs:\n"
        "  <meshtag>.bmp             color texture     (8-bit indexed)\n"
        "  <meshtag>_shadow.bmp      shadow map         (8-bit greyscale)\n"
        "  <meshtag>_lit.bmp         color*shadow      (24-bit RGB, multiply)\n"
        "  <meshtag>_passability.bmp passability map    (24-bit RGB, color-coded)\n"
        "  <meshtag>_water.bmp       water map          (24-bit RGB, blue=water)\n"
        "  <meshtag>_slope.bmp       slope map          (8-bit greyscale, black=flat)\n"
        "  <meshtag>_watermask.bmp   water mask         (8-bit, white=water, RLE)\n"
        "  <meshtag>_clut.bmp        color LUT         (256xN, palette-indexed)\n"
        "  <meshtag>_overhead.bmp    overhead map       (8-bit indexed, if present)\n"
        "  <meshtag>_pregame.bmp     pre-game screen    (377x190, if present)\n"
        "  <meshtag>_postgame.bmp    post-game screen   (377x190, if present)\n\n"
        "Examples:\n"
        "  %s tags.gor sega\n"
        "  %s tags.gor sega seven_gates.bmp\n",
        prog, prog, prog);
}

int main(int argc, char* argv[]) {
    if(argc < 3){ usage(argv[0]); return 1; }

    const char* tagsGorPath = argv[1];
    const char* meshTagName = argv[2];
    if(strlen(meshTagName)!=4){fprintf(stderr,"Error: tag must be 4 chars\n");return 1;}

    std::string tagsDir      = dirOf(tagsGorPath);
    std::string artsoundPath = tagsDir + "artsound.gor";
    std::string tag4         = std::string(meshTagName, 4);
    std::string texPath      = argc>=4 ? argv[3] : tag4 + "/" + tag4 + ".bmp";

    auto insertSuffix=[](const std::string& base, const std::string& sfx){
        size_t dot=base.rfind('.');
        return dot!=std::string::npos ? base.substr(0,dot)+sfx+base.substr(dot) : base+sfx;
    };
    std::string shadowPath    = insertSuffix(texPath,"_shadow");
    std::string litPath       = insertSuffix(texPath,"_lit");
    std::string passPath      = insertSuffix(texPath,"_passability");
    std::string waterPath     = insertSuffix(texPath,"_water");
    std::string slopePath     = insertSuffix(texPath,"_slope");
    std::string waterMaskPath = insertSuffix(texPath,"_watermask");
    std::string clutPath      = insertSuffix(texPath,"_clut");
    std::string overheadPath  = insertSuffix(texPath,"_overhead");
    std::string prePath       = insertSuffix(texPath,"_pregame");
    std::string postPath      = insertSuffix(texPath,"_postgame");

    printf("Myth Texture Extractor\n======================\n");
    printf("tags.gor:     %s\n", tagsGorPath);
    printf("artsound:     %s\n", artsoundPath.c_str());
    printf("Mesh tag:     %.4s\n", meshTagName);
    printf("Outputs:      %s\n", texPath.c_str());
    printf("              %s\n", shadowPath.c_str());
    printf("              %s\n", litPath.c_str());
    printf("              %s\n", passPath.c_str());
    printf("              %s\n", waterPath.c_str());
    printf("              %s\n", slopePath.c_str());
    printf("              %s\n", waterMaskPath.c_str());
    printf("              %s\n", clutPath.c_str());
    printf("              %s\n", overheadPath.c_str());
    printf("              %s\n", prePath.c_str());
    printf("              %s\n\n", postPath.c_str());

    // Step 1: mesh tag -> grid + dimensions
    FILE* tagsGor = fopen(tagsGorPath,"rb");
    if(!tagsGor){fprintf(stderr,"Cannot open: %s\n",tagsGorPath);return 1;}
    fseek(tagsGor,0,SEEK_END); long tagsSize=ftell(tagsGor);

    long meshOffset=0, meshLength=0;
    printf("Step 1: Finding mesh tag '%.4s' in tags.gor...\n", meshTagName);
    if(!findTagInGor(tagsGor,tagsSize,"mesh",meshTagName,meshOffset,meshLength)){
        fprintf(stderr,"  Error: not found\n"); fclose(tagsGor); return 1;
    }
    printf("  Found at offset %ld\n", meshOffset);

    char dot256Name[5]={};
    int meshW=0, meshH=0, gridW=0, gridH=0;
    int32_t tagMeshOffset=0, waterMaskOffset=0, waterMaskLength=0;
    char overheadName[5]={}, preScreenName[5]={}, postScreenName[5]={};
    std::vector<GridPoint> grid;
    if(!readMeshGrid(tagsGor,meshOffset,dot256Name,meshW,meshH,gridW,gridH,grid,
                     tagMeshOffset,waterMaskOffset,waterMaskLength,
                     overheadName,preScreenName,postScreenName)){
        fclose(tagsGor); return 1;
    }
    printf("  .256 tag:     '%.4s'\n", dot256Name);
    printf("  Mesh size:    %d x %d tiles\n", meshW, meshH);
    printf("  Grid:         %d x %d points\n", gridW, gridH);
    printf("  Water mask:   offset %d, length %d\n", waterMaskOffset, waterMaskLength);
    printf("  Overhead map: '%.4s'%s\n", overheadName,  isNullTag(overheadName)  ? " (none)" : "");
    printf("  Pre-game:     '%.4s'%s\n", preScreenName, isNullTag(preScreenName) ? " (none)" : "");
    printf("  Post-game:    '%.4s'%s\n\n", postScreenName, isNullTag(postScreenName) ? " (none)" : "");

    if(meshW<1||meshW>64||meshH<1||meshH>64){fprintf(stderr,"  Error: bad dimensions\n");fclose(tagsGor);return 1;}

    int texW = meshW * 256;
    int texH = meshH * 256;

    // Create output directory if needed
    makeDir(dirOf(texPath));

    // Step 1b: decode water mask (still have tagsGor open)
    printf("Step 1b: Decoding water mask (%dx%d)...\n", texW, texH);
    std::vector<uint8_t> waterMaskPx = decodeWaterMask(tagsGor, meshOffset,
                                                        tagMeshOffset,
                                                        waterMaskOffset,
                                                        waterMaskLength,
                                                        texW, texH);
    if(waterMaskLength > 0)
        printf("  OK (%d RLE bytes)\n\n", waterMaskLength);
    else
        printf("  Not present in this tag.\n\n");

    fclose(tagsGor);

    // Step 2: find .256 tag in artsound.gor
    FILE* artsound = fopen(artsoundPath.c_str(),"rb");
    if(!artsound){
        fprintf(stderr,"Step 2: Cannot open artsound.gor: %s\n"
                       "  Make sure it is in the same directory as tags.gor\n",
                artsoundPath.c_str());
        return 1;
    }
    fseek(artsound,0,SEEK_END); long artsoundSize=ftell(artsound);

    long dot256Offset=0, dot256Length=0;
    printf("Step 2: Finding .256 tag '%.4s' in artsound.gor...\n", dot256Name);
    if(!findTagInGor(artsound,artsoundSize,".256",dot256Name,dot256Offset,dot256Length)){
        fprintf(stderr,"  Error: not found\n"); fclose(artsound); return 1;
    }
    printf("  Found at offset %ld\n\n", dot256Offset);

    // Step 3: read .256 structure
    printf("Step 3: Reading .256 tag structure...\n\n");
    std::vector<SecEntry> secs;
    Myth256Palette pal;
    if(!readDot256(artsound,dot256Offset,meshW,meshH,secs,pal)){fclose(artsound);return 1;}

    // Step 4: color texture
    printf("Step 4: Assembling color texture (%dx%d)...\n", texW, texH);
    std::vector<uint8_t> colorPx = assembleTiles(artsound,secs,meshW,meshH,0);
    printf("  Writing: %s\n", texPath.c_str());
    if(!writeBMPMythPal(texPath.c_str(),texW,texH,pal,colorPx)){fclose(artsound);return 1;}
    printf("  Done.\n\n");

    // Step 5: shadow map
    printf("Step 5: Assembling shadow map (%dx%d)...\n", texW, texH);
    std::vector<uint8_t> shadowPx = assembleTiles(artsound,secs,meshW,meshH,1);
    printf("  Writing: %s\n", shadowPath.c_str());
    if(!writeBMPGrey(shadowPath.c_str(),texW,texH,shadowPx)){fclose(artsound);return 1;}
    printf("  Done.\n\n");

    // Step 11: CLUT (from the terrain .256 tag already located)
    printf("Step 11: Extracting CLUT from terrain .256 tag...\n");
    if(!extractClut(artsound, dot256Offset, pal, clutPath.c_str()))
        printf("  Skipped (no CLUT in this tag).\n\n");
    else
        printf("  Done.\n\n");

    // Step 12: overhead map
    if(!isNullTag(overheadName)){
        printf("Step 12: Extracting overhead map '%.4s'...\n", overheadName);
        if(!extractOverheadMap(artsound, artsoundSize, overheadName, overheadPath.c_str()))
            printf("  Skipped.\n\n");
        else
            printf("  Done.\n\n");
    } else {
        printf("Step 12: No overhead map for this level.\n\n");
    }

    // Step 13: pre-game screen
    if(!isNullTag(preScreenName)){
        printf("Step 13: Extracting pre-game screen '%.4s'...\n", preScreenName);
        if(!extractScreenBmp(artsound, artsoundSize, preScreenName, prePath.c_str()))
            printf("  Skipped.\n\n");
        else
            printf("  Done.\n\n");
    } else {
        printf("Step 13: No pre-game screen for this level.\n\n");
    }

    // Step 14: post-game screen
    if(!isNullTag(postScreenName)){
        printf("Step 14: Extracting post-game screen '%.4s'...\n", postScreenName);
        if(!extractScreenBmp(artsound, artsoundSize, postScreenName, postPath.c_str()))
            printf("  Skipped.\n\n");
        else
            printf("  Done.\n\n");
    } else {
        printf("Step 14: No post-game screen for this level.\n\n");
    }

    fclose(artsound);

    // Step 6: multiply composite
    printf("Step 6: Compositing color * shadow (multiply)...\n");
    std::vector<uint8_t> litPx = multiplyComposite(colorPx,shadowPx,pal,texW,texH);
    printf("  Writing: %s\n", litPath.c_str());
    if(!writeBMP24(litPath.c_str(),texW,texH,litPx)) return 1;
    printf("  Done.\n\n");

    // Step 7: passability map
    printf("Step 7: Building passability map (%dx%d from %dx%d grid)...\n",
           texW,texH,gridW,gridH);
    std::vector<uint8_t> passPx = buildPassabilityMap(grid,gridW,gridH,texW,texH);
    printf("  Writing: %s\n", passPath.c_str());
    if(!writeBMP24(passPath.c_str(),texW,texH,passPx)) return 1;
    printf("  Done.\n\n");

    // Step 8: water map
    printf("Step 8: Building water map (%dx%d from %dx%d grid)...\n",
           texW,texH,gridW,gridH);
    std::vector<uint8_t> waterPx = buildWaterMap(grid,gridW,gridH,texW,texH);
    printf("  Writing: %s\n", waterPath.c_str());
    if(!writeBMP24(waterPath.c_str(),texW,texH,waterPx)) return 1;
    printf("  Done.\n\n");

    // Step 9: slope map
    printf("Step 9: Building slope map (%dx%d from %dx%d grid)...\n",
           texW,texH,gridW,gridH);
    std::vector<uint8_t> slopePx = buildSlopeMap(grid,gridW,gridH,texW,texH);
    printf("  Writing: %s\n", slopePath.c_str());
    if(!writeBMP24(slopePath.c_str(),texW,texH,slopePx)) return 1;
    printf("  Done.\n\n");

    // Step 10: water mask
    printf("Step 10: Writing water mask (%dx%d)...\n", texW, texH);
    printf("  Writing: %s\n", waterMaskPath.c_str());
    if(!writeBMPGrey(waterMaskPath.c_str(),texW,texH,waterMaskPx)) return 1;
    printf("  Done.\n\n");

    printf("Success!\n");
    printf("  Color:       %s\n", texPath.c_str());
    printf("  Shadow:       %s\n", shadowPath.c_str());
    printf("  Lit:          %s  (use for OBJ texture)\n", litPath.c_str());
    printf("  Passability:  %s\n", passPath.c_str());
    printf("  Water:        %s\n", waterPath.c_str());
    printf("  Slope:        %s\n", slopePath.c_str());
    printf("  Water mask:   %s\n", waterMaskPath.c_str());
    printf("  CLUT:         %s\n", clutPath.c_str());
    if(!isNullTag(overheadName))  printf("  Overhead map: %s\n", overheadPath.c_str());
    if(!isNullTag(preScreenName)) printf("  Pre-game:     %s\n", prePath.c_str());
    if(!isNullTag(postScreenName))printf("  Post-game:    %s\n", postPath.c_str());
    printf("\n");
    printf("Passability color key:\n");
    printf("  Dark grey=Clear  Lt.blue=Human water  Blue=Giant water  Navy=Deep water\n");
    printf("  Yellow=Sloped  Orange=Steep  Green=Grass  Tan=Desert  Grey=Rocky\n");
    printf("  Teal=Marsh  White=Snow  Dk.green=Forest  Dark red=Walk impass  Red=Fly impass\n");
    printf("Slope:  black=flat, white=steepest\n");
    return 0;
}
