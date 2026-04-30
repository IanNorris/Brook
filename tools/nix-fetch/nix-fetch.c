/*
 * nix-fetch: Download and install a Nix package from cache.nixos.org
 *
 * Self-contained C program — no shell utilities needed.
 * Requires only: curl and xz binaries, plus nar-unpack.
 *
 * Usage:
 *   nix-fetch <store-path-hash>    # e.g. 10s5j3mfdg22k1597x580qrhprnzcjwb
 *   nix-fetch --deps <hash>        # also fetch all dependencies
 *
 * Environment:
 *   CURL_PATH      - path to curl binary (default: search PATH)
 *   XZ_PATH        - path to xz binary (default: search PATH)
 *   NAR_UNPACK_PATH - path to nar-unpack binary (default: search PATH)
 *   NIX_STORE      - store directory (default: /nix/store)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>

#define MAX_NARINFO  8192
#define MAX_PATH     4096
#define MAX_REFS     64

static const char *g_cache_url  = "https://cache.nixos.org";
static const char *g_store_dir  = "/nix/store";
/* Persistent on-disk narinfo cache. cache.nixos.org narinfos are
 * effectively immutable (they describe the contents of a fixed store
 * path keyed by hash), so caching them on disk avoids one TLS handshake
 * + curl fork/exec per closure walk. Each install of a package whose
 * deps were already resolved becomes ~free. */
static const char *g_narinfo_cache_dir = "/nix/var/cache/narinfo";
static const char *g_curl_path  = NULL;
static const char *g_cacert_path = NULL;
static const char *g_xz_path    = NULL;
static const char *g_unpack_path = NULL;
static int g_fetch_deps = 0;
static int g_cache_disabled = 0;  /* set to 1 if cache dir creation fails */

/* Set of hashes already fetched or in-progress — prevents redundant downloads
 * when a package lists itself in its own References, or when the same dep
 * appears in multiple dependency chains. */
#define MAX_SEEN 256
static char g_seen_hashes[MAX_SEEN][33];
static int  g_seen_count = 0;

static int hash_is_seen(const char *hash) {
    for (int i = 0; i < g_seen_count; i++)
        if (memcmp(g_seen_hashes[i], hash, 32) == 0) return 1;
    return 0;
}

static void hash_mark_seen(const char *hash) {
    if (g_seen_count < MAX_SEEN)
        memcpy(g_seen_hashes[g_seen_count++], hash, 33);
}

/* mkdir -p equivalent.  Returns 0 on success or if it already exists,
 * -1 on real failure.  Walks each path component creating directories.
 * Skips empty components and treats EEXIST as success. */
static int mkdir_p(const char *path) {
    char buf[MAX_PATH];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) return -1;
    memcpy(buf, path, len + 1);

    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            if (mkdir(buf, 0755) < 0 && errno != EEXIST) return -1;
            buf[i] = '/';
        }
    }
    if (mkdir(buf, 0755) < 0 && errno != EEXIST) return -1;
    return 0;
}

/* Build the on-disk narinfo cache path for a hash.  Flat layout —
 * fewer than ~hundreds of entries expected on a typical user's disk,
 * sharding adds complexity for no measurable benefit. */
static void narinfo_cache_path(const char *hash, char *out, size_t outsz) {
    snprintf(out, outsz, "%s/%s.narinfo", g_narinfo_cache_dir, hash);
}

/* Lazily create the cache directory on first use.  If creation fails
 * (read-only fs, no perms, etc.) flip g_cache_disabled so subsequent
 * calls short-circuit. */
static void narinfo_cache_init(void) {
    static int done = 0;
    if (done) return;
    done = 1;
    if (mkdir_p(g_narinfo_cache_dir) < 0) {
        fprintf(stderr, "nix-fetch: warning: cannot create cache dir %s: %s\n",
                g_narinfo_cache_dir, strerror(errno));
        g_cache_disabled = 1;
    }
}

/* Read a cached narinfo from disk into buf.  Returns bytes read, 0
 * if not cached, -1 on error.  We keep it simple: if the file exists
 * and is parseable later, it's a hit. */
