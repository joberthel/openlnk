/* cleanroom/desktop/main_client.c */
#include "interlnk/interlnk.h"

#include <stdio.h>
#include <string.h>

static void print_usage(void) {
    printf("ilnk-client (libinterlnk 0.1.0)\n");
    printf("Usage:\n");
    printf("  ilnk-client --version\n");
    printf("  ilnk-client --port <device> --baud <rate>\n");
}

int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("ilnk-client 0.1.0 (C99 zero-alloc core)\n");
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
