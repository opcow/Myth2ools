// myth_assemble.cpp
// Reassembles a Myth: The Fallen Lords map asset folder into a .gor plugin.
//
// Usage:
//   myth_assemble <folder> [output.gor] [--edit] [--obj <input.obj>] [--heightscale <n>]
//
//   folder           <tag>/ directory created by myth_extract
//   output.gor       defaults to <tag>_plugin.gor
//   --edit           re-read terrain BMPs and patch the mesh + terrain tag

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>
#include <string>
#include <utility>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p,0755)
#endif

// ---------------------------------------------------------------------------
// CRC32 (Myth variant -- no final XOR)
// ---------------------------------------------------------------------------
static uint32_t CRC_TABLE[256];
static bool crcTableReady=false;
static void initCrcTable(){
    if(crcTableReady) return;
    for(int i=0;i<256;i++){
        uint32_t c=(uint32_t)i;
        for(int j=0;j<8;j++) c=(c&1)?(c>>1)^0xEDB88320u:(c>>1);
        CRC_TABLE[i]=c;
    }
    crcTableReady=true;
}
static uint32_t crcUpdate(uint32_t crc, const uint8_t* data, size_t len){
    initCrcTable();
    for(size_t i=0;i<len;i++)
        crc=((crc>>8)&0x00FFFFFFu)^CRC_TABLE[(crc^data[i])&0xFF];
    return crc;
}

// ---------------------------------------------------------------------------
// Endian helpers
// ---------------------------------------------------------------------------
static void put32be(uint8_t* b, uint32_t v){
    b[0]=(v>>24)&0xFF; b[1]=(v>>16)&0xFF; b[2]=(v>>8)&0xFF; b[3]=v&0xFF;
}
static void put16be(uint8_t* b, uint16_t v){ b[0]=(v>>8)&0xFF; b[1]=v&0xFF; }
static uint32_t rb32(const uint8_t* b){ // big-endian unsigned
    return ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];
}
static int32_t rb32s(const uint8_t* b){ return (int32_t)rb32(b); }
static uint32_t get32le(const uint8_t* b){
    return (uint32_t)b[0]|((uint32_t)b[1]<<8)|((uint32_t)b[2]<<16)|((uint32_t)b[3]<<24);
}
static int32_t  get32les(const uint8_t* b){ return (int32_t)get32le(b); }
static uint16_t get16le(const uint8_t* b){ return (uint16_t)(b[0]|((uint16_t)b[1]<<8)); }

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------
static std::vector<uint8_t> readFile(const std::string& path){
    FILE* f=fopen(path.c_str(),"rb");
    if(!f) return {};
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<=0){fclose(f);return {};}
    std::vector<uint8_t> buf((size_t)sz);
    if(fread(buf.data(),1,(size_t)sz,f)!=(size_t)sz){fclose(f);return {};}
    fclose(f); return buf;
}
static bool writeFile(const std::string& path, const uint8_t* data, size_t len){
    FILE* f=fopen(path.c_str(),"wb");
    if(!f){fprintf(stderr,"Cannot write: %s\n",path.c_str());return false;}
    bool ok=(fwrite(data,1,len,f)==len);
    fclose(f); return ok;
}
static bool fileExists(const std::string& p){
    FILE* f=fopen(p.c_str(),"rb"); if(!f) return false; fclose(f); return true;
}
static std::string readTextFile(const std::string& path){
    FILE* f=fopen(path.c_str(),"rb");
    if(!f) return {};
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<0){ fclose(f); return {}; }
    std::string s((size_t)sz,'\0');
    if(sz>0 && fread(&s[0],1,(size_t)sz,f)!=(size_t)sz){ fclose(f); return {}; }
    fclose(f);
    return s;
}
static std::string trimSlash(const std::string& p){
    std::string q=p;
    while(!q.empty()&&(q.back()=='/'||q.back()=='\\')) q.pop_back();
    return q;
}
static std::string baseName(const std::string& p){
    std::string q=trimSlash(p);
    size_t s=q.find_last_of("/\\");
    return s==std::string::npos ? q : q.substr(s+1);
}

// ---------------------------------------------------------------------------
// BMP readers
// ---------------------------------------------------------------------------
static std::vector<uint8_t> readBMP8(const std::string& path, int& outW, int& outH){
    auto raw=readFile(path);
    if(raw.size()<54||raw[0]!='B'||raw[1]!='M'){
        fprintf(stderr,"  Warning: not a BMP: %s\n",path.c_str()); return {};
    }
    uint32_t dataOff=get32le(raw.data()+10);
    int32_t w=get32les(raw.data()+18), h=get32les(raw.data()+22);
    uint16_t bits=get16le(raw.data()+28);
    if(bits!=8){ fprintf(stderr,"  Warning: expected 8-bit BMP: %s\n",path.c_str()); return {}; }
    if(w<=0||h<=0) return {};
    outW=w; outH=h;
    int stride=(w+3)&~3;
    if(dataOff+(size_t)stride*h>raw.size()) return {};
    std::vector<uint8_t> px((size_t)w*h);
    for(int row=0;row<h;row++){
        const uint8_t* src=raw.data()+dataOff+(size_t)(h-1-row)*stride;
        memcpy(px.data()+(size_t)row*w,src,w);
    }
    return px;
}
static std::vector<uint8_t> readBMP24(const std::string& path, int& outW, int& outH){
    auto raw=readFile(path);
    if(raw.size()<54||raw[0]!='B'||raw[1]!='M'){
        fprintf(stderr,"  Warning: not a BMP: %s\n",path.c_str()); return {};
    }
    uint32_t dataOff=get32le(raw.data()+10);
    int32_t w=get32les(raw.data()+18), h=get32les(raw.data()+22);
    uint16_t bits=get16le(raw.data()+28);
    if(bits!=24){ fprintf(stderr,"  Warning: expected 24-bit BMP: %s\n",path.c_str()); return {}; }
    if(w<=0||h<=0) return {};
    outW=w; outH=h;
    int stride=(w*3+3)&~3;
    if(dataOff+(size_t)stride*h>raw.size()) return {};
    std::vector<uint8_t> px((size_t)w*h*3);
    for(int row=0;row<h;row++){
        const uint8_t* src=raw.data()+dataOff+(size_t)(h-1-row)*stride;
        uint8_t* dst=px.data()+(size_t)row*w*3;
        for(int x=0;x<w;x++){
            dst[x*3+0]=src[x*3+2];
            dst[x*3+1]=src[x*3+1];
            dst[x*3+2]=src[x*3+0];
        }
    }
    return px;
}

