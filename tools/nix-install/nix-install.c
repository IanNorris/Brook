/*
 * nix-install: Install packages from the Nix binary cache into Brook
 *
 * Looks up package name in /nix/index/packages.idx,
 * calls nix-fetch --deps to download the full closure,
 * and updates the user's profile manifest.
 *
 * Usage:
 *   nix install <package>       Install a package
 *   nix remove <package>        Remove a package
 *   nix list                    List installed packages
 *
 * Build: musl-gcc -static -O2 -o nix-install nix-install.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <elf.h>

#define INDEX_PATH      "/nix/index/packages.idx"
#define PROFILE_DIR     "/nix/profile"
#define MANIFEST_PATH   "/nix/profile/manifest.tsv"
#define PROFILE_BIN     "/nix/profile/bin"
#define PROFILE_LIB     "/nix/profile/lib"
#define APPS_DIR        "/nix/share/applications"
#define APPS_IDX        "/nix/share/applications/applications.idx"
#define APPS_ICONS_DIR  "/nix/share/applications/icons"
#define MAX_LINE        4096
#define MAX_PACKAGES    256

/* Package info from the index */
struct pkg_info {
    char name[256];
    char version[128];
    char store_name[512];
    char description[1024];
};

/* Installed package from manifest */
struct installed_pkg {
    char name[256];
    char version[128];
    char store_name[512];
};

static int lookup_package(const char *query, struct pkg_info *out) {
    FILE *f = fopen(INDEX_PATH, "r");
    if (!f) {
        fprintf(stderr, "Error: cannot open %s\n", INDEX_PATH);
        return -1;
    }

    char line[MAX_LINE];
    struct pkg_info exact = {0};
    int found_exact = 0;

    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';

        char *name = line;
        char *version = strchr(name, '\t');
        if (!version) continue;
        *version++ = '\0';

        char *store_name = strchr(version, '\t');
        if (!store_name) continue;
        *store_name++ = '\0';

        char *description = strchr(store_name, '\t');
        if (!description) continue;
        *description++ = '\0';

        if (strcasecmp(name, query) == 0) {
            /* Exact match - prefer the one with a version */
            if (!found_exact || (version[0] && !exact.version[0])) {
                strncpy(exact.name, name, sizeof(exact.name) - 1);
                strncpy(exact.version, version, sizeof(exact.version) - 1);
                strncpy(exact.store_name, store_name, sizeof(exact.store_name) - 1);
                strncpy(exact.description, description, sizeof(exact.description) - 1);
                found_exact = 1;
            }
        }
    }

    fclose(f);

    if (found_exact) {
        *out = exact;
        return 0;
    }
    return -1;
}

static int mkdir_p(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    return mkdir(tmp, 0755);
}

static int run_program(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "Error: fork failed for %s: %s\n", argv[0], strerror(errno));
        return -1;
    }
    if (pid == 0) {
        execv(argv[0], argv);
        fprintf(stderr, "Error: exec failed for %s: %s\n", argv[0], strerror(errno));
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        fprintf(stderr, "Error: waitpid failed for %s: %s\n", argv[0], strerror(errno));
        return -1;
    }

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

static int run_nix_fetch_deps(const char *hash, int force) {
    char *const argv[] = {
        (char*)"/nix/bin/nix-fetch",
        (char*)"--deps",
        force ? (char*)"--force" : (char*)hash,
        force ? (char*)hash : NULL,
        NULL,
    };
    return run_program(argv);
}

static int read_full_at(int fd, void *buf, size_t len, off_t off) {
    if (lseek(fd, off, SEEK_SET) < 0) return -1;
    size_t done = 0;
    while (done < len) {
        ssize_t r = read(fd, (char*)buf + done, len - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;
        done += (size_t)r;
    }
    return 0;
}

static int elf_file_healthy(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 1;

    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return 0;
    }

    Elf64_Ehdr eh;
    if (read_full_at(fd, &eh, sizeof(eh), 0) != 0) {
        close(fd);
        return 0;
    }

    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0) {
        close(fd);
        return 1;
    }

    int healthy = 1;
    if (eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != ELFDATA2LSB ||
        eh.e_phentsize != sizeof(Elf64_Phdr) ||
        eh.e_phoff > (uint64_t)st.st_size ||
        (uint64_t)eh.e_phnum * sizeof(Elf64_Phdr) > (uint64_t)st.st_size - eh.e_phoff) {
        healthy = 0;
        goto out;
    }

    if (eh.e_shoff != 0 && eh.e_shnum != 0) {
        if (eh.e_shentsize == 0 ||
            eh.e_shoff > (uint64_t)st.st_size ||
            (uint64_t)eh.e_shnum * eh.e_shentsize > (uint64_t)st.st_size - eh.e_shoff) {
            healthy = 0;
            goto out;
        }
    }

    for (uint16_t i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph;
        if (read_full_at(fd, &ph, sizeof(ph),
                         (off_t)(eh.e_phoff + (uint64_t)i * sizeof(ph))) != 0) {
            healthy = 0;
            goto out;
        }
        if (ph.p_type != PT_DYNAMIC) continue;

        if (ph.p_offset > (uint64_t)st.st_size ||
            ph.p_filesz > (uint64_t)st.st_size - ph.p_offset) {
            healthy = 0;
            goto out;
        }

        uint64_t count = ph.p_filesz / sizeof(Elf64_Dyn);
        int saw_non_null = 0;
        for (uint64_t j = 0; j < count; ++j) {
            Elf64_Dyn dyn;
            if (read_full_at(fd, &dyn, sizeof(dyn),
                             (off_t)(ph.p_offset + j * sizeof(dyn))) != 0) {
                healthy = 0;
                goto out;
            }
            if (dyn.d_tag == DT_NULL) break;
            saw_non_null = 1;
        }
        if (!saw_non_null) {
            healthy = 0;
            goto out;
        }
    }

