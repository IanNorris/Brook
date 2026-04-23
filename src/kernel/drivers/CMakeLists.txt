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

add_driver_module(intel_hda
    "${BROOK_ROOT}/src/drivers/intel_hda/intel_hda_mod.cpp"
)