// ---------------------------------------------------------------------------
// Manifest
// ---------------------------------------------------------------------------
struct Manifest {
    int meshWidth=0, meshHeight=0;
    int meshOffset=1024;
    int waterMaskOffset=0, waterMaskLength=0;
    int unknown3Offset=0, unknown3Length=0;
    int heightMin=0, heightMax=255;
};
static int jsonInt(const std::string& j, const std::string& key, int def=0){
    for(const char* sep : {"\": ", "\":"}) {
        std::string needle="\""+key+sep;
        size_t pos=j.find(needle);
        if(pos==std::string::npos) continue;
        pos+=needle.size();
        while(pos<j.size()&&(j[pos]==' '||j[pos]=='\t')) pos++;
        if(pos>=j.size()) continue;
        bool neg=(j[pos]=='-'); if(neg) pos++;
        int val=0; bool got=false;
        while(pos<j.size()&&j[pos]>='0'&&j[pos]<='9'){val=val*10+(j[pos]-'0');pos++;got=true;}
        if(!got) continue;
        return neg?-val:val;
    }
    return def;
}
static bool readManifest(const std::string& path, Manifest& m){
    auto raw=readFile(path);
    if(raw.empty()){ fprintf(stderr,"  Cannot read: %s\n",path.c_str()); return false; }
    std::string j(raw.begin(),raw.end());
    m.meshWidth       = jsonInt(j,"width",0);
    m.meshHeight      = jsonInt(j,"height",0);
    m.meshOffset      = jsonInt(j,"mesh_header_size",1024);
    m.waterMaskOffset = jsonInt(j,"water_mask_offset",0);
    m.waterMaskLength = jsonInt(j,"water_mask_length",0);
    m.unknown3Offset  = jsonInt(j,"unknown3_offset",0);
    m.unknown3Length  = jsonInt(j,"unknown3_length",0);
    m.heightMin       = jsonInt(j,"min",0);
    m.heightMax       = jsonInt(j,"max",255);
    return m.meshWidth>0 && m.meshHeight>0;
}

// ---------------------------------------------------------------------------
// Mesh patchers
// ---------------------------------------------------------------------------
static bool parseOBJHeights(const std::string& path, float hs, int gridW, int gridH,
                            std::vector<int16_t>& heightsOut){
    FILE* f=fopen(path.c_str(),"r");
    if(!f){ fprintf(stderr,"  Cannot open OBJ: %s\n",path.c_str()); return false; }

    float halfW=(float)(gridW-1)*0.5f;
    float halfH=(float)(gridH-1)*0.5f;
    int total=gridW*gridH;
    heightsOut.assign(total,0);
    std::vector<uint8_t> filled((size_t)total,0);

    int vCount=0, mapped=0, clamped=0;
    char line[256];
    while(fgets(line,sizeof(line),f)){
        if(line[0]!='v' || line[1]!=' ') continue;
        float x=0.0f,y=0.0f,z=0.0f;
        if(sscanf(line+2,"%f %f %f",&x,&y,&z)!=3) continue;
        vCount++;

        int col=(int)roundf(x+halfW);
        int row=(int)roundf(-z+halfH);
        if(col<0||col>=gridW||row<0||row>=gridH){
            fprintf(stderr,"  Warning: OBJ vertex out of range: v %g %g %g -> (%d,%d)\n",
                    x,y,z,row,col);
            continue;
        }

        float hf=y/hs;
        int16_t h=0;
        if(hf<-32768.0f){ h=-32768; clamped++; }
        else if(hf>32767.0f){ h=32767; clamped++; }
        else h=(int16_t)roundf(hf);

        int idx=row*gridW+col;
        heightsOut[(size_t)idx]=h;
        filled[(size_t)idx]=1;
        mapped++;
    }
    fclose(f);

    int missing=0;
    for(int i=0;i<total;i++) if(!filled[(size_t)i]) missing++;
    printf("    OBJ vertices: %d read, %d mapped (%d missing, %d clamped)\n",
           vCount,mapped,missing,clamped);
    if(missing>0){
        fprintf(stderr,"  Warning: OBJ does not cover all grid points; missing points default to 0.\n");
    }
    return mapped>0;
}

static uint8_t slopeByteFromMagnitude(float mag){
    float v=mag/11.0f;
    if(v<0.0f) v=0.0f;
    if(v>255.0f) v=255.0f;
    return (uint8_t)roundf(v);
}