out:
    close(fd);
    return healthy;
}

static int store_path_healthy(const char *store_name) {
    char store_path[512];
    snprintf(store_path, sizeof(store_path), "/nix/store/%s", store_name);

    struct stat st;
    if (stat(store_path, &st) != 0 || !S_ISDIR(st.st_mode))
        return 0;

    char bin_dir[512];
    snprintf(bin_dir, sizeof(bin_dir), "%s/bin", store_path);
    DIR *d = opendir(bin_dir);
    if (!d)
        return 1;

    int healthy = 1;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", bin_dir, ent->d_name);
        if (stat(path, &st) != 0)
            continue;
        if (!S_ISREG(st.st_mode))
            continue;

        if (!elf_file_healthy(path)) {
            fprintf(stderr, "nix-install: corrupt ELF detected: %s\n", path);
            healthy = 0;
            break;
        }
    }
    closedir(d);
    return healthy;
}

/* Look up an installed package by name. If found, copy its store_name
 * into out_store (size out_sz) and return 1. Returns 0 if not installed. */
static int find_installed(const char *name, char *out_store, size_t out_sz) {
    FILE *f = fopen(MANIFEST_PATH, "r");
    if (!f) return 0;

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';

        char *p_name = line;
        char *p_ver = strchr(p_name, '\t');
        if (!p_ver) continue;
        *p_ver++ = '\0';
        char *p_store = strchr(p_ver, '\t');
        if (!p_store) continue;
        *p_store++ = '\0';

        if (strcasecmp(p_name, name) == 0) {
            if (out_store && out_sz) {
                strncpy(out_store, p_store, out_sz - 1);
                out_store[out_sz - 1] = '\0';
            }
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

static int is_installed(const char *name) {
    return find_installed(name, NULL, 0);
}

static int add_to_manifest(const struct pkg_info *pkg) {
    FILE *f = fopen(MANIFEST_PATH, "a");
    if (!f) {
        fprintf(stderr, "Error: cannot write manifest %s: %s\n",
                MANIFEST_PATH, strerror(errno));
        return -1;
    }
    fprintf(f, "%s\t%s\t%s\n", pkg->name, pkg->version, pkg->store_name);
    fclose(f);
    return 0;
}

static int remove_from_manifest(const char *name) {
    FILE *f = fopen(MANIFEST_PATH, "r");
    if (!f) return -1;

    /* Read all lines except the matching one */
    struct installed_pkg pkgs[MAX_PACKAGES];
    int count = 0;
    char line[MAX_LINE];
    int found = 0;

    while (fgets(line, sizeof(line), f) && count < MAX_PACKAGES) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        if (len == 0) continue;

        char *p_name = line;
        char *p_ver = strchr(p_name, '\t');
        if (!p_ver) continue;
        *p_ver++ = '\0';
        char *p_store = strchr(p_ver, '\t');
        if (!p_store) continue;
        *p_store++ = '\0';

        if (strcasecmp(p_name, name) == 0) {
            found = 1;
            continue; /* skip this entry */
        }

        strncpy(pkgs[count].name, p_name, sizeof(pkgs[count].name) - 1);
        strncpy(pkgs[count].version, p_ver, sizeof(pkgs[count].version) - 1);
        strncpy(pkgs[count].store_name, p_store, sizeof(pkgs[count].store_name) - 1);
        count++;
    }
    fclose(f);

    if (!found) return -1;

    /* Rewrite manifest */
    f = fopen(MANIFEST_PATH, "w");
    if (!f) return -1;
    for (int i = 0; i < count; i++) {
        fprintf(f, "%s\t%s\t%s\n", pkgs[i].name, pkgs[i].version, pkgs[i].store_name);
    }
    fclose(f);
    return 0;
}

/* Create symlinks from profile/bin/ to the package's bin/ directory.
 * Use RELATIVE targets so that path resolution works across mount
 * boundaries. The symlink lives at /nix/profile/bin/<name> and must
 * reach /nix/store/<pkg>/bin/<name>, so we need to go up TWO levels
 * (profile/bin -> profile -> /nix) before descending into store:
 *   ../../store/<pkg>/bin/<name>
 * A one-level-up target (../store/...) was wrong — it resolved to
 * /nix/profile/store/... and the kernel's VFS correctly reported the
 * path as non-existent, which caused execve to fall back to busybox
 * and every installed binary to exit 127. */
static int link_package_bins(const char *store_name) {
    char bin_dir[512];
    snprintf(bin_dir, sizeof(bin_dir), "/nix/store/%s/bin", store_name);

    DIR *d = opendir(bin_dir);
    if (!d) return 0; /* No bin dir is OK (e.g., libraries) */

    mkdir_p(PROFILE_BIN);

    struct dirent *ent;
    int linked = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char rel_target[1024], dst[1024], src_abs[1024];
        /* Relative target: /nix/profile/bin/foo -> ../../store/<name>/bin/foo
         * (two levels up: bin/ -> profile/ -> nix/, then down into store/) */
        snprintf(rel_target, sizeof(rel_target), "../../store/%s/bin/%s",
                 store_name, ent->d_name);
        snprintf(src_abs, sizeof(src_abs), "%s/%s", bin_dir, ent->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", PROFILE_BIN, ent->d_name);

        /* Check if it's a regular file or symlink (stat the real path) */
        struct stat st;
        if (stat(src_abs, &st) == 0 && (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))) {
            unlink(dst); /* Remove existing link if any */
            if (symlink(rel_target, dst) == 0) {
                linked++;
            }
        }
    }
    closedir(d);
    return linked;
}

