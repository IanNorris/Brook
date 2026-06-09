echo DISKBENCH_START
echo "== PROBE snapshot A (pre-cold) =="
busybox cat /proc/blkprobe
echo "== COLD reads (first touch, virtio-blk round-trip) =="
echo "-- DOOM1.WAD 4.2MB bs=64k --"
busybox dd if=/boot/DOOM1.WAD of=/dev/null bs=64k
echo "-- WALLPAPER.RAW 8MB bs=64k --"
busybox dd if=/boot/WALLPAPER.RAW of=/dev/null bs=64k
echo "== PROBE snapshot B (post-cold) =="
busybox cat /proc/blkprobe
echo "== WARM reads (FAT cache) =="
echo "-- DOOM1.WAD again --"
busybox dd if=/boot/DOOM1.WAD of=/dev/null bs=64k
echo "-- WALLPAPER.RAW again --"
busybox dd if=/boot/WALLPAPER.RAW of=/dev/null bs=64k
echo "== PROBE snapshot C (post-warm) =="
busybox cat /proc/blkprobe
echo DISKBENCH_DONE
