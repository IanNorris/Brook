#!/boot/BIN/BASH
echo TCC_START
/boot/BIN/tcc -B /boot/TCC/lib/tcc -I /boot/TCC/include -L /boot/TCC/lib -static -nostdlib -o /boot/hello /boot/SRC/hello.c /boot/TCC/lib/crt1.o /boot/TCC/lib/crti.o /boot/TCC/lib/crtn.o /boot/TCC/lib/libc.a
echo TCC_EXIT=$?
busybox ls -la /boot/hello
/boot/hello
echo HELLO_EXIT=$?
echo TCC_DONE
exit 0
