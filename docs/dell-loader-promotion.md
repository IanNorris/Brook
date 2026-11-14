# Dell default loader promotion (v5 guarded → timing loader)

This replaces `EFI/BOOT/BOOTX64.EFI` on the Dell's SSD ESP with a newly built guarded loader. It keeps every earlier loader and both recovery paths. It is a supervised step: Ian has to select the USB maintenance boot, and must be nearby in case the firmware falls back to F5 or the USB.

## Why the USB maintenance boot

While a guarded desktop kernel is running it holds leases on `RECOVERY/GUARD0.BIN` and `GUARD1.BIN`, so opening them returns `EBUSY` (journal, 7 Nov). The promotion tool must read and hash both guards, so it only works from the USB maintenance kernel (payload `dell-usb-ssd-maintenance-20261106`). That kernel mounts the identity-verified SSD ESP at `/media/system` and does not take the leases. RAM transfers are not executable there; `EFI/BOOT/BRKMODE.ELF <transaction> <sha256>` marks one hash-matched upload executable.

## ESP state expected before the change

These hashes were last observed on 7 Nov 2026: inventory job `e25b310b…` on USB boot `16dff15e…`, followed by v5 repromotion `fc2294ba…`. The inventory step **re-verifies every one live** and stops on any difference.

| Path | SHA-256 | What it is |
|---|---|---|
| `EFI/BOOT/BOOTX64.EFI` | `c717c048…0c00` | v5 guarded default (current) |
| `EFI/BOOT/BROOKV5.EFI` | `c717c048…0c00` | v5 staged copy |
| `EFI/BOOT/BROOKV4.EFI` | `2576762f…1997` | v4 guarded (previous default) |
| `EFI/BOOT/BROOKG2.EFI` | `2576762f…1997` | v4 staged copy |
| `EFI/BOOT/BROOKOLD.EFI` | `cc1989c8…ba09` | original unguarded loader |
| `EFI/BOOT/BROOKF5.EFI` | `8f29b69f…61b1` | firmware F5 entry (unguarded, independent) |
| `EFI/BOOT/BRKMODE.ELF` | `ba2c6ca7…1351` | RAM exec helper |
| `RECOV2/KERNEL.ELF` / `INITRD.IMG` / `SELECT.BIN` | `b7e2684e…` / `068876ff…` / `fc624637…` | alternate recovery pair |
| `RECOVERY/GUARD0.BIN` = `GUARD1.BIN` | live only (was `6c4da583…`) | guard records; may legitimately change |

The full values are in `scripts/dell_loader_promotion.py` (`EXPECTED`). The new names used are `BROOKT6.EFI` (staged loader) and `BROOKP5.EFI` (the v5 default after promotion). Both must be absent beforehand, and so must `BROOKNEW.EFI`.

## Build

```sh
nix-shell --run 'python3 scripts/build_dell_loader.py --output build/dell/loader-<label>'
```

The script reads the private identity and data bounds from `shared/dell/guard-loader-options.txt` (mode 600). It writes `BROOKGUARD.efi` and `manifest.json`. The build directory is mode 700 and contains the identity; never commit it. The build is reproducible: two clean builds of `e550476b` gave the same hash.

**QEMU check of the same source:**
1. Build again with `--shared /data/Programming/Brook/shared/dell/qemu-check/loader-options-qemu`. That file holds the QEMU fixture identity; every other option is identical.
2. Run:
   ```sh
   scripts/check_dell_image_qemu.py --pair <pair> --efi <qemu-loader>/BROOKGUARD.efi --output <dir>
   ```

A physical-identity loader correctly refuses the VM disk, so it can't be tested directly in QEMU.

## Procedure

Run every command from the repo root with `BROOK_ROOT=/data/Programming/Brook/display-perf`, the registry holding the Dell session. Each step writes a new `--output` directory containing `receipt.json` with every job and transfer ID.

1. **Before restarting.** Stop or finish any manual bridge jobs; a running supervisor job blocks the checked restart ("Closing apps"). Note the current generation and payload (`hw_doctor`, `/boot/HWSTATUS.ELF`).
2. **Restart and choose the USB stick.** Restart:
   ```sh
   tools/vm_bridge/server.py --call hw_power --arguments '{"session":S,"request_id":<new>,"operation":"restart"}'
   ```
   Then `commit` when asked. Ian presses F12 at the Dell logo and picks the USB maintenance stick, which is not the default.
3. **Attach.** Use `hw_attach` with the Dell IP (192.168.85.106 at last use) to get a new session. Run `hw_doctor`; it must be `verified` with `identity_matches`.
4. **Dry run, read-only** (safe to repeat):
   ```sh
   python3 scripts/dell_loader_promotion.py inventory --session <S> --output build/dell/promo-<n>-inventory
   ```
   - It refuses unless the payload is a USB maintenance payload.
   - It builds the inventory tool, uploads it, makes it executable with BRKMODE, runs it, and prints `esp_state`, `guard_clean` and `differences`.
   - **Any difference stops the procedure** until it is reviewed. RECOV2 may have been restaged since 7 Nov; if so, update `EXPECTED` only after confirming why.
   - The identity is passed in the tool's argv, so it appears in the host bridge job records, as on 7 Nov.
