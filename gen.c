// Generates edge case test files.

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define KiB 1024
#define MiB (KiB * 1024)
#define GiB (MiB * 1024)

__attribute__((noreturn))
static void fatal(const char *msg)
{
    printf("%s", msg);
    if (errno) {
        printf(" - %s", strerror(errno));
    }
    printf("\n");
    exit(1);
}

static inline uint8_t next(void)
{
    static uint32_t s = 0xDEADBEEF;
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

#define next_printable() ((next() & 63) + 33)

static void create_large_text(void)
{
    fputs("creating 0large.txt\n", stderr);

    FILE *fd = fopen("0large.txt", "w+");
    if (!fd) {
        fatal("create_large_text");
    }

    uint8_t buf[80];
    for (int i = 0; i < 1 * GiB; i += 80) {
        for (int j = 0; j < 79; ++j) {
            buf[j] = next_printable();
        }
        buf[79] = '\n';

        fwrite(buf, sizeof(buf), 1, fd);
    }

    fclose(fd);
}

static void create_very_long_line(void)
{
    fputs("creating 0longline.txt\n", stderr);

    FILE *fd = fopen("0longline.txt", "w+");
    if (!fd) {
        fatal("create_very_long_line");
    }

    uint8_t buf[80];
    for (int i = 0; i < 100 * MiB; i += 80) {
        for (int j = 0; j < 80; ++j) {
            buf[j] = next_printable();
        }

        fwrite(buf, sizeof(buf), 1, fd);
    }

    fclose(fd);
}

static void create_mix_binary_ascii(void)
{
    fputs("creating 0binascii.txt\n", stderr);

    FILE *fd = fopen("0binascii.txt", "w+");
    if (!fd) {
        fatal("create_large_text");
    }

    for (int i = 0; i < 1 * MiB; ++i) {
        const char c = next();
        fputc(c, fd);
    }

    fclose(fd);
}

int main(void)
{
    create_large_text();
    create_very_long_line();
    create_mix_binary_ascii();
}
