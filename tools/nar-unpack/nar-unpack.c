/*
 * nar-unpack: Extract a Nix Archive (NAR) from stdin to a directory.
 *
 * NAR format:
 *   - Strings are: uint64_le(len) + bytes + padding to 8-byte boundary
 *   - Archive starts with literal string "nix-archive-1"
 *   - Then a recursive node structure using tagged fields
 *
 * Usage: nar-unpack <dest-dir>
 *        cat foo.nar | nar-unpack /nix/store/xxx-foo
 *        curl ... | xz -d | nar-unpack /nix/store/xxx-foo
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>

#define MAX_STR  (16 * 1024 * 1024)  /* 16MB max string/file-contents chunk */
#define PATH_MAX_NAR 4096

static uint64_t total_bytes = 0;

static void die(const char *msg) {
    fprintf(stderr, "nar-unpack: %s (at byte %llu)\n", msg,
            (unsigned long long)total_bytes);
    exit(1);
}

static void die2(const char *msg, const char *detail) {
    fprintf(stderr, "nar-unpack: %s: %s (at byte %llu)\n", msg, detail,
            (unsigned long long)total_bytes);
    exit(1);
}

/* Read exactly n bytes from stdin */
static void read_exact(void *buf, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(STDIN_FILENO, (char *)buf + done, n - done);
        if (r <= 0) {
            if (r == 0) die("unexpected end of input");
            if (errno == EINTR) continue;
            die("read error");
        }
        done += r;
    }
    total_bytes += n;
}

/* Skip n bytes */
static void skip_bytes(size_t n) {
    char tmp[4096];
    while (n > 0) {
        size_t chunk = n < sizeof(tmp) ? n : sizeof(tmp);
        read_exact(tmp, chunk);
        n -= chunk;
    }
}

/* Read a NAR string into a buffer. Returns length. */
static uint64_t read_str(char *buf, size_t bufsz) {
    uint64_t len;
    read_exact(&len, 8);

    if (len >= bufsz) die("string too long");
    if (len > 0) read_exact(buf, len);
    buf[len] = '\0';

    /* Skip padding to 8-byte boundary */
    size_t pad = (8 - (len % 8)) % 8;
    if (pad > 0) skip_bytes(pad);

    return len;
}

/* Expect a specific string token */
static void expect(const char *expected) {
    char buf[256];
    read_str(buf, sizeof(buf));
    if (strcmp(buf, expected) != 0) {
        fprintf(stderr, "nar-unpack: expected '%s', got '%s'\n", expected, buf);
        exit(1);
    }
}

/* Build a path, ensuring no overflow */
static void path_join(char *out, size_t outsz, const char *dir, const char *name) {
    size_t dlen = strlen(dir);
    size_t nlen = strlen(name);
    if (dlen + 1 + nlen + 1 > outsz) die("path too long");
    memcpy(out, dir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, name, nlen);
    out[dlen + 1 + nlen] = '\0';
}

/* Create directory and parents */
static void mkdirs(const char *path) {
    char tmp[PATH_MAX_NAR];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) die("path too long in mkdirs");
    memcpy(tmp, path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* Parse a node recursively */
static void parse_node(const char *path) {
    char tag[256];

    expect("(");
    expect("type");

    read_str(tag, sizeof(tag));

    if (strcmp(tag, "regular") == 0) {
        int executable = 0;
        char field[256];
        read_str(field, sizeof(field));

        if (strcmp(field, "executable") == 0) {
            /* Read and discard the empty string value */
            char empty[64];
            read_str(empty, sizeof(empty));
            executable = 1;
            read_str(field, sizeof(field));
        }

        if (strcmp(field, "contents") != 0) {
            die2("expected 'contents'", field);
        }

        /* Read file length */
        uint64_t flen;
        read_exact(&flen, 8);

        /* Open output file */
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC,
                       executable ? 0755 : 0644);
        if (fd < 0) die2("cannot create file", path);

        /* Stream file contents */
        char buf[65536];
        uint64_t remaining = flen;
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(buf) ? (size_t)remaining : sizeof(buf);
            read_exact(buf, chunk);
            size_t written = 0;
            while (written < chunk) {
                ssize_t w = write(fd, buf + written, chunk - written);
                if (w <= 0) {
                    if (errno == EINTR) continue;
                    die2("write error", path);
                }
                written += w;
            }
            remaining -= chunk;
        }
        close(fd);

        /* Skip padding */
        size_t pad = (8 - (flen % 8)) % 8;
        if (pad > 0) skip_bytes(pad);

        expect(")");

    } else if (strcmp(tag, "directory") == 0) {
        mkdirs(path);

        char field[256];
        for (;;) {
            read_str(field, sizeof(field));

            if (strcmp(field, ")") == 0) {
                break;  /* end of directory */
            }

            if (strcmp(field, "entry") != 0) {
                die2("expected 'entry' or ')'", field);
            }

            expect("(");
            expect("name");

            char name[1024];
            read_str(name, sizeof(name));

            /* Reject path traversal */
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 ||
                strchr(name, '/') != NULL) {
                die2("invalid entry name", name);
            }

            expect("node");

            char child_path[PATH_MAX_NAR];
            path_join(child_path, sizeof(child_path), path, name);

            parse_node(child_path);

            expect(")");
        }

    } else if (strcmp(tag, "symlink") == 0) {
        expect("target");

        char target[PATH_MAX_NAR];
        read_str(target, sizeof(target));

        if (symlink(target, path) != 0) {
            /* If symlink fails, print warning but continue */
            fprintf(stderr, "nar-unpack: warning: symlink %s -> %s: %s\n",
                    path, target, strerror(errno));
        }

        expect(")");

    } else {
        die2("unknown node type", tag);
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: nar-unpack <dest-dir>\n");
        fprintf(stderr, "Reads NAR from stdin, extracts to <dest-dir>\n");
        return 1;
    }

    const char *dest = argv[1];

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Read and verify magic */
    expect("nix-archive-1");

    /* Parse root node */
    parse_node(dest);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000L
            + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

    fprintf(stderr, "nar-unpack: extracted to %s (%llu bytes, %ldms, %lluKB/s)\n",
            dest, (unsigned long long)total_bytes, ms,
            ms > 0 ? (unsigned long long)total_bytes / (unsigned long long)ms
                   : 0ULL);

    return 0;
}
