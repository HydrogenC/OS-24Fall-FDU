#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    /* (Final) TODO BEGIN */
    if (argc < 2) {
        printf("mkdir: no dir name specified\n");
        exit(0);
    }

    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i], 0) < 0) {
            printf("mkdir: failed to create dir `%s`\n", argv[i]);
            exit(0);
        }
    }

    /* (Final) TODO END */
    exit(0);
}