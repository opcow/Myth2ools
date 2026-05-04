// myth_mesh_import.cpp
// Reads a Wavefront OBJ (exported by myth_mesh) and patches the height
// and slope data back into the raw mesh tag binary produced by myth_extract.
//
// Usage:
//   myth_mesh_import <tag_folder> <input.obj> [heightscale]
//
//   tag_folder   <tag>/ directory produced by myth_extract
//                  Must contain raw/mesh_tag.bin and manifest.json.
//   input.obj    Modified OBJ file (from myth_mesh, edited in Blender etc.)
//   heightscale  Vertical scale that was used when exporting (default: 0.002)
//                Must match the value used with myth_mesh.
//
// Reads raw/mesh_tag.bin, patches height and slope bytes from the OBJ,
// and writes raw/mesh_tag.bin back in-place.  Water, passability, and
// unknown bytes are preserved from the original.
//
// After running this tool, use myth_assemble to rebuild the .gor plugin:
//   myth_assemble <tag_folder> output.gor
//
// Slope approximation:
//   Myth precomputes a slope-magnitude byte for each of the two triangles
//   in every grid cell.  The exact engine formula is not publicly documented.
//   This tool uses:
//
//     slope_tri0[r][c] = clamp(sqrt(dR^2 + dD^2) / 11, 0, 255)
//     slope_tri1[r][c] = clamp(sqrt(dRB^2 + dDB^2) / 11, 0, 255)
//
//   where all heights are raw int16 Myth units and:
//     dR  = h[r][c+1] - h[r][c]         right edge, top-left triangle
//     dD  = h[r+1][c] - h[r][c]         down  edge, top-left triangle
//     dRB = h[r+1][c+1] - h[r+1][c]     right edge, bottom-right triangle
//     dDB = h[r+1][c+1] - h[r][c+1]     down  edge, bottom-right triangle
//
//   Empirically ~12% of Bungie-original values match within ±1 of this
//   formula.  The Bungie originals were generated from an earlier,
//   pre-quantisation stage of the asset pipeline and cannot be perfectly
//   reconstructed from the final height data alone.
//
// Grid/triangle topology (from GridPoint documentation in myth_mesh.cpp):
//   Each grid square is split with the anti-diagonal (top-right -> bottom-left):
//     Triangle 0 (upper-left):  [r][c], [r][c+1], [r+1][c]
//     Triangle 1 (lower-right): [r][c+1], [r+1][c+1], [r+1][c]
//   Note: the OBJ face topology uses the main diagonal for smooth shading
//   in Blender, but the slope bytes follow the anti-diagonal convention above.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

// ---------------------------------------------------------------------------
// Endian helpers
// ---------------------------------------------------------------------------
static uint16_t swap16(uint16_t n){ return (uint16_t)((n>>8)|(n<<8)); }
static int16_t readBE16s(const uint8_t* b, size_t o){
    uint16_t v; memcpy(&v,b+o,2); return (int16_t)swap16(v);
}
static void writeBE16s(uint8_t* b, size_t o, int16_t v){
    uint16_t u=(uint16_t)v;
    b[o]=(uint8_t)(u>>8); b[o+1]=(uint8_t)(u&0xFF);
}

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------
static std::vector<uint8_t> readFile(const std::string& path){
    FILE* f=fopen(path.c_str(),"rb");
    if(!f) return {};
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    if(sz<=0){ fclose(f); return {}; }
    std::vector<uint8_t> buf((size_t)sz);
    if(fread(buf.data(),1,(size_t)sz,f)!=(size_t)sz){ fclose(f); return {}; }
    fclose(f); return buf;
}
static bool writeFile(const std::string& path, const std::vector<uint8_t>& data){
    FILE* f=fopen(path.c_str(),"wb");
    if(!f){ fprintf(stderr,"Cannot write: %s\n",path.c_str()); return false; }
    bool ok=(fwrite(data.data(),1,data.size(),f)==data.size());
    fclose(f);
    return ok;
}

// ---------------------------------------------------------------------------
// Minimal JSON int reader (same approach as myth_assemble)
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Manifest (subset we need)
// ---------------------------------------------------------------------------
struct Manifest {
    int meshWidth=0, meshHeight=0;
    int meshOffset=1024;  // offset from start of tag binary to grid data
};
static bool readManifest(const std::string& path, Manifest& m){
    auto raw=readFile(path);
    if(raw.empty()){ fprintf(stderr,"Cannot read: %s\n",path.c_str()); return false; }
    std::string j(raw.begin(),raw.end());
    m.meshWidth   = jsonInt(j,"width",0);
    m.meshHeight  = jsonInt(j,"height",0);
    m.meshOffset  = jsonInt(j,"mesh_header_size",1024);
    return m.meshWidth>0 && m.meshHeight>0;
}

