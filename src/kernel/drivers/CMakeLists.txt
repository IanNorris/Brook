# Brook OS kernel driver modules
# Each add_driver_module() call produces a .mod file in build/kernel/drivers/

add_driver_module(virtio_blk
    "${BROOK_ROOT}/src/drivers/virtio_blk/virtio_blk_mod.cpp"
)

add_driver_module(ps2_kbd
    "${BROOK_ROOT}/src/drivers/ps2_kbd/kbd_mod.cpp"
)