static void computeObjSlopes(const std::vector<int16_t>& h, int gridW, int gridH,
                             std::vector<uint8_t>& s0, std::vector<uint8_t>& s1){
    int total=gridW*gridH;
    s0.assign((size_t)total,0);
    s1.assign((size_t)total,0);
    auto H=[&](int r,int c)->float{ return (float)h[(size_t)r*gridW+c]; };
    for(int r=0;r<gridH-1;r++){
        for(int c=0;c<gridW-1;c++){
            float dR  = H(r,c+1)   - H(r,c);
            float dD  = H(r+1,c)   - H(r,c);
            float dRB = H(r+1,c+1) - H(r+1,c);
            float dDB = H(r+1,c+1) - H(r,c+1);
            s0[(size_t)r*gridW+c]=slopeByteFromMagnitude(sqrtf(dR*dR+dD*dD));
            s1[(size_t)r*gridW+c]=slopeByteFromMagnitude(sqrtf(dRB*dRB+dDB*dDB));
        }
    }
}

static bool applyObjEdit(std::vector<uint8_t>& mesh, const Manifest& m,
                         const std::string& objPath, float heightScale){
    int gridW=m.meshWidth*16, gridH=m.meshHeight*16;
    std::vector<int16_t> heights;
    if(!parseOBJHeights(objPath,heightScale,gridW,gridH,heights)) return false;

    std::vector<uint8_t> s0,s1;
    computeObjSlopes(heights,gridW,gridH,s0,s1);

    int hChanged=0, sChanged=0;
    for(int row=0;row<gridH;row++){
        for(int col=0;col<gridW;col++){
            size_t off=(size_t)m.meshOffset+(size_t)(row*gridW+col)*8;
            if(off+7>=mesh.size()) continue;

            int16_t oldH=(int16_t)(((uint16_t)mesh[off]<<8)|mesh[off+1]);
            int16_t newH=heights[(size_t)row*gridW+col];
            if(oldH!=newH){
                put16be(mesh.data()+off,(uint16_t)newH);
                hChanged++;
            }

            if(mesh[off+2]!=s0[(size_t)row*gridW+col]){ mesh[off+2]=s0[(size_t)row*gridW+col]; sChanged++; }
            if(mesh[off+3]!=s1[(size_t)row*gridW+col]){ mesh[off+3]=s1[(size_t)row*gridW+col]; sChanged++; }
        }
    }
    printf("    OBJ height changes: %d\n",hChanged);
    printf("    OBJ slope changes:  %d (approximate)\n",sChanged);
    return true;
}

static void patchHeights(std::vector<uint8_t>& mesh, const Manifest& m,
                          const std::vector<uint8_t>& bmp, int bW, int bH){
    int gridW=m.meshWidth*16, gridH=m.meshHeight*16;
    int blkW=bW/gridW, blkH=bH/gridH;
    if(blkW<=0||blkH<=0) return;
    int changed=0;
    for(int row=0;row<gridH;row++) for(int col=0;col<gridW;col++){
        int px=(gridW-1-col)*blkW+blkW/2, py=row*blkH+blkH/2;
        if(px>=bW||py>=bH) continue;
        uint8_t pv=bmp[(size_t)py*bW+px];
        int16_t h=(int16_t)(m.heightMin+(int)pv*(m.heightMax-m.heightMin)/255);
        size_t off=(size_t)m.meshOffset+(size_t)(row*gridW+col)*8;
        if(off+1>=mesh.size()) continue;
        int16_t old=(int16_t)(((uint16_t)mesh[off]<<8)|mesh[off+1]);
        if(old!=h){ mesh[off]=(uint8_t)((uint16_t)h>>8); mesh[off+1]=(uint8_t)((uint16_t)h&0xFF); changed++; }
    }
    printf("    %d grid points changed\n",changed);
}
static uint8_t nearestPass(uint8_t r, uint8_t g, uint8_t b){
    if(r>200&&g<100&&b<100) return 1;
    if(g>200&&r<100&&b<100) return 2;
    if(b>200&&r<100&&g<100) return 3;
    return 0;
}
static void patchPassability(std::vector<uint8_t>& mesh, const Manifest& m,
                              const std::vector<uint8_t>& bmp, int bW, int bH){
    int gridW=m.meshWidth*16, gridH=m.meshHeight*16;
    int blkW=bW/gridW, blkH=bH/gridH;
    if(blkW<=0||blkH<=0) return;
    int changed=0;
    for(int row=0;row<gridH;row++) for(int col=0;col<gridW;col++){
        int baseX=(gridW-1-col)*blkW, baseY=row*blkH;
        int px0=baseX+blkW/4, py0=baseY+blkH/4, px1=baseX+3*blkW/4, py1=baseY+3*blkH/4;
        if(px0>=bW||py0>=bH||px1>=bW||py1>=bH) continue;
        uint8_t p0=nearestPass(bmp[((size_t)py0*bW+px0)*3],bmp[((size_t)py0*bW+px0)*3+1],bmp[((size_t)py0*bW+px0)*3+2]);
        uint8_t p1=nearestPass(bmp[((size_t)py1*bW+px1)*3],bmp[((size_t)py1*bW+px1)*3+1],bmp[((size_t)py1*bW+px1)*3+2]);
        uint8_t pNew=(uint8_t)((p1<<4)|p0);
        size_t off=(size_t)m.meshOffset+(size_t)(row*gridW+col)*8+5;
        if(off>=mesh.size()) continue;
        if(mesh[off]!=pNew){mesh[off]=pNew;changed++;}
    }
    printf("    %d grid points changed\n",changed);
}
static void patchWater(std::vector<uint8_t>& mesh, const Manifest& m,
                        const std::vector<uint8_t>& bmp, int bW, int bH){
    int gridW=m.meshWidth*16, gridH=m.meshHeight*16;
    int blkW=bW/gridW, blkH=bH/gridH;
    if(blkW<=0||blkH<=0) return;
    int changed=0;
    for(int row=0;row<gridH;row++) for(int col=0;col<gridW;col++){
        int px=(gridW-1-col)*blkW+blkW/2, py=row*blkH+blkH/2;
        if(px>=bW||py>=bH) continue;
        uint8_t blue=bmp[((size_t)py*bW+px)*3+2];
        uint8_t wNew=(blue<128)?0:(uint8_t)((int)(blue-128)*255/127);
        size_t off=(size_t)m.meshOffset+(size_t)(row*gridW+col)*8+4;
        if(off>=mesh.size()) continue;
        if(mesh[off]!=wNew){mesh[off]=wNew;changed++;}
    }
    printf("    %d grid points changed\n",changed);
}