// ---------------------------------------------------------------------------
// OBJ vertex parser
//
// myth_mesh writes vertices as:
//   v X Y Z
// where:
//   X = col - halfW       (halfW = (gridW-1)/2.0)
//   Y = height * hs
//   Z = -(row - halfH)    (halfH = (gridH-1)/2.0)
//
// So:
//   col = round(X + halfW)
//   row = round(-Z + halfH)
//   height_raw = round(Y / hs)
// ---------------------------------------------------------------------------
static bool parseOBJ(const std::string& path, float hs,
                     int gridW, int gridH,
                     std::vector<int16_t>& heightsOut)
{
    FILE* f=fopen(path.c_str(),"r");
    if(!f){ fprintf(stderr,"Cannot open OBJ: %s\n",path.c_str()); return false; }

    float halfW=(float)(gridW-1)*0.5f;
    float halfH=(float)(gridH-1)*0.5f;
    int total=gridW*gridH;
    heightsOut.assign(total,0);
    std::vector<bool> filled(total,false);

    int vCount=0, mapped=0, clampedH=0;
    char line[256];
    while(fgets(line,sizeof(line),f)){
        if(line[0]!='v'||line[1]!=' ') continue;   // only 'v X Y Z' lines
        float x,y,z;
        if(sscanf(line+2,"%f %f %f",&x,&y,&z)!=3) continue;
        vCount++;

        int col=(int)roundf(x+halfW);
        int row=(int)roundf(-z+halfH);
        if(col<0||col>=gridW||row<0||row>=gridH){
            fprintf(stderr,"  Warning: vertex out of range: v %g %g %g -> (%d,%d)\n",
                    x,y,z,row,col);
            continue;
        }
        float hf=y/hs;
        int16_t h;
        if(hf<-32768.0f){ h=-32768; clampedH++; }
        else if(hf>32767.0f){ h=32767; clampedH++; }
        else h=(int16_t)roundf(hf);

        int idx=row*gridW+col;
        heightsOut[idx]=h;
        filled[idx]=true;
        mapped++;
    }
    fclose(f);

    // Check coverage
    int missing=0;
    for(int i=0;i<total;i++) if(!filled[i]) missing++;

    printf("  OBJ vertices: %d read, %d mapped to grid (%d missing, %d height-clamped)\n",
           vCount,mapped,missing,clampedH);
    if(missing>0){
        fprintf(stderr,"  Warning: %d grid points not covered by OBJ vertices.\n",missing);
        fprintf(stderr,"  Heights for those points default to 0.\n");
    }
    return mapped>0;
}

// ---------------------------------------------------------------------------
// Slope computation
//
// slope_tri0[r][c]: upper-left triangle of cell (r,c):
//   vertices: (r,c), (r,c+1), (r+1,c)
//   face normal has horizontal magnitude = sqrt(dR^2 + dD^2)
//   where dR = h[r][c+1]-h[r][c],  dD = h[r+1][c]-h[r][c]
//
// slope_tri1[r][c]: lower-right triangle of cell (r,c):
//   vertices: (r,c+1), (r+1,c+1), (r+1,c)
//   face normal has horizontal magnitude = sqrt(dRB^2 + dDB^2)
//   where dRB = h[r+1][c+1]-h[r+1][c],  dDB = h[r+1][c+1]-h[r][c+1]
// ---------------------------------------------------------------------------
static const float SLOPE_FACTOR = 11.0f;

static uint8_t slopeByte(float mag){
    float v=mag/SLOPE_FACTOR;
    if(v<0.0f) v=0.0f;
    if(v>255.0f) v=255.0f;
    return (uint8_t)roundf(v);
}

static void computeSlopes(const std::vector<int16_t>& h, int gridW, int gridH,
                           std::vector<uint8_t>& s0, std::vector<uint8_t>& s1)
{
    int total=gridW*gridH;
    s0.assign(total,0);
    s1.assign(total,0);
    auto H=[&](int r,int c)->float{ return (float)h[r*gridW+c]; };
    for(int r=0;r<gridH-1;r++){
        for(int c=0;c<gridW-1;c++){
            float dR  = H(r,c+1)  - H(r,c);
            float dD  = H(r+1,c)  - H(r,c);
            float dRB = H(r+1,c+1)- H(r+1,c);
            float dDB = H(r+1,c+1)- H(r,c+1);
            s0[r*gridW+c] = slopeByte(sqrtf(dR*dR + dD*dD));
            s1[r*gridW+c] = slopeByte(sqrtf(dRB*dRB + dDB*dDB));
        }
    }
}

