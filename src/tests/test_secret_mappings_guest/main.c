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
#define MARKER_SIZE 192
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

static pid_t run_crash(bool suppress)
{
    pid_t pid = fork();
    check(pid >= 0, "fork");
    if (!pid) {
        check(prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 1, "default dumpable");
        if (suppress) {
            check(prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) == 0, "set dumpable 0");
            check(prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 0, "get dumpable 0");
        } else {
            unsigned char *ordinary = malloc(MARKER_SIZE);
            check(ordinary != NULL, "heap");
            fill_marker(ordinary, 1);
            void *private = mmap(NULL, PROBE_PAGE_SIZE, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            check(private != MAP_FAILED, "anonymous mmap");
            check(madvise(private, PROBE_PAGE_SIZE, MADV_DONTDUMP) == 0, "DONTDUMP");
            fill_marker(private, 2);
            int fd = (int)syscall(447, 0); /* memfd_secret */
            check(fd >= 0 && ftruncate(fd, PROBE_PAGE_SIZE) == 0, "memfd_secret");
            void *secret = mmap(NULL, PROBE_PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
            check(secret != MAP_FAILED, "secret mmap");
            check(madvise(secret, PROBE_PAGE_SIZE, MADV_DODUMP) == -1 && errno == EINVAL, "immutable exclusion");
            fill_marker(secret, 3);
            pid_t witness = fork();
            check(witness >= 0, "secret fork");
            if (!witness) {
                check(mprotect(secret, PROBE_PAGE_SIZE, PROT_READ) == -1 && errno == ENOMEM, "secret was inherited");
                check(mmap(NULL, PROBE_PAGE_SIZE, PROT_READ, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EPERM,
                      "inherited secret fd");
                _exit(0);
            }
            int status;
            check(waitpid(witness, &status, 0) == witness && WIFEXITED(status) && !WEXITSTATUS(status),
                  "secret child isolation");
            close(fd);
        }
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

int main(void)
{
    check(mkdir("/data/home", 0755) == 0 || errno == EEXIST, "make home");
    check(chown("/data/home", 1000, 1000) == 0, "chown home");
    check(setgid(1000) == 0 && setuid(1000) == 0, "desktop credentials");
    pid_t suppressed = run_crash(true);
    pid_t expected = run_crash(false);
    int fd = -1;
    for (unsigned i = 0; i < 160 && fd < 0; ++i) {
        usleep(50000);
        fd = find_core(expected);
    }
    check(fd >= 0, "core published");
    check(find_core(suppressed) < 0, "dumpable 0 core absent");

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
    check(!contains_marker(core, size, 2), "DONTDUMP marker absent");
    check(!contains_marker(core, size, 3), "memfd_secret marker absent");
    check(has_secret_note(core, size), "secret range ELF metadata");
    dprintf(1, "SECRET_CORE_PASS ordinary marker present, two secrets absent, ranges recorded, dumpable=0 suppressed\n");
    return 0;
}