// ---------------------------------------------------------------------------
// Water mask encoder
// ---------------------------------------------------------------------------
static std::vector<uint8_t> encodeWaterMask(const std::vector<uint8_t>& bmp, int tW, int tH){
    std::vector<uint8_t> out;
    auto push16=[&](uint16_t v){out.push_back(uint8_t(v>>8));out.push_back(uint8_t(v&0xFF));};
    for(int y=0;y<tH;y++){
        std::vector<std::pair<uint16_t,uint16_t>> rowSpans;
        int c=0;
        while(c<tW){
            if(bmp[(size_t)y*tW+c]>=128){
                int s=c;
                while(c<tW&&bmp[(size_t)y*tW+c]>=128) c++;
                rowSpans.push_back({(uint16_t)(tW-c),(uint16_t)(tW-1-s)});
            } else c++;
        }
        for(int i=(int)rowSpans.size()-1;i>=0;i--){ push16(rowSpans[i].first); push16(rowSpans[i].second); }
        push16((uint16_t)tW);
    }
    return out;
}
static bool rebuildWaterMaskSection(std::vector<uint8_t>& mesh, const Manifest& m,
                                     const std::vector<uint8_t>& newMask){
    long bef=(long)m.meshOffset+(long)m.waterMaskOffset;
    long aft=(long)m.meshOffset+(long)m.unknown3Offset;
    if(bef<0||(size_t)bef>mesh.size()) return false;
    std::vector<uint8_t> unk3;
    if(m.unknown3Length>0&&aft>=0&&aft+(long)m.unknown3Length<=(long)mesh.size())
        unk3.assign(mesh.begin()+aft,mesh.begin()+aft+m.unknown3Length);
    std::vector<uint8_t> n;
    n.insert(n.end(),mesh.begin(),mesh.begin()+bef);
    n.insert(n.end(),newMask.begin(),newMask.end());
    n.insert(n.end(),unk3.begin(),unk3.end());
    if(n.size()<212) return false;
    long newWML=(long)newMask.size();
    long newU3O=(long)m.waterMaskOffset+newWML;
    // tagLength at offset 28 = mesh section size (does NOT include 1024-byte preamble)
    long newTL=newU3O+(long)m.unknown3Length;
    put32be(n.data()+28,(uint32_t)newTL);
    put32be(n.data()+196,(uint32_t)newWML);
    put32be(n.data()+204,(uint32_t)newU3O);
    printf("    water mask: %d -> %ld bytes\n",m.waterMaskLength,newWML);
    mesh=std::move(n); return true;
}
static bool applyBmpEdits(std::vector<uint8_t>& mesh, const Manifest& m, const std::string& folder,
                          const std::string& objPath, float objHeightScale, bool objExplicit){
    auto tp=[&](const std::string& s){ return folder+"/terrain/"+s; };
    bool any=false; int texW=m.meshWidth*256, texH=m.meshHeight*256;
    bool objUsed=false;
    if(!objPath.empty()){
        if(fileExists(objPath)){
            printf("  %s...\n",objPath.c_str());
            if(applyObjEdit(mesh,m,objPath,objHeightScale)){
                any=true;
                objUsed=true;
            }
        } else if(objExplicit){
            fprintf(stderr,"  OBJ not found: %s\n",objPath.c_str());
        }
    }
    if(!objUsed && fileExists(tp("height.bmp"))){
        int bW=0,bH=0; auto bmp=readBMP8(tp("height.bmp"),bW,bH);
        if(!bmp.empty()&&bW==texW&&bH==texH){ printf("  height.bmp...\n"); patchHeights(mesh,m,bmp,bW,bH); any=true; }
        else if(!bmp.empty()) fprintf(stderr,"  height.bmp: %dx%d != expected %dx%d, skipped\n",bW,bH,texW,texH);
    }
    if(fileExists(tp("passability.bmp"))){
        int bW=0,bH=0; auto bmp=readBMP24(tp("passability.bmp"),bW,bH);
        if(!bmp.empty()&&bW==texW&&bH==texH){ printf("  passability.bmp...\n"); patchPassability(mesh,m,bmp,bW,bH); any=true; }
        else if(!bmp.empty()) fprintf(stderr,"  passability.bmp: %dx%d != expected %dx%d, skipped\n",bW,bH,texW,texH);
    }
    if(fileExists(tp("water.bmp"))){
        int bW=0,bH=0; auto bmp=readBMP24(tp("water.bmp"),bW,bH);
        if(!bmp.empty()&&bW==texW&&bH==texH){ printf("  water.bmp...\n"); patchWater(mesh,m,bmp,bW,bH); any=true; }
    }
    if(fileExists(tp("watermask.bmp"))){
        int bW=0,bH=0; auto bmp=readBMP8(tp("watermask.bmp"),bW,bH);
        if(!bmp.empty()&&bW==texW&&bH==texH){
            printf("  watermask.bmp...\n");
            auto enc=encodeWaterMask(bmp,texW,texH);
            rebuildWaterMaskSection(mesh,m,enc); any=true;
        } else if(!bmp.empty()) fprintf(stderr,"  watermask.bmp size mismatch, skipped\n");
    }
    return any;
}

