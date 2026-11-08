/* lbctl: admin client for the master's Unix socket (C6). Placeholder. */
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    (void)argv;
    if (argc < 2) {
        fprintf(stderr, "usage: lbctl <command>\n");
        return 2;
    }
    fprintf(stderr, "lbctl: not implemented yet (C6)\n");
    return 1;
}
