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
#include <dirent.h>
#include <errno.h>

#define INDEX_PATH      "/nix/index/packages.idx"
#define PROFILE_DIR     "/nix/profile"
#define MANIFEST_PATH   "/nix/profile/manifest.tsv"
#define PROFILE_BIN     "/nix/profile/bin"
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

static int is_installed(const char *name) {
    FILE *f = fopen(MANIFEST_PATH, "r");
    if (!f) return 0;

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';

        char *tab = strchr(line, '\t');
        if (tab) *tab = '\0';

        if (strcasecmp(line, name) == 0) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
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

/* Create symlinks from profile/bin/ to the package's bin/ directory */
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

        char src[1024], dst[1024];
        snprintf(src, sizeof(src), "%s/%s", bin_dir, ent->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", PROFILE_BIN, ent->d_name);

        /* Check if it's a regular file or symlink */
        struct stat st;
        if (stat(src, &st) == 0 && (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))) {
            unlink(dst); /* Remove existing link if any */
            if (symlink(src, dst) == 0) {
                linked++;
            }
        }
    }
    closedir(d);
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

static int cmd_install(const char *name) {
    printf("Looking up '%s'...\n", name);

    if (is_installed(name)) {
        printf("'%s' is already installed.\n", name);
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

    if (in_store) {
        printf("Already in store: %s\n", pkg.store_name);
    } else {
        /* Fetch package and dependencies */
        printf("Fetching %s and dependencies...\n", hash);
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "/nix/bin/nix-fetch --deps %s", hash);
        int ret = system(cmd);
        if (ret != 0) {
            fprintf(stderr, "Error: nix-fetch failed (exit %d)\n", ret);
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

    printf("\n\033[32m✓ Installed %s %s\033[0m\n", pkg.name, pkg.version);
    if (linked > 0)
        printf("  %d binary(ies) added to %s\n", linked, PROFILE_BIN);

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
    }

    /* Remove from manifest */
    if (remove_from_manifest(name) != 0) {
        fprintf(stderr, "Error: failed to update manifest\n");
        return 1;
    }

    printf("\033[32m✓ Removed %s\033[0m\n", name);
    return 0;
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
        } else if (strcmp(cmd, "search") == 0 || strcmp(cmd, "s") == 0) {
            /* Delegate to nix-search */
            if (argc < 3) {
                fprintf(stderr, "Usage: %s search <query>\n", prog);
                return 1;
            }
            char cmd_buf[1024];
            snprintf(cmd_buf, sizeof(cmd_buf), "/nix/bin/nix-search %s", argv[2]);
            return system(cmd_buf);
        } else if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
            usage(prog);
            return 0;
        } else {
            fprintf(stderr, "Unknown command: %s\n", cmd);
            usage(prog);
            return 1;
        }
    }

    /* Called as nix-install directly */
    if (argc < 2) {
        fprintf(stderr, "Usage: nix-install <package>\n");
        return 1;
    }
    return cmd_install(argv[1]);
}
