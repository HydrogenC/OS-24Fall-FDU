#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char buf[512];

int cat(int fd)
{
    int len;
    while ((len = read(fd, buf, sizeof(buf))) > 0) {
        write(STDOUT_FILENO, buf, len);
    }

    if (len < 0) {
        printf("cat: failed to read file `%s`\n", fd);
        exit(0);
    }
}

int main(int argc, char *argv[])
{
    /* (Final) TODO BEGIN */
    if (argc < 2) {
        // Cat stdin
        cat(0);
        exit(0);
    }

    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], 0);
        if (fd < 0) {
            printf("cat: failed to open file `%s`\n", fd);
            exit(0);
        }

        cat(fd);
        write(STDOUT_FILENO, "\n", 1);
        close(fd);
    }

    /* (Final) TODO END */
    return 0;
}