static int link_package_libs_to_dir(const char *store_name, const char *dst_dir,
                                    const char *rel_prefix, int replace_regular) {
    char lib_dir[512];
    snprintf(lib_dir, sizeof(lib_dir), "/nix/store/%s/lib", store_name);

    DIR *d = opendir(lib_dir);
    if (!d) return 0; /* No lib dir is OK (e.g., tools) */

    mkdir_p(dst_dir);

    struct dirent *ent;
    int linked = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char rel_target[1024], dst[1024], src_abs[1024];
        snprintf(rel_target, sizeof(rel_target), "%s%s/lib/%s",
                 rel_prefix, store_name, ent->d_name);
        snprintf(src_abs, sizeof(src_abs), "%s/%s", lib_dir, ent->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", dst_dir, ent->d_name);

        struct stat st;
        if (stat(src_abs, &st) == 0 && (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))) {
            struct stat dst_st;
            if (lstat(dst, &dst_st) == 0) {
                if (!S_ISLNK(dst_st.st_mode) && !replace_regular)
                    continue;
                /* Skip if existing symlink already points to the same target —
                 * re-linking would briefly invalidate mmap'd libraries used by
                 * running processes (e.g. bash using glibc from this path). */
                if (S_ISLNK(dst_st.st_mode)) {
                    char existing[1024];
                    ssize_t rl = readlink(dst, existing, sizeof(existing) - 1);
                    if (rl > 0) {
                        existing[rl] = '\0';
                        if (strcmp(existing, rel_target) == 0)
                            continue; /* already correct */
                    }
                }
                unlink(dst);
            }
            if (symlink(rel_target, dst) == 0)
                linked++;
        }
    }
    closedir(d);
    return linked;
}

static int link_package_libs(const char *store_name) {
    return link_package_libs_to_dir(store_name, PROFILE_LIB, "../../store/", 1);
}

static void store_hash_from_name(const char *store_name, char out[33]) {
    out[0] = '\0';
    const char *dash = strchr(store_name, '-');
    size_t hlen = dash ? (size_t)(dash - store_name) : strlen(store_name);
    if (hlen == 32) {
        memcpy(out, store_name, 32);
        out[32] = '\0';
    }
}

/* Seen-set for the transitive closure walk: grows dynamically so a large
 * closure (hundreds of paths) is fully covered. */
static char (*g_libseen)[33] = NULL;
static int   g_libseen_count = 0;
static int   g_libseen_cap   = 0;

static int libseen_test_and_add(const char *hash) {
    for (int i = 0; i < g_libseen_count; i++)
        if (memcmp(g_libseen[i], hash, 32) == 0) return 1; /* already seen */
    if (g_libseen_count >= g_libseen_cap) {
        int newcap = g_libseen_cap ? g_libseen_cap * 2 : 512;
        void *np = realloc(g_libseen, (size_t)newcap * 33);
        if (!np) return 1; /* OOM: treat as seen to bound recursion */
        g_libseen = (char (*)[33])np;
        g_libseen_cap = newcap;
    }
    memcpy(g_libseen[g_libseen_count++], hash, 33);
    return 0;
}

/* Link the libs of a store path AND its entire transitive reference closure
 * into /nix/profile/lib. The previous one-level walk missed libraries that are
 * only reachable through an intermediate package (e.g. ffmpeg -> ffmpeg-lib ->
 * libopus), so ffplay failed at runtime with "libopus.so.0: cannot open shared
 * object file" despite the .so being present in the store. */
