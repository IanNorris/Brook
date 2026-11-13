{ pkgs ? import ../../nix/nixpkgs.nix {} }:
pkgs.stdenv.mkDerivation {
  pname = "brook-vulkan-compositor";
  version = "0.1";
  src = ./.;
  nativeBuildInputs = [ pkgs.glslang pkgs.python3 ];
  buildInputs = [ pkgs.vulkan-headers pkgs.vulkan-loader ];
  buildPhase = ''
    cp ${../../src/shared/gpu_output_abi.h} gpu_output_abi.h
    cp ${../../src/shared/desktop_layers_abi.h} desktop_layers_abi.h
    cp ${../../src/shared/gpu_buffer_abi.h} gpu_buffer_abi.h
    cp ${../../src/shared/gpu_window_source_abi.h} gpu_window_source_abi.h
    cp ${../../src/shared/gpu_window_channel.h} gpu_window_channel.h
    cp ${../../scripts/workloads/intel_cpu_try_probe.c} intel_cpu_try_probe.c
    cp ${../../scripts/workloads/intel_sdl_same_title_probe.c} intel_sdl_same_title_probe.c
    cp ${../../scripts/workloads/desktop_pair_pixel_probe.c} desktop_pair_pixel_probe.c
    cp ${../../src/shared/wm_window_state.h} wm_window_state.h
    cp ${../../src/shared/brook_debug_frame.h} brook_debug_frame.h
    glslangValidator -V --target-env vulkan1.1 composite.comp -o composite.spv
    glslangValidator -V --target-env vulkan1.1 -DBROOK_EXTERNAL_SOURCE=1 composite.comp -o composite_external.spv
    glslangValidator -V --target-env vulkan1.1 -DBROOK_MULTI_SOURCE=1 composite.comp -o composite_multi.spv
    glslangValidator -V --target-env vulkan1.1 -DBROOK_MULTI_SOURCE=1 -DBROOK_EXTERNAL_OUTPUT=1 composite.comp -o composite_output.spv
    glslangValidator -V --target-env vulkan1.1 graphics_layers.frag -o graphics_layers.spv
    glslangValidator -V --target-env vulkan1.1 graphics_scene.frag -o graphics_scene.spv
    glslangValidator -V --target-env vulkan1.1 graphics_pass.vert -o graphics_pass_vertex.spv
    python3 - <<'PY'
    import struct
    from pathlib import Path
    data = Path('composite.spv').read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    Path('compositor_spv.h').write_text('static const uint32_t compositorCode[] = {\n' +
        ',\n'.join('0x%08xu' % word for word in words) + '\n};\n')
    data = Path('composite_external.spv').read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    Path('compositor_external_spv.h').write_text('static const uint32_t compositorExternalCode[] = {\n' +
        ',\n'.join('0x%08xu' % word for word in words) + '\n};\n')
    data = Path('composite_multi.spv').read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    Path('compositor_multi_spv.h').write_text('static const uint32_t compositorMultiCode[] = {\n' +
        ',\n'.join('0x%08xu' % word for word in words) + '\n};\n')
    data = Path('composite_output.spv').read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    Path('compositor_output_spv.h').write_text('static const uint32_t compositorOutputCode[] = {\n' +
        ',\n'.join('0x%08xu' % word for word in words) + '\n};\n')
    text = ""
    for name,symbol in (('graphics_pass_vertex','graphicsVertexCode'),('graphics_scene','graphicsFragmentCode')):
        data = Path(name+'.spv').read_bytes()
        words = struct.unpack('<'+'I'*(len(data)//4),data)
        text += 'static const uint32_t '+symbol+'[] = {\n'+',\n'.join('0x%08xu' % word for word in words)+'\n};\n'
    Path('graphics_pass_spv.h').write_text(text)
    PY
    $CC -std=c11 -O2 -Wall -Wextra -Werror -c renderer.c -o renderer.o
    $CC -std=c11 -O2 -Wall -Wextra -Werror intel_sdl_same_title_probe.c -ldl -o brook-same-title-gpu-probe
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_pair_pixel_probe.c -o brook-pair-pixel-probe
    $CC -std=c11 -O2 -Wall -Wextra -Werror -c graphics_image.c -o graphics_image.o
    $CC -std=c11 -O2 -Wall -Wextra -Werror -c graphics_pass.c -o graphics_pass.o
    $CC -std=c11 -O2 -Wall -Wextra -Werror graphics_layers_test.c -o brook-graphics-layers-test
    ./brook-graphics-layers-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_graphics_damage_test.c -o brook-desktop-graphics-damage-test
    ./brook-desktop-graphics-damage-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror import_cache_test.c -lvulkan -o brook-import-cache-test
    ./brook-import-cache-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror -c surface.c -o surface.o
    $AR rcs libbrook-vulkan-compositor.a renderer.o surface.o graphics_image.o graphics_pass.o
    $CC -std=c11 -O2 -Wall -Wextra -Werror renderer_test.c renderer.o -lvulkan -o brook-vulkan-compositor-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror renderer_timeout_test.c renderer.o -lvulkan -o brook-vulkan-timeout-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror external_renderer_test.c renderer.o -lvulkan -o brook-vulkan-external-compositor-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror cross_process_renderer_test.c renderer.o -lvulkan -o brook-vulkan-cross-process-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror gpu_window_channel_test.c -o brook-gpu-window-channel-test
    ./brook-gpu-window-channel-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_gpu_channel_test.c -o brook-desktop-gpu-channel-test
    ./brook-desktop-gpu-channel-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_async_source_barrier_test.c -o brook-desktop-async-source-barrier-test
    ./brook-desktop-async-source-barrier-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror multi_renderer_test.c renderer.o surface.o -lvulkan -o brook-vulkan-multi-compositor-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror async_pair_test.c renderer.o -lvulkan -o brook-vulkan-async-pair-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_chrome_bench.c renderer.o -lvulkan -o brook-desktop-chrome-bench
    $CC -std=c11 -O2 -Wall -Wextra -Werror intel_cpu_try_probe.c -o brook-intel-cpu-try-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror surface_test.c renderer.o surface.o -lvulkan -o brook-vulkan-surface-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_layers_test.c -o brook-desktop-layers-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_source_commit_test.c -o brook-desktop-source-commit-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_capture_timing.c -o brook-desktop-capture-timing
    $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-misleading-indentation desktop_caret_test.c -o brook-desktop-caret-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_supervisor.c -o brook-desktop-supervisor
    $CC -std=c11 -O2 -Wall -Wextra -Werror -DBROOK_SUPERVISOR_SERIAL=1 desktop_supervisor.c -o brook-desktop-supervisor-serial
    $CC -std=c11 -O2 -Wall -Wextra -Werror -DBROOK_SUPERVISOR_TEST_LATE_FENCE=1 desktop_supervisor.c -o brook-desktop-supervisor-test-late-fence
    $CC -std=c11 -O2 -Wall -Wextra -Werror -DBROOK_SUPERVISOR_TEST_BLOCKED_FENCE=1 desktop_supervisor.c -o brook-desktop-supervisor-test-blocked-fence
    $CC -std=c11 -O2 -Wall -Wextra -Werror -DBROOK_SUPERVISOR_TEST_CHROME_SPANS=1 desktop_supervisor.c -o brook-desktop-supervisor-test-chrome-spans
    $CC -std=c11 -O2 -Wall -Wextra -Werror -DBROOK_SUPERVISOR_TEST_GPU_SOURCE=1 desktop_supervisor.c -o brook-desktop-supervisor-test-gpu-source
    $CC -std=c11 -O2 -Wall -Wextra -Werror -DBROOK_SUPERVISOR_TEST_SELECTIVE_CAPTURE=1 desktop_supervisor.c -o brook-desktop-supervisor-test-selective-capture
    $CC -std=c11 -O2 -Wall -Wextra -Werror -DBROOK_SUPERVISOR_TEST_SELECTIVE_TIMING=1 desktop_supervisor.c -o brook-desktop-supervisor-test-selective-timing
    python3 desktop_supervisor_test.py ./brook-desktop-supervisor
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_backdrop_test.c -o brook-desktop-backdrop-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_chrome_test.c -o brook-desktop-chrome-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-misleading-indentation desktop_chrome_present_test.c -o brook-desktop-chrome-present-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_layers_vulkan_test.c renderer.o -lvulkan -o brook-desktop-layers-vulkan-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_renderer.c renderer.o graphics_image.o graphics_pass.o -lvulkan -o brook-desktop-renderer
    $CC -std=c11 -O2 -Wall -Wextra -Werror -DBROOK_SUPERVISOR_DIAGNOSTIC_REUSE_BACKDROP=1 desktop_supervisor.c -o brook-desktop-supervisor-reuse-backdrop
    $CC -std=c11 -O2 -Wall -Wextra -Werror -DBROOK_SUPERVISOR_DIAGNOSTIC_BARE=1 desktop_supervisor.c -o brook-desktop-supervisor-bare
    $CC -std=c11 -O2 -Wall -Wextra -Werror -DBROOK_RENDER_TEST_SOURCE_CRASH=1 desktop_renderer.c renderer.o graphics_image.o graphics_pass.o -lvulkan -o brook-desktop-renderer-test-source-crash
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_renderer_client_test.c -o brook-desktop-renderer-client-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_present_test.c -o brook-desktop-present-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_same_title_test.c -o brook-desktop-same-title-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_opacity_present_test.c -o brook-desktop-opacity-present-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-misleading-indentation desktop_legacy_present_test.c -o brook-desktop-legacy-present-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror -Wno-misleading-indentation desktop_occlusion_present_test.c -o brook-desktop-occlusion-present-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_damage_test.c -o brook-desktop-damage-test
    ./brook-desktop-damage-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror source_size_policy_test.c -o brook-desktop-source-size-policy-test
    ./brook-desktop-source-size-policy-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_content_layer_test.c -o brook-desktop-content-layer-test
    ./brook-desktop-content-layer-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_chrome_spans_test.c -o brook-desktop-chrome-spans-test
    ./brook-desktop-chrome-spans-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_fence_recovery_test.c -o brook-desktop-fence-recovery-test
    ./brook-desktop-fence-recovery-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_blocked_recovery_test.c -o brook-desktop-blocked-recovery-test
    ./brook-desktop-blocked-recovery-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_blocked_hardware_test.c -o brook-desktop-blocked-hardware-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_independent_queue_test.c -o brook-desktop-independent-queue-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_shared_queue_test.c -o brook-desktop-shared-queue-test
    $CC -std=c11 -O2 -Wall -Wextra -Werror desktop_tile_test.c -o brook-desktop-tile-test
    ./brook-desktop-tile-test
  '';
  installPhase = ''
    mkdir -p $out/bin $out/lib $out/include
    cp brook-same-title-gpu-probe brook-pair-pixel-probe $out/bin/
    cp brook-vulkan-compositor-test brook-vulkan-external-compositor-test brook-vulkan-multi-compositor-test brook-vulkan-surface-test $out/bin/
    cp brook-vulkan-cross-process-test $out/bin/
    cp brook-vulkan-async-pair-test $out/bin/
    cp brook-desktop-chrome-bench $out/bin/
    cp brook-intel-cpu-try-test $out/bin/
    cp brook-vulkan-timeout-test $out/bin/
    cp brook-desktop-layers-test $out/bin/
    cp brook-desktop-source-commit-test $out/bin/
    cp brook-desktop-capture-timing $out/bin/
    cp brook-desktop-caret-test $out/bin/
    cp brook-desktop-supervisor brook-desktop-supervisor-serial $out/bin/
    cp brook-desktop-supervisor-test-late-fence $out/bin/
    cp brook-desktop-supervisor-test-blocked-fence $out/bin/
    cp brook-desktop-supervisor-test-chrome-spans $out/bin/
    cp brook-desktop-supervisor-test-gpu-source $out/bin/
    cp brook-desktop-supervisor-test-selective-capture $out/bin/
    cp brook-desktop-supervisor-test-selective-timing $out/bin/
    cp brook-desktop-backdrop-test $out/bin/
    cp brook-desktop-chrome-test $out/bin/
    cp brook-desktop-chrome-present-test $out/bin/
    cp brook-desktop-layers-vulkan-test $out/bin/
    cp brook-desktop-renderer $out/bin/
    cp brook-desktop-supervisor-bare brook-desktop-supervisor-reuse-backdrop $out/bin/
    cp brook-desktop-renderer-test-source-crash $out/bin/
    cp brook-desktop-renderer-client-test $out/bin/
    cp brook-desktop-blocked-hardware-test $out/bin/
    cp brook-desktop-independent-queue-test $out/bin/
    cp brook-desktop-shared-queue-test $out/bin/
    cp brook-desktop-present-test $out/bin/
    cp brook-desktop-same-title-test $out/bin/
    cp brook-desktop-opacity-present-test $out/bin/
    cp brook-desktop-legacy-present-test $out/bin/
    cp brook-desktop-occlusion-present-test $out/bin/
    cp libbrook-vulkan-compositor.a $out/lib/
    cp renderer.h scene.h surface.h graphics_image.h graphics_layers.h graphics_pass.h $out/include/
  '';
}