// ---------------------------------------------------------------------------
// Patch the raw mesh binary in-place
// ---------------------------------------------------------------------------
static int patchMesh(std::vector<uint8_t>& meshBin, int meshOffset, int gridW, int gridH,
                      const std::vector<int16_t>& newH,
                      const std::vector<uint8_t>& s0,
                      const std::vector<uint8_t>& s1)
{
    int total=gridW*gridH;
    int hChanged=0, sChanged=0;
    for(int i=0;i<total;i++){
        size_t off=(size_t)meshOffset+(size_t)i*8;
        if(off+7>=meshBin.size()) break;

        // Height (bytes 0-1, big-endian int16)
        int16_t oldH=readBE16s(meshBin.data(),off);
        if(oldH!=newH[i]){ writeBE16s(meshBin.data(),off,newH[i]); hChanged++; }

        // Slopes (bytes 2-3)
        if(meshBin[off+2]!=s0[i]){ meshBin[off+2]=s0[i]; sChanged++; }
        if(meshBin[off+3]!=s1[i]){ meshBin[off+3]=s1[i]; sChanged++; }
        // Bytes 4-7 (water, passability, unk1, unk2) are preserved
    }
    printf("  Height bytes changed:   %d / %d\n",hChanged,total);
    printf("  Slope bytes changed:    %d / %d (approx; may differ from Bungie originals)\n",
           sChanged,total*2);
    return hChanged;
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------
static void usage(const char* prog){
    fprintf(stderr,
        "Myth: The Fallen Lords -- Terrain Mesh Importer\n\n"
        "Usage:\n"
        "  %s <tag_folder> <input.obj> [heightscale]\n\n"
        "  tag_folder   <tag>/ directory from myth_extract\n"
        "               (must contain raw/mesh_tag.bin and manifest.json)\n"
        "  input.obj    Modified OBJ from myth_mesh (edited in Blender etc.)\n"
        "  heightscale  Scale used when exporting (default: 0.002)\n\n"
        "Patches raw/mesh_tag.bin in-place with new heights and recomputed slopes.\n"
        "Water, passability, and unknown bytes are preserved.\n\n"
        "After importing, run myth_assemble to rebuild the .gor:\n"
        "  myth_assemble <tag_folder> output.gor\n",
        prog);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]){
    if(argc<3){ usage(argv[0]); return 1; }

    std::string folder=argv[1];
    while(!folder.empty()&&(folder.back()=='/'||folder.back()=='\\'))
        folder.pop_back();

    std::string objPath=argv[2];
    float hs=(argc>=4)?(float)atof(argv[3]):0.002f;
    if(hs<=0.0f){ fprintf(stderr,"Error: heightscale must be positive\n"); return 1; }

    std::string manifestPath = folder+"/manifest.json";
    std::string binPath      = folder+"/raw/mesh_tag.bin";

    printf("Myth Terrain Mesh Importer\n==========================\n");
    printf("Tag folder:   %s\n",folder.c_str());
    printf("OBJ:          %s\n",objPath.c_str());
    printf("Height scale: %.6f\n\n",hs);

    // Step 1: Read manifest
    printf("Step 1: Reading manifest...\n");
    Manifest mf;
    if(!readManifest(manifestPath,mf)) return 1;
    int gridW=mf.meshWidth*16, gridH=mf.meshHeight*16;
    printf("  Mesh tiles:  %d x %d\n",mf.meshWidth,mf.meshHeight);
    printf("  Grid points: %d x %d = %d\n",gridW,gridH,gridW*gridH);
    printf("  Grid offset: %d bytes from tag start\n\n",mf.meshOffset);

    // Step 2: Read existing binary
    printf("Step 2: Reading %s...\n",binPath.c_str());
    std::vector<uint8_t> meshBin=readFile(binPath);
    if(meshBin.empty()){ fprintf(stderr,"Cannot read: %s\n",binPath.c_str()); return 1; }
    int expectedGridBytes=gridW*gridH*8;
    long actualGridBytes=(long)meshBin.size()-(long)mf.meshOffset;
    printf("  File size:   %zu bytes\n",meshBin.size());
    printf("  Grid bytes:  %ld actual, %d expected from grid dimensions  %s\n\n",
           actualGridBytes,expectedGridBytes,
           actualGridBytes>=expectedGridBytes?"OK":"WARNING: smaller than expected");

    // Step 3: Parse OBJ
    printf("Step 3: Parsing OBJ vertices...\n");
    std::vector<int16_t> newHeights;
    if(!parseOBJ(objPath,hs,gridW,gridH,newHeights)) return 1;

    // Height range summary
    int16_t hmin=newHeights[0], hmax=newHeights[0];
    for(auto v:newHeights){ hmin=std::min(hmin,v); hmax=std::max(hmax,v); }
    printf("  Height range: %d .. %d (raw units)\n\n",hmin,hmax);

    // Step 4: Compute slopes
    printf("Step 4: Computing slope bytes (approx. sqrt(dR^2+dD^2)/%.0f)...\n",SLOPE_FACTOR);
    std::vector<uint8_t> s0,s1;
    computeSlopes(newHeights,gridW,gridH,s0,s1);
    printf("\n");

    // Step 5: Patch binary
    printf("Step 5: Patching binary...\n");
    patchMesh(meshBin,mf.meshOffset,gridW,gridH,newHeights,s0,s1);
    printf("\n");

    // Step 6: Write back
    printf("Step 6: Writing %s...\n",binPath.c_str());
    if(!writeFile(binPath,meshBin)){
        fprintf(stderr,"Write failed.\n"); return 1;
    }
    printf("  Written: %zu bytes\n\n",meshBin.size());

    printf("Done.\n");
    printf("  Next step: myth_assemble %s output.gor\n\n",folder.c_str());
    printf("Note: slope bytes are approximated (~12%% match vs Bungie originals).\n");
    printf("Visual slope artefacts are expected on steep terrain.\n");
    return 0;
}