// ---------------------------------------------------------------------------
// .256 terrain texture: inject terrain.bmp into tile sections
//
// .256 layout (all offsets from tag start):
//   [0..319]        320-byte header
//     hdr[68]       palOff  - offset (from HDR) to Myth256Palette
//     hdr[96]       sections count
//     hdr[100]      secOff  - offset (from HDR) to section table
//   [HDR + secOff]  section table: sections * 128 bytes each
//     entry[64]     relOff  - offset (from HDR) to section data (incl. 52-byte texhdr)
//     entry[68]     length  - section data length (excl. texhdr)
//   [HDR + relOff + 52]  raw 256x256 pixel data, one row per 256 bytes
//
// assembleTiles tile order (secOff=0 for color, 1 for shadow):
//   for r in 0..meshH-1:
//     for c in meshW-1..0:   (right to left)
//       idx = 2*(r*meshW + (meshW-1-c)) + secOff
//       pixels in each row are stored REVERSED (left<->right)
// ---------------------------------------------------------------------------
static bool injectColor(std::vector<uint8_t>& terrain,
                          const std::vector<uint8_t>& bmp, int bW, int bH,
                          const std::vector<uint8_t>& bmpRaw)
{
    const size_t HDR=320, SECSZ=128, TEXHDR=52;
    if(terrain.size()<HDR+4) return false;

    int32_t sections = rb32s(terrain.data()+96);
    int32_t secOff   = rb32s(terrain.data()+100);
    int32_t palOff   = rb32s(terrain.data()+68);
    if(sections<2||sections>4000) return false;

    int meshW=bW/256, meshH=bH/256;
    if(meshW*meshH*2 != sections){
        fprintf(stderr,"  terrain.bmp tile count %dx%d*2=%d != sections %d\n",
                meshW,meshH,meshW*meshH*2,sections);
        return false;
    }

    // -----------------------------------------------------------------------
    // 1. Update Myth256Palette from BMP palette
    //    Myth256Palette: int32 colors; int32 unk[7]; MythColor color[256];
    //    MythColor: red, fr, green, fg, blue, fb, flag, ff (8 bytes)
    //    readBMP copies BMP BGRA palette directly into red/green/blue, zeroes fr/fg/fb.
    // -----------------------------------------------------------------------
    size_t palBase = HDR + (size_t)palOff;
    size_t colorBase = palBase + 32; // skip 32-byte palette header
    if(colorBase + 256*8 > terrain.size()){
        fprintf(stderr,"  palette out of bounds\n"); return false;
    }
    // Read BMP palette: at offset 14+DIBsize, 4 bytes per entry (BGRA)
    if(bmpRaw.size() >= 54){
        uint32_t dibSz = get32le(bmpRaw.data()+14);
        uint32_t palStart = 14 + dibSz;
        uint32_t pixOff   = get32le(bmpRaw.data()+10);
        uint32_t nEntries = (pixOff > palStart) ? (pixOff - palStart) / 4 : 0;
        if(nEntries > 256) nEntries = 256;
        printf("    updating palette: %u entries from BMP\n", nEntries);
        // Zero all 256 entries first
        for(int i=0;i<256;i++){
            uint8_t* ce = terrain.data()+colorBase+(size_t)i*8;
            ce[0]=0; ce[1]=0; ce[2]=0; ce[3]=0;
            ce[4]=0; ce[5]=0; ce[6]=0; ce[7]=0;
        }
        // Copy BMP BGRA palette → MythColor{red,fr,green,fg,blue,fb,flag,ff}
        for(uint32_t i=0;i<nEntries;i++){
            uint8_t b=bmpRaw[palStart+i*4+0];
            uint8_t g=bmpRaw[palStart+i*4+1];
            uint8_t r=bmpRaw[palStart+i*4+2];
            uint8_t* ce = terrain.data()+colorBase+(size_t)i*8;
            ce[0]=r; ce[1]=0; ce[2]=g; ce[3]=0; ce[4]=b; ce[5]=0; ce[6]=0; ce[7]=0;
        }
    }

    // -----------------------------------------------------------------------
    // 2. Inject pixel data into sections (rows reversed, columns right-to-left)
    // -----------------------------------------------------------------------
    std::vector<uint32_t> relOfs((size_t)sections);
    for(int i=0;i<sections;i++){
        size_t eStart=HDR+(size_t)secOff+(size_t)i*SECSZ;
        if(eStart+68+4>terrain.size()) return false;
        relOfs[i]=rb32(terrain.data()+eStart+64);
    }
    int changed=0;
    for(int r=0;r<meshH;r++){
        for(int c=0;c<meshW;c++){
            int idx=2*(r*meshW+(meshW-1-c));
            size_t pixStart=HDR+relOfs[idx]+TEXHDR;
            if(pixStart+256*256>terrain.size()){
                fprintf(stderr,"  section %d out of bounds\n",idx); return false;
            }
            for(int line=0;line<256;line++){
                const uint8_t* src=bmp.data()+(size_t)(r*256+line)*bW+c*256;
                uint8_t* dst=terrain.data()+pixStart+(size_t)line*256;
                for(int x=0;x<256;x++) dst[x]=src[255-x];
            }
            changed++;
        }
    }
    printf("    %d tiles injected\n", changed);

    // -----------------------------------------------------------------------
    // 3. Regenerate 256x256 color map (mixing table)
    //    colorMap[0][x] = x  (identity row)
    //    colorMap[y][0] = y  (identity column)
    //    colorMap[y][x] = palette index closest to avg(palette[x], palette[y])
    // -----------------------------------------------------------------------
    // Find color map location from header offset 160: cmaps, cmOff, cmLen
    if(terrain.size() >= 172){
        int32_t cmaps = rb32s(terrain.data()+160);
        int32_t cmOff = rb32s(terrain.data()+164);
        int32_t cmLen = rb32s(terrain.data()+168);
        if(cmaps>0 && cmLen==65536){
            size_t cmBase = HDR + (size_t)cmOff;
            if(cmBase+65536 <= terrain.size()){
                printf("    rebuilding 256x256 color map...\n");
                uint8_t* cm = terrain.data()+cmBase;
                // Cache palette RGB
                uint8_t pr[256], pg[256], pb[256];
                for(int i=0;i<256;i++){
                    const uint8_t* ce = terrain.data()+colorBase+(size_t)i*8;
                    pr[i]=ce[0]; pg[i]=ce[2]; pb[i]=ce[4];
                }
                // Row 0: identity
                for(int x=0;x<256;x++) cm[x]=(uint8_t)x;
                // Remaining rows
                for(int y=1;y<256;y++){
                    cm[y*256+0]=(uint8_t)y; // column 0: identity
                    int yr=pr[y], yg=pg[y], yb=pb[y];
                    for(int x=1;x<256;x++){
                        int avgR=(pr[x]+yr)/2;
                        int avgG=(pg[x]+yg)/2;
                        int avgB=(pb[x]+yb)/2;
                        int best=0x7fffffff; uint8_t bestIdx=0;
                        for(int i=255;i>=0;i--){
                            int dr=avgR-pr[i], dg=avgG-pg[i], db=avgB-pb[i];
                            int d=dr*dr+dg*dg+db*db;
                            if(d<best){ best=d; bestIdx=(uint8_t)i; }
                            if(best==0) break;
                        }
                        cm[y*256+x]=bestIdx;
                    }
                }
            }
        }
    }
    return true;
}
static bool applyTerrainEdits(std::vector<uint8_t>& terrain,
                               const std::string& folder, int meshW, int meshH)
{
    std::string path=folder+"/terrain/terrain.bmp";
    if(!fileExists(path)) return false;
    int bW=0,bH=0;
    auto bmpRaw=readFile(path);          // raw bytes for palette extraction
    auto bmp=readBMP8(path,bW,bH);       // flipped pixel array
    int expW=meshW*256, expH=meshH*256;
    if(bmp.empty()||bW!=expW||bH!=expH){
        if(!bmp.empty()) fprintf(stderr,"  terrain.bmp: %dx%d != expected %dx%d, skipped\n",bW,bH,expW,expH);
        return false;
    }
    printf("  terrain.bmp...\n");
    return injectColor(terrain,bmp,bW,bH,bmpRaw);
}

