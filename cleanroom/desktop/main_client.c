/* cleanroom/desktop/main_client.c */
#include "openlnk/openlnk.h"

#include <stdio.h>
#include <string.h>

static void print_usage(void) {
    printf("openlnk (libopenlnk 0.2.0)\n");
    printf("Usage:\n");
    printf("  openlnk --version\n");
    printf("  openlnk --port <device> --baud <rate>\n");
}

int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("openlnk 0.2.0 (libopenlnk C99 zero-alloc MS-DOS 6.22 protocol)\n");
            return 0;
        }
        if ((strcmp(argv[i], "--help") == 0) || (strcmp(argv[i], "-h") == 0)) {
            print_usage();
            return 0;
        }
    }
    print_usage();
    return 0;
}
