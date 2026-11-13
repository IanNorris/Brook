# Isolated HasVK compatibility experiment

This candidate makes i915 error-state capture optional for HasVK. If the
kernel reports that full capture is unavailable, normal operation omits the
capture flag instead of refusing to enumerate a device. On Brook only,
`BROOK_I915_PARTIAL_CAPTURE=1` requests Brook's supported bounded capture
hint, without changing the kernel's capability answer. Do not set that option
on kernels that reject the flag.

The package is separate from the ordinary Mesa installation. It does not implement missing kernel synchronization,
general DRM timeline semantics, full i915 error-state capture or crash
isolation. Results from this candidate must identify the patched driver and the
option used; do not describe it as an upstream HasVK validation.

The networked hardware desktop package includes the explicit launcher
`brook-intel-vulkan PROGRAM [ARG ...]`. It selects this ICD and requests the
bounded capture hint for the launched program; ordinary OpenGL driver selection
is unchanged. The launcher is not a claim that every Vulkan feature is working.
The hardware store builder also copies the driver and its rewritten manifest
into the root-owned `/nix/system/graphics/intel-vulkan-26.0.6-brook1` path.
The kernel selects that path automatically when the Intel backend is enabled
and the path passes its ownership checks. This applies to newly built stores;
it does not change an existing installation.
