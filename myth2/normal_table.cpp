// normal_table.cpp
// Reconstruct the Myth II 256-entry precalculated mesh normal table.
//
// This is based on the engine-side normal-table builders found in Myth II.exe:
//   FUN_004d95aa  - seeds the 1024-entry Q14 trig tables
//   FUN_00472237  - builds the 256-entry mesh normal table
//
// Usage:
//   normal_table
//   normal_table <index>

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

static constexpr int FULL = 1024;
static constexpr int Q14_ONE = 0x4000;
static constexpr int TABLE_SIZE = 256;

struct Myth2NormalEntry {
    int16_t x = 0;
    int16_t y = 0;
    int16_t z = 0;
    int16_t yaw = 0;
    int16_t pitch = 0;
};

static int16_t q14FromFloat(double v) {
    long n = lround(v * (double)Q14_ONE);
    if (n < -32768) n = -32768;
    if (n > 32767) n = 32767;
    return (int16_t)n;
}

static void buildTrigTables(std::vector<int16_t>& sinQ14, std::vector<int16_t>& cosQ14) {
    sinQ14.resize(FULL);
    cosQ14.resize(FULL);

    const double twoPi = 6.28318530717958647692;
    for (int i = 0; i < FULL; i++) {
        double a = ((double)i * twoPi) / (double)FULL;
        sinQ14[(size_t)i] = q14FromFloat(std::sin(a));
        cosQ14[(size_t)i] = q14FromFloat(std::cos(a));
    }

    // Match the engine's explicit exact-cardinal fixes.
    sinQ14[0]   = 0;         cosQ14[0]   =  Q14_ONE;
    sinQ14[256] = Q14_ONE;   cosQ14[256] =  0;
    sinQ14[512] = 0;         cosQ14[512] = -Q14_ONE;
    sinQ14[768] = -Q14_ONE;  cosQ14[768] =  0;
}

static int16_t mythAngleFromXY(int x, int y) {
    double a = std::atan2((double)y, (double)x);
    if (a < 0.0) a += 6.28318530717958647692;
    long n = lround(a * (65536.0 / 6.28318530717958647692));
    return (int16_t)(n & 0xFFFF);
}

static std::vector<Myth2NormalEntry> buildMeshNormalTable() {
    std::vector<int16_t> sinQ14, cosQ14;
    buildTrigTables(sinQ14, cosQ14);

    std::vector<Myth2NormalEntry> out(TABLE_SIZE);
    int entry = 0;
    int local8 = 0x4020;

    while (entry < TABLE_SIZE) {
        unsigned pitchIdx = (unsigned)((local8 >> 6) & 0x3FF);
        int ringCount = (((int)cosQ14[pitchIdx] << 2) / 0x444) + 1;
        if (ringCount > 0) {
            int localC = 0x20;
            while (ringCount > 0 && entry < TABLE_SIZE) {
                unsigned azIdx = (unsigned)((localC >> 6) & 0x3FF);

                Myth2NormalEntry e;
                e.x = (int16_t)(((int)cosQ14[azIdx] * (int)cosQ14[pitchIdx]) >> 14);
                e.y = (int16_t)(((int)sinQ14[azIdx] * (int)cosQ14[pitchIdx]) >> 14);
                e.z = sinQ14[pitchIdx];
                e.yaw = mythAngleFromXY((int)e.x, (int)e.y);

                // The engine computes pitch from z and sqrt(x^2+y^2). We use the same
                // style of 16-bit Myth angle space for a practical reconstruction.
                double radial = std::sqrt((double)e.x * (double)e.x + (double)e.y * (double)e.y);
                e.pitch = mythAngleFromXY((int)e.z, (int)lround(radial));

                out[(size_t)entry] = e;
                entry++;

                localC += (int)(0x10000 / (long long)((((int)cosQ14[pitchIdx] << 2) / 0x444) + 1));
                ringCount--;
            }
        }

        local8 -= 0x444;
    }

    return out;
}

static void usage(const char* p) {
    fprintf(stderr,
        "Myth II Normal Table\n\n"
        "Usage:\n"
        "  %s\n"
        "  %s <index>\n",
        p, p);
}

int main(int argc, char* argv[]) {
    if (argc > 2) {
        usage(argv[0]);
        return 1;
    }

    int wanted = -1;
    if (argc == 2) {
        wanted = atoi(argv[1]);
        if (wanted < 0 || wanted >= TABLE_SIZE) {
            fprintf(stderr, "Index must be in the range 0..255\n");
            return 1;
        }
    }

    auto table = buildMeshNormalTable();

    printf("Myth II Normal Table\n");
    printf("====================\n");
    printf("Entries: %d\n\n", TABLE_SIZE);

    if (wanted >= 0) {
        const Myth2NormalEntry& e = table[(size_t)wanted];
        printf("Index  %d\n", wanted);
        printf("x      %6d\n", (int)e.x);
        printf("y      %6d\n", (int)e.y);
        printf("z      %6d\n", (int)e.z);
        printf("yaw    0x%04X\n", (uint16_t)e.yaw);
        printf("pitch  0x%04X\n", (uint16_t)e.pitch);
        printf("nx     %.6f\n", (double)e.x / (double)Q14_ONE);
        printf("ny     %.6f\n", (double)e.y / (double)Q14_ONE);
        printf("nz     %.6f\n", (double)e.z / (double)Q14_ONE);
        return 0;
    }

    printf("%-5s %-7s %-7s %-7s %-8s %-8s %-10s %-10s %-10s\n",
           "idx", "x", "y", "z", "yaw", "pitch", "nx", "ny", "nz");
    printf("%s\n", std::string(80, '-').c_str());
    for (int i = 0; i < TABLE_SIZE; i++) {
        const Myth2NormalEntry& e = table[(size_t)i];
        printf("%-5d %-7d %-7d %-7d 0x%04X   0x%04X   %-10.6f %-10.6f %-10.6f\n",
               i, (int)e.x, (int)e.y, (int)e.z,
               (uint16_t)e.yaw, (uint16_t)e.pitch,
               (double)e.x / (double)Q14_ONE,
               (double)e.y / (double)Q14_ONE,
               (double)e.z / (double)Q14_ONE);
    }

    return 0;
}
