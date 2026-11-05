// Export the newest intact panic packet from a fixed journal file. This runs
// after reboot; it never writes to the journal or changes its FAT allocation.
#include "../src/kernel/src/panic_journal.h"
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
struct Reader {
    int fd;
    bool read(uint32_t sector, uint8_t* out) {
        if (sector >= brook::PanicJournalSectors) return false;
        size_t done = 0;
        while (done < brook::PanicJournalSector) {
            const ssize_t n = pread(fd, out + done, brook::PanicJournalSector - done,
                                    off_t(sector) * brook::PanicJournalSector + done);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return false;
            done += size_t(n);
        }
        return true;
    }
};
uint8_t packet[brook::PanicJournalPayloadMax];
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: PANICLOG journal-file new-output-file\n");
        return 2;
    }
    const int input = open(argv[1], O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st{};
    if (input < 0 || fstat(input, &st) || !S_ISREG(st.st_mode) ||
        st.st_size != off_t(brook::PanicJournalSectors) * brook::PanicJournalSector) {
        if (input >= 0) close(input);
        std::fprintf(stderr, "PANICLOG: journal unavailable or wrong size\n");
        return 1;
    }
    Reader reader{input};brook::PanicJournalRecord record{};
    const auto result = brook::PanicJournalReadLatest(reader, packet, sizeof(packet), record);
    close(input);
    if (result != brook::PanicJournalResult::Ok) {
        std::fprintf(stderr, "PANICLOG: no usable journal record (status=%d)\n", int(result));
        return 1;
    }
    const int output = open(argv[2], O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (output < 0) { std::perror("PANICLOG: create output"); return 1; }
    size_t done = 0;
    while (done < record.bytes) {
        const ssize_t n = write(output, packet + done, record.bytes - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { std::perror("PANICLOG: write output"); close(output); return 1; }
        done += size_t(n);
    }
    if (close(output)) { std::perror("PANICLOG: close output"); return 1; }
    std::printf("PANICLOG: sequence=%llu bytes=%u crc32=%08x output=%s\n",
                static_cast<unsigned long long>(record.sequence), record.bytes, record.checksum, argv[2]);
    return 0;
}
