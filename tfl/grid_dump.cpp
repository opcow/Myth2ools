// grid_dump.cpp
// Dumps the raw grid data from a Myth mesh tag so we can verify the structure.
// Usage: grid_dump <tags.gor> <meshtag> [numpoints]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <vector>
#include <string>

static uint32_t swap32(uint32_t n) {
    return ((n>>24)&0xFF)|((n>>8)&0xFF00)|((n<<8)&0xFF0000)|((n<<24)&0xFF000000);
}
static int32_t swap32s(int32_t n) { return (int32_t)swap32((uint32_t)n); }
static uint16_t swap16(uint16_t n) { return (uint16_t)((n>>8)|(n<<8)); }
static int16_t readBE16s(const uint8_t* b, size_t o) {
    uint16_t v; memcpy(&v,b+o,2); return (int16_t)swap16(v);
}
static uint32_t readBE32(const uint8_t* b, size_t o) {
    uint32_t v; memcpy(&v,b+o,4); return swap32(v);
}
static int32_t readBE32s(const uint8_t* b, size_t o) { return (int32_t)readBE32(b,o); }

static bool findTag(FILE* f, long fsz, const char* type4, const char* name4,
                    long& off, long& len) {
    long start = fsz - 2*1024*1024; if(start<0) start=0;
    fseek(f,start,SEEK_SET);
    std::vector<uint8_t> buf(fsz-start);
    size_t got=fread(buf.data(),1,buf.size(),f);
    for(size_t i=0;i+64<=got;i++){
        if(memcmp(buf.data()+i,type4,4)!=0) continue;
        if(memcmp(buf.data()+i+4,name4,4)!=0) continue;
        if(i<40) continue;
        size_t e=i-40;
        uint32_t o=readBE32(buf.data(),e+56), l=readBE32(buf.data(),e+60);
        if((long)o<128||(long)o>=fsz) continue;
        if(!l||(long)o+(long)l>fsz) continue;
        off=(long)o; len=(long)l; return true;
    }
    return false;
}

static void hexline(const uint8_t* b, int n, long base) {
    printf("  %08lX: ", base);
    for(int i=0;i<16;i++) { if(i<n) printf("%02X ",b[i]); else printf("   "); }
    printf(" |");
    for(int i=0;i<n;i++) printf("%c",(b[i]>=32&&b[i]<127)?b[i]:'.');
    printf("|\n");
}

