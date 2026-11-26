# Input preferences

Settings saves the keyboard layout and cursor size with the other desktop
preferences. Restart Brook to apply them across the kernel, Wayland, Xwayland
and new application processes. The settings can also be set under `[Desktop]`
in `/data/home/.config/brook/settings.ini`.

- `keyboard_layout=gb` selects UK English. `keyboard_layout=us` selects US
  English. The Dell and RTX image builders expose `--keyboard-layout gb|us`
  and default to GB. Images without a layout marker default to US.
- `cursor_size=24`, `32`, `48` or `64` selects the cursor theme size and
  scales the built-in arrow and resize cursors relative to size 24. The
  desktop scale is applied to compositor-owned shapes. Client cursor
  surfaces already carry their own scale. The compositor accepts images up
  to 128 pixels per side. When an image is larger than the hardware cursor
  plane, Brook draws it in the compositor or renderer.

## Trace pointer startup

Boot with the pointer attached as it normally is. As soon as the desktop
appears, move the mouse over a Wayland app. Read the serial log or retrieve
current kernel log pages with `hw_logs` beginning at cursor 0. Keep the lines
starting with `BOOT_POINTER` and `BOOT_TIMING`. The same lines enter the ring
used by lastlog. After a reboot, retrieve the saved prior log using
`lastlog previous OFFSET COUNT` in chunks of at most 4096 bytes.

The `kernel_ms` fields use the same kernel-entry TSC origin as
`BOOT_TIMING`. The `tick_ms` fallback is the APIC boot tick. USB lines carry
the xHCI controller, port path and USB vendor/product IDs in decimal. Event
types are 2 for motion, 3–4 for buttons, 5 for scroll and 6 for relative
motion. The cursor marker records the first visible cursor supplied to the
compositor or renderer and the eligibility condition that changed. The
Wayland marker records queueing the first event to a client with a pointer
resource. Compare its timestamp with the client's own receipt when needed.

On xHCI, match the `usb_*` lines by controller and port. Once the endpoint
is queued, the lines also include its slot and endpoint address. Keep these
lines and the `ingress` and `wm` lines from the same boot.

- `usb_enumerated`, `usb_queued` and `usb_bound` show discovery, the first
  interrupt-IN doorbell and the end of bind. The queue line includes the
  outcome of SET_PROTOCOL, the device speed, transaction-translator slot
  and port, descriptor bInterval, and programmed service interval in µs.
- `usb_complete` records when Brook first drains a matching transfer event.
  It includes the xHCI completion code and residual length. Completed length
  is reported for success (1) and short packet (13) with a valid residual.
- `usb_rearm` records when Brook rings the doorbell for the next transfer.
  A transfer error ends the endpoint and does not rearm it.
- `usb_report` records the first boot-format report with motion, wheel
  movement, or button activity. `usb_ignored` records the first rejected
  completion, idle report, suppressed shared-button transition or input
  queue overflow. It may be absent if the endpoint has none.

Brook stamps these events at the xHCI driver and prints them later from the
compositor thread. A gap before `usb_complete` includes the time the device
spent waiting to produce a report and any delay draining the transfer event.
