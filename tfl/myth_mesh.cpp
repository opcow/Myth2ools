// myth_mesh.cpp
// Extracts the terrain mesh from a Myth: The Fallen Lords mesh tag
// and writes a Wavefront OBJ file suitable for import into Blender.
//
// Usage:
//   myth_mesh <tags.gor> <meshtag> [output.obj] [heightscale]
//
// The MTL references <meshtag>_lit.bmp as the texture (produced by myth_extract).
// Run myth_extract first, then myth_mesh.
//
// Examples:
//   myth_mesh tags.gor sega
//   myth_mesh tags.gor sega seven_gates.obj
//   myth_mesh tags.gor sega seven_gates.obj 0.002
//
// Mesh structure (confirmed by hex analysis):
//   meshHeader (1024 bytes):
//     [+ 0]  4   .256TagID
//     [+ 4]  4   waterType1
//     [+ 8]  2   meshWidth    (tile columns, int16 BE)
//     [+10]  2   meshHeight   (tile rows,    int16 BE)
//     [+16]  4   meshLength   (gridData byte count)
//     [+24]  4   meshOffset   (offset from tag start to gridData)
//
//   gridData: meshW*16 * meshH*16 entries, each 8 bytes:
//     [+0]  2   height      (signed int16, big-endian)
//     [+2]  1   slope_tri0  precomputed slope magnitude, triangle 0 (upper-left)
//     [+3]  1   slope_tri1  precomputed slope magnitude, triangle 1 (lower-right)
//     [+4]  1   water       (0=land, non-zero=water type)
//     [+5]  1   passability flags
//     [+6]  2   unknown (often 0xFFFF)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

static uint32_t swap32(uint32_t n) {
    return ((n&0xFF000000u)>>24)|((n&0x00FF0000u)>>8)
          |((n&0x0000FF00u)<<8)|((n&0x000000FFu)<<24);
}
static int32_t  swap32s(int32_t n)  { return (int32_t)swap32((uint32_t)n); }
static uint16_t swap16(uint16_t n)  { return (uint16_t)((n>>8)|(n<<8)); }
static uint32_t readBE32 (const uint8_t* b, size_t o) { uint32_t v; memcpy(&v,b+o,4); return swap32(v); }
static int32_t  readBE32s(const uint8_t* b, size_t o) { return (int32_t)readBE32(b,o); }
static int16_t  readBE16s(const uint8_t* b, size_t o) { uint16_t v; memcpy(&v,b+o,2); return (int16_t)swap16(v); }

static std::string stemOf(const std::string& path) {
    size_t s=path.find_last_of("/\\");
    std::string n=s==std::string::npos?path:path.substr(s+1);
    size_t d=n.rfind('.'); return d==std::string::npos?n:n.substr(0,d);
}

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
// Find a tag by type+name in a .gor index (last 2MB)
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
// Grid point and mesh info
// ---------------------------------------------------------------------------
// Each grid cell is split diagonally into two triangles:
//   Triangle A (upper-left):  [row][col], [row][col+1], [row+1][col]
//   Triangle B (lower-right): [row][col+1], [row+1][col+1], [row+1][col]
// The slope and passability bytes at each point describe the cell to the
// lower-right of that point: tri0/lo-nibble = tri A, tri1/hi-nibble = tri B.
struct GridPoint {
    int16_t  height;
    // Precomputed slope magnitude per triangle (Pearson r~0.8 with computed
    // height-gradient magnitude).  Direction is not stored; derived at runtime
    // from neighbour heights.  Scale: approximately mag/11, quantised 0-255.
    // Visual comparison with passability and shadow maps on 58cm is consistent
    // with this interpretation, but not yet conclusive.
    uint8_t  slope_tri0;   // slope magnitude, triangle A (upper-left)
    uint8_t  slope_tri1;   // slope magnitude, triangle B (lower-right)
    uint8_t  water;        // 0=dry, non-zero=water type
    uint8_t  passability;  // lo nibble = tri A terrain type, hi nibble = tri B
    uint8_t  unk1, unk2;
};
struct MeshInfo {
    char   dot256Name[5];
    int    meshW, meshH, gridW, gridH;
    std::vector<GridPoint> grid;
};

