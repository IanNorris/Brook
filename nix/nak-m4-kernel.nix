# M4 step 4: the TU104 compute kernel and its QMD, produced by Mesa's NAK compiler (NVK) from the
# pinned nixpkgs Mesa source, with no GPU. A test added to NAK (nak-m4/brook_m4.rs, MIT) builds
# the kernel with NAK's own TestShaderBuilder for SM75, encodes it, fills the QMD with
# nak_fill_qmd for TURING_COMPUTE_A at fixed VAs, and writes kernel.bin / kernel.asm / qmd.bin /
# info.txt. $out also carries Mesa's licence text and a PROVENANCE file (Mesa version and source
# store path). scripts/nak_m4_header.py turns the output into the kernel header the live verb uses.
{ pkgs ? import ./nixpkgs.nix { } }:
let
  mesa = pkgs.mesa;
in
pkgs.stdenv.mkDerivation {
  pname = "brook-nak-m4-kernel";
  version = mesa.version;
  src = mesa.src;
  inherit (mesa) depsBuildBuild;
  # mesa_clc / vtn_bindgen2 from the pinned Mesa itself, so NVK's CLC shaders build without LLVM here.
  nativeBuildInputs = mesa.nativeBuildInputs ++ [ mesa.cross_tools ];
  buildInputs = mesa.buildInputs;
  __structuredAttrs = true;
  env.MESON_PACKAGE_CACHE_DIR = mesa.env.MESON_PACKAGE_CACHE_DIR;
  postPatch = (mesa.postPatch or "") + ''
    cp ${./nak-m4/brook_m4.rs} src/nouveau/compiler/nak/brook_m4.rs
    printf '\n#[cfg(test)]\nmod brook_m4;\n' >> src/nouveau/compiler/nak/lib.rs
  '';
  mesonBuildType = "release";
  mesonFlags = [
    "-Dauto_features=disabled"
    "-Dvulkan-drivers=nouveau"
    "-Dgallium-drivers="
    "-Dplatforms="
    "-Dglx=disabled"
    "-Degl=disabled"
    "-Dgbm=disabled"
    "-Dopengl=false"
    "-Dgles1=disabled"
    "-Dgles2=disabled"
    "-Dllvm=disabled"
    "-Dmesa-clc=system"
    "-Dprecomp-compiler=system"
    "-Dvideo-codecs="
    "-Dtools="
    "-Dvalgrind=disabled"
    "-Dbuild-tests=true"
    "-Dvulkan-layers="
  ];
  doCheck = false;
  buildPhase = ''
    runHook preBuild
    ninja -t targets all | grep -q '^src/nouveau/compiler/nak: rust_COMPILER' || { echo "no NAK test target"; exit 1; }
    ninja src/nouveau/compiler/nak
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    mkdir -p $out
    NAK_DEBUG=print BROOK_NAK_OUT=$out ./src/nouveau/compiler/nak --exact brook_m4::dump --nocapture --test-threads=1 2> nak.stderr
    sed -n '/^NAK shader:/,$p' nak.stderr | grep -v '^test \|^running\|^$' > $out/kernel.asm || true
    test -s $out/kernel.bin && test -s $out/qmd.bin && test -s $out/kernel.asm
    cp ../docs/license.rst $out/LICENSE.mesa.rst
    cat > $out/PROVENANCE <<EOF2
    Produced by Mesa ${mesa.version} NAK (src/nouveau/compiler/nak), nixpkgs-pinned source:
      ${mesa.src}
    with the added test nix/nak-m4/brook_m4.rs (Brook, MIT), run without a GPU.
    Mesa is MIT licensed (LICENSE.mesa.rst).
    EOF2
    runHook postInstall
  '';
  dontFixup = true;
}
