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
