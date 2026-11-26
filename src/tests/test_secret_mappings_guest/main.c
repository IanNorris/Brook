#define _GNU_SOURCE
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define PROBE_PAGE_SIZE 4096
#define MARKER_SIZE 96
#define CORE_NOTE_SECRET_RANGES 0x42524b53u

static const char directory[] = "/data/home/.local/share/brook/cores";

static void check(bool ok, const char *step)
{
    if (!ok) {
        dprintf(2, "SECRET_CORE_FAIL %s errno=%d\n", step, errno);
        _exit(1);
    }
}

static unsigned char marker_byte(unsigned seed, unsigned i)
{
    return (unsigned char)(seed * 31 + i * 17 + (i * i >> 5));
}

static void fill_marker(volatile unsigned char *data, unsigned seed)
{
    for (unsigned i = 0; i < MARKER_SIZE; ++i)
        data[i] = marker_byte(seed, i);
}

static bool contains_marker(const void *data, size_t size, unsigned seed)
{
    unsigned char marker[MARKER_SIZE];
    fill_marker(marker, seed);
    return memmem(data, size, marker, sizeof marker) != NULL;
}

static void desktop_credentials(void)
{
    check(setgid(1000) == 0 && setuid(1000) == 0, "desktop credentials");
}

static volatile unsigned char *map_anon(int sharing)
{
    void *map = mmap(NULL, PROBE_PAGE_SIZE, PROT_READ | PROT_WRITE, sharing | MAP_ANONYMOUS, -1, 0);
    check(map != MAP_FAILED, "anonymous mmap");
    return map;
}