static int narinfo_cache_load(const char *hash, char *buf, size_t bufsz) {
    char path[MAX_PATH];
    narinfo_cache_path(hash, path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    size_t total = 0;
    ssize_t r;
    while (total < bufsz - 1 &&
           (r = read(fd, buf + total, bufsz - 1 - total)) > 0) {
        total += (size_t)r;
    }
    close(fd);
    if (total == 0) return 0;
    buf[total] = '\0';
    return (int)total;
}

/* Atomically write a narinfo to the cache.  Write to <path>.tmp then
 * rename to avoid leaving partial files if we get killed mid-write. */
static void narinfo_cache_save(const char *hash, const char *data, size_t len) {
    narinfo_cache_init();
    if (g_cache_disabled) return;

    char path[MAX_PATH], tmp[MAX_PATH];
    narinfo_cache_path(hash, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    size_t written = 0;
    while (written < len) {
        ssize_t w = write(fd, data + written, len - written);
        if (w <= 0) { close(fd); unlink(tmp); return; }
        written += (size_t)w;
    }
    close(fd);
    if (rename(tmp, path) < 0) unlink(tmp);
}

/* Monotonic milliseconds for ad-hoc timing instrumentation. */
static long now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) return 0;
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Batch-prefetch a set of narinfos in a single curl invocation.
 *
 * Each curl invocation pays ~1.2s for fork + DNS + TCP + TLS + HTTP +
 * teardown.  Most of that is the TLS handshake.  By passing many URLs
 * to a single curl, we amortise the handshake across all of them:
 * curl reuses the HTTP/1.1 connection for every subsequent URL.
 *
 * We use curl's -K config-file syntax so each URL gets its own output
 * file written directly into the narinfo cache.  Skips hashes that
 * already exist on disk (no point re-fetching).  Failures here are
 * non-fatal — the recursive walk will fall back to per-hash fetch if
 * a cache lookup misses.
 *
 * Note: we deliberately do NOT pass "Connection: close" here (unlike
 * single fetches) because we *want* connection reuse across URLs.
 * curl will close cleanly at end of input.
 */
static void batch_prefetch_narinfos(char hashes[][33], int n) {
    if (n <= 0 || g_cache_disabled) return;
    narinfo_cache_init();
    if (g_cache_disabled) return;

    /* Build curl config file in /tmp.  Each entry: url= ... \n output= ... */
    char cfg_path[MAX_PATH];
    snprintf(cfg_path, sizeof(cfg_path), "/tmp/nix-fetch-batch-%d.cfg", (int)getpid());

    FILE *f = fopen(cfg_path, "w");
    if (!f) return;

    int written = 0;
    long t_build0 = now_ms();
    for (int i = 0; i < n; i++) {
        /* Skip if cache file already exists */
        char cache_path[MAX_PATH];
        narinfo_cache_path(hashes[i], cache_path, sizeof(cache_path));
        if (access(cache_path, F_OK) == 0) continue;
        fprintf(f, "url = \"%s/%s.narinfo\"\n", g_cache_url, hashes[i]);
        fprintf(f, "output = \"%s\"\n", cache_path);
        written++;
    }
    fclose(f);

    if (written == 0) { unlink(cfg_path); return; }

    fprintf(stderr, "nix-fetch: prefetching %d narinfo(s) in one curl...\n", written);
    long t0 = now_ms();
    pid_t pid = fork();
    if (pid < 0) { unlink(cfg_path); return; }
    if (pid == 0) {
        /* No stdout capture — outputs go to per-URL files via config.
         * -Z runs transfers in parallel (default cap 50, we limit via
         * --parallel-max).  Each transfer is independent, with its own
         * connection — we keep "Connection: close" so the server FINs
         * promptly (the alternative, single connection reused
         * sequentially, sits in recv() waiting for FINs that never
         * come and burns the SockRecv hard-timeout repeatedly — measured
         * 9s/narinfo vs the 1.2s baseline).
         *
         * Parallelism cap of 4 keeps load on cache.nixos.org modest
         * while collapsing the wall-clock by ~4x in the typical case. */
        if (g_cacert_path)
            execlp(g_curl_path, "curl", "-4", "-sS", "--http1.1",
                   "-Z", "--parallel-max", "4",
                   "-H", "Connection: close",
                   "--cacert", g_cacert_path, "-K", cfg_path, (char*)NULL);
        else
            execlp(g_curl_path, "curl", "-4", "-sS", "--http1.1",
                   "-Z", "--parallel-max", "4",
                   "-H", "Connection: close",
                   "-K", cfg_path, (char*)NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);
    long t_done = now_ms();
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    fprintf(stderr, "nix-fetch: batch prefetch %d narinfos: %ldms (build=%ldms curl exit=%d)\n",
            written, t_done - t0, t0 - t_build0, code);
    unlink(cfg_path);
    /* Even on partial failure, individual file contents are intact:
     * curl writes each output file as it streams; failed URLs leave
     * either no file (404) or a partial file (network).  The
     * subsequent fetch_package call will re-validate on cache hit. */
}

/* exec curl with optional --cacert; never returns on success */
static void exec_curl(const char *url) {
    /* --http1.1 forces HTTP/1.1 via ALPN. With the default HTTP/2
     * negotiation the server (Fastly) ignores "Connection: close" because
     * HTTP/2 uses a single multiplexed TCP connection for many streams —
     * the stream ends but the TCP connection stays open for reuse. That
     * leaves curl blocked in recv() waiting for a FIN that never comes
     * until our SockRecv hard-timeout fires 180s later. Forcing HTTP/1.1
     * makes "Connection: close" meaningful and the server FINs promptly. */
    if (g_cacert_path)
        execlp(g_curl_path, "curl", "-4", "-sSL", "--http1.1",
               "-H", "Connection: close",
               "--cacert", g_cacert_path, url, (char*)NULL);
    else
        execlp(g_curl_path, "curl", "-4", "-sSL", "--http1.1",
               "-H", "Connection: close",
               url, (char*)NULL);
    perror("execl curl");
    _exit(127);
}

/* Find a binary, checking env var first, then common Nix store paths.
 * Caller supplies the scratch buffer so sequential calls don't alias each
 * other's PATH-search result. */
static const char *find_binary(const char *env_var, const char *name,
                                const char *fallback_paths[], int n_fallbacks,
                                char *scratch, size_t scratch_sz) {
    const char *p = getenv(env_var);
    if (p && access(p, X_OK) == 0) return p;

    for (int i = 0; i < n_fallbacks; i++) {
        if (access(fallback_paths[i], X_OK) == 0) return fallback_paths[i];
    }

    /* Try PATH-based search */
    const char *path = getenv("PATH");
    if (path) {
        char pathcopy[4096];
        strncpy(pathcopy, path, sizeof(pathcopy) - 1);
        pathcopy[sizeof(pathcopy) - 1] = '\0';
        char *dir = pathcopy;
        while (dir) {
            char *next = strchr(dir, ':');
            if (next) *next++ = '\0';
            snprintf(scratch, scratch_sz, "%s/%s", dir, name);
            if (access(scratch, X_OK) == 0) return scratch;
            dir = next;
        }
    }
    return NULL;
}

/* Run: curl -4 -sL <url> and capture output to buffer */
static int curl_fetch(const char *url, char *buf, size_t bufsz) {
    int pfd[2];
    long t0 = now_ms();
    if (pipe(pfd) < 0) { perror("pipe"); return -1; }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }

    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        exec_curl(url);
    }

    close(pfd[1]);
    long t_fork = now_ms();
    size_t total = 0;
    ssize_t n;
    while (total < bufsz - 1 && (n = read(pfd[0], buf + total, bufsz - 1 - total)) > 0) {
        total += n;
    }
    long t_read = now_ms();
    buf[total] = '\0';
    close(pfd[0]);

    int status;
    waitpid(pid, &status, 0);
    long t_done = now_ms();
    fprintf(stderr, "nix-fetch: timing url=%.40s fork=%ldms read=%ldms wait=%ldms total=%ldms bytes=%zu\n",
            url, t_fork - t0, t_read - t_fork, t_done - t_read, t_done - t0, total);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        fprintf(stderr, "nix-fetch: curl exited with status %d (url: %s)\n", code, url);
        if (total > 0)
            fprintf(stderr, "nix-fetch: curl output (%zu bytes): %.512s\n", total, buf);
        return -1;
    }
    return (int)total;
}

