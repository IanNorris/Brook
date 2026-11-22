// Boot-stick file sets (STICK_SETS=1): root set, PREV/ set and a one-shot
// trial marker. See src/shared/update/stick_set.h for the record format.
#include "stick_sets.h"
#include "fs.h"
#include "memory.h"
#include "console.h"
#include "recovery_device_path.h"
#include "../../shared/update/stick_set.h"
#include "../../shared/update/executable.h"

#include "Protocol/LoadedImage.h"
#include "Protocol/SimpleFileSystem.h"
#include "Protocol/DevicePath.h"
#include "Guid/FileInfo.h"

namespace brook { namespace bootloader {

static uint8_t g_pathCopy[4096];

static uint32_t DevicePathBytes(const EFI_DEVICE_PATH_PROTOCOL* raw) {
    if (!raw) return 0;
    const auto* p = reinterpret_cast<const uint8_t*>(raw);
    uint32_t at = 0;
    for (unsigned nodes = 0; nodes < 128 && at <= 4092; ++nodes) {
        const uint32_t len = uint32_t(p[at + 2]) | (uint32_t(p[at + 3]) << 8);
        if (len < 4 || len > 4096 - at) return 0;
        if (p[at] == 0x7f) return p[at + 1] == 0xff && len == 4 ? at + 4 : 0;
        at += len;
    }
    return 0;
}

static void FillVolume(EFI_HANDLE image, EFI_BOOT_SERVICES* bs, BootStick& out) {
    EFI_GUID loadedGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID, pathGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL* loaded = nullptr;
    if (EFI_ERROR(bs->HandleProtocol(image, &loadedGuid, reinterpret_cast<void**>(&loaded))) ||
        !loaded || !loaded->DeviceHandle) return;
    EFI_DEVICE_PATH_PROTOCOL* raw = nullptr;
    if (EFI_ERROR(bs->HandleProtocol(loaded->DeviceHandle, &pathGuid, reinterpret_cast<void**>(&raw)))) return;
    const uint32_t bytes = DevicePathBytes(raw);
    if (!bytes || bytes > sizeof(g_pathCopy)) return;
    for (uint32_t i = 0; i < bytes; ++i) g_pathCopy[i] = reinterpret_cast<const uint8_t*>(raw)[i];
    RecoveryEspPath parsed{};
    if (!RecoveryParseEspPath(g_pathCopy, bytes, parsed)) return;
    out.volumeValid = 1;
    out.partitionNumber = parsed.partitionNumber;
    out.firstLba = parsed.first;
    out.sectors = parsed.sectors;
    for (unsigned i = 0; i < 16; ++i) out.partitionGuid[i] = parsed.guid[i];
}

static EFI_FILE_PROTOCOL* OpenRoot(EFI_HANDLE image, EFI_BOOT_SERVICES* bs) {
    EFI_GUID loadedGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID, fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL* loaded = nullptr;
    if (EFI_ERROR(bs->HandleProtocol(image, &loadedGuid, reinterpret_cast<void**>(&loaded))) || !loaded) return nullptr;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL* fs = nullptr;
    if (EFI_ERROR(bs->OpenProtocol(loaded->DeviceHandle, &fsGuid, reinterpret_cast<void**>(&fs), image, nullptr,
                                   EFI_OPEN_PROTOCOL_GET_PROTOCOL)) || !fs) return nullptr;
    EFI_FILE_PROTOCOL* root = nullptr;
    if (EFI_ERROR(fs->OpenVolume(fs, &root))) return nullptr;
    return root;
}

static bool Exists(EFI_HANDLE image, EFI_BOOT_SERVICES* bs, const char16_t* path) {
    EFI_FILE_PROTOCOL* root = OpenRoot(image, bs);
    if (!root) return false;
    EFI_FILE_PROTOCOL* file = nullptr;
    const bool found = !EFI_ERROR(root->Open(root, &file, (CHAR16*)path, EFI_FILE_MODE_READ, 0));
    if (found) file->Close(file);
    root->Close(root);
    return found;
}

// Rename root file `from` to `to` (same directory) through SetInfo. This is
// the only write the loader makes, and only when a trial marker exists.
static bool RenameRootFile(EFI_HANDLE image, EFI_BOOT_SERVICES* bs, const char16_t* from, const char16_t* to) {
    EFI_FILE_PROTOCOL* root = OpenRoot(image, bs);
    if (!root) return false;
    EFI_FILE_PROTOCOL* file = nullptr;
    if (EFI_ERROR(root->Open(root, &file, (CHAR16*)from, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) {
        root->Close(root);
        return false;
    }
    EFI_GUID infoGuid = EFI_FILE_INFO_ID;
    alignas(8) UINT8 buffer[512];
    UINTN size = sizeof(buffer) - 64;
    bool ok = !EFI_ERROR(file->GetInfo(file, &infoGuid, &size, buffer)) && size >= SIZE_OF_EFI_FILE_INFO;
    if (ok) {
        auto* info = reinterpret_cast<EFI_FILE_INFO*>(buffer);
        UINTN n = 0;
        while (to[n] && n < 31) { info->FileName[n] = to[n]; ++n; }
        info->FileName[n] = 0;
        info->Size = SIZE_OF_EFI_FILE_INFO + (n + 1) * sizeof(CHAR16);
        ok = !EFI_ERROR(file->SetInfo(file, &infoGuid, info->Size, info)) && !EFI_ERROR(file->Flush(file));
    }
    file->Close(file);
    root->Close(root);
    return ok;
}

static bool LoadSet(EFI_HANDLE image, EFI_BOOT_SERVICES* bs, bool prev, uint8_t*& kernel, UINTN& kb,
                    uint8_t*& initrd, UINTN& ib, BootStick& out) {
    kernel = initrd = nullptr; kb = ib = 0;
    UINTN rn = 0;
    uint8_t* record = ReadFile(image, bs, prev ? u"PREV\\SET.BIN" : u"SET.BIN", &rn, BROOK_STICK_SET_BYTES);
    if (!record) return false;
    bool ok = rn == BROOK_STICK_SET_BYTES && BrookStickSetValid(record);
    if (ok) {
        kernel = ReadFile(image, bs, prev ? u"PREV\\BROOK.ELF" : u"KERNEL\\BROOK.ELF", &kb, BROOK_STICK_SET_KERNEL_MAX);
        initrd = kernel ? ReadFile(image, bs, prev ? u"PREV\\INITRD.IMG" : u"INITRD.IMG", &ib,
                                   BROOK_STICK_SET_INITRD_MAX) : nullptr;
        ok = kernel && initrd && BrookStickSetMatches(record, kernel, kb, initrd, ib) &&
             BrookUpdateExecutable(kernel, kb);
        if (ok) {
            for (unsigned i = 0; i < 32; ++i) {
                out.kernelSha256[i] = BrookStickSetKernelSha(record)[i];
                out.initrdSha256[i] = BrookStickSetInitrdSha(record)[i];
            }
        }
    }
    FreePages(bs, reinterpret_cast<EFI_PHYSICAL_ADDRESS>(record), rn);
    if (!ok) {
        if (initrd) FreePages(bs, reinterpret_cast<EFI_PHYSICAL_ADDRESS>(initrd), ib);
        if (kernel) FreePages(bs, reinterpret_cast<EFI_PHYSICAL_ADDRESS>(kernel), kb);
        kernel = initrd = nullptr; kb = ib = 0;
    }
    return ok;
}

bool LoadStickSet(EFI_HANDLE image, EFI_BOOT_SERVICES* bs, uint8_t*& kernel, UINTN& kb,
                  uint8_t*& initrd, UINTN& ib, BootStick& out) {
    out = {};
    FillVolume(image, bs, out);
    const bool pending = Exists(image, bs, u"TRIAL.BIN");
    const bool run = Exists(image, bs, u"TRIAL.RUN");
    const int first = BrookStickFirstSet(pending, run);
    if (pending) {
        // One attempt for the new set: consume the marker before booting it.
        if (RenameRootFile(image, bs, u"TRIAL.BIN", u"TRIAL.RUN")) out.trial = BROOK_STICK_TRIAL_STARTED;
        else out.trial = BROOK_STICK_TRIAL_MARK_FAILED;
    } else if (run) {
        out.trial = BROOK_STICK_TRIAL_UNCONFIRMED;
    }
    for (int attempt = 0; attempt < 2; ++attempt) {
        const bool prev = (attempt == 0) == (first == BROOK_STICK_PREV);
        if (LoadSet(image, bs, prev, kernel, kb, initrd, ib, out)) {
            out.set = prev ? BROOK_STICK_PREV : BROOK_STICK_ROOT;
            ConsoleSetQuiet(false);
            if (prev) ConsolePrintLine(out.trial == BROOK_STICK_TRIAL_UNCONFIRMED
                ? u"Brook stick: new set was not confirmed; booting the previous set (PREV)"
                : u"Brook stick: root set invalid; booting the previous set (PREV)");
            else if (out.trial == BROOK_STICK_TRIAL_STARTED)
                ConsolePrintLine(u"Brook stick: trial boot of the new set (one attempt)");
            else if (out.trial == BROOK_STICK_TRIAL_MARK_FAILED)
                ConsolePrintLine(u"Brook stick: could not mark the trial; booting the new set");
            else ConsolePrintLine(u"Brook stick: root set verified");
            return true;
        }
    }
    ConsoleSetQuiet(false);
    ConsolePrintLine(u"Brook stick: no verified set; using the configured kernel");
    return false;
}

}} // namespace brook::bootloader