static void populate(bool full)
{
    unsigned char *ordinary = malloc(MARKER_SIZE);
    check(ordinary != NULL, "heap");
    fill_marker(ordinary, 1);
    fill_marker(map_anon(MAP_PRIVATE), 4);
    if (full) fill_marker(map_anon(MAP_SHARED), 5);
    volatile unsigned char *private = map_anon(MAP_PRIVATE);
    check(madvise((void *)private, PROBE_PAGE_SIZE, MADV_DONTDUMP) == 0, "DONTDUMP");
    fill_marker(private, 2);
    int fd = (int)syscall(447, 0); /* memfd_secret */
    check(fd >= 0 && ftruncate(fd, 100) == 0, "memfd_secret short truncate");
    check(ftruncate(fd, 100) == -1 && errno == EINVAL, "repeat truncate");
    void *secret = mmap(NULL, 100, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    check(secret != MAP_FAILED, "short secret mmap");
    check(madvise(secret, 100, MADV_DODUMP) == -1 && errno == EINVAL, "immutable exclusion");
    fill_marker(secret, 3);
    pid_t witness = fork();
    check(witness >= 0, "secret fork");
    if (!witness) {
        check(mprotect(secret, 100, PROT_READ) == -1 && errno == ENOMEM, "secret was inherited");
        check(mmap(NULL, 100, PROT_READ, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EPERM,
              "inherited secret fd");
        _exit(0);
    }
    int status;
    check(waitpid(witness, &status, 0) == witness && WIFEXITED(status) && !WEXITSTATUS(status),
          "secret child isolation");
    close(fd);
}

static void check_memfd_eof(void)
{
    pid_t pid = fork();
    check(pid >= 0, "EOF fork");
    if (!pid) {
        int fd = (int)syscall(447, 0);
        check(prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0, "suppress EOF core");
        check(fd >= 0 && ftruncate(fd, 100) == 0, "EOF memfd_secret");
        volatile unsigned char *mapped = mmap(NULL, 2 * PROBE_PAGE_SIZE, PROT_READ, MAP_SHARED, fd, 0);
        check(mapped != MAP_FAILED && mapped[0] == 0, "mapping extends past EOF");
        _exit(mapped[PROBE_PAGE_SIZE]);
    }
    int status;
    check(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGBUS,
          "secret EOF is SIGBUS");
}

static pid_t run_crash(bool suppress)
{
    pid_t pid = fork();
    check(pid >= 0, "fork");
    if (!pid) {
        check(prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 1, "default dumpable");
        if (suppress) {
            check(prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0, "set dumpable 0");
            check(prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 0, "get dumpable 0");
        } else populate(false);
        raise(SIGSEGV);
        _exit(127);
    }
    int status;
    check(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV,
          "fatal signal");
    return pid;
}

static int find_core(pid_t pid)
{
    DIR *dir = opendir(directory);
    if (!dir) {
        check(errno == ENOENT, "open core directory");
        return -1;
    }
    char component[32];
    snprintf(component, sizeof component, ".%d.", (int)pid);
    int fd = -1;
    for (struct dirent *entry; (entry = readdir(dir));) {
        if (strstr(entry->d_name, component) && strstr(entry->d_name, ".core")) {
            fd = openat(dirfd(dir), entry->d_name, O_RDONLY);
            check(fd >= 0, "open published core");
            break;
        }
    }
    closedir(dir);
    return fd;
}

static bool has_secret_note(const unsigned char *data, size_t size)
{
    Elf64_Ehdr hdr;
    if (size < sizeof hdr) return false;
    memcpy(&hdr, data, sizeof hdr);
    if (memcmp(hdr.e_ident, ELFMAG, SELFMAG) || hdr.e_type != ET_CORE ||
        hdr.e_phentsize != sizeof(Elf64_Phdr) || hdr.e_phoff > size ||
        hdr.e_phnum > (size - hdr.e_phoff) / sizeof(Elf64_Phdr)) return false;
    for (unsigned i = 0; i < hdr.e_phnum; ++i) {
        Elf64_Phdr ph;
        memcpy(&ph, data + hdr.e_phoff + i * sizeof ph, sizeof ph);
        if (ph.p_type != PT_NOTE || ph.p_offset > size || ph.p_filesz > size - ph.p_offset) continue;
        size_t at = ph.p_offset, end = at + ph.p_filesz;
        while (end - at >= sizeof(Elf64_Nhdr)) {
            Elf64_Nhdr note;
            memcpy(&note, data + at, sizeof note);
            at += sizeof note;
            size_t names = ((size_t)note.n_namesz + 3) & ~(size_t)3;
            size_t bytes = ((size_t)note.n_descsz + 3) & ~(size_t)3;
            if (names > end - at || bytes > end - at - names) break;
            if (note.n_type == CORE_NOTE_SECRET_RANGES && note.n_namesz == 6 &&
                !memcmp(data + at, "BROOK", 6) && note.n_descsz == 7 * sizeof(uint64_t)) {
                uint64_t description[7];
                memcpy(description, data + at + names, sizeof description);
                return description[0] == 2 && description[1] == 2 && description[2] == 2 &&
                       description[4] - description[3] == PROBE_PAGE_SIZE &&
                       description[6] - description[5] == PROBE_PAGE_SIZE;
            }
            at += names + bytes;
        }
    }
    return false;
}

static bool core_complete(int fd)
{
    struct stat stat;
    Elf64_Ehdr header;
    if (fstat(fd, &stat) || pread(fd, &header, sizeof header, 0) != sizeof header ||
        memcmp(header.e_ident, ELFMAG, SELFMAG) || !header.e_phnum ||
        header.e_phnum > 1024 || header.e_phentsize != sizeof(Elf64_Phdr)) return false;
    for (unsigned i = 0; i < header.e_phnum; ++i) {
        Elf64_Phdr ph;
        off_t offset = (off_t)header.e_phoff + i * sizeof ph;
        if (pread(fd, &ph, sizeof ph, offset) != sizeof ph || ph.p_offset > (uint64_t)stat.st_size ||
            ph.p_filesz > (uint64_t)stat.st_size - ph.p_offset) return false;
    }
    return true;
}

static void verify_core(int fd, bool full)
{
    struct stat stat;
    check(fstat(fd, &stat) == 0 && stat.st_size > 0 && stat.st_size <= 64 * 1024 * 1024, "core size");
    size_t size = (size_t)stat.st_size;
    unsigned char *core = malloc(size);
    check(core != NULL, "core buffer");
    for (size_t read_bytes = 0; read_bytes < size;) {
        ssize_t count = read(fd, core + read_bytes, size - read_bytes);
        check(count > 0, "read core bytes");
        read_bytes += (size_t)count;
    }
    check(contains_marker(core, size, 1), "ordinary heap marker present");
    check(contains_marker(core, size, 4), "ordinary private mmap marker present");
    if (full) check(contains_marker(core, size, 5), "ordinary shared mmap marker present in full core");
    check(!contains_marker(core, size, 2), "DONTDUMP marker absent");
    check(!contains_marker(core, size, 3), "memfd_secret marker absent");
    check(has_secret_note(core, size), "secret range ELF metadata");
    free(core);
    close(fd);
}

static void verify_autodump(void)
{
    desktop_credentials();
    check_memfd_eof();
    pid_t suppressed = run_crash(true);
    pid_t expected = run_crash(false);
    int fd = -1;
    for (unsigned i = 0; i < 160 && fd < 0; ++i) {
        usleep(50000);
        fd = find_core(expected);
    }
    check(fd >= 0, "core published");
    check(find_core(suppressed) < 0, "dumpable 0 core absent");
    verify_core(fd, false);
}

int main(int argc, char **argv)
{
    check(mkdir("/data/home", 0755) == 0 || errno == EEXIST, "make home");
    check(chown("/data/home", 1000, 1000) == 0, "chown home");
    if (argc < 2) {
        verify_autodump();
        dprintf(1, "SECRET_CORE_PASS autodump controls and exclusions, dumpable=0, SIGBUS\n");
        return 0;
    }
    check(!strcmp(argv[1], "--full"), "probe mode");
    pid_t child = fork();
    check(child >= 0, "full dump fork");
    if (!child) {
        desktop_credentials();
        populate(true);
        dprintf(1, "SECRET_CORE_FULL_READY %d\n", (int)getpid());
        for (;;) pause();
    }
    int fd = -1;
    for (unsigned i = 0; i < 400; ++i) {
        if (fd < 0) fd = open("/data/full.core", O_RDONLY);
        if (fd >= 0 && core_complete(fd)) break;
        usleep(50000);
    }
    check(fd >= 0 && core_complete(fd), "full core completed");
    verify_core(fd, true);
    check(kill(child, SIGTERM) == 0 && waitpid(child, NULL, 0) == child, "reap full dump fixture");
    dprintf(1, "SECRET_CORE_PASS full dump includes private and shared controls, both secrets excluded\n");
    return 0;
}
