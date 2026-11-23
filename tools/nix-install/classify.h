/*
 * Read-only "what does this package give the user?" probe for the Gallery.
 *
 *   desktop       the package ships a launcher entry (Terminal=false)
 *   gui           no entry; the program links a windowing/graphics library
 *   probably-gui  no entry; a managed runtime's UI stack is present
 *                 (.NET Avalonia/SkiaSharp, JavaFX/AWT, Electron)
 *   cli           runs in a terminal
 *   unsupported   not an x86-64 program (Brook loads x86-64 ELF only)
 *   none          nothing to run (library or data package)
 *
 * Nix wrappers are followed to the real program: makeWrapper/wrapProgram
 * shell scripts (`exec ... "/nix/store/.../.foo-wrapped"`), makeCWrapper
 * binaries and `.foo-wrapped` siblings. Every read is bounded; nothing is
 * executed and nothing is written.
 *
 * Included by nix-install.c after the index, manifest and desktop helpers.
 */
#ifndef BROOK_NIX_CLASSIFY_H
#define BROOK_NIX_CLASSIFY_H

#define CLASSIFY_MAX_BINS   96
#define CLASSIFY_MAX_DEPTH  5
#define CLASSIFY_SCRIPT_MAX (64 * 1024)
#define CLASSIFY_SCAN_MAX   (2u * 1024 * 1024)
#define CLASSIFY_MAX_NEEDED 256

enum program_kind { KIND_NONE, KIND_CLI, KIND_PROBABLY_GUI, KIND_GUI, KIND_UNSUPPORTED };

struct program_probe {
    enum program_kind kind;
    char reason[256];
};

static const char *const gui_library_prefixes[] = {
    "libwayland-client.so", "libX11.so", "libxcb.so", "libgtk-3.so", "libgtk-4.so",
    "libgtk-x11-2.0.so", "libgdk-3.so", "libgdk-x11-2.0.so", "libQt5Gui.so", "libQt6Gui.so",
    "libQtGui.so", "libSDL2-2.0.so", "libSDL3.so", "libSDL-1.2.so", "libglfw.so", "libEGL.so",
    "libGL.so", "libGLX.so", "libOpenGL.so", "libvulkan.so", "libwx_gtk", "libfltk.so",
    "libsfml-window.so", "libraylib.so", "liballegro.so", "libelementary.so", "libtk8",
};

/* Map a guest absolute path into this build's store/profile roots (tests use
 * private trees; on Brook both are identities). */
static void classify_map_path(const char *guest, char *out, size_t capacity) {
    static const char store[] = "/nix/store/", profile_bin[] = "/nix/profile/bin/";
    if (!strncmp(guest, store, sizeof(store) - 1))
        snprintf(out, capacity, "%s/%s", STORE_DIR, guest + sizeof(store) - 1);
    else if (!strncmp(guest, profile_bin, sizeof(profile_bin) - 1))
        snprintf(out, capacity, "%s/%s", PROFILE_BIN, guest + sizeof(profile_bin) - 1);
    else
        snprintf(out, capacity, "%s", guest);
}

static int classify_read(int fd, uint64_t offset, void *buffer, size_t length) {
    return pread(fd, buffer, length, (off_t)offset) == (ssize_t)length ? 0 : -1;
}

/* DT_NEEDED scan of an ELF64 image. Returns 1 if a GUI library is needed,
 * 0 if not, -2 for a non-x86-64 ELF, -1 for a malformed/unreadable one. */