static bool readMesh(FILE* tagsGor, long tagOffset, MeshInfo& out)
{
    uint8_t hdr[1024];
    fseek(tagsGor,tagOffset,SEEK_SET);
    if(fread(hdr,1,1024,tagsGor)!=1024){
        fprintf(stderr,"  Error: failed to read mesh header\n"); return false;
    }
    memcpy(out.dot256Name,hdr,4); out.dot256Name[4]=0;
    out.meshW=(int)readBE16s(hdr,8);
    out.meshH=(int)readBE16s(hdr,10);
    int32_t meshLength=readBE32s(hdr,16);
    int32_t meshOffset=readBE32s(hdr,24);
    out.gridW=out.meshW*16;
    out.gridH=out.meshH*16;
    int totalPts=out.gridW*out.gridH;
    int expectedBytes=totalPts*8;
    printf("  .256 tag:      '%.4s'\n",out.dot256Name);
    printf("  Tiles:         %d x %d\n",out.meshW,out.meshH);
    printf("  Grid:          %d x %d = %d points\n",out.gridW,out.gridH,totalPts);
    printf("  Grid bytes:    %d reported, %d expected  %s\n",
           meshLength,expectedBytes,meshLength==expectedBytes?"OK":"MISMATCH");
    if(out.meshW<1||out.meshW>64||out.meshH<1||out.meshH>64){
        fprintf(stderr,"  Error: implausible tile dimensions\n"); return false;
    }
    fseek(tagsGor,tagOffset+(long)meshOffset,SEEK_SET);
    out.grid.resize(totalPts);
    for(int i=0;i<totalPts;i++){
        uint8_t e[8];
        if(fread(e,1,8,tagsGor)!=8){
            fprintf(stderr,"  Error: short read at grid point %d\n",i); return false;
        }
        out.grid[i].height     =readBE16s(e,0);
        out.grid[i].slope_tri0 = e[2];
        out.grid[i].slope_tri1 = e[3];
        out.grid[i].water      =e[4];
        out.grid[i].passability=e[5];
        out.grid[i].unk1=e[6]; out.grid[i].unk2=e[7];
    }
    int16_t lo=out.grid[0].height,hi=out.grid[0].height;
    for(auto& p:out.grid){lo=std::min(lo,p.height);hi=std::max(hi,p.height);}
    printf("  Height range:  %d .. %d\n\n",lo,hi);
    return true;
}

