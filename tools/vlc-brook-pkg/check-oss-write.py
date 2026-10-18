#!/usr/bin/env python3
"""Compile VLC's actual OSS Play callback against bounded device fault injection."""
import argparse
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
PREAMBLE = r'''
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
typedef struct { int fd; } aout_sys_t;
typedef struct { aout_sys_t *sys; } audio_output_t;
typedef struct { unsigned char *p_buffer; size_t i_buffer; } block_t;
enum { AOUT_RESTART_OUTPUT = 1 };
static unsigned mode, calls, accepted, restarts, releases, errors;
static unsigned char payload[8];
static ssize_t device_write(int fd, const void *data, size_t size) {
    assert(fd == 7 && data == payload + accepted && size == 8 - accepted);
    if (++calls > 12) { fputs("NO_PROGRESS_LOOP\n", stderr); exit(75); }
    if ((mode == 2 || mode == 6) && calls == 1) { errno=EINTR; return -1; }
    if (mode == 4) return 0;
    if (mode == 3 || mode == 6) { errno=EPIPE; return -1; }
    if (mode == 7) { errno=EAGAIN; return -1; }
    if (mode == 8) { errno=ENODEV; return -1; }
    if (mode == 5 && calls > 1) { errno=EIO; return -1; }
    size_t count = mode == 1 || mode == 5 ? 2 : size;
    accepted += count;
    return count;
}
static void release_block(block_t *block) { (void)block; ++releases; }
static void restart_output(audio_output_t *aout, unsigned flags) {
    (void)aout; assert(flags == AOUT_RESTART_OUTPUT); ++restarts;
}
#define write device_write
#define block_Release release_block
#define aout_RestartRequest restart_output
#define msg_Err(...) (++errors)
#define vlc_strerror_c strerror
'''
MAIN = r'''
int main(int argc, char **argv) {
    assert(argc == 2); mode = atoi(argv[1]);
    aout_sys_t sys = {7}; audio_output_t aout = {&sys};
    block_t block = {payload, mode == 9 ? 0 : sizeof(payload)};
    Play(&aout, &block);
    assert(releases == 1);
    int failed = mode >= 3 && mode <= 8;
    assert(restarts == (unsigned)failed);
    assert(accepted == (mode == 9 ? 0 : failed ? mode == 5 ? 2 : 0 : 8));
    assert(calls == (mode == 9 ? 0 : mode == 1 ? 4 :
                     mode == 2 || mode == 5 || mode == 6 ? 2 : 1));
    assert(errors == (unsigned)(failed || (PROBE_ORIGINAL && mode == 2)));
    printf("OSSWRITE PASS mode=%u accepted=%u restarts=%u\n", mode, accepted, restarts);
    return 0;
}
'''


def callback(source):
    start = source.index('static void Play (audio_output_t *aout, block_t *block)\n{')
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--patched-source', action='store_true')
    args = parser.parse_args()
    relative = Path('modules/audio_output/oss.c')
    source = (args.source / relative).read_text()
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)

        def check(text, patched):
            c = root / 'probe.c'
            binary = root / 'probe'
            c.write_text(PREAMBLE + callback(text) + MAIN)
            subprocess.run([args.cc, '-std=c11', '-UNDEBUG',
                            '-DPROBE_ORIGINAL=' + str(int(not patched)),
                            str(c), '-o', str(binary)], check=True)
            failed = []
            for mode in range(10):
                result = subprocess.run([str(binary), str(mode)], capture_output=True, text=True, timeout=3)
                if result.returncode:
                    failed.append(mode)
                    if not patched:
                        assert result.returncode == 75 and 'NO_PROGRESS_LOOP' in result.stderr
                else:
                    print(('PATCHED ' if patched else 'ORIGINAL ') + result.stdout.strip())
            assert failed == ([] if patched else [3, 4, 5, 6, 7, 8]), failed
            if not patched:
                print('ORIGINAL bounded no-progress loops reproduced:', failed)

        if not args.patched_source:
            check(source, False)
            target = root / relative
            target.parent.mkdir(parents=True)
            target.write_text(source)
            subprocess.run(['patch', '-p1', '-i', str(HERE / 'oss-write-recovery.patch')], cwd=root, check=True)
            source = target.read_text()
        check(source, True)


if __name__ == '__main__':
    main()