static int link_closure_libs_transitive(const char *store_name) {
    char hash[33];
    store_hash_from_name(store_name, hash);
    if (!hash[0]) return 0;
    if (libseen_test_and_add(hash)) return 0;

    int linked = link_package_libs(store_name);

    char narinfo_path[512];
    snprintf(narinfo_path, sizeof(narinfo_path),
             "/nix/var/cache/narinfo/%s.narinfo", hash);
    FILE *f = fopen(narinfo_path, "r");
    if (!f) return linked;

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "References:", 11) != 0) continue;
        char *p = line + 11;
        while (*p && isspace((unsigned char)*p)) p++;

        /* Collect refs first, then recurse — strtok_r state does not survive
         * the recursive call (it reuses the same global tokeniser). */
        char refs[MAX_LINE];
        snprintf(refs, sizeof(refs), "%s", p);
        char *saveptr = NULL;
        char *tok = strtok_r(refs, " \t\r\n", &saveptr);
        char ref_names[64][256];
        int nref = 0;
        while (tok && nref < 64) {
            snprintf(ref_names[nref++], 256, "%s", tok);
            tok = strtok_r(NULL, " \t\r\n", &saveptr);
        }
        for (int i = 0; i < nref; i++) {
            char rh[33];
            store_hash_from_name(ref_names[i], rh);
            if (rh[0] && strcmp(ref_names[i], store_name) != 0)
                linked += link_closure_libs_transitive(ref_names[i]);
        }
        break;
    }
    fclose(f);
    return linked;
}

/* Free + reset the seen-set between top-level installs. */
static void link_closure_reset(void) {
    free(g_libseen);
    g_libseen = NULL;
    g_libseen_count = 0;
    g_libseen_cap = 0;
}

static int link_closure_libs_into_package(const char *store_name) {
    char package_lib_dir[512];
    snprintf(package_lib_dir, sizeof(package_lib_dir), "/nix/store/%s/lib", store_name);

    char hash[33];
    store_hash_from_name(store_name, hash);
    if (!hash[0]) return 0;

    char narinfo_path[512];
    snprintf(narinfo_path, sizeof(narinfo_path),
             "/nix/var/cache/narinfo/%s.narinfo", hash);

    FILE *f = fopen(narinfo_path, "r");
    if (!f) return 0;

    int linked = 0;
    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "References:", 11) != 0) continue;

        char *p = line + 11;
        while (*p && isspace((unsigned char)*p)) p++;

        char *saveptr = NULL;
        char *tok = strtok_r(p, " \t\r\n", &saveptr);
        while (tok) {
            if (strcmp(tok, store_name) != 0)
                linked += link_package_libs_to_dir(tok, package_lib_dir, "../../", 0);
            tok = strtok_r(NULL, " \t\r\n", &saveptr);
        }
        break;
    }

    fclose(f);
    return linked;
}

/* Remove symlinks that point into a package's store path */
static int unlink_package_bins(const char *store_name) {
    DIR *d = opendir(PROFILE_BIN);
    if (!d) return 0;

    char prefix[512];
    snprintf(prefix, sizeof(prefix), "/nix/store/%s/", store_name);
    size_t prefix_len = strlen(prefix);

    struct dirent *ent;
    int removed = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", PROFILE_BIN, ent->d_name);

        char target[1024];
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = '\0';
            if (strncmp(target, prefix, prefix_len) == 0) {
                unlink(path);
                removed++;
            }
        }
    }
    closedir(d);
    return removed;
}

static int unlink_package_libs(const char *store_name) {
    DIR *d = opendir(PROFILE_LIB);
    if (!d) return 0;

    char prefix[512];
    snprintf(prefix, sizeof(prefix), "../../store/%s/lib/", store_name);
    size_t prefix_len = strlen(prefix);

    struct dirent *ent;
    int removed = 0;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", PROFILE_LIB, ent->d_name);

        char target[1024];
        ssize_t n = readlink(path, target, sizeof(target) - 1);
        if (n > 0) {
            target[n] = '\0';
            if (strncmp(target, prefix, prefix_len) == 0) {
                unlink(path);
                removed++;
            }
        }
    }
    closedir(d);
    return removed;
}

/* -----------------------------------------------------------------------
 * Desktop entry collection — rebuild applications.idx after install/remove.
 * Scans nix store packages for .desktop files and writes a tab-separated
 * manifest. Icon conversion is skipped (no libpng in userspace) — existing
 * .rgba icons from the host build are preserved.
 * ----------------------------------------------------------------------- */

/* Simple .desktop file parser — extract Name, Exec, Icon, Categories */
static int parse_desktop_entry(const char *path, char *name, size_t name_sz,
                               char *exec_cmd, size_t exec_sz,
                               char *icon, size_t icon_sz,
                               char *categories, size_t cat_sz)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    name[0] = exec_cmd[0] = icon[0] = categories[0] = '\0';
    int in_desktop = 0;
    char line[1024];

    while (fgets(line, sizeof(line), f)) {
        /* Strip trailing newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (strcmp(line, "[Desktop Entry]") == 0) { in_desktop = 1; continue; }
        if (line[0] == '[') { in_desktop = 0; continue; }
        if (!in_desktop) continue;

        if (strncmp(line, "Name=", 5) == 0)
            snprintf(name, name_sz, "%s", line + 5);
        else if (strncmp(line, "Exec=", 5) == 0)
            snprintf(exec_cmd, exec_sz, "%s", line + 5);
        else if (strncmp(line, "Icon=", 5) == 0)
            snprintf(icon, icon_sz, "%s", line + 5);
        else if (strncmp(line, "Categories=", 11) == 0)
            snprintf(categories, cat_sz, "%s", line + 11);
        else if (strncmp(line, "Hidden=true", 11) == 0) { fclose(f); return -1; }
        else if (strncmp(line, "NoDisplay=true", 14) == 0) { fclose(f); return -1; }
        else if (strncmp(line, "Type=", 5) == 0 && strcmp(line + 5, "Application") != 0)
            { fclose(f); return -1; }
    }
    fclose(f);

    if (name[0] == '\0' || exec_cmd[0] == '\0') return -1;

    /* Strip Exec field codes (%f, %F, %u, %U, etc.) */
    char *pct = exec_cmd;
    while ((pct = strchr(pct, '%')) != NULL) {
        if (pct[1]) {
            /* Remove %X and any preceding space */
            char *start = pct;
            if (start > exec_cmd && start[-1] == ' ') start--;
            memmove(start, pct + 2, strlen(pct + 2) + 1);
            pct = start;
        } else {
            *pct = '\0';
        }
    }
    /* Trim trailing spaces */
    size_t elen = strlen(exec_cmd);
    while (elen > 0 && exec_cmd[elen-1] == ' ') exec_cmd[--elen] = '\0';

    return 0;
}

