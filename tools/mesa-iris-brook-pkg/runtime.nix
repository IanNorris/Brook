{ pkgs ? import ../../nix/nixpkgs.nix { config.allowUnfree = false; } }:
let
  stock = import ../../nix/runtime-graphics.nix { inherit pkgs; };
  iris = import ./default.nix { inherit pkgs; waylandBufferDepth = true; };
  llvm = pkgs.lib.getLib pkgs.llvmPackages_21.libllvm;
  storeName = path: builtins.substring 33 (-1) (builtins.baseNameOf (toString path));
in pkgs.runCommand (storeName iris) {
  nativeBuildInputs = [ pkgs.patchelf pkgs.python3 ];
  disallowedReferences = [ iris llvm ];
  meta.license = pkgs.mesa.meta.license;
} ''
  mkdir -p $out/lib/brook-iris $out/share/brook-iris/egl_vendor.d
  install -m 0644 ${iris}/lib/libEGL_mesa.so.0 $out/lib/brook-iris/libEGL_mesa.so.0
  install -m 0644 ${iris}/lib/libgallium-${pkgs.mesa.version}.so $out/lib/brook-iris/

  # Use the installed Mesa's DRI paths and the compact LLVM runtime.
  # Equal-length prefixes preserve the ELF layout and string-table offsets.
  python3 - "$out" ${iris} ${stock.mesaRuntime} ${llvm} ${stock.llvmRuntime} <<'PY'
  from pathlib import Path
  import sys

  output, original, stock, llvm, llvm_runtime = map(str.encode, sys.argv[1:])
  assert len(output) == len(original) == len(stock) and len(llvm) == len(llvm_runtime)
  for binary in (Path(sys.argv[1]) / 'lib/brook-iris').glob('*.so*'):
      data = binary.read_bytes().replace(original+b'/lib/dri', stock+b'/lib/dri')
      binary.write_bytes(data.replace(original, output).replace(llvm, llvm_runtime))
  PY
  patchelf --set-rpath "\$ORIGIN:$(patchelf --print-rpath "$out/lib/brook-iris/libEGL_mesa.so.0")" \
    "$out/lib/brook-iris/libEGL_mesa.so.0"
  cat > $out/share/brook-iris/egl_vendor.d/50_mesa.json <<JSON
  {"file_format_version":"1.0.0","ICD":{"library_path":"$out/lib/brook-iris/libEGL_mesa.so.0"}}
  JSON
  mkdir -p $out/bin
  cat > $out/bin/brook-iris-run <<'SCRIPT'
  #!${pkgs.bash}/bin/bash
  unset LIBGL_ALWAYS_SOFTWARE
  export LIBGL_DRIVERS_PATH="''${LIBGL_DRIVERS_PATH:-${stock.mesaRuntime}/lib/dri}"
  export GBM_BACKENDS_PATH="''${GBM_BACKENDS_PATH:-${stock.mesaRuntime}/lib/gbm}"
  # A process such as Chromium can load Gallium before the EGL vendor. Make
  # both users resolve the same opt-in library instead of reusing stock Mesa.
  export LD_LIBRARY_PATH="@IRIS@/lib/brook-iris''${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
  unset __EGL_VENDOR_LIBRARY_FILENAMES
  export __EGL_VENDOR_LIBRARY_DIRS=@IRIS@/share/brook-iris/egl_vendor.d
  exec "$@"
  SCRIPT
  substituteInPlace $out/bin/brook-iris-run --replace-fail @IRIS@ "$out"
  chmod 0755 $out/bin/brook-iris-run
''
