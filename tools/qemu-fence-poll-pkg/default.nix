# Reproduce the matched host controls used by the GPU frame investigation.
# This does not change Brook's normal QEMU selection. Callers opt in by setting
# BROOK_GPU_QEMU to the resulting store path. The guest ABI is unchanged.
{ pkgs ? import ../../nix/nixpkgs.nix {}, fencePollMs ? 1, fencePollUs ? null }:
assert builtins.elem fencePollMs [ 1 10 ];
assert fencePollUs == null || (fencePollMs == 1 && builtins.elem fencePollUs [ 250 1000 ]);
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
  pname = "qemu-brook-fence-poll-" + (if fencePollUs == null then "${toString fencePollMs}ms" else "${toString fencePollUs}us");
  postPatch = (old.postPatch or "") + (if fencePollUs == null then ''
${pkgs.python3}/bin/python3 - <<'PY'
from pathlib import Path
p = Path('hw/display/virtio-gpu-virgl.c')
text = p.read_text()
original = 'timer_mod(gl->fence_poll, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);'
assert text.count(original) == 1, 'QEMU fence polling site changed; inspect before patching'
p.write_text(text.replace(original,
    'timer_mod(gl->fence_poll, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + ${toString fencePollMs});'))
PY
'' else ''
${pkgs.python3}/bin/python3 - <<'PY'
from pathlib import Path
p = Path('hw/display/virtio-gpu-virgl.c')
text = p.read_text()
original = 'timer_mod(gl->fence_poll, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 10);'
allocation = 'gl->fence_poll = timer_new_ms(QEMU_CLOCK_VIRTUAL,'
assert text.count(original) == 1 and text.count(allocation) == 1, 'QEMU fence timer sites changed; inspect before patching'
text = text.replace(allocation, 'gl->fence_poll = timer_new_ns(QEMU_CLOCK_VIRTUAL,')
text = text.replace(original,
    'timer_mod(gl->fence_poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ${toString (fencePollUs * 1000)});')
p.write_text(text)
PY
'');
})
