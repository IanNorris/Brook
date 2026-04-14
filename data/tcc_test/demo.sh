#!/boot/BIN/BASH
echo "=== Brook OS - TCC Compiler Demo ==="
echo ""
echo "--- Source code (hello.c) ---"
busybox cat /boot/SRC/hello.c
echo ""
echo "--- Compiling with TCC ---"
/boot/BIN/tcc -B /boot/TCC/lib/tcc -I /boot/TCC/include -L /boot/TCC/lib -static -nostdlib -o /boot/hello /boot/SRC/hello.c /boot/TCC/lib/crt1.o /boot/TCC/lib/crti.o /boot/TCC/lib/crtn.o /boot/TCC/lib/libc.a
echo "Compile exit code: $?"
echo ""
echo "--- Binary info ---"
busybox ls -la /boot/hello
echo ""
echo "--- Running compiled binary ---"
/boot/hello
echo ""
echo "=== Demo complete ==="
busybox sleep 120