static void refresh_desktop_entries(void)
{
    /* Ensure output directories exist */
    mkdir(APPS_DIR, 0755);
    mkdir(APPS_ICONS_DIR, 0755);

    /* Scan nix store packages for .desktop files */
    DIR *store = opendir("/nix/store");
    if (!store) return;

    struct {
        char name[256];
        char exec_cmd[512];
        char icon_file[256];
        char categories[256];
    } entries[MAX_PACKAGES];
    int entry_count = 0;
    char seen_names[MAX_PACKAGES][256];
    int seen_count = 0;

    struct dirent *pkg_ent;
    while ((pkg_ent = readdir(store)) != NULL && entry_count < MAX_PACKAGES) {
        if (pkg_ent->d_name[0] == '.') continue;

        char apps_path[1024];
        snprintf(apps_path, sizeof(apps_path),
                 "/nix/store/%s/share/applications", pkg_ent->d_name);

        DIR *apps = opendir(apps_path);
        if (!apps) continue;

        struct dirent *de;
        while ((de = readdir(apps)) != NULL && entry_count < MAX_PACKAGES) {
            size_t nlen = strlen(de->d_name);
            if (nlen < 9 || strcmp(de->d_name + nlen - 8, ".desktop") != 0)
                continue;

            char desktop_path[1536];
            snprintf(desktop_path, sizeof(desktop_path), "%s/%s", apps_path, de->d_name);

            char name[256], exec_cmd[512], icon[256], categories[256];
            if (parse_desktop_entry(desktop_path, name, sizeof(name),
                                    exec_cmd, sizeof(exec_cmd),
                                    icon, sizeof(icon),
                                    categories, sizeof(categories)) != 0)
                continue;

            /* Deduplicate by name */
            int dup = 0;
            for (int i = 0; i < seen_count; i++) {
                if (strcmp(seen_names[i], name) == 0) { dup = 1; break; }
            }
            if (dup) continue;
            snprintf(seen_names[seen_count++], 256, "%s", name);

            /* Check if an .rgba icon already exists from host-side build */
            char icon_file[256] = "";
            if (icon[0]) {
                char safe[256];
                int si = 0;
                for (int i = 0; icon[i] && si < 254; i++) {
                    char c = icon[i];
                    if (c == '/' || c == ' ') c = '_';
                    if (isalnum(c) || c == '-' || c == '_') safe[si++] = c;
                }
                safe[si] = '\0';

                char rgba_path[512];
                snprintf(rgba_path, sizeof(rgba_path), "%s/%s.rgba", APPS_ICONS_DIR, safe);
                if (access(rgba_path, F_OK) == 0) {
                    snprintf(icon_file, sizeof(icon_file), "%s.rgba", safe);
                }
            }

            snprintf(entries[entry_count].name, 256, "%s", name);
            snprintf(entries[entry_count].exec_cmd, 512, "%s", exec_cmd);
            snprintf(entries[entry_count].icon_file, 256, "%s", icon_file);
            snprintf(entries[entry_count].categories, 256, "%s", categories);
            entry_count++;
        }
        closedir(apps);
    }
    closedir(store);

    /* Write applications.idx */
    FILE *idx = fopen(APPS_IDX, "w");
    if (!idx) {
        fprintf(stderr, "Warning: could not write %s\n", APPS_IDX);
        return;
    }
    for (int i = 0; i < entry_count; i++) {
        fprintf(idx, "%s\t%s\t%s\t%s\n",
                entries[i].name, entries[i].exec_cmd,
                entries[i].icon_file, entries[i].categories);
    }
    fclose(idx);

    if (entry_count > 0)
        printf("  Updated launcher: %d app(s) in %s\n", entry_count, APPS_IDX);
}