static int classify_elf(int fd, uint64_t size, char *found, size_t found_capacity) {
    Elf64_Ehdr header;
    unsigned char ident[EI_NIDENT];
    if (classify_read(fd, 0, ident, sizeof(ident)) < 0) return -1;
    if (ident[EI_CLASS] != ELFCLASS64) return -2;
    if (classify_read(fd, 0, &header, sizeof(header)) < 0) return -1;
    if (header.e_machine != EM_X86_64) return -2;
    if (header.e_phentsize != sizeof(Elf64_Phdr) || !header.e_phnum || header.e_phnum > 128 ||
        header.e_phoff > size || header.e_phnum * sizeof(Elf64_Phdr) > size - header.e_phoff)
        return -1;
    Elf64_Phdr phdrs[128];
    if (classify_read(fd, header.e_phoff, phdrs, header.e_phnum * sizeof(Elf64_Phdr)) < 0) return -1;
    const Elf64_Phdr *dynamic = NULL;
    for (int i = 0; i < header.e_phnum; ++i)
        if (phdrs[i].p_type == PT_DYNAMIC) dynamic = &phdrs[i];
    if (!dynamic) return 0; /* static: no shared GUI library */
    if (dynamic->p_offset > size || dynamic->p_filesz > size - dynamic->p_offset ||
        dynamic->p_filesz > 64 * 1024)
        return -1;
    Elf64_Dyn entries[4096];
    size_t count = dynamic->p_filesz / sizeof(Elf64_Dyn);
    if (classify_read(fd, dynamic->p_offset, entries, count * sizeof(Elf64_Dyn)) < 0) return -1;
    uint64_t strtab = 0, strsz = 0, needed[CLASSIFY_MAX_NEEDED];
    size_t needed_count = 0;
    for (size_t i = 0; i < count && entries[i].d_tag != DT_NULL; ++i) {
        if (entries[i].d_tag == DT_STRTAB) strtab = entries[i].d_un.d_ptr;
        else if (entries[i].d_tag == DT_STRSZ) strsz = entries[i].d_un.d_val;
        else if (entries[i].d_tag == DT_NEEDED && needed_count < CLASSIFY_MAX_NEEDED)
            needed[needed_count++] = entries[i].d_un.d_val;
    }
    if (!strtab || !strsz || strsz > 4u * 1024 * 1024) return needed_count ? -1 : 0;
    /* DT_STRTAB is a virtual address: translate through the PT_LOAD maps. */
    uint64_t strtab_offset = UINT64_MAX;
    for (int i = 0; i < header.e_phnum; ++i) {
        const Elf64_Phdr *p = &phdrs[i];
        if (p->p_type == PT_LOAD && strtab >= p->p_vaddr && strtab - p->p_vaddr < p->p_filesz) {
            strtab_offset = p->p_offset + (strtab - p->p_vaddr);
            break;
        }
    }
    if (strtab_offset == UINT64_MAX || strtab_offset > size || strsz > size - strtab_offset) return -1;
    char *strings = malloc(strsz + 1);
    if (!strings) return -1;
    if (classify_read(fd, strtab_offset, strings, strsz) < 0) { free(strings); return -1; }
    strings[strsz] = 0;
    int gui = 0;
    for (size_t i = 0; i < needed_count && !gui; ++i) {
        if (needed[i] >= strsz) continue;
        const char *library = strings + needed[i];
        for (size_t j = 0; j < sizeof(gui_library_prefixes) / sizeof(gui_library_prefixes[0]); ++j) {
            size_t n = strlen(gui_library_prefixes[j]);
            if (!strncmp(library, gui_library_prefixes[j], n)) {
                snprintf(found, found_capacity, "%s", library);
                gui = 1;
                break;
            }
        }
    }
    free(strings);
    return gui;
}

/* First absolute path argument of the last `exec` line of a shell wrapper. */
static int classify_script_target(const char *text, char *out, size_t capacity) {
    int found = 0;
    for (const char *line = text; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t length = end ? (size_t)(end - line) : strlen(line);
        const char *p = line;
        while (p < line + length && (*p == ' ' || *p == '\t')) ++p;
        if ((size_t)(line + length - p) > 5 && !strncmp(p, "exec ", 5)) {
            /* Skip `-a "$0"` / `-a name` before the program. */
            for (p += 5; p < line + length;) {
                while (p < line + length && *p == ' ') ++p;
                if (p + 1 < line + length && p[0] == '-' && p[1] == 'a') {
                    p += 2;
                    while (p < line + length && *p == ' ') ++p;
                    char quote = (*p == '"' || *p == '\'') ? *p++ : 0;
                    while (p < line + length && (quote ? *p != quote : *p != ' ')) ++p;
                    if (quote && p < line + length) ++p;
                    continue;
                }
                break;
            }
            char quote = (p < line + length && (*p == '"' || *p == '\'')) ? *p++ : 0;
            if (p < line + length && *p == '/') {
                size_t n = 0;
                while (p + n < line + length && (quote ? p[n] != quote : (p[n] != ' ' && p[n] != '\t')) &&
                       n + 1 < capacity)
                    ++n;
                if (n && !memchr(p, '$', n)) {
                    memcpy(out, p, n);
                    out[n] = 0;
                    found = 1;
                }
            }
        }
        line = end ? end + 1 : NULL;
    }
    return found;
}

