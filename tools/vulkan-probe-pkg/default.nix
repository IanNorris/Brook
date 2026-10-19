{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.stdenv.mkDerivation {
  pname = "brook-vulkan-probe";
  version = "0.1";
  src = ./.;
  nativeBuildInputs = [ pkgs.glslang pkgs.python3 ];
  buildInputs = [ pkgs.vulkan-headers pkgs.vulkan-loader pkgs.libdrm ];
  buildPhase = ''
    runHook preBuild
    glslangValidator -V --target-env vulkan1.1 probe.comp -o probe.spv
    python3 - <<'PY'
    import struct
    from pathlib import Path
    data = Path('probe.spv').read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    Path('probe_spv.h').write_text('static const uint32_t shaderCode[] = {\n' +
        ',\n'.join('0x%08xu' % word for word in words) + '\n};\n')
    PY
    $CC -std=c11 -O2 -Wall -Wextra -Werror probe.c -lvulkan -o vulkan-probe
    $CC -D_GNU_SOURCE -std=c11 -O2 -Wall -Wextra -Werror \
      -I${pkgs.libdrm.dev}/include/libdrm virtgpu-query.c -o virtgpu-query
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    cp vulkan-probe virtgpu-query $out/bin/
    runHook postInstall
  '';
}
