/* imgcmp: reads two disc images (ISO, CSO or ZSO) sector by sector and checks they hold the
   same data. imgcmp <a> <b>. Exits with 1 if they differ. */
#include <stdio.h>
#include <string.h>

#include "host_source.h"

#define RUN 32

int main(int argc, char **argv)
{
    static unsigned char x[RUN * UMD_SECTOR_SIZE], y[RUN * UMD_SECTOR_SIZE];
    umd_source a, b;

    if (argc != 3) {
        fprintf(stderr, "usage: imgcmp <image> <image>\n");
        return 2;
    }
    if (host_source_open(&a, argv[1]) < 0 || host_source_open(&b, argv[2]) < 0) {
        fprintf(stderr, "cannot open the images\n");
        return 1;
    }
    if (a.sector_count != b.sector_count) {
        printf("sector counts differ: %u and %u\n", a.sector_count, b.sector_count);
        return 1;
    }
    for (uint32_t lba = 0; lba < a.sector_count; lba += RUN) {
        uint32_t n = a.sector_count - lba < RUN ? a.sector_count - lba : RUN;
        if (a.read_sectors(a.user, lba, n, x) < 0 || b.read_sectors(b.user, lba, n, y) < 0) {
            printf("read failed at sector %u\n", lba);
            return 1;
        }
        if (memcmp(x, y, n * UMD_SECTOR_SIZE) != 0) {
            printf("differs in sectors %u to %u\n", lba, lba + n - 1);
            return 1;
        }
    }
    printf("same: %u sectors\n", a.sector_count);
    host_source_close(&a);
    host_source_close(&b);
    return 0;
}