/* makeCWrapper binaries embed "makeCWrapper '<target>'" in their source note. */
static int classify_cwrapper_target(int fd, uint64_t size, char *out, size_t capacity) {
    if (size > CLASSIFY_SCAN_MAX) return 0;
    char *bytes = malloc(size + 1);
    if (!bytes) return 0;
    int found = 0;
    if (classify_read(fd, 0, bytes, size) == 0) {
        bytes[size] = 0;
        static const char marker[] = "makeCWrapper '";
        for (uint64_t i = 0; i + sizeof(marker) < size; ++i) {
            if (memcmp(bytes + i, marker, sizeof(marker) - 1)) continue;
            const char *start = bytes + i + sizeof(marker) - 1;
            const char *end = memchr(start, '\'', size - (uint64_t)(start - bytes));
            if (end && *start == '/' && (size_t)(end - start) < capacity) {
                memcpy(out, start, (size_t)(end - start));
                out[end - start] = 0;
                found = 1;
            }
            break;
        }
    }
    free(bytes);
    return found;
}

static int classify_contains(const char *text, const char *const *markers, size_t count,
                             char *found, size_t capacity) {
    for (size_t i = 0; i < count; ++i)
        if (strstr(text, markers[i])) { snprintf(found, capacity, "%s", markers[i]); return 1; }
    return 0;
}

/* Managed UI runtimes beside the real program (one directory, bounded). */
static int classify_runtime_markers(const char *directory, char *found, size_t capacity) {
    static const char *const files[] = {
        "Avalonia.X11.dll", "Avalonia.Desktop.dll", "libSkiaSharp.so", "libHarfBuzzSharp.so",
        "resources/app.asar", "chrome_100_percent.pak", "libjfxwebkit.so", "libglass.so",
        "javafx.graphics.jar",
    };
    char path[2048];
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        snprintf(path, sizeof(path), "%s/%s", directory, files[i]);
        if (access(path, F_OK) == 0) { snprintf(found, capacity, "%s", files[i]); return 1; }
    }
    return 0;
}

