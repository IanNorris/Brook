/*
 * nix-search: Search the Brook Nix package index
 *
 * Reads /nix/index/packages.idx (TSV: name\tversion\tstore_name\tdescription)
 * and searches for matching packages by name or description.
 *
 * Usage: nix-search <query> [--name-only] [--limit N]
 *
 * Build: musl-gcc -static -O2 -o nix-search nix-search.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define INDEX_PATH "/nix/index/packages.idx"
#define MAX_LINE 4096
#define DEFAULT_LIMIT 50

/* Case-insensitive substring search */
static const char *strcasestr_impl(const char *haystack, const char *needle) {
    if (!*needle) return haystack;
    size_t nlen = strlen(needle);
    for (; *haystack; haystack++) {
        if (strncasecmp(haystack, needle, nlen) == 0)
            return haystack;
    }
    return NULL;
}

/* ANSI color codes */
#define C_RESET  "\033[0m"
#define C_GREEN  "\033[32m"
#define C_CYAN   "\033[36m"
#define C_DIM    "\033[2m"
#define C_BOLD   "\033[1m"

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <query> [--name-only] [--limit N]\n", prog);
    fprintf(stderr, "\nSearches the Nix package index for matching packages.\n");
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --name-only   Only match package names, not descriptions\n");
    fprintf(stderr, "  --limit N     Show at most N results (default: %d)\n", DEFAULT_LIMIT);
}

int main(int argc, char *argv[]) {
    const char *query = NULL;
    int name_only = 0;
    int limit = DEFAULT_LIMIT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--name-only") == 0) {
            name_only = 1;
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
            if (limit <= 0) limit = DEFAULT_LIMIT;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        } else if (!query) {
            query = argv[i];
        } else {
            fprintf(stderr, "Error: unexpected argument '%s'\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    if (!query) {
        usage(argv[0]);
        return 1;
    }

    FILE *f = fopen(INDEX_PATH, "r");
    if (!f) {
        fprintf(stderr, "Error: cannot open %s\n", INDEX_PATH);
        fprintf(stderr, "Is the Nix disk mounted?\n");
        return 1;
    }

    char line[MAX_LINE];
    int count = 0;
    int total_matches = 0;

    while (fgets(line, sizeof(line), f)) {
        /* Remove trailing newline */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';

        /* Parse TSV: name\tversion\tstore_name\tdescription */
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

        /* Match query against name (and optionally description) */
        int match = 0;
        if (strcasestr_impl(name, query))
            match = 1;
        else if (!name_only && strcasestr_impl(description, query))
            match = 1;

        if (match) {
            total_matches++;
            if (count < limit) {
                printf(C_BOLD C_GREEN "* %s" C_RESET, name);
                if (version[0])
                    printf(" " C_CYAN "%s" C_RESET, version);
                printf("\n");
                if (description[0])
                    printf("  %s\n", description);
                printf(C_DIM "  nix install %s" C_RESET "\n\n", name);
                count++;
            }
        }
    }

    fclose(f);

    if (total_matches == 0) {
        printf("No packages found matching '%s'\n", query);
        return 0;
    }

    if (total_matches > limit) {
        printf(C_DIM "... and %d more (use --limit to show more)" C_RESET "\n",
               total_matches - limit);
    }

    return 0;
}