5. **Stage (create-only):**
   ```sh
   python3 scripts/dell_loader_promotion.py stage --session <S> --loader build/dell/loader-<label> --output build/dell/promo-<n>-stage
   ```
   Without `--execute` it prints the plan; then rerun it with `--execute`. It writes `EFI/BOOT/BROOKT6.EFI` with `O_EXCL`, fsyncs it and reads it back (`EFI_INVENTORY_STAGE_OK`). Nothing existing is touched. If anything goes wrong at this step, `BOOTX64.EFI` is unchanged and the machine still boots normally.
6. **Inventory again.** Confirm `BROOKT6.EFI` has the manifest hash.
7. **Promote:**
   ```sh
   python3 scripts/dell_loader_promotion.py promote --session <S> --loader build/dell/loader-<label> --output build/dell/promo-<n>-promote [--execute]
   ```
   - It reruns the inventory in the same invocation and passes all 12 live hashes to `hardware_guard_default_stage.c` built with `BROOK_GUARD_REPROMOTE_NEXT`.
   - **The tool:** writes and fsyncs `BROOKNEW.EFI` and reads it back, renames `BOOTX64.EFI` → `BROOKP5.EFI`, renames `BROOKNEW.EFI` → `BOOTX64.EFI`, then fsyncs and reads both back.
   - **Success:** it prints `GUARD_DEFAULT_OK` and removes `/tmp/hardware-update.lock`.
8. **Inventory again.** Expect `esp_state: promoted`: `BOOTX64.EFI` equals `BROOKT6.EFI` and `BROOKP5.EFI` holds `c717…`, with no differences.
9. **Restart into the SSD.**
   - Run a checked restart (`hw_power` restart then commit) and leave the boot device at its default (the SSD).
   - Attach to the new boot and check the payload with HWSTATUS: it should be the previously active generation.
   - Check the boot toast or log line: `BOOT_TIMING firmware_ms=… wait_ms=… loader_ms=… kernel_ms=… proto=5`.
   - **The kernel matters:** a kernel from before `d4d9915d` (gen ≤ 369) reads loader timing only from protocol ≥ 6, so it shows `na` for wait and loader. EFI power-off is unaffected (QEMU: `efi-runtime: power-off opt-in enabled`). The first kernel built at or after `d4d9915d` shows all values.
   - Also confirm `power:` shows the EFI or Dell power-off method as before. AC-loss power-off depends on it.

## Rollback and recovery

- **The new loader boots but misbehaves:** from a USB maintenance boot, promote the old loader back the same way. Build a variant with `-DBROOK_GUARD_NEXT_STAGE='"BROOKP5.EFI"' -DBROOK_GUARD_NEXT_BACKUP='"BROOKT7.EFI"'`, or copy `BROOKP5.EFI` to a fresh stage name with the stage tool and promote that. Do not delete anything.
- **The new loader does not boot:** use the firmware F5 entry (`BROOKF5.EFI`, unguarded, independent of `BOOTX64.EFI`), or the USB maintenance stick, then roll back as above.
- **Interrupted promotion** (no `GUARD_DEFAULT_OK`, or the job state is uncertain):
  - Do not restart and do not rerun. Inspect the same job with `hw_job`, then run `inventory`. The lock file stays in place after a partial mutation.
  - If the first rename failed: `BOOTX64.EFI` is still v5 and `BROOKNEW.EFI` exists. Delete only `BROOKNEW.EFI` after verifying its hash.
  - If the second rename failed: there is no `BOOTX64.EFI`, `BROOKP5.EFI` is v5 and `BROOKNEW.EFI` is the new loader. Rename one of them to `BOOTX64.EFI` before any restart.
  - If power is lost between the renames, the default boot entry finds no file. Boot through F5 or the USB stick and repair from the maintenance boot.

## Uncertain or unverified

- **ESP state since 7 Nov is assumed, not observed.** The `EXPECTED` table is from 7 Nov. Changes since then are known only from the journal (`RECOVERY/POWER.BIN` added on 12 Nov). The live inventory decides.
- **Firmware selection by file name is assumed.** The firmware's SSD boot entry has no filename, so loading `EFI/BOOT/BOOTX64.EFI` is the fallback-path assumption. It held for v4→v5.
- **The physical Dell loader has never booted.** Only the QEMU-identity build with identical options passed the guard repair-to-desktop fixture, with the gen 369 kernel.
- **The two new helpers are not yet proven on the maintenance kernel itself.** The transaction tool and the T6 stage were tested on the host and on cloned SATA in QEMU using that kernel (`dell-usb-ssd-maintenance-candidate-20261106`). The updated inventory tool (optional entries) was compiled but not run in QEMU, because the fixture's guards don't match its identity.
- **Ownership of `/data/.brook-transfer` uploads is assumed.** BRKMODE requires the upload to be root-owned. It was in the 7 Nov run.
