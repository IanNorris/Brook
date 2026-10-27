{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.stdenv.mkDerivation {
  pname = "brook-vulkan-probe";
  version = "0.1";
  src = ./.;
  nativeBuildInputs = [ pkgs.glslang pkgs.python3 ];
  buildInputs = [ pkgs.vulkan-headers pkgs.vulkan-loader pkgs.libdrm pkgs.xorg.libX11 ];
  buildPhase = ''
    runHook preBuild
    glslangValidator -V --target-env vulkan1.1 probe.comp -o probe.spv
    glslangValidator -V --target-env vulkan1.1 triangle.vert -o vertex.spv
    glslangValidator -V --target-env vulkan1.1 triangle.frag -o fragment.spv
    python3 - <<'PY'
    import struct
    from pathlib import Path
    data = Path('probe.spv').read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    Path('probe_spv.h').write_text('static const uint32_t shaderCode[] = {\n' +
        ',\n'.join('0x%08xu' % word for word in words) + '\n};\n')
    text = ""
    for name in ('vertex', 'fragment'):
        data = Path(name + '.spv').read_bytes()
        words = struct.unpack('<' + 'I' * (len(data) // 4), data)
        text += 'static const uint32_t ' + name + 'Code[] = {\n' + ',\n'.join('0x%08xu' % word for word in words) + '\n};\n'
    Path('triangle_spv.h').write_text(text)
    PY
    $CC -std=c11 -O2 -Wall -Wextra -Werror probe.c -lvulkan -o vulkan-probe
    $CC -std=c11 -O2 -Wall -Wextra -Werror wsi.c -lvulkan -lX11 -o vulkan-wsi-probe
    $CC -D_GNU_SOURCE -std=c11 -O2 -Wall -Wextra -Werror \
      -I${pkgs.libdrm.dev}/include/libdrm virtgpu-query.c -o virtgpu-query
    $CC -D_GNU_SOURCE -std=c11 -O2 -Wall -Wextra -Werror \
      -I${pkgs.libdrm.dev}/include/libdrm virtgpu-context.c -o virtgpu-context
    $CC -std=c11 -O2 -Wall -Wextra -Werror \
      -I${pkgs.libdrm.dev}/include/libdrm gem-capacity.c -o gem-capacity
    $CC -D_GNU_SOURCE -std=c11 -O2 -Wall -Wextra -Werror \
      -I${pkgs.libdrm.dev}/include/libdrm virtgpu-blob.c -o virtgpu-blob
    $CC -D_GNU_SOURCE -std=c11 -O2 -Wall -Wextra -Werror \
      -I${pkgs.libdrm.dev}/include/libdrm virtgpu-sync-file.c -o virtgpu-sync-file
    runHook postBuild
  '';
  installPhase = ''
    runHook preInstall
    mkdir -p $out/bin
    cp vulkan-probe vulkan-wsi-probe virtgpu-query virtgpu-context gem-capacity virtgpu-blob virtgpu-sync-file $out/bin/
    runHook postInstall
  '';
}