static int cmd_install(const char *name) {
    printf("Looking up '%s'...\n", name);

    char existing_store[512] = {0};
    if (find_installed(name, existing_store, sizeof(existing_store))) {
        if (!store_path_healthy(existing_store)) {
            char hash[64] = {0};
            const char *dash = strchr(existing_store, '-');
            if (dash) {
                size_t hlen = dash - existing_store;
                if (hlen < sizeof(hash)) {
                    memcpy(hash, existing_store, hlen);
                    hash[hlen] = '\0';
                }
            }
            if (!hash[0]) {
                fprintf(stderr, "Error: invalid store name '%s'\n", existing_store);
                return 1;
            }

            printf("'%s' is already installed but its store path is incomplete; repairing...\n", name);
            int code = run_nix_fetch_deps(hash, 1);
            if (code != 0) {
                fprintf(stderr, "Error: nix-fetch repair failed (exit %d)\n", code);
                return 1;
            }
        }

        /* Already in manifest. Re-link bins in case the profile/bin
         * symlinks are missing (e.g. wiped, or earlier link step never
         * ran). This is idempotent — link_package_bins unlinks first. */
        int linked = link_package_bins(existing_store);
        int lib_linked = link_package_libs(existing_store);
        link_closure_reset();
        lib_linked += link_closure_libs_transitive(existing_store);
        int rpath_linked = link_closure_libs_into_package(existing_store);
        printf("'%s' is already installed.\n", name);
        if (linked > 0)
            printf("  re-linked %d binary(ies) in %s\n", linked, PROFILE_BIN);
        if (lib_linked > 0)
            printf("  re-linked %d library file(s) in %s\n", lib_linked, PROFILE_LIB);
        if (rpath_linked > 0)
            printf("  re-linked %d runtime library file(s) beside package binaries\n", rpath_linked);
        return 0;
    }

    struct pkg_info pkg;
    if (lookup_package(name, &pkg) != 0) {
        fprintf(stderr, "Package '%s' not found in index.\n", name);
        fprintf(stderr, "Try: nix search %s\n", name);
        return 1;
    }

    printf("Installing %s %s...\n", pkg.name, pkg.version);
    if (pkg.description[0])
        printf("  %s\n", pkg.description);

    /* Extract store hash from store_name (hash-name) */
    char hash[64] = {0};
    const char *dash = strchr(pkg.store_name, '-');
    if (dash) {
        size_t hlen = dash - pkg.store_name;
        if (hlen < sizeof(hash)) {
            memcpy(hash, pkg.store_name, hlen);
            hash[hlen] = '\0';
        }
    }

    if (!hash[0]) {
        fprintf(stderr, "Error: invalid store name '%s'\n", pkg.store_name);
        return 1;
    }

    /* Check if already in store (verify it's not just an empty dir) */
    char store_path[512];
    snprintf(store_path, sizeof(store_path), "/nix/store/%s", pkg.store_name);
    struct stat st;
    int in_store = 0;
    if (stat(store_path, &st) == 0) {
        /* Verify the path has actual content (bin/, lib/, or share/) */
        char check[512];
        snprintf(check, sizeof(check), "%s/bin", store_path);
        if (stat(check, &st) == 0) {
            in_store = 1;
        } else {
            snprintf(check, sizeof(check), "%s/lib", store_path);
            if (stat(check, &st) == 0) {
                in_store = 1;
            } else {
                snprintf(check, sizeof(check), "%s/share", store_path);
                if (stat(check, &st) == 0)
                    in_store = 1;
            }
        }
    }

    if (in_store && !store_path_healthy(pkg.store_name)) {
        printf("Store path exists but appears incomplete; repairing %s...\n", pkg.store_name);
        int code = run_nix_fetch_deps(hash, 1);
        if (code != 0) {
            fprintf(stderr, "Error: nix-fetch repair failed (exit %d)\n", code);
            return 1;
        }
    } else if (in_store) {
        printf("Already in store: %s\n", pkg.store_name);
    } else {
        /* Fetch package and dependencies */
        printf("Fetching %s and dependencies...\n", hash);
        int code = run_nix_fetch_deps(hash, 0);
        if (code != 0) {
            fprintf(stderr, "Error: nix-fetch failed (exit %d)\n", code);
            return 1;
        }
    }

    /* Create profile directories */
    mkdir_p(PROFILE_DIR);

    /* Add to manifest */
    if (add_to_manifest(&pkg) != 0) {
        fprintf(stderr, "Error: failed to update manifest\n");
        return 1;
    }

    /* Link binaries */
    int linked = link_package_bins(pkg.store_name);
    int lib_linked = link_package_libs(pkg.store_name);
    link_closure_reset();
    lib_linked += link_closure_libs_transitive(pkg.store_name);
    int rpath_linked = link_closure_libs_into_package(pkg.store_name);

    printf("\n\033[32m✓ Installed %s %s\033[0m\n", pkg.name, pkg.version);
    if (linked > 0)
        printf("  %d binary(ies) added to %s\n", linked, PROFILE_BIN);
    if (lib_linked > 0)
        printf("  %d library file(s) added to %s\n", lib_linked, PROFILE_LIB);
    if (rpath_linked > 0)
        printf("  %d runtime library file(s) linked beside package binaries\n", rpath_linked);

    refresh_desktop_entries();

    return 0;
}