static void classify_program(const char *path, struct program_probe *probe) {
    static const char *const script_markers[] = {
        "javafx", "openjfx", "java.awt", "electron", "gi.require_version('Gtk'",
        "gi.require_version(\"Gtk\"", "from gi.repository import Gtk", "PyQt5", "PyQt6", "PySide2",
        "PySide6", "import tkinter", "from tkinter", "import pygame", "import wx",
    };
    char current[2048], next[2048], found[256];
    snprintf(current, sizeof(current), "%s", path);
    probe->kind = KIND_CLI;
    probe->reason[0] = 0;
    int marked = 0;
    for (int depth = 0; depth < CLASSIFY_MAX_DEPTH; ++depth) {
        int fd = open(current, O_RDONLY | O_CLOEXEC);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
            if (fd >= 0) close(fd);
            if (!depth) { probe->kind = KIND_NONE; snprintf(probe->reason, sizeof(probe->reason), "not a regular file"); }
            return;
        }
        unsigned char magic[4] = {0};
        ssize_t got = pread(fd, magic, sizeof(magic), 0);
        char directory[2048];
        snprintf(directory, sizeof(directory), "%s", current);
        char *slash = strrchr(directory, '/');
        if (slash) *slash = 0;
        if (got == 4 && !memcmp(magic, ELFMAG, SELFMAG)) {
            int gui = classify_elf(fd, (uint64_t)st.st_size, found, sizeof(found));
            if (gui == -2) {
                close(fd);
                probe->kind = KIND_UNSUPPORTED;
                snprintf(probe->reason, sizeof(probe->reason), "not an x86-64 program (Brook runs 64-bit x86 programs only)");
                return;
            }
            if (gui == 1) {
                close(fd);
                probe->kind = KIND_GUI;
                snprintf(probe->reason, sizeof(probe->reason), "links %s", found);
                return;
            }
            /* A C wrapper's own DT_NEEDED is just libc: follow its target. */
            int followed = classify_cwrapper_target(fd, (uint64_t)st.st_size, next, sizeof(next));
            close(fd);
            if (followed) { classify_map_path(next, current, sizeof(current)); continue; }
            const char *base = strrchr(current, '/');
            base = base ? base + 1 : current;
            snprintf(next, sizeof(next), "%s/.%s-wrapped", directory, base);
            if (base[0] != '.' && access(next, F_OK) == 0) { snprintf(current, sizeof(current), "%s", next); continue; }
            if (!marked && classify_runtime_markers(directory, found, sizeof(found))) {
                probe->kind = KIND_PROBABLY_GUI;
                snprintf(probe->reason, sizeof(probe->reason), "desktop UI runtime present (%s)", found);
            }
            return;
        }
        if (got >= 2 && magic[0] == '#' && magic[1] == '!') {
            char *text = malloc(CLASSIFY_SCRIPT_MAX + 1);
            if (!text) { close(fd); return; }
            ssize_t n = pread(fd, text, CLASSIFY_SCRIPT_MAX, 0);
            close(fd);
            text[n > 0 ? n : 0] = 0;
            if (!marked && classify_contains(text, script_markers, sizeof(script_markers) / sizeof(script_markers[0]),
                                             found, sizeof(found))) {
                marked = 1;
                probe->kind = KIND_PROBABLY_GUI;
                snprintf(probe->reason, sizeof(probe->reason), "script uses %s", found);
            }
            int followed = classify_script_target(text, next, sizeof(next));
            if (!followed) {
                /* An interpreted program (python, perl, ...): its markers decided. */
                free(text);
                return;
            }
            free(text);
            classify_map_path(next, current, sizeof(current));
            continue;
        }
        close(fd);
        return;
    }
}

static const char *program_kind_name(enum program_kind kind) {
    switch (kind) {
    case KIND_GUI: return "gui";
    case KIND_PROBABLY_GUI: return "probably-gui";
    case KIND_UNSUPPORTED: return "unsupported";
    case KIND_NONE: return "none";
    default: return "cli";
    }
}

static int classify_executable_entry(const char *bin_dir, const char *name) {
    if (name[0] == '.') return 0;
    char path[2048];
    struct stat st;
    snprintf(path, sizeof(path), "%s/%s", bin_dir, name);
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111);
}