// ---------------------------------------------------------------------------
// STLI text patching
// ---------------------------------------------------------------------------
static bool applyStliText(std::vector<uint8_t>& tagData, const std::string& textPath){
    if(!fileExists(textPath)) return false;

    std::string text=readTextFile(textPath);

    std::vector<uint8_t> out;
    out.reserve(text.size()+1);
    for(size_t i=0;i<text.size();i++){
        unsigned char c=(unsigned char)text[i];
        if(c=='\r'){
            out.push_back(0x0D);
            if(i+1<text.size() && text[i+1]=='\n') i++;
        } else if(c=='\n'){
            out.push_back(0x0D);
        } else if(c=='\t' || (c>=0x20 && c<=0x7E)){
            out.push_back(c);
        }
    }

    if(out.empty() || out.back()!=0x0D)
        out.push_back(0x0D);

    tagData=std::move(out);
    printf("  %s...\n",textPath.c_str());
    printf("    stli bytes: %zu\n",tagData.size());
    return true;
}

// ---------------------------------------------------------------------------
// Tag entry + loader
// ---------------------------------------------------------------------------
struct TagEntry {
    std::string label;
    uint8_t plugHdr[64];
    std::vector<uint8_t> data;
};
static bool loadTag(const std::string& bin, const std::string& hdr,
                    const std::string& lbl, TagEntry& out){
    auto d=readFile(bin); if(d.empty()) return false;
    auto h=readFile(hdr);
    out.label=lbl;
    out.data=std::move(d);
    memset(out.plugHdr,0,64);
    if(h.size()>=64) memcpy(out.plugHdr,h.data(),64);
    return true;
}

