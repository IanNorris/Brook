# Reproduce the matched host controls used by the GPU frame investigation.
# This does not change Brook's normal QEMU selection. Callers opt in by setting
# BROOK_GPU_QEMU to the resulting store path. The guest ABI is unchanged.
{ pkgs ? import ../../nix/nixpkgs.nix {}, fencePollMs ? 1 }:
assert builtins.elem fencePollMs [ 1 10 ];
let
  original = pkgs.qemu_full;
  narrowTargets = original.override {
    hostCpuTargets = [ "x86_64-softmmu" ];
    enableDocs = false;
    enableTools = false;
  };
in
assert original.version == "10.2.2";
narrowTargets.overrideAttrs (old: {
  pname = "qemu-brook-fence-poll-${toString fencePollMs}ms";
  postPatch = (old.postPatch or "") + ''
${pkgs.python3}/bin/python3 - <<'PY'
from pathlib import Path
p = Path('hw/display/virtio-gpu-virgl.c')
text = p.read_text()
original = 'timer_mod(gl->fence_poll, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);'
assert text.count(original) == 1, 'QEMU fence polling site changed; inspect before patching'
p.write_text(text.replace(original,
    'timer_mod(gl->fence_poll, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + ${toString fencePollMs});'))
PY
'';
})
