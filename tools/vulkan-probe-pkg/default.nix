{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.stdenv.mkDerivation {
  pname = "brook-vulkan-probe";
  version = "0.1";
  src = ./.;
  nativeBuildInputs = [ pkgs.glslang pkgs.python3 ];
  buildInputs = [ pkgs.vulkan-headers pkgs.vulkan-loader pkgs.libdrm pkgs.xorg.libX11 ];
  buildPhase = ''
    runHook preBuild
    cp ${../../src/shared/gpu_output_abi.h} gpu_output_abi.h
    cp ${../vulkan-compositor-pkg/graphics_layers.h} graphics_layers.h
    cp ${../vulkan-compositor-pkg/graphics_image.h} graphics_image.h
    cp ${../vulkan-compositor-pkg/graphics_image.c} graphics_image.c
    cp ${../vulkan-compositor-pkg/graphics_pass.h} graphics_pass.h
    cp ${../vulkan-compositor-pkg/graphics_pass.c} graphics_pass.c
    cp ${../vulkan-compositor-pkg/renderer.h} renderer.h
    glslangValidator -V --target-env vulkan1.1 probe.comp -o probe.spv
    glslangValidator -V --target-env vulkan1.1 ${../vulkan-compositor-pkg/composite.comp} -o compositor.spv
    cp ${../vulkan-compositor-pkg/scene.h} scene.h
    cp ${../vulkan-compositor-pkg/compositor_probe.h} compositor_probe.h
    glslangValidator -V --target-env vulkan1.1 triangle.vert -o vertex.spv
    glslangValidator -V --target-env vulkan1.1 triangle.frag -o fragment.spv
    glslangValidator -V --target-env vulkan1.1 external_sample.vert -o sample_vertex.spv
    glslangValidator -V --target-env vulkan1.1 external_sample.frag -o sample_fragment.spv
    glslangValidator -V --target-env vulkan1.1 external_sample_xrgb.frag -o sample_xrgb.spv
    glslangValidator -V --target-env vulkan1.1 external_sample_exact.frag -o sample_exact.spv
    glslangValidator -V --target-env vulkan1.1 ${../vulkan-compositor-pkg/graphics_layers.frag} -o sample_layers.spv
    glslangValidator -V --target-env vulkan1.1 ${../vulkan-compositor-pkg/graphics_scene.frag} -o graphics_scene.spv
    glslangValidator -V --target-env vulkan1.1 ${../vulkan-compositor-pkg/graphics_pass.vert} -o graphics_pass_vertex.spv
    python3 - <<'PY'
    import struct
    from pathlib import Path
    data = Path('probe.spv').read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    Path('probe_spv.h').write_text('static const uint32_t shaderCode[] = {\n' +
        ',\n'.join('0x%08xu' % word for word in words) + '\n};\n')
    data = Path('compositor.spv').read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    Path('compositor_spv.h').write_text('static const uint32_t compositorCode[] = {\n' +
        ',\n'.join('0x%08xu' % word for word in words) + '\n};\n')
    text = ""
    for name in ('vertex', 'fragment'):
        data = Path(name + '.spv').read_bytes()
        words = struct.unpack('<' + 'I' * (len(data) // 4), data)
        text += 'static const uint32_t ' + name + 'Code[] = {\n' + ',\n'.join('0x%08xu' % word for word in words) + '\n};\n'
    Path('triangle_spv.h').write_text(text)
    text = ""
    for name in ('sample_vertex', 'sample_fragment', 'sample_xrgb', 'sample_exact', 'sample_layers'):
        data = Path(name + '.spv').read_bytes()
        words = struct.unpack('<' + 'I' * (len(data) // 4), data)
        text += 'static const uint32_t ' + name + 'Code[] = {\n' + ',\n'.join('0x%08xu' % word for word in words) + '\n};\n'
    Path('external_sample_spv.h').write_text(text)
    text = ""
    for name,symbol in (('graphics_pass_vertex','graphicsVertexCode'),('graphics_scene','graphicsFragmentCode')):
        data = Path(name+'.spv').read_bytes()
        words = struct.unpack('<'+'I'*(len(data)//4),data)
        text += 'static const uint32_t '+symbol+'[] = {\n'+',\n'.join('0x%08xu' % word for word in words)+'\n};\n'
    Path('graphics_pass_spv.h').write_text(text)
    PY
    $CC -std=c11 -O2 -Wall -Wextra -Werror probe.c -lvulkan -o vulkan-probe
    $CC -std=c11 -O2 -Wall -Wextra -Werror external_image_probe.c -lvulkan -o vulkan-external-image-probe
    $CC -std=c11 -O2 -Wall -Wextra -Werror external_image_roundtrip.c graphics_image.c graphics_pass.c -lvulkan -o vulkan-external-image-roundtrip
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
    cp vulkan-probe vulkan-external-image-probe vulkan-external-image-roundtrip vulkan-wsi-probe virtgpu-query virtgpu-context gem-capacity virtgpu-blob virtgpu-sync-file $out/bin/
    runHook postInstall
  '';
}
