{ pkgs ? import ../../nix/nixpkgs.nix {} }:

let
  # Override the ladybird package to build tests
  ladybirdTests = pkgs.ladybird.overrideAttrs (prev: {
    pname = "ladybird-tests";

    cmakeFlags = prev.cmakeFlags ++ [
      "-DBUILD_TESTING=ON"
      "-DENABLE_GUI_TARGETS=OFF"
    ];

    # Don't wrap Qt apps (we're not building the GUI)
    dontWrapQtApps = true;

    # Build only specific test targets instead of "all"
    buildFlags = [
      "TestLibCoreStream"
      "TestLibCoreMappedFile"
      "TestLibCoreAnonymousBuffer"
      "TestLibCoreFileWatcher"
      "TestLibCoreEventLoop"
      "TestDNSResolver"
      "TestTLSHandshake"
      "TestTLSCertificateParser"
      "TestTransportSocket"
      "TestThread"
    ];

    # Custom install phase: grab test binaries + data files + shared libs
    installPhase = ''
      mkdir -p $out/bin $out/lib $out/data

      # Copy test binaries
      for name in TestLibCoreStream TestLibCoreMappedFile TestLibCoreAnonymousBuffer \
                  TestLibCoreFileWatcher TestLibCoreEventLoop TestDNSResolver \
                  TestTLSHandshake TestTLSCertificateParser TestTransportSocket TestThread; do
        if [ -f "bin/$name" ]; then
          cp "bin/$name" "$out/bin/"
        fi
      done

      # Copy all lagom shared libraries (needed at runtime)
      cp lib/liblagom-*.so* $out/lib/ 2>/dev/null || true

      # Copy test data files
      cp $src/Tests/LibCore/long_lines.txt $out/data/
      cp $src/Tests/LibCore/small.txt $out/data/ 2>/dev/null || true
      cp $src/Tests/LibCore/10kb.txt $out/data/ 2>/dev/null || true
    '';

    # Skip the standard check phase
    doCheck = false;
  });
in
  ladybirdTests
