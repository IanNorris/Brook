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
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>

#define MAX_NARINFO  8192
#define MAX_PATH     4096
#define MAX_REFS     64

static const char *g_cache_url  = "https://cache.nixos.org";
static const char *g_store_dir  = "/nix/store";
static const char *g_curl_path  = NULL;
static const char *g_cacert_path = NULL;
static const char *g_xz_path    = NULL;
static const char *g_unpack_path = NULL;
static int g_fetch_deps = 0;

/* exec curl with optional --cacert; never returns on success */
static void exec_curl(const char *url) {
    if (g_cacert_path)
        execlp(g_curl_path, "curl", "-4", "-sL", "--cacert", g_cacert_path, url, (char*)NULL);
    else
        execlp(g_curl_path, "curl", "-4", "-sL", url, (char*)NULL);
    perror("execl curl");
    _exit(127);
}

/* Find a binary, checking env var first, then common Nix store paths */
static const char *find_binary(const char *env_var, const char *name,
                                const char *fallback_paths[], int n_fallbacks) {
    const char *p = getenv(env_var);
    if (p && access(p, X_OK) == 0) return p;

    for (int i = 0; i < n_fallbacks; i++) {
        if (access(fallback_paths[i], X_OK) == 0) return fallback_paths[i];
    }

    /* Try PATH-based search using /usr/bin/which or just direct access */
    static char buf[MAX_PATH];
    const char *path = getenv("PATH");
    if (path) {
        char pathcopy[4096];
        strncpy(pathcopy, path, sizeof(pathcopy) - 1);
        pathcopy[sizeof(pathcopy) - 1] = '\0';
        char *dir = pathcopy;
        while (dir) {
            char *next = strchr(dir, ':');
            if (next) *next++ = '\0';
            snprintf(buf, sizeof(buf), "%s/%s", dir, name);
            if (access(buf, X_OK) == 0) return buf;
            dir = next;
        }
    }
    return NULL;
}

/* Run: curl -4 -sL <url> and capture output to buffer */
static int curl_fetch(const char *url, char *buf, size_t bufsz) {
    int pfd[2];
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
    size_t total = 0;
    ssize_t n;
    while (total < bufsz - 1 && (n = read(pfd[0], buf + total, bufsz - 1 - total)) > 0) {
        total += n;
    }
    buf[total] = '\0';
    close(pfd[0]);

    int status;
    waitpid(pid, &status, 0);
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

/* Parse references from narinfo and fetch missing ones */
static void fetch_dependencies(const char *refs_str) {
    char buf[4096];
    strncpy(buf, refs_str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *tok = strtok(buf, " \t");
    while (tok) {
        /* Extract hash (first 32 chars before the dash) */
        char ref_hash[33];
        char *dash = strchr(tok, '-');
        size_t hlen = dash ? (size_t)(dash - tok) : strlen(tok);
        if (hlen == 32) {
            memcpy(ref_hash, tok, 32);
            ref_hash[32] = '\0';

            /* Check if already present */
            char path[MAX_PATH];
            snprintf(path, sizeof(path), "%s/%s", g_store_dir, tok);
            if (access(path, F_OK) != 0) {
                printf("  Fetching dependency: %s\n", tok);
                fetch_package(ref_hash);
            }
        }
        tok = strtok(NULL, " \t");
    }
}

static int fetch_package(const char *hash) {
    char url[MAX_PATH];
    snprintf(url, sizeof(url), "%s/%s.narinfo", g_cache_url, hash);

    printf("Fetching narinfo for %s...\n", hash);

    char narinfo[MAX_NARINFO];
    int n = curl_fetch(url, narinfo, sizeof(narinfo));
    if (n <= 0) {
        fprintf(stderr, "nix-fetch: failed to fetch narinfo for %s\n", hash);
        return -1;
    }

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
        "/nix/store/xkqd49dmldkqn4xk6dlm640f5blbv6hp-curl-8.18.0-bin/bin/curl",
        "/nix/bin/curl",   // static mini-curl fallback
        "/usr/bin/curl",
    };
    const char *cacert_paths[] = {
        "/nix/store/mg063aj0crwhchqayf2qbyf28k6mlrxm-nss-cacert-3.121/etc/ssl/certs/ca-bundle.crt",
        "/etc/ssl/certs/ca-bundle.crt",
        "/etc/ssl/certs/ca-certificates.crt",
    };
    const char *xz_paths[] = {
        "/nix/store/g6mlwdvpg92rchq352ll7jbi0pz7h43r-xz-5.8.2-bin/bin/xz",
        "/nix/bin/xz",
        "/usr/bin/xz",
    };
    const char *unpack_paths[] = {
        "/nix/bin/nar-unpack",
        "/usr/bin/nar-unpack",
    };

    g_curl_path = find_binary("CURL_PATH", "curl", curl_paths, 3);
    g_xz_path = find_binary("XZ_PATH", "xz", xz_paths, 3);
    g_unpack_path = find_binary("NAR_UNPACK_PATH", "nar-unpack", unpack_paths, 2);

    /* Find CA certificate bundle */
    const char *cacert_env = getenv("CURL_CA_BUNDLE");
    if (cacert_env && access(cacert_env, R_OK) == 0) {
        g_cacert_path = cacert_env;
    } else {
        for (int i = 0; i < 3; i++) {
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