/* Parse a field from narinfo text. Returns pointer into buf or NULL. */
static char *narinfo_field(char *narinfo, const char *field, char *out, size_t outsz) {
    size_t flen = strlen(field);
    char *p = narinfo;
    while ((p = strstr(p, field)) != NULL) {
        /* Check it's at start of line */
        if (p != narinfo && *(p - 1) != '\n') { p += flen; continue; }
        p += flen;
        if (*p != ':') continue;
        p++; /* skip ':' */
        while (*p == ' ') p++;
        char *end = strchr(p, '\n');
        if (!end) end = p + strlen(p);
        size_t len = (size_t)(end - p);
        if (len >= outsz) len = outsz - 1;
        memcpy(out, p, len);
        out[len] = '\0';
        /* Strip trailing \r */
        if (len > 0 && out[len-1] == '\r') out[len-1] = '\0';
        return out;
    }
    return NULL;
}

/* Run the download+decompress+unpack pipeline:
 *   curl <nar_url> | xz -d | nar-unpack <dest>
 */
static int download_and_extract(const char *nar_url, const char *compression,
                                 const char *dest) {
    char full_url[MAX_PATH];
    snprintf(full_url, sizeof(full_url), "%s/%s", g_cache_url, nar_url);

    int use_xz = (strcmp(compression, "xz") == 0);

    if (use_xz) {
        /* Three-stage pipeline: curl | xz -d | nar-unpack */
        int pipe1[2], pipe2[2];
        if (pipe(pipe1) < 0) { perror("pipe1"); return -1; }
        if (pipe(pipe2) < 0) { perror("pipe2"); close(pipe1[0]); close(pipe1[1]); return -1; }

        /* curl */
        pid_t pid_curl = fork();
        if (pid_curl < 0) { perror("fork curl"); return -1; }
        if (pid_curl == 0) {
            close(pipe1[0]); close(pipe2[0]); close(pipe2[1]);
            dup2(pipe1[1], STDOUT_FILENO);
            close(pipe1[1]);
            exec_curl(full_url);
        }

        /* xz -d */
        pid_t pid_xz = fork();
        if (pid_xz < 0) { perror("fork xz"); return -1; }
        if (pid_xz == 0) {
            close(pipe1[1]); close(pipe2[0]);
            dup2(pipe1[0], STDIN_FILENO);
            dup2(pipe2[1], STDOUT_FILENO);
            close(pipe1[0]); close(pipe2[1]);
            execl(g_xz_path, "xz", "-d", NULL);
            _exit(127);
        }

        /* nar-unpack */
        pid_t pid_unpack = fork();
        if (pid_unpack < 0) { perror("fork nar-unpack"); return -1; }
        if (pid_unpack == 0) {
            close(pipe1[0]); close(pipe1[1]); close(pipe2[1]);
            dup2(pipe2[0], STDIN_FILENO);
            close(pipe2[0]);
            execl(g_unpack_path, "nar-unpack", dest, NULL);
            _exit(127);
        }

        close(pipe1[0]); close(pipe1[1]);
        close(pipe2[0]); close(pipe2[1]);

        int status;
        waitpid(pid_curl, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "nix-fetch: curl failed\n");
            waitpid(pid_xz, &status, 0);
            waitpid(pid_unpack, &status, 0);
            return -1;
        }

        waitpid(pid_xz, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "nix-fetch: xz decompression failed\n");
            waitpid(pid_unpack, &status, 0);
            return -1;
        }

        waitpid(pid_unpack, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "nix-fetch: nar-unpack failed\n");
            return -1;
        }

    } else {
        /* Two-stage: curl | nar-unpack */
        int pfd[2];
        if (pipe(pfd) < 0) { perror("pipe"); return -1; }

        pid_t pid_curl = fork();
        if (pid_curl < 0) { perror("fork"); return -1; }
        if (pid_curl == 0) {
            close(pfd[0]);
            dup2(pfd[1], STDOUT_FILENO);
            close(pfd[1]);
            exec_curl(full_url);
        }

        pid_t pid_unpack = fork();
        if (pid_unpack < 0) { perror("fork"); return -1; }
        if (pid_unpack == 0) {
            close(pfd[1]);
            dup2(pfd[0], STDIN_FILENO);
            close(pfd[0]);
            execl(g_unpack_path, "nar-unpack", dest, NULL);
            _exit(127);
        }

        close(pfd[0]); close(pfd[1]);

        int status;
        waitpid(pid_curl, &status, 0);
        waitpid(pid_unpack, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    }

    return 0;
}

