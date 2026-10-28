# Retain the tested graphics binaries, excluding LLVM's static development
# archives from the runtime closure. Equal-length store prefixes let us change
# embedded runtime paths without changing ELF offsets or rebuilding LLVM.
{ pkgs ? import ./nixpkgs.nix { config.allowUnfree = false; } }:
let
  lib = pkgs.lib;
  llvm = lib.getLib pkgs.llvmPackages_21.libllvm;
  storeName = path: builtins.substring 33 (-1) (builtins.baseNameOf (toString path));
  rewrite = ''
    import os, pathlib, sys
    root, before, after = map(os.fsencode, sys.argv[1:])
    if len(before) != len(after):
        raise SystemExit("Runtime store prefix length mismatch")
    for directory, dirs, files in os.walk(root, followlinks=False):
        for name in dirs + files:
            path = os.path.join(directory, name)
            if os.path.islink(path):
                target = os.readlink(path)
                if before in target:
                    os.unlink(path)
                    os.symlink(target.replace(before, after), path)
            elif os.path.isfile(path):
                data = pathlib.Path(os.fsdecode(path)).read_bytes()
                if before in data:
                    pathlib.Path(os.fsdecode(path)).write_bytes(data.replace(before, after))
  '';
  script = pkgs.writeText "rewrite-runtime-prefix.py" rewrite;
  llvmRuntime = pkgs.runCommand (storeName llvm) {
    nativeBuildInputs = [ pkgs.python3 ];
    disallowedReferences = [ llvm ];
    meta.license = llvm.meta.license;
  } ''
    mkdir -p $out/lib
    cp -a ${llvm}/lib/*.so* $out/lib/
    chmod -R u+w $out
    python3 ${script} $out ${llvm} $out
    test -f $out/lib/libLLVM.so.21.1
    test -z "$(find $out -name '*.a' -print -quit)"
  '';
  mesaRuntime = pkgs.runCommand (storeName pkgs.mesa) {
    nativeBuildInputs = [ pkgs.python3 ];
    disallowedReferences = [ pkgs.mesa llvm ];
    meta.license = pkgs.mesa.meta.license;
  } ''
    mkdir -p $out
    cp -a ${pkgs.mesa}/. $out/
    chmod -R u+w $out
    python3 ${script} $out ${pkgs.mesa} $out
    python3 ${script} $out ${llvm} ${llvmRuntime}
  '';
in { inherit llvmRuntime mesaRuntime; }