static int cmd_remove(const char *name) {
    if (!is_installed(name)) {
        fprintf(stderr, "'%s' is not installed.\n", name);
        return 1;
    }

    /* Find store name before removing from manifest */
    FILE *f = fopen(MANIFEST_PATH, "r");
    char store_name[512] = {0};
    if (f) {
        char line[MAX_LINE];
        while (fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
            char *p = line;
            char *tab1 = strchr(p, '\t');
            if (!tab1) continue;
            *tab1++ = '\0';
            char *tab2 = strchr(tab1, '\t');
            if (!tab2) continue;
            *tab2++ = '\0';
            if (strcasecmp(p, name) == 0) {
                strncpy(store_name, tab2, sizeof(store_name) - 1);
                break;
            }
        }
        fclose(f);
    }

    /* Remove symlinks */
    if (store_name[0]) {
        unlink_package_bins(store_name);
        unlink_package_libs(store_name);
    }

    /* Remove from manifest */
    if (remove_from_manifest(name) != 0) {
        fprintf(stderr, "Error: failed to update manifest\n");
        return 1;
    }

    printf("\033[32m✓ Removed %s\033[0m\n", name);
    refresh_desktop_entries();
    return 0;
}

/* Fetch a package's closure into the store (no manifest / profile mutation).
 * Returns 0 on success, sets *out_store_name to the package store dir name. */
static int fetch_package_to_store(const char *name, char *out_store_name, size_t cap) {
    struct pkg_info pkg;
    if (lookup_package(name, &pkg) != 0) {
        fprintf(stderr, "Package '%s' not found in index.\n", name);
        fprintf(stderr, "Try: nix search %s\n", name);
        return -1;
    }

    char hash[64] = {0};
    const char *dash = strchr(pkg.store_name, '-');
    if (dash) {
        size_t hlen = dash - pkg.store_name;
        if (hlen < sizeof(hash)) {
            memcpy(hash, pkg.store_name, hlen);
            hash[hlen] = '\0';
        }
    }
    if (!hash[0]) {
        fprintf(stderr, "Error: invalid store name '%s'\n", pkg.store_name);
        return -1;
    }

    /* Check whether the store path already has real content */
    char store_path[512];
    snprintf(store_path, sizeof(store_path), "/nix/store/%s", pkg.store_name);
    int in_store = 0;
    struct stat st;
    const char *probes[] = { "bin", "lib", "share" };
    for (int i = 0; i < 3 && !in_store; ++i) {
        char check[640];
        snprintf(check, sizeof(check), "%s/%s", store_path, probes[i]);
        if (stat(check, &st) == 0) in_store = 1;
    }

    if (!in_store) {
        printf("Fetching %s %s...\n", pkg.name, pkg.version);
        int code = run_nix_fetch_deps(hash, 0);
        if (code != 0) {
            fprintf(stderr, "Error: nix-fetch failed (exit %d)\n", code);
            return -1;
        }
    } else if (!store_path_healthy(pkg.store_name)) {
        printf("Store path exists but appears incomplete; repairing %s...\n", pkg.store_name);
        int code = run_nix_fetch_deps(hash, 1);
        if (code != 0) {
            fprintf(stderr, "Error: nix-fetch repair failed (exit %d)\n", code);
            return -1;
        }
    }

    strncpy(out_store_name, pkg.store_name, cap - 1);
    out_store_name[cap - 1] = '\0';
    return 0;
}

/* nix shell <pkg...> — start an interactive shell with each pkg's bin dir
 * prepended to PATH.  The store files are fetched (via nix-fetch) if absent
 * but the user's profile/manifest is NOT modified — the shell is ephemeral.
 * Packages remain in /nix/store afterwards (no GC implemented yet). */