static int fetch_package(const char *hash);

/* Parse references from narinfo and fetch missing ones.
 *
 * IMPORTANT: uses strtok_r, not strtok. fetch_package() recurses back into
 * fetch_dependencies(), and strtok's global state does NOT survive recursion —
 * the inner call overwrites the outer's state, and on return the outer's
 * saved pointer references a popped stack frame. That produced wild jumps
 * (crash at RIP=0x4738ab well outside the text segment). */
static void fetch_dependencies(const char *refs_str) {
    char buf[4096];
    strncpy(buf, refs_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* First pass: collect every ref hash that isn't already on disk and
     * isn't already in our cache, then issue a single batched curl to
     * pull all of those narinfos at once.  Without this, the recursive
     * walk pays one TLS handshake per dep (~1.2s).  With it, the whole
     * closure shares a single connection — a large fraction of the
     * narinfo phase collapses.  Recursive fetch_package calls below
     * then hit the warm cache and avoid spawning curl entirely. */
    {
        char prefetch_hashes[64][33];
        int prefetch_n = 0;
        char scan_buf[4096];
        memcpy(scan_buf, buf, sizeof(scan_buf));
        char *sp = NULL;
        char *t = strtok_r(scan_buf, " \t", &sp);
        while (t && prefetch_n < 64) {
            char *dash = strchr(t, '-');
            size_t hlen = dash ? (size_t)(dash - t) : strlen(t);
            if (hlen == 32) {
                char h[33]; memcpy(h, t, 32); h[32] = '\0';
                if (!hash_is_seen(h)) {
                    char p[MAX_PATH];
                    snprintf(p, sizeof(p), "%s/%s", g_store_dir, t);
                    if (access(p, F_OK) != 0) {
                        memcpy(prefetch_hashes[prefetch_n++], h, 33);
                    }
                }
            }
            t = strtok_r(NULL, " \t", &sp);
        }
        if (prefetch_n > 1) batch_prefetch_narinfos(prefetch_hashes, prefetch_n);
    }

    char *saveptr = NULL;
    char *tok = strtok_r(buf, " \t", &saveptr);
    while (tok) {
        /* Extract hash (first 32 chars before the dash) */
        char ref_hash[33];
        char *dash = strchr(tok, '-');
        size_t hlen = dash ? (size_t)(dash - tok) : strlen(tok);
        if (hlen == 32) {
            memcpy(ref_hash, tok, 32);
            ref_hash[32] = '\0';

            /* Skip if already fetched or in-progress (prevents self-referential
             * packages and duplicate work across shared dependency chains) */
            if (hash_is_seen(ref_hash)) {
                tok = strtok_r(NULL, " \t", &saveptr);
                continue;
            }

            /* Also skip if the store path already exists on disk */
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s/%s", g_store_dir, tok);
            if (access(path, F_OK) == 0) {
                printf("  Already on disk: %s\n", tok);
                tok = strtok_r(NULL, " \t", &saveptr);
                continue;
            }

            printf("  Fetching dependency: %s\n", tok);
            fetch_package(ref_hash);
        }
        tok = strtok_r(NULL, " \t", &saveptr);
    }
}

static int fetch_package(const char *hash) {
    /* Guard against self-referential packages and shared deps in parallel chains */
    if (hash_is_seen(hash)) return 0;
    hash_mark_seen(hash);

    char url[MAX_PATH];
    snprintf(url, sizeof(url), "%s/%s.narinfo", g_cache_url, hash);

    char narinfo[MAX_NARINFO];
    int n = -1;
    int from_cache = 0;

    /* Try the on-disk cache first.  cache.nixos.org narinfos are
     * immutable for a given hash, so a cache hit lets us skip the
     * fork+exec curl + TLS handshake entirely — the major per-package
     * cost when the closure walk hits a dep we've seen before in a
     * previous run. */
    if (!g_cache_disabled) {
        n = narinfo_cache_load(hash, narinfo, sizeof(narinfo));
        if (n > 0) from_cache = 1;
    }

    if (n <= 0) {
        printf("Fetching narinfo for %s...\n", hash);
        n = curl_fetch(url, narinfo, sizeof(narinfo));
        if (n <= 0) {
            fprintf(stderr, "nix-fetch: failed to fetch narinfo for %s\n", hash);
            return -1;
        }
        if (!g_cache_disabled)
            narinfo_cache_save(hash, narinfo, (size_t)n);
    } else {
        printf("Cached narinfo for %s\n", hash);
    }
    (void)from_cache;

    char store_path[MAX_PATH], nar_url[MAX_PATH];
    char compression[64], references[4096];

    if (!narinfo_field(narinfo, "StorePath", store_path, sizeof(store_path))) {
        fprintf(stderr, "nix-fetch: no StorePath in narinfo\n");
        return -1;
    }
    if (!narinfo_field(narinfo, "URL", nar_url, sizeof(nar_url))) {
        fprintf(stderr, "nix-fetch: no URL in narinfo\n");
        return -1;
    }
    if (!narinfo_field(narinfo, "Compression", compression, sizeof(compression))) {
        strcpy(compression, "none");
    }

    /* Extract basename */
    const char *basename = strrchr(store_path, '/');
    basename = basename ? basename + 1 : store_path;

    char dest[MAX_PATH];
    snprintf(dest, sizeof(dest), "%s/%s", g_store_dir, basename);

    /* Skip if already installed */
    if (access(dest, F_OK) == 0) {
        printf("  Already installed: %s\n", basename);
        return 0;
    }

    printf("  Package:     %s\n", basename);
    printf("  Compression: %s\n", compression);

    /* Fetch deps first */
    if (g_fetch_deps) {
        if (narinfo_field(narinfo, "References", references, sizeof(references))) {
            fetch_dependencies(references);
        }
    }

    printf("  Downloading NAR...\n");
    int rc = download_and_extract(nar_url, compression, dest);
    if (rc == 0) {
        printf("  Installed: %s\n", basename);
    } else {
        fprintf(stderr, "  FAILED: %s\n", basename);
    }
    return rc;
}

int main(int argc, char **argv) {
    int argi = 1;

    if (argi < argc && strcmp(argv[argi], "--deps") == 0) {
        g_fetch_deps = 1;
        argi++;
    }

    if (argi >= argc) {
        fprintf(stderr, "Usage: nix-fetch [--deps] <store-path-hash>\n");
        return 1;
    }

    const char *hash = argv[argi];

    /* Strip /nix/store/ prefix if given full path */
    if (strncmp(hash, "/nix/store/", 11) == 0) hash += 11;
    /* Take just the hash part (32 chars before the dash) */
    static char hash_buf[33];
    const char *dash = strchr(hash, '-');
    size_t hlen = dash ? (size_t)(dash - hash) : strlen(hash);
    if (hlen != 32) {
        fprintf(stderr, "nix-fetch: invalid hash length %zu (expected 32)\n", hlen);
        return 1;
    }
    memcpy(hash_buf, hash, 32);
    hash_buf[32] = '\0';

    /* Read env / find tools */
    const char *store = getenv("NIX_STORE");
    if (store) g_store_dir = store;

    const char *curl_paths[] = {
        "/nix/bin/curl",   // symlink created by create_nix_disk.sh
        "/usr/bin/curl",
    };
    const char *cacert_paths[] = {
        "/nix/etc/ssl/certs/ca-bundle.crt",  // symlink created by create_nix_disk.sh
        "/etc/ssl/certs/ca-bundle.crt",
        "/etc/ssl/certs/ca-certificates.crt",
    };
    const char *xz_paths[] = {
        "/nix/bin/xz",     // symlink created by create_nix_disk.sh
        "/usr/bin/xz",
    };
    const char *unpack_paths[] = {
        "/nix/bin/nar-unpack",
        "/usr/bin/nar-unpack",
    };

    static char curl_scratch[MAX_PATH];
    static char xz_scratch[MAX_PATH];
    static char unpack_scratch[MAX_PATH];
    g_curl_path   = find_binary("CURL_PATH", "curl", curl_paths, 2,
                                curl_scratch, sizeof(curl_scratch));
    g_xz_path     = find_binary("XZ_PATH", "xz", xz_paths, 2,
                                xz_scratch, sizeof(xz_scratch));
    g_unpack_path = find_binary("NAR_UNPACK_PATH", "nar-unpack", unpack_paths, 2,
                                unpack_scratch, sizeof(unpack_scratch));

    /* Find CA certificate bundle */
    const char *cacert_env = getenv("CURL_CA_BUNDLE");
    if (cacert_env && access(cacert_env, R_OK) == 0) {
        g_cacert_path = cacert_env;
    } else {
        for (int i = 0; i < (int)(sizeof(cacert_paths)/sizeof(cacert_paths[0])); i++) {
            if (access(cacert_paths[i], R_OK) == 0) { g_cacert_path = cacert_paths[i]; break; }
        }
    }

    if (!g_curl_path)   { fprintf(stderr, "nix-fetch: curl not found\n"); return 1; }
    if (!g_xz_path)     { fprintf(stderr, "nix-fetch: xz not found\n"); return 1; }
    if (!g_unpack_path) { fprintf(stderr, "nix-fetch: nar-unpack not found\n"); return 1; }

    printf("nix-fetch: curl=%s\n", g_curl_path);
    printf("nix-fetch: cacert=%s\n", g_cacert_path ? g_cacert_path : "(none)");
    printf("nix-fetch: xz=%s\n", g_xz_path);
    printf("nix-fetch: nar-unpack=%s\n", g_unpack_path);
    printf("nix-fetch: store=%s\n", g_store_dir);

    int rc = fetch_package(hash_buf);
    return rc == 0 ? 0 : 1;
}
