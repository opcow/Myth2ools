// shadow_check.cpp
// Reads a shadow BMP and reports pixel value distribution
// to understand why the multiply composite looks all white.
// Usage: shadow_check <shadow.bmp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 2) { printf("Usage: shadow_check <shadow.bmp>\n"); return 1; }

    FILE* f = fopen(argv[1], "rb");
    if (!f) { fprintf(stderr, "Cannot open: %s\n", argv[1]); return 1; }

    // Read BMP file header (14 bytes)
    uint8_t fh[14]; fread(fh, 1, 14, f);
    if (fh[0]!='B'||fh[1]!='M') { fprintf(stderr, "Not a BMP\n"); return 1; }

    uint32_t dataOff = fh[10]|(fh[11]<<8)|(fh[12]<<16)|(fh[13]<<24);

    // Read info header (40 bytes)
    uint8_t ih[40]; fread(ih, 1, 40, f);
    int w = (int)(ih[4]|(ih[5]<<8)|(ih[6]<<16)|(ih[7]<<24));
    int h = (int)(ih[8]|(ih[9]<<8)|(ih[10]<<16)|(ih[11]<<24));
    int bpp = ih[14]|(ih[15]<<8);

    printf("BMP: %dx%d, %d bpp, data at offset %u\n\n", w, h, bpp, dataOff);

    if (bpp != 8) { fprintf(stderr, "Expected 8bpp, got %d\n", bpp); return 1; }

    // Read palette (256 * 4 bytes = 1024)
    uint8_t pal[1024]; fread(pal, 1, 1024, f);
    printf("Palette sample (B G R A):\n");
    for (int i = 0; i < 256; i += 32)
        printf("  [%3d] %3d %3d %3d %3d\n", i, pal[i*4], pal[i*4+1], pal[i*4+2], pal[i*4+3]);
    printf("\n");

    // Read pixel data (8-bit indexed, bottom-up, stride padded to 4)
    int stride = (w + 3) & ~3;
    fseek(f, dataOff, SEEK_SET);
    std::vector<uint8_t> pixels((size_t)stride * h);
    fread(pixels.data(), 1, (size_t)stride * h, f);
    fclose(f);

    // Histogram of raw index values
    int hist[256] = {};
    long total = 0;
    long sum = 0;
    uint8_t minV = 255, maxV = 0;
    for (int row = 0; row < h; row++) {
        const uint8_t* rowptr = pixels.data() + (size_t)(h-1-row) * stride; // BMP is bottom-up
        for (int col = 0; col < w; col++) {
            uint8_t v = rowptr[col];
            hist[v]++;
            sum += v;
            total++;
            if (v < minV) minV = v;
            if (v > maxV) maxV = v;
        }
    }

    printf("Pixel index value statistics:\n");
    printf("  Min:  %d\n", minV);
    printf("  Max:  %d\n", maxV);
    printf("  Mean: %.1f\n", (double)sum / total);
    printf("  Total pixels: %ld\n\n", total);

    // Print histogram buckets
    printf("Histogram (index ranges and counts):\n");
    for (int bucket = 0; bucket < 256; bucket += 8) {
        int count = 0;
        for (int i = bucket; i < bucket+8 && i < 256; i++) count += hist[i];
        if (count == 0) continue;
        int bar = count * 40 / (int)total;  // scale to 40 chars
        printf("  [%3d-%3d] %8d  |", bucket, bucket+7, count);
        for (int i = 0; i < bar; i++) printf("#");
        printf("\n");
    }

    // Also look at the palette grey values for the min/max indices
    printf("\nPalette grey values at key indices:\n");
    for (int i = minV; i <= maxV; i += (maxV-minV > 32 ? (maxV-minV)/16 : 1)) {
        uint8_t b=pal[i*4], g=pal[i*4+1], r=pal[i*4+2];
        printf("  index %3d -> R=%3d G=%3d B=%3d  (grey ramp would be: %3d)\n", i, r, g, b, i);
    }

    return 0;
}