static int cmd_shell(int npkgs, char *const *pkgs) {
    if (npkgs < 1) {
        fprintf(stderr, "Usage: nix shell <package> [<package>...]\n");
        return 1;
    }

    /* Build up new PATH: join each pkg's bin dir, then prepend to existing PATH. */
    size_t path_cap = 8192;
    char *new_path = malloc(path_cap);
    if (!new_path) return 1;
    new_path[0] = '\0';
    size_t used = 0;

    for (int i = 0; i < npkgs; ++i) {
        char store_name[512];
        if (fetch_package_to_store(pkgs[i], store_name, sizeof(store_name)) != 0) {
            free(new_path);
            return 1;
        }
        char bin[640];
        snprintf(bin, sizeof(bin), "/nix/store/%s/bin", store_name);
        /* Only add to PATH if the dir actually exists (libraries may not
         * have one — skip silently). */
        struct stat st;
        if (stat(bin, &st) != 0) continue;

        size_t need = used + strlen(bin) + 2;
        if (need > path_cap) {
            path_cap = need * 2;
            char *grown = realloc(new_path, path_cap);
            if (!grown) { free(new_path); return 1; }
            new_path = grown;
        }
        if (used > 0) new_path[used++] = ':';
        memcpy(new_path + used, bin, strlen(bin) + 1);
        used += strlen(bin);
    }

    const char *old_path = getenv("PATH");
    if (!old_path) old_path = "/nix/profile/bin:/boot/BIN";

    size_t total = used + 1 + strlen(old_path) + 1;
    if (total > path_cap) {
        char *grown = realloc(new_path, total);
        if (!grown) { free(new_path); return 1; }
        new_path = grown;
    }
    if (used > 0) { new_path[used++] = ':'; }
    memcpy(new_path + used, old_path, strlen(old_path) + 1);

    if (setenv("PATH", new_path, 1) != 0) {
        fprintf(stderr, "Error: setenv PATH failed: %s\n", strerror(errno));
        free(new_path);
        return 1;
    }

    /* Mark the shell so it's clear we're in a nix shell. */
    char ps1_prefix[256];
    size_t ps1_used = (size_t)snprintf(ps1_prefix, sizeof(ps1_prefix), "[nix-shell:%s", pkgs[0]);
    for (int i = 1; i < npkgs && ps1_used + strlen(pkgs[i]) + 3 < sizeof(ps1_prefix); ++i) {
        ps1_used += (size_t)snprintf(ps1_prefix + ps1_used, sizeof(ps1_prefix) - ps1_used,
                                     ",%s", pkgs[i]);
    }
    snprintf(ps1_prefix + ps1_used, sizeof(ps1_prefix) - ps1_used, "] \\w \\$ ");
    setenv("PS1", ps1_prefix, 1);
    setenv("IN_NIX_SHELL", "impure", 1);

    printf("\033[36mEntering nix shell with %d package(s). Exit to return.\033[0m\n",
           npkgs);
    printf("  PATH=%s\n", new_path);
    free(new_path);

    /* exec bash — prefer /nix/profile/bin/bash, fall back to /boot/BIN/BASH. */
    const char *shells[] = { "/nix/profile/bin/bash", "/boot/BIN/BASH", "/bin/bash", NULL };
    for (int i = 0; shells[i]; ++i) {
        struct stat st;
        if (stat(shells[i], &st) != 0) continue;
        execl(shells[i], shells[i], NULL);
    }

    fprintf(stderr, "Error: no usable shell found (tried /nix/profile/bin/bash, /boot/BIN/BASH)\n");
    return 1;
}

static int cmd_list(void) {
    FILE *f = fopen(MANIFEST_PATH, "r");
    if (!f) {
        printf("No packages installed.\n");
        return 0;
    }

    char line[MAX_LINE];
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        if (len == 0) continue;

        char *name = line;
        char *version = strchr(name, '\t');
        if (version) {
            *version++ = '\0';
            char *store = strchr(version, '\t');
            if (store) *store = '\0';
            printf("  %s %s\n", name, version);
        } else {
            printf("  %s\n", name);
        }
        count++;
    }
    fclose(f);

    if (count == 0)
        printf("No packages installed.\n");
    else
        printf("\n%d package(s) installed.\n", count);

    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr, "Brook Nix Package Manager\n\n");
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s install <package>   Install a package\n", prog);
    fprintf(stderr, "  %s remove <package>    Remove a package\n", prog);
    fprintf(stderr, "  %s list                List installed packages\n", prog);
    fprintf(stderr, "  %s search <query>      Search for packages\n", prog);
    fprintf(stderr, "  %s shell <pkg...>      Start a temporary shell with the packages on PATH\n", prog);
}

int main(int argc, char *argv[]) {
    /* Determine command - support being called as "nix" or "nix-install" */
    const char *prog = argv[0];
    const char *base = strrchr(prog, '/');
    base = base ? base + 1 : prog;

    /* If called as "nix", first arg is the subcommand */
    if (strcmp(base, "nix") == 0) {
        if (argc < 2) {
            usage(prog);
            return 1;
        }
        const char *cmd = argv[1];
        if (strcmp(cmd, "install") == 0 || strcmp(cmd, "i") == 0) {
            if (argc < 3) {
                fprintf(stderr, "Usage: %s install <package>\n", prog);
                return 1;
            }
            return cmd_install(argv[2]);
        } else if (strcmp(cmd, "remove") == 0 || strcmp(cmd, "rm") == 0) {
            if (argc < 3) {
                fprintf(stderr, "Usage: %s remove <package>\n", prog);
                return 1;
            }
            return cmd_remove(argv[2]);
        } else if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0) {
            return cmd_list();
        } else if (strcmp(cmd, "shell") == 0) {
            if (argc < 3) {
                fprintf(stderr, "Usage: %s shell <package> [<package>...]\n", prog);
                return 1;
            }
            return cmd_shell(argc - 2, argv + 2);
        } else if (strcmp(cmd, "search") == 0 || strcmp(cmd, "s") == 0) {
            /* Delegate to nix-search */
            if (argc < 3) {
                fprintf(stderr, "Usage: %s search <query>\n", prog);
                return 1;
            }
            char *const search_argv[] = {
                (char*)"/nix/bin/nix-search",
                argv[2],
                NULL,
            };
            return run_program(search_argv);
        } else if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
            usage(prog);
            return 0;
        } else {
            fprintf(stderr, "Unknown command: %s\n", cmd);
            usage(prog);
            return 1;
        }
    }

    /* Called as nix-install directly. Accepts one or more packages. */
    if (argc < 2) {
        fprintf(stderr, "Usage: nix-install <package> [<package>...]\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int r = cmd_install(argv[i]);
        if (r != 0) rc = r;
    }
    return rc;
}
