#pragma once
/* Try to synchronize a shared GPU buffer for CPU reading.
 * Arguments: (shared buffer fd, flags=0, 0, 0, 0, 0).
 * Returns 0 when the CPU cache transition has completed, -EAGAIN when the
 * producer or another transition is busy, -EIO on a failed/uncertain buffer,
 * -EBADF for an invalid fd, or -ENOTSUP for an unsupported buffer backend.
 * This does not wait for a busy producer, although cache synchronization of
 * an idle buffer takes time proportional to its size. The caller must retain
 * the fd and independently exclude new producer writes for the entire CPU
 * access; success is not a lease and does not revoke existing CPU mappings. */
#define BROOK_SYS_GPU_BUFFER_TRY_CPU_READ 0xB1B
/* Return a close-on-exec sync-file fd for the GPU submissions already
 * admitted against this shared buffer, or a negative errno. EAGAIN means a
 * submission is being prepared or its ownership cannot be proved; EIO means
 * failed/uncertain GPU state. Later submissions are deliberately excluded.
 * The caller must retain the buffer and exclude all new producer writes until
 * every consumer has retired; this snapshot is not an ownership lease. */
#define BROOK_SYS_GPU_BUFFER_SNAPSHOT_FENCE 0xB1C