// ---------------------------------------------------------------------------
// .gor builder  (old Myth TFL format)
//
// Layout:
//   [0..127]         128-byte file header: 0x00010001 + gorName + zeros
//   [128..dataEnd]   tag data, packed with no gaps
//   [dataEnd..end]   index: one 64-byte plug header per tag, contiguous
//                    plug header [48..51] = absolute data offset in file
//                    plug header [52..55] = data length
// ---------------------------------------------------------------------------
static bool buildGor(const std::vector<TagEntry>& tags, const char* outPath, const char* gorName){
    const size_t FILE_HDR=128;

    // File header
    uint8_t fileHdr[FILE_HDR]={};
    fileHdr[0]=0x00; fileHdr[1]=0x01; fileHdr[2]=0x00; fileHdr[3]=0x01;
    std::string gn=std::string(gorName)+".gor";
    memcpy(fileHdr+4,gn.c_str(),std::min(gn.size(),(size_t)(FILE_HDR-4)));

    // Calculate absolute data offsets (packed, no gaps)
    std::vector<uint32_t> offsets(tags.size());
    size_t cur=FILE_HDR;
    for(size_t i=0;i<tags.size();i++){
        offsets[i]=(uint32_t)cur;
        cur+=tags[i].data.size();
    }
    // cur is now the index start offset

    // Write index metadata into header (required for game to locate the index)
    // [0x28..0x2B] = big-endian uint32: offset to start of index
    // [0x2C..0x2D] = big-endian uint16: number of entries
    // [0x2E..0x2F] = big-endian uint16: entry size (64 = 0x40)
    // [0x30..0x31] = big-endian uint16: version (1)
    uint32_t idxOff=(uint32_t)cur;
    uint16_t cnt=(uint16_t)tags.size();
    fileHdr[0x28]=(idxOff>>24)&0xFF; fileHdr[0x29]=(idxOff>>16)&0xFF;
    fileHdr[0x2A]=(idxOff>> 8)&0xFF; fileHdr[0x2B]= idxOff     &0xFF;
    fileHdr[0x2C]=(cnt>>8)&0xFF;     fileHdr[0x2D]= cnt         &0xFF;
    fileHdr[0x2E]=0x00;               fileHdr[0x2F]=0x40;
    fileHdr[0x30]=0x00;               fileHdr[0x31]=0x01;

    std::vector<uint8_t> out;
    auto push=[&](const uint8_t* b,size_t n){out.insert(out.end(),b,b+n);};

    // Header + data
    push(fileHdr,FILE_HDR);
    for(size_t i=0;i<tags.size();i++)
        push(tags[i].data.data(),tags[i].data.size());

    // Index: plug headers with embedded absolute offsets
    for(size_t i=0;i<tags.size();i++){
        uint8_t hdr[64];
        memcpy(hdr,tags[i].plugHdr,64);
        put32be(hdr+48,offsets[i]);
        put32be(hdr+52,(uint32_t)tags[i].data.size());
        push(hdr,64);
    }

    if(!writeFile(outPath,out.data(),out.size())) return false;
    printf("Wrote %zu bytes to %s\n",out.size(),outPath);
    return true;
}