int main(int argc, char* argv[]) {
    if(argc<3){ printf("Usage: grid_dump <tags.gor> <meshtag> [numpoints]\n"); return 1; }
    const char* gorPath = argv[1];
    const char* tagName = argv[2];
    int numPoints = argc>=4 ? atoi(argv[3]) : 64;

    FILE* f = fopen(gorPath,"rb");
    if(!f){ fprintf(stderr,"Cannot open %s\n",gorPath); return 1; }
    fseek(f,0,SEEK_END); long fsz=ftell(f);

    long tagOff=0,tagLen=0;
    if(!findTag(f,fsz,"mesh",tagName,tagOff,tagLen)){
        fprintf(stderr,"Tag '%.4s' not found\n",tagName); fclose(f); return 1;
    }
    printf("Mesh tag '%.4s' at file offset %ld\n\n",tagName,tagOff);

    // Read 1024-byte header
    uint8_t hdr[1024];
    fseek(f,tagOff,SEEK_SET);
    fread(hdr,1,1024,f);

    char dot256[5]={};  memcpy(dot256,hdr,4);
    int meshW    = readBE16s(hdr, 8);
    int meshH    = readBE16s(hdr,10);
    int32_t meshLength = readBE32s(hdr,16);
    int32_t meshOffset = readBE32s(hdr,24);

    // Grid is meshW*16 wide, meshH*16 tall (no +1 -- confirmed by meshLength)
    int gridW = meshW * 16;
    int gridH = meshH * 16;
    int totalPts = gridW * gridH;
    int bytesAt8 = totalPts * 8;

    printf("  .256 tag:    '%.4s'\n", dot256);
    printf("  meshW:       %d tiles\n", meshW);
    printf("  meshH:       %d tiles\n", meshH);
    printf("  gridW:       %d  (meshW*16)\n", gridW);
    printf("  gridH:       %d  (meshH*16)\n", gridH);
    printf("  grid points: %d  (gridW*gridH)\n", totalPts);
    printf("  meshOffset:  %d  (file offset %ld)\n", meshOffset, tagOff+(long)meshOffset);
    printf("  meshLength:  %d bytes (reported)\n", meshLength);
    printf("  at 8b/pt:    %d bytes (expected)\n", bytesAt8);
    printf("  match:       %s\n\n", (meshLength==bytesAt8)?"YES":"NO -- check entry size");

    if(meshLength != bytesAt8) {
        float bpp = (float)meshLength / totalPts;
        printf("  bytes/point: %.3f  (trying %d bytes/point)\n\n",
               bpp, (int)roundf(bpp));
    }

    // Dump mesh header key fields
    printf("=== Mesh header key fields ===\n");
    printf("  [+ 0] .256TagID:       '%.4s'\n", hdr);
    printf("  [+ 4] waterType1:      0x%08X\n", readBE32(hdr,4));
    printf("  [+ 8] meshWidth:       %d\n", readBE16s(hdr,8));
    printf("  [+10] meshHeight:      %d\n", readBE16s(hdr,10));
    printf("  [+12] unknown1:        %d\n", readBE32s(hdr,12));
    printf("  [+16] meshLength:      %d\n", readBE32s(hdr,16));
    printf("  [+20] unknown2:        %d\n", readBE32s(hdr,20));
    printf("  [+24] meshOffset:      %d\n", readBE32s(hdr,24));
    printf("  [+28] tagLength:       %d\n", readBE32s(hdr,28));
    printf("  [+32] unknown3:        %d\n", readBE32s(hdr,32));
    printf("  [+36] itemTypeCount:   %d\n", readBE32s(hdr,36));
    printf("  [+40] itemTypeOffset:  %d (from gridData start)\n", readBE32s(hdr,40));
    printf("  [+44] itemTypeLength:  %d\n", readBE32s(hdr,44));
    printf("  [+48] unknown4:        %d\n", readBE32s(hdr,48));
    printf("  [+52] itemCount:       %d\n", readBE32s(hdr,52));
    printf("  [+56] itemOffset:      %d (from gridData start)\n", readBE32s(hdr,56));
    printf("  [+60] itemLength:      %d\n\n", readBE32s(hdr,60));

    // Raw hex at grid data start
    long gridFileOff = tagOff + (long)meshOffset;
    int dumpBytes = numPoints * 8 + 32;
    printf("=== Raw bytes at gridData (file offset %ld = 0x%lX) ===\n",
           gridFileOff, gridFileOff);
    fseek(f, gridFileOff, SEEK_SET);
    std::vector<uint8_t> raw(dumpBytes);
    fread(raw.data(),1,raw.size(),f);
    for(int i=0;i<dumpBytes;i+=16)
        hexline(raw.data()+i, dumpBytes-i<16?dumpBytes-i:16, gridFileOff+i);
    printf("\n");

    // Interpret as 8-byte entries.
    // bytes [2-3]: slope_tri0 and slope_tri1 -- precomputed slope magnitude
    // for the two triangles in this grid cell (upper-left and lower-right).
    // Pearson r with computed height-gradient magnitude: ~0.8 for both bytes.
    // Direction is NOT stored; neither byte correlates with atan2(dz,dx).
    // Scale is approximately mag/11, quantised to uint8 (0=flat, 255=steepest).
    printf("=== Grid points as 8-byte entries (first %d) ===\n", numPoints);
    printf("  %-5s  %-7s  %-8s  %-8s  %-5s  %-4s  %-4s  raw\n",
           "idx","height","slo_t0","slo_t1","water","pass","u1u2");

    int16_t minH=32767, maxH=-32768;
    for(int i=0;i<numPoints && i<totalPts;i++){
        const uint8_t* e = raw.data() + i*8;
        int16_t  h   = readBE16s(e,0);
        uint8_t  st0 = e[2];   // slope_tri0: magnitude for upper-left triangle
        uint8_t  st1 = e[3];   // slope_tri1: magnitude for lower-right triangle
        if(h<minH) minH=h; if(h>maxH) maxH=h;
        printf("  %-5d  %-7d  %-8d  %-8d  0x%02X  0x%02X  %02X%02X  "
               "%02X%02X %02X%02X %02X %02X %02X%02X\n",
               i, h, st0, st1, e[4], e[5], e[6], e[7],
               e[0],e[1],e[2],e[3],e[4],e[5],e[6],e[7]);
    }
    printf("\n  Height range in sample: %d .. %d\n\n", minH, maxH);

    // Scan full grid for height range + slope field summary
    {
        int scanPts = (meshLength/8 < totalPts) ? meshLength/8 : totalPts;
        std::vector<uint8_t> full(meshLength);
        fseek(f, gridFileOff, SEEK_SET);
        fread(full.data(),1,meshLength,f);

        int16_t lo=32767, hi=-32768;
        int flatCount=0, maxSteep=0;
        for(int i=0;i<scanPts;i++){
            int16_t h   = readBE16s(full.data(), i*8);
            uint8_t st0 = full[i*8+2];
            if(h<lo)lo=h; if(h>hi)hi=h;
            if(st0==0) flatCount++;
            if(st0>maxSteep) maxSteep=st0;
        }
        int slopedCount = scanPts - flatCount;
        printf("  Full grid height range: %d .. %d  (over %d points)\n\n",
               lo, hi, scanPts);
        printf("=== Slope magnitude summary ([2]=slope_tri0, [3]=slope_tri1) ===\n");
        printf("  Flat  (tri0==0):   %d / %d  (%.1f%%)\n",
               flatCount, scanPts, 100.0*flatCount/scanPts);
        printf("  Sloped (tri0>0):   %d / %d  (%.1f%%)\n",
               slopedCount, scanPts, 100.0*slopedCount/scanPts);
        printf("  Max slope_tri0:    %d\n\n", maxSteep);

        // -----------------------------------------------------------------------
        // Cross-reference stored bytes [2-3] with height gradients computed from
        // the actual height data using central differences:
        //   dx = (h[row][col+1] - h[row][col-1]) / 2
        //   dz = (h[row+1][col] - h[row-1][col]) / 2
        //
        // We test four hypotheses:
        //   Cartesian-signed:  b2 = k*dx,       b3 = k*dz      (signed int8)
        //   Cartesian-abs:     b2 = k*|dx|,      b3 = k*|dz|   (unsigned)
        //   Polar-mag:         b2 = k*sqrt(dx^2+dz^2)          (unsigned)
        //   Polar-dir:         b3 = quantised atan2(dz,dx)
        //
        // We report Pearson r for each pairing and a sample table so you can
        // eyeball the relationship directly.
        // -----------------------------------------------------------------------

        // Parse full grid heights and bytes [2-3] into flat arrays
        std::vector<int16_t> H(scanPts);
        std::vector<uint8_t> B2(scanPts), B3(scanPts);
        for(int i=0;i<scanPts;i++){
            H[i]  = readBE16s(full.data(), i*8);
            B2[i] = full[i*8+2];
            B3[i] = full[i*8+3];
        }

        // Helper: Pearson r between two float vectors
        auto pearson = [](const std::vector<float>& x, const std::vector<float>& y) -> float {
            int n = (int)x.size();
            if(n<2) return 0.0f;
            double sx=0,sy=0,sxx=0,syy=0,sxy=0;
            for(int i=0;i<n;i++){sx+=x[i];sy+=y[i];sxx+=x[i]*x[i];syy+=y[i]*y[i];sxy+=x[i]*y[i];}
            double mx=sx/n, my=sy/n;
            double cov=sxy/n - mx*my;
            double sdx=sqrt(sxx/n - mx*mx), sdy=sqrt(syy/n - my*my);
            return (sdx<1e-9||sdy<1e-9) ? 0.0f : (float)(cov/(sdx*sdy));
        };

        // Collect interior points only (skip 1-point border where central diff is undefined)
        std::vector<float> vDx, vDz, vMag, vAng;
        std::vector<float> vB2u, vB3u, vB2s, vB3s;
        for(int row=1; row<gridH-1; row++){
            for(int col=1; col<gridW-1; col++){
                int i = row*gridW + col;
                if(i >= scanPts) continue;
                float dx = ((float)H[row*gridW+(col+1)] - (float)H[row*gridW+(col-1)]) * 0.5f;
                float dz = ((float)H[(row+1)*gridW+col]   - (float)H[(row-1)*gridW+col])   * 0.5f;
                float mag = sqrtf(dx*dx + dz*dz);
                // atan2 result in [0,2*pi), quantised to 0-255
                float ang = atan2f(dz, dx);
                if(ang < 0) ang += 2.0f*3.14159265f;
                ang = ang * (256.0f / (2.0f*3.14159265f));

                vDx.push_back(dx);   vDz.push_back(dz);
                vMag.push_back(mag); vAng.push_back(ang);
                vB2u.push_back((float)B2[i]);
                vB3u.push_back((float)B3[i]);
                vB2s.push_back((float)(int8_t)B2[i]);
                vB3s.push_back((float)(int8_t)B3[i]);
            }
        }
        int N = (int)vDx.size();

        printf("=== Height gradient correlation (interior %d points) ===\n\n", N);

        // Sample table: 20 evenly-spaced interior points
        printf("  Sample (every %d-th interior point):\n", N/20);
        printf("  %-5s  %-7s  %-7s  %-7s  %-7s  %-4s  %-4s  b2/mag  b3/mag\n",
               "i","dx","dz","mag","ang_q","b2","b3");
        int step = N/20; if(step<1) step=1;
        for(int k=0; k<N; k+=step){
            float mag = vMag[k];
            printf("  %-5d  %-7.1f  %-7.1f  %-7.1f  %-7.1f  %-4.0f  %-4.0f  %-7.3f %-7.3f\n",
                   k, vDx[k], vDz[k], mag, vAng[k],
                   vB2u[k], vB3u[k],
                   mag>0.5f ? vB2u[k]/mag : 0.0f,
                   mag>0.5f ? vB3u[k]/mag : 0.0f);
        }
        printf("\n");

        // Pearson correlations
        printf("  Pearson r -- higher |r| means stronger linear relationship:\n\n");
        printf("  Cartesian signed (b2=k*dx, b3=k*dz):\n");
        printf("    r(b2_signed, dx) = %+.4f\n", pearson(vB2s, vDx));
        printf("    r(b3_signed, dz) = %+.4f\n", pearson(vB3s, vDz));
        printf("    r(b2_signed, dz) = %+.4f  (cross-check: should be ~0)\n", pearson(vB2s, vDz));
        printf("    r(b3_signed, dx) = %+.4f  (cross-check: should be ~0)\n\n", pearson(vB3s, vDx));
        printf("  Polar magnitude (b2=k*sqrt(dx^2+dz^2)):\n");
        printf("    r(b2_unsigned, mag) = %+.4f\n", pearson(vB2u, vMag));
        printf("    r(b3_unsigned, mag) = %+.4f\n\n", pearson(vB3u, vMag));
        printf("  Polar direction (b3=quantised angle):\n");
        printf("    r(b3_unsigned, ang_quantised) = %+.4f\n\n", pearson(vB3u, vAng));

        // Scale factor estimate (median of b2/mag for non-flat points)
        {
            std::vector<float> ratios;
            for(int k=0;k<N;k++) if(vMag[k]>1.0f) ratios.push_back(vB2u[k]/vMag[k]);
            if(!ratios.empty()){
                std::sort(ratios.begin(),ratios.end());
                float median = ratios[ratios.size()/2];
                printf("  Median b2/mag (for sloped points): %.4f\n", median);
                printf("  If polar-mag hypothesis holds, this is the scale factor k.\n\n");
            }
        }
    }

    fclose(f);
    return 0;
}
