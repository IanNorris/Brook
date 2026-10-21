# Private causal control, not a replacement for the normal QEMU launcher.
{ pkgs ? import ../../nix/nixpkgs.nix {}, fencePollMs ? 10 }:
assert builtins.isInt fencePollMs && fencePollMs >= 1 && fencePollMs <= 1000;
let
  base = pkgs.qemu_full.override {
    hostCpuTargets = [ "x86_64-softmmu" ];
    enableDocs = false;
    enableTools = false;
  };
in base.overrideAttrs (old: {
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