// ---------------------------------------------------------------------------
// Tag name derivation:
//   stli: last char -> 's'   e.g. "99mc" -> "99ms"
// ---------------------------------------------------------------------------
static std::string deriveStliName(const std::string& t){
    std::string s=t; if(s.size()==4) s[3]='s'; return s;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
static void usage(const char* p){
    fprintf(stderr,
        "Myth TFL Map Asset Assembler\n\n"
        "Usage:  %s <folder> [output.gor] [--edit] [--obj <input.obj>] [--heightscale <n>]\n\n"
        "  folder           <tag>/ directory from myth_extract\n"
        "  output.gor       defaults to <tag>_plugin.gor\n"
        "  --edit           apply mesh/terrain edits before assembly\n"
        "  --obj <file>     import mesh heights from OBJ and recompute slopes\n"
        "                   (during --edit; skips height.bmp when used)\n"
        "  --heightscale <n> OBJ vertical scale, default 0.002\n",p);
}

int main(int argc, char* argv[])
{
    if(argc<2){usage(argv[0]);return 1;}
    std::string folder=trimSlash(argv[1]);
    std::string tag4=baseName(folder);
    if(tag4.size()!=4){ fprintf(stderr,"Error: folder must be a 4-char tag name\n"); return 1; }
    std::string outPath=tag4+"_plugin.gor";
    bool doEdit=false;
    std::string objPath;
    bool objExplicit=false;
    float objHeightScale=0.002f;
    bool outPathExplicit=false;
    for(int i=2;i<argc;i++){
        if(strcmp(argv[i],"--edit")==0) doEdit=true;
        else if(strcmp(argv[i],"--obj")==0){
            objExplicit=true;
            if(i+1<argc) objPath=argv[++i];
            else{ fprintf(stderr,"Error: --obj requires a path\n"); return 1; }
        } else if(strcmp(argv[i],"--heightscale")==0){
            if(i+1<argc) objHeightScale=(float)atof(argv[++i]);
            else{ fprintf(stderr,"Error: --heightscale requires a number\n"); return 1; }
            if(objHeightScale<=0.0f){ fprintf(stderr,"Error: --heightscale must be positive\n"); return 1; }
        } else if(argv[i][0]=='-'){
            fprintf(stderr,"Error: unknown option: %s\n",argv[i]);
            return 1;
        } else if(!outPathExplicit){
            outPath=argv[i];
            outPathExplicit=true;
        } else {
            fprintf(stderr,"Error: multiple output paths specified: %s and %s\n",
                    outPath.c_str(), argv[i]);
            return 1;
        }
    }

    printf("Myth TFL Map Asset Assembler\n============================\n");
    printf("Folder: %s/\nMode:   %s -> .gor\n\n",
           folder.c_str(), doEdit?"edit":"pass-through");
    if(doEdit){
        if(objExplicit) printf("OBJ:    %s  (scale %.6f)\n\n",objPath.c_str(),objHeightScale);
        else printf("OBJ:    auto-detect %s/%s.obj  (scale %.6f)\n\n",folder.c_str(),tag4.c_str(),objHeightScale);
    }

    struct Cand{ std::string bin,hdr,lbl; };
    std::vector<Cand> cands={
        {folder+"/terrain/terrain_tag.bin", folder+"/terrain/terrain_plug.hdr", ".256"},
        {folder+"/screens/pregame_tag.bin", folder+"/screens/pregame_plug.hdr", "pre" },
        {folder+"/screens/postgame_tag.bin",folder+"/screens/postgame_plug.hdr","post"},
        {folder+"/screens/overhead_tag.bin",folder+"/screens/overhead_plug.hdr","over"},
        {folder+"/raw/mesh_tag.bin",         folder+"/raw/mesh_plug.hdr",        "mesh"},
        {folder+"/strings/name_tag.bin",     folder+"/strings/name_plug.hdr",    "stli"},
        {folder+"/strings/captions_tag.bin", folder+"/strings/captions_plug.hdr","capt"},
    };

    std::vector<TagEntry> tags;
    printf("Loading tags:\n");
    for(auto& c:cands){
        TagEntry te;
        if(loadTag(c.bin,c.hdr,c.lbl,te)){
            printf("  %-6s  %s\n",c.lbl.c_str(),c.bin.c_str());
            tags.push_back(std::move(te));
        } else printf("  %-6s  (not present)\n",c.lbl.c_str());
    }
    if(tags.empty()){fprintf(stderr,"Error: no tags found\n");return 1;}

    TagEntry* meshEntry=nullptr;
    TagEntry* terrainEntry=nullptr;
    TagEntry* nameStliEntry=nullptr;
    TagEntry* captionsEntry=nullptr;
    for(auto& t:tags){
        if(t.label=="mesh")  meshEntry=&t;
        if(t.label==".256")  terrainEntry=&t;
        if(t.label=="stli")  nameStliEntry=&t;
        if(t.label=="capt")  captionsEntry=&t;
    }

    // Always rename stli to a unique name and patch mesh offset 140.
    // If stli keeps its original name it collides with another base-game
    // mesh that also references that stli, causing that mesh to appear
    // as a spurious map entry and crash when loaded.
    if(nameStliEntry){
        std::string sname=deriveStliName(tag4);
        char old[5]={0}; memcpy(old,nameStliEntry->plugHdr+36,4);
        memcpy(nameStliEntry->plugHdr+36,sname.c_str(),4);
        if(meshEntry && meshEntry->data.size()>143){
            meshEntry->data[140]=sname[0]; meshEntry->data[141]=sname[1];
            meshEntry->data[142]=sname[2]; meshEntry->data[143]=sname[3];
        }
        if(strcmp(old,sname.c_str())!=0)
            printf("Stli tag:    '%s' -> '%s'\n",old,sname.c_str());
    }

    // Apply BMP edits if requested
    if(doEdit){
        std::string mf=folder+"/manifest.json";
        Manifest m;
        bool haveManifest=fileExists(mf)&&readManifest(mf,m);
        std::string effectiveObjPath=objPath;
        if(effectiveObjPath.empty()){
            std::string autoObj=folder+"/"+tag4+".obj";
            if(fileExists(autoObj)) effectiveObjPath=autoObj;
        }

        if(meshEntry && haveManifest){
            printf("\nManifest: %dx%d tiles, height %d..%d\n",
                   m.meshWidth,m.meshHeight,m.heightMin,m.heightMax);
            printf("Applying mesh edits...\n");
            if(!applyBmpEdits(meshEntry->data,m,folder,effectiveObjPath,objHeightScale,objExplicit))
                printf("  (no applicable OBJ/BMP mesh edits found)\n");
            put32be(meshEntry->plugHdr+52,(uint32_t)meshEntry->data.size());
        }

        if(terrainEntry && haveManifest){
            printf("Applying terrain BMP edits...\n");
            if(applyTerrainEdits(terrainEntry->data,folder,m.meshWidth,m.meshHeight))
                put32be(terrainEntry->plugHdr+52,(uint32_t)terrainEntry->data.size());
            else
                printf("  (no terrain.bmp found or size mismatch)\n");
        }

        bool anyStli=false;
        if(nameStliEntry){
            if(applyStliText(nameStliEntry->data,folder+"/strings/name.txt")){
                put32be(nameStliEntry->plugHdr+52,(uint32_t)nameStliEntry->data.size());
                anyStli=true;
            }
        }
        if(captionsEntry){
            if(applyStliText(captionsEntry->data,folder+"/strings/captions.txt")){
                put32be(captionsEntry->plugHdr+52,(uint32_t)captionsEntry->data.size());
                anyStli=true;
            }
        }
        if(anyStli) printf("Applied STLI text edits.\n");
    }

    printf("\nAssembling %zu tag(s) into: %s\n\n",tags.size(),outPath.c_str());
    std::string desc="plugin:"+tag4;
    if(!buildGor(tags,outPath.c_str(),desc.c_str())) return 1;
    printf("\nDone.\n");
    return 0;
}