static int classify_name_compare(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* First Name= of a launchable (non-terminal) desktop entry, if any. */
static int classify_package_desktop(const char *store_name, char *name, size_t capacity) {
    DIR *directory = NULL;
    if (open_package_directory(store_name, "share/applications", &directory) <= 0) return 0;
    struct dirent *entry;
    int found = 0;
    while (!found && (entry = readdir(directory))) {
        size_t length = strlen(entry->d_name);
        if (length < 9 || strcmp(entry->d_name + length - 8, ".desktop")) continue;
        char path[2048], exec_cmd[512], icon[256], categories[256], comment[64];
        snprintf(path, sizeof(path), STORE_DIR "/%s/share/applications/%s", store_name, entry->d_name);
        if (parse_desktop_entry(path, name, capacity, exec_cmd, sizeof(exec_cmd), icon, sizeof(icon),
                                categories, sizeof(categories), comment, sizeof(comment)) == 0)
            found = 1;
    }
    closedir(directory);
    return found;
}

static void added_launcher_path(const char *package, char *out, size_t capacity) {
    snprintf(out, capacity, "%s/brook-added-%s.desktop", PROFILE_APPS, package);
}

struct package_class {
    enum program_kind kind;
    int desktop;              /* package ships a launcher entry */
    int added;                /* user added a launcher entry from the Gallery */
    char desktop_name[256];
    char program[256];        /* basename in PROFILE_BIN */
    char reason[256];
    char description[1024];
};

/* Returns 0 with *out filled for an installed package, -1 if not installed. */
static int classify_package(const char *package, struct package_class *out) {
    memset(out, 0, sizeof(*out));
    char store_name[512] = {0};
    if (!find_installed(package, store_name, sizeof(store_name)) || !store_name[0] || strchr(store_name, '/'))
        return -1;
    struct pkg_info info;
    memset(&info, 0, sizeof(info));
    int indexed = lookup_package(package, &info) == 0;
    if (indexed) snprintf(out->description, sizeof(out->description), "%s", info.description);
    out->desktop = classify_package_desktop(store_name, out->desktop_name, sizeof(out->desktop_name));
    char added[1024];
    struct stat st;
    added_launcher_path(package, added, sizeof(added));
    out->added = stat(added, &st) == 0;

    char bin_dir[1024];
    snprintf(bin_dir, sizeof(bin_dir), STORE_DIR "/%s/bin", store_name);
    char *names[CLASSIFY_MAX_BINS];
    int count = 0;
    DIR *directory = opendir(bin_dir);
    if (directory) {
        struct dirent *entry;
        while (count < CLASSIFY_MAX_BINS && (entry = readdir(directory)))
            if (classify_executable_entry(bin_dir, entry->d_name)) names[count++] = strdup(entry->d_name);
        closedir(directory);
    }
    int valid = 0;
    for (int i = 0; i < count; ++i) if (names[i]) names[valid++] = names[i];
    count = valid;
    qsort(names, (size_t)count, sizeof(names[0]), classify_name_compare);
    out->kind = KIND_NONE;
    if (!count) {
        snprintf(out->reason, sizeof(out->reason), "no programs (library or data package)");
        return 0;
    }
    /* meta.mainProgram first; then the package-named program; then the rest. */
    int order[CLASSIFY_MAX_BINS], ordered = 0, preferred = -1;
    for (int i = 0; i < count; ++i)
        if (indexed && info.main_program[0] && !strcmp(names[i], info.main_program)) preferred = i;
    if (preferred < 0)
        for (int i = 0; i < count; ++i)
            if (!strcasecmp(names[i], package)) { preferred = i; break; }
    if (preferred >= 0) order[ordered++] = preferred;
    for (int i = 0; i < count; ++i) if (i != preferred) order[ordered++] = i;
    int chosen = -1;
    struct program_probe best = {KIND_NONE, ""};
    int bounded = ordered < 24 ? ordered : 24; /* bound the deep inspection */
    for (int i = 0; i < bounded; ++i) {
        char path[2048];
        struct program_probe probe;
        snprintf(path, sizeof(path), "%s/%s", bin_dir, names[order[i]]);
        classify_program(path, &probe);
        int better = chosen < 0 ||
            (probe.kind == KIND_GUI && best.kind != KIND_GUI) ||
            (probe.kind == KIND_PROBABLY_GUI && best.kind != KIND_GUI && best.kind != KIND_PROBABLY_GUI) ||
            (probe.kind == KIND_CLI && (best.kind == KIND_NONE || best.kind == KIND_UNSUPPORTED));
        if (better) { chosen = order[i]; best = probe; }
        /* The preferred program decides unless it is plainly not runnable. */
        if (i == 0 && preferred >= 0 && probe.kind != KIND_NONE && probe.kind != KIND_UNSUPPORTED &&
            probe.kind != KIND_CLI) break;
        if (best.kind == KIND_GUI) break;
    }
    /* With no GUI program, keep the preferred command for the terminal hint. */
    if (best.kind == KIND_CLI && preferred >= 0) chosen = preferred;
    out->kind = best.kind;
    snprintf(out->program, sizeof(out->program), "%s", names[chosen]);
    snprintf(out->reason, sizeof(out->reason), "%s", best.reason);
    for (int i = 0; i < count; ++i) free(names[i]);
    return 0;
}

static void print_field(const char *key, const char *value) {
    printf("%s=", key);
    for (const char *p = value; *p; ++p) putchar(*p == '\n' || *p == '\t' ? ' ' : *p);
    putchar('\n');
}

/* nix classify <pkg>: key=value lines for the Gallery. Read-only, no lock. */
static int cmd_classify(const char *package) {
    struct package_class result;
    if (classify_package(package, &result) < 0) {
        fprintf(stderr, "'%s' is not installed.\n", package);
        return 1;
    }
    print_field("package", package);
    print_field("kind", program_kind_name(result.kind));
    print_field("launcher", result.desktop ? "package" : result.added ? "added" : "none");
    if (result.desktop) print_field("launcher_name", result.desktop_name);
    if (result.program[0]) {
        char command[1024];
        snprintf(command, sizeof(command), "/nix/profile/bin/%s", result.program);
        print_field("program", result.program);
        print_field("command", command);
    }
    if (result.reason[0]) print_field("reason", result.reason);
    return 0;
}

/* Desktop entry text values may not carry control characters. */
static void desktop_value(char *out, size_t capacity, const char *value) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p && n + 1 < capacity; ++p)
        out[n++] = (*p < ' ' || *p == 127) ? ' ' : (char)*p;
    out[n] = 0;
}