// ---------------------------------------------------------------------------
// Write OBJ + MTL
//
// Coordinate system (Blender Y-up):
//   X = col - gridW/2    (centred on X axis)
//   Y = height * hs
//   Z = -(row - gridH/2) (centred on Z axis, negated so +Z = north)
//
// UV:   u = col/(gridW-1),   v = 1 - row/(gridH-1)
//
// Faces: each grid square -> 2 CCW triangles (A,B,C) and (A,C,D)
//   A=(col,row)  B=(col+1,row)  C=(col+1,row+1)  D=(col,row+1)
//
// Normals: per-vertex averaged from adjacent face normals (smooth shading)
// ---------------------------------------------------------------------------
static bool writeOBJ(const char* objPath, const char* mtlPath,
                     const char* textureBmp,
                     const MeshInfo& mesh, float hs)
{
    FILE* obj=fopen(objPath,"w");
    if(!obj){fprintf(stderr,"Cannot create: %s\n",objPath); return false;}
    FILE* mtl=fopen(mtlPath,"w");
    if(!mtl){fclose(obj);fprintf(stderr,"Cannot create: %s\n",mtlPath); return false;}

    int gw=mesh.gridW, gh=mesh.gridH;
    int totalVerts=gw*gh;
    int totalFaces=(gw-1)*(gh-1)*2;

    // Half-extents for centering
    float halfW = (float)(gw - 1) * 0.5f;
    float halfH = (float)(gh - 1) * 0.5f;

    int16_t lo=mesh.grid[0].height,hi=mesh.grid[0].height;
    for(auto& p:mesh.grid){lo=std::min(lo,p.height);hi=std::max(hi,p.height);}

    printf("  Height scale:  %.6f\n",hs);
    printf("  OBJ Y range:   %.3f .. %.3f\n",lo*hs,hi*hs);
    printf("  X range:       %.1f .. %.1f  (centred)\n", -halfW, halfW);
    printf("  Z range:       %.1f .. %.1f  (centred)\n", -halfH, halfH);
    printf("  Vertices:      %d\n",totalVerts);
    printf("  Triangles:     %d\n",totalFaces);

    // Strip directory from filenames for portability
    auto basename=[](const std::string& p){
        size_t s=p.find_last_of("/\\");
        return s==std::string::npos?p:p.substr(s+1);
    };
    std::string texFile=basename(textureBmp);
    std::string mtlFile=basename(mtlPath);

    // MTL
    fprintf(mtl,"# Myth: The Fallen Lords terrain material\n");
    fprintf(mtl,"newmtl terrain\n");
    fprintf(mtl,"Ka 1.0 1.0 1.0\n");
    fprintf(mtl,"Kd 1.0 1.0 1.0\n");
    fprintf(mtl,"Ks 0.0 0.0 0.0\n");
    fprintf(mtl,"illum 1\n");
    fprintf(mtl,"map_Kd %s\n",texFile.c_str());
    fclose(mtl);

    // OBJ header
    fprintf(obj,"# Myth: The Fallen Lords terrain -- %.4s\n",mesh.dot256Name);
    fprintf(obj,"# Tiles: %dx%d  Grid: %dx%d  Verts: %d  Tris: %d\n",
            mesh.meshW,mesh.meshH,gw,gh,totalVerts,totalFaces);
    fprintf(obj,"# Height scale: %.6f  Raw range: %d..%d\n",hs,lo,hi);
    fprintf(obj,"# Origin centred: X in [%.1f, %.1f], Z in [%.1f, %.1f]\n\n",
            -halfW,halfW,-halfH,halfH);
    fprintf(obj,"mtllib %s\n\n",mtlFile.c_str());

    // Vertices: v X Y Z  (origin centred on X and Z)
    fprintf(obj,"# Vertices (%d)\n",totalVerts);
    for(int row=0;row<gh;row++){
        for(int col=0;col<gw;col++){
            const GridPoint& p=mesh.grid[row*gw+col];
            float x = (float)col - halfW;
            float y = (float)p.height * hs;
            float z = -((float)row - halfH);
            fprintf(obj,"v %.4f %.4f %.4f\n", x, y, z);
        }
    }
    fprintf(obj,"\n");

    // UV coordinates (unchanged -- UVs map to texture, not world space)
    fprintf(obj,"# UV coordinates (%d)\n",totalVerts);
    float uS=gw>1?1.0f/(float)(gw-1):1.0f;
    float vS=gh>1?1.0f/(float)(gh-1):1.0f;
    for(int row=0;row<gh;row++){
        for(int col=0;col<gw;col++){
            fprintf(obj,"vt %.6f %.6f\n",
                    (float)col*uS, 1.0f-(float)row*vS);
        }
    }
    fprintf(obj,"\n");

    // Smooth normals (computed from centred positions -- same result,
    // normals are direction vectors so the offset doesn't matter)
    fprintf(obj,"# Normals (%d)\n",totalVerts);
    std::vector<float> nx(totalVerts,0),ny(totalVerts,0),nz(totalVerts,0);
    for(int row=0;row<gh-1;row++){
        for(int col=0;col<gw-1;col++){
            int iA=row*gw+col, iB=row*gw+(col+1),
                iC=(row+1)*gw+(col+1), iD=(row+1)*gw+col;
            auto vx=[&](int i){return (float)(i%gw) - halfW;};
            auto vy=[&](int i){return (float)mesh.grid[i].height*hs;};
            auto vz=[&](int i){return -((float)(i/gw) - halfH);};
            int tris[2][3]={{iA,iB,iC},{iA,iC,iD}};
            for(auto& t:tris){
                float ax=vx(t[1])-vx(t[0]),ay=vy(t[1])-vy(t[0]),az=vz(t[1])-vz(t[0]);
                float bx=vx(t[2])-vx(t[0]),by=vy(t[2])-vy(t[0]),bz=vz(t[2])-vz(t[0]);
                float cx=ay*bz-az*by,cy=az*bx-ax*bz,cz=ax*by-ay*bx;
                for(int j=0;j<3;j++){nx[t[j]]+=cx;ny[t[j]]+=cy;nz[t[j]]+=cz;}
            }
        }
    }
    for(int i=0;i<totalVerts;i++){
        float len=sqrtf(nx[i]*nx[i]+ny[i]*ny[i]+nz[i]*nz[i]);
        if(len<1e-6f) len=1.0f;
        fprintf(obj,"vn %.4f %.4f %.4f\n",nx[i]/len,ny[i]/len,nz[i]/len);
    }
    fprintf(obj,"\n");

    // Faces
    fprintf(obj,"usemtl terrain\ns 1\n\n");
    fprintf(obj,"# Faces (%d triangles)\n",totalFaces);
    for(int row=0;row<gh-1;row++){
        for(int col=0;col<gw-1;col++){
            int A=row*gw+col+1,          B=row*gw+(col+1)+1,
                C=(row+1)*gw+(col+1)+1,  D=(row+1)*gw+col+1;
            fprintf(obj,"f %d/%d/%d %d/%d/%d %d/%d/%d\n",A,A,A,B,B,B,C,C,C);
            fprintf(obj,"f %d/%d/%d %d/%d/%d %d/%d/%d\n",A,A,A,C,C,C,D,D,D);
        }
    }

    fclose(obj);
    return true;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
static void usage(const char* p){
    fprintf(stderr,
        "Myth: The Fallen Lords -- Terrain Mesh Extractor\n\n"
        "Usage:\n"
        "  %s <tags.gor> <meshtag> [output.obj] [heightscale]\n\n"
        "  tags.gor     Path to tags.gor\n"
        "  meshtag      4-char mesh tag name (e.g. sega, balo, land)\n"
        "  output.obj   Output filename (default: <meshtag>.obj)\n"
        "  heightscale  Vertical scale factor (default: 0.002)\n\n"
        "The MTL references <meshtag>_lit.bmp (the multiply-composited\n"
        "texture produced by myth_extract). Run myth_extract first.\n\n"
        "Blender import: File > Import > Wavefront (.obj)\n"
        "  Forward Axis: -Z   Up Axis: Y\n\n"
        "Examples:\n"
        "  %s tags.gor sega\n"
        "  %s tags.gor sega sega.obj 0.002\n",p,p,p);
}

int main(int argc, char* argv[]){
    if(argc<3){usage(argv[0]); return 1;}

    const char* tagsGorPath=argv[1];
    const char* meshTagName=argv[2];
    if(strlen(meshTagName)!=4){
        fprintf(stderr,"Error: tag must be exactly 4 chars\n"); return 1;
    }

    std::string tag4    = std::string(meshTagName, 4);
    std::string objPath = argc>=4 ? argv[3] : tag4 + "/" + tag4 + ".obj";
    float heightScale   = argc>=5 ? (float)atof(argv[4]) : 0.002f;

    std::string mtlPath=objPath;
    {size_t d=mtlPath.rfind('.'); if(d!=std::string::npos) mtlPath=mtlPath.substr(0,d); mtlPath+=".mtl";}

    // texBmp: look for myth_extract output in the same subfolder
    std::string texBmp = dirOf(objPath) + tag4 + "_lit.bmp";

    printf("Myth Terrain Mesh Extractor\n===========================\n");
    printf("tags.gor:     %s\n",tagsGorPath);
    printf("Mesh tag:     %.4s\n",meshTagName);
    printf("OBJ:          %s\n",objPath.c_str());
    printf("MTL:          %s\n",mtlPath.c_str());
    printf("Texture ref:  %s\n",texBmp.c_str());
    printf("Height scale: %.6f\n\n",heightScale);

    // Create output directory if needed
    makeDir(dirOf(objPath));

    FILE* tagsGor=fopen(tagsGorPath,"rb");
    if(!tagsGor){fprintf(stderr,"Cannot open: %s\n",tagsGorPath); return 1;}
    fseek(tagsGor,0,SEEK_END); long tagsSize=ftell(tagsGor);

    long meshOffset=0,meshLength=0;
    printf("Step 1: Finding mesh tag '%.4s'...\n",meshTagName);
    if(!findTagInGor(tagsGor,tagsSize,"mesh",meshTagName,meshOffset,meshLength)){
        fprintf(stderr,"  Error: not found\n"); fclose(tagsGor); return 1;
    }
    printf("  Found at offset %ld\n\n",meshOffset);

    MeshInfo mesh;
    printf("Step 2: Reading grid data...\n");
    if(!readMesh(tagsGor,meshOffset,mesh)){fclose(tagsGor); return 1;}
    fclose(tagsGor);

    printf("Step 3: Writing OBJ...\n");
    if(!writeOBJ(objPath.c_str(),mtlPath.c_str(),texBmp.c_str(),mesh,heightScale))
        return 1;

    printf("\nSuccess!\n");
    printf("  %s\n",objPath.c_str());
    printf("  %s\n\n",mtlPath.c_str());
    printf("Blender: File > Import > Wavefront (.obj)\n");
    printf("  Forward Axis = -Z,  Up Axis = Y\n");
    printf("  Make sure %s is in the same folder as the OBJ.\n\n",texBmp.c_str());
    printf("Tip: run myth_extract first to generate the _lit.bmp texture.\n");
    return 0;
}
