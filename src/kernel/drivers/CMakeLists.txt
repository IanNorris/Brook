# Brook OS kernel driver modules
# Each add_driver_module() call produces a .mod file in build/kernel/drivers/

add_driver_module(virtio_blk
    "${BROOK_ROOT}/src/drivers/virtio_blk/virtio_blk_mod.cpp"
)

add_driver_module(ps2_kbd
    "${BROOK_ROOT}/src/drivers/ps2_kbd/kbd_mod.cpp"
)

add_driver_module(sched_mlfq
    "${BROOK_ROOT}/src/drivers/sched_mlfq/sched_mlfq_mod.cpp"
    "${BROOK_ROOT}/src/modules/sched_mlfq.cpp"
)

add_driver_module(bochs_display
    "${BROOK_ROOT}/src/drivers/bochs_display/bochs_display_mod.cpp"
)

add_driver_module(ps2_mouse
    "${BROOK_ROOT}/src/drivers/ps2_mouse/mouse_mod.cpp"
)

add_driver_module(virtio_input
    "${BROOK_ROOT}/src/drivers/virtio_input/virtio_input_mod.cpp"
    EXTRA_FLAGS "-mgeneral-regs-only" "-mno-sse" "-mno-sse2" "-mfpmath="
)

add_driver_module(virtio_net
    "${BROOK_ROOT}/src/drivers/virtio_net/virtio_net_mod.cpp"
    EXTRA_FLAGS "-mgeneral-regs-only" "-mno-sse" "-mno-sse2" "-mfpmath="
)

add_driver_module(virtio_rng
    "${BROOK_ROOT}/src/drivers/virtio_rng/virtio_rng_mod.cpp"
)

add_driver_module(virtio_gpu
    "${BROOK_ROOT}/src/drivers/virtio_gpu/virtio_gpu_mod.cpp"
    EXTRA_FLAGS "-mgeneral-regs-only" "-mno-sse" "-mno-sse2" "-mfpmath="
)

add_driver_module(intel_hda
    "${BROOK_ROOT}/src/drivers/intel_hda/intel_hda_mod.cpp"
)

add_driver_module(xhci
    "${BROOK_ROOT}/src/drivers/xhci/xhci_mod.cpp"
    EXTRA_FLAGS "-mgeneral-regs-only" "-mno-sse" "-mno-sse2" "-mfpmath="
)

# ---- lwext4 filesystem module ----
# Pure-C lwext4 library + C OSAL shim, compiled as a kernel module.
# The shim include dir provides stdio.h/stdlib.h/errno.h/inttypes.h stubs.
set(LWEXT4_DIR "${BROOK_ROOT}/src/kernel/vendor/lwext4")
add_driver_module(ext4
    "${LWEXT4_DIR}/src/ext4.c"
    "${LWEXT4_DIR}/src/ext4_balloc.c"
    "${LWEXT4_DIR}/src/ext4_bcache.c"
    "${LWEXT4_DIR}/src/ext4_bitmap.c"
    "${LWEXT4_DIR}/src/ext4_block_group.c"
    "${LWEXT4_DIR}/src/ext4_blockdev.c"
    "${LWEXT4_DIR}/src/ext4_crc32.c"
    "${LWEXT4_DIR}/src/ext4_debug.c"
    "${LWEXT4_DIR}/src/ext4_dir.c"
    "${LWEXT4_DIR}/src/ext4_dir_idx.c"
    "${LWEXT4_DIR}/src/ext4_extent.c"
    "${LWEXT4_DIR}/src/ext4_fs.c"
    "${LWEXT4_DIR}/src/ext4_hash.c"
    "${LWEXT4_DIR}/src/ext4_ialloc.c"
    "${LWEXT4_DIR}/src/ext4_inode.c"
    "${LWEXT4_DIR}/src/ext4_journal.c"
    "${LWEXT4_DIR}/src/ext4_mbr.c"
    "${LWEXT4_DIR}/src/ext4_mkfs.c"
    "${LWEXT4_DIR}/src/ext4_super.c"
    "${LWEXT4_DIR}/src/ext4_trans.c"
    "${LWEXT4_DIR}/src/ext4_xattr.c"
    "${BROOK_ROOT}/src/drivers/ext4/ext4_osal_brook.c"
    "${BROOK_ROOT}/src/drivers/ext4/ext4_vfs_mod.cpp"
    EXTRA_FLAGS
        "-I${BROOK_ROOT}/src/drivers/ext4/include"
        "-I${LWEXT4_DIR}/include"
        "-I${LWEXT4_DIR}/include/generated"
        "-Wno-sign-compare"
        "-Wno-shadow"
        "-Wno-cast-qual"
        "-Wno-missing-field-initializers"
        "-Wno-unused-function"
        "-Wno-unused-but-set-variable"
        "-Wno-null-pointer-arithmetic"
        "-Wno-null-pointer-subtraction"
)