/* nix launcher-add <pkg>: the user's explicit choice in the Gallery to pin a
 * GUI program that ships no desktop entry. Entries are profile files named
 * brook-added-<pkg>.desktop; `nix remove` deletes them with the package. */
static int cmd_launcher_add(const char *package) {
    if (acquire_profile_lock() < 0) return 1;
    struct package_class result;
    if (classify_package(package, &result) < 0) {
        fprintf(stderr, "'%s' is not installed.\n", package);
        return 1;
    }
    if (result.desktop) {
        printf("'%s' already has a launcher entry (%s).\n", package, result.desktop_name);
        return 0;
    }
    if (result.kind != KIND_GUI && result.kind != KIND_PROBABLY_GUI) {
        fprintf(stderr, "'%s' was not detected as a desktop program; run %s from Terminal.\n",
                package, result.program[0] ? result.program : package);
        return 1;
    }
    if (strchr(result.program, ' ') || strchr(result.program, '"') || strchr(result.program, '%')) {
        fprintf(stderr, "Program name '%s' cannot be used in a launcher entry.\n", result.program);
        return 1;
    }
    if (mkdir_p(PROFILE_APPS) < 0 && errno != EEXIST) {
        fprintf(stderr, "Error: cannot create %s: %s\n", PROFILE_APPS, strerror(errno));
        return 1;
    }
    char path[1024], staging[1100], name[256], comment[256];
    added_launcher_path(package, path, sizeof(path));
    snprintf(staging, sizeof(staging), "%s.new", path);
    desktop_value(name, sizeof(name), result.program);
    desktop_value(comment, sizeof(comment), result.description);
    FILE *f = fopen(staging, "w");
    if (!f) { fprintf(stderr, "Error: cannot write %s: %s\n", staging, strerror(errno)); return 1; }
    fprintf(f, "[Desktop Entry]\nType=Application\nName=%s\nExec=%s/%s\n", name, PROFILE_BIN, result.program);
    if (comment[0]) fprintf(f, "Comment=%s\n", comment);
    fprintf(f, "X-Brook-Added-Package=%s\n", package);
    int ok = fflush(f) == 0 && fsync(fileno(f)) == 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(staging, path) != 0) {
        fprintf(stderr, "Error: cannot publish %s: %s\n", path, strerror(errno));
        unlink(staging);
        return 1;
    }
    refresh_desktop_entries();
    printf("Added %s to the launcher.\n", name);
    return 0;
}

static int remove_added_launcher(const char *package) {
    char path[1024];
    added_launcher_path(package, path, sizeof(path));
    if (unlink(path) == 0) return 1;
    return errno == ENOENT ? 0 : -1;
}

#endif
