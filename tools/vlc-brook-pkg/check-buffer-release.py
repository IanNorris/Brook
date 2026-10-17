#!/usr/bin/env python3
"""Exercise the actual VLC callbacks before/after our patch, without a display.

Pass the matching VLC source directory (the package derivation's `src`). This
checks picture ownership, not libwayland event delivery or guest shutdown.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
PREAMBLE = r'''
#include <assert.h>
#include <stdio.h>
#include <stddef.h>
struct wl_buffer { void *user; unsigned destroyed; };
typedef struct picture {
    void *p_sys;
    struct picture *backing;
    unsigned refs, releases, destroyed;
} picture_t;
static unsigned duplicate_releases;
static void wl_buffer_set_user_data(struct wl_buffer *b, void *p) { b->user=p; }
static void *wl_buffer_get_user_data(struct wl_buffer *b) { return b->user; }
static void wl_buffer_destroy(struct wl_buffer *b) { ++b->destroyed; }
/* Keep dead test objects addressable to detect stale releases deterministically.
 * Pool clones own a reference to a distinct backing picture, as in VLC. */
static void picture_Release(picture_t *p) {
    ++p->releases;
    if (!p->refs) { ++duplicate_releases; return; }
    if (--p->refs==0) {
        ++p->destroyed;
        if (p->backing) picture_Release(p->backing);
    }
}
'''
MAIN = r'''
int main(void) {
    unsigned failures=0;
    for (unsigned mode=0;mode<5;++mode) {
        struct wl_buffer buffer={0};
        picture_t original={.p_sys=&buffer,.refs=1};
        picture_t clone={.p_sys=&buffer,.backing=&original,.refs=1};
        picture_t next={.p_sys=&buffer,.backing=&original,.refs=1};
        duplicate_releases=0;
        if (mode!=2) {
            ++original.refs;
            wl_buffer_set_user_data(&buffer,&clone); /* Prepare */
            if (mode!=1) buffer_release_cb(&clone,&buffer);
            if (mode>=3) {
                ++original.refs;
                wl_buffer_set_user_data(&buffer,&next); /* Prepare new clone */
                if (mode==3) buffer_release_cb(&next,&buffer);
            }
        }
        PictureDetach(NULL,&original); /* picture_pool_Enum uses original */
        int ok=!duplicate_releases && original.refs==1 && !original.destroyed
            && buffer.destroyed==1;
        if (mode!=2) ok=ok && clone.releases==1 && clone.destroyed==1;
        if (mode>=3) ok=ok && next.releases==1 && next.destroyed==1;
        picture_Release(&original); /* picture_pool_Release */
        ok=ok && original.refs==0 && original.destroyed==1;
        printf("BUFFERRELEASE %s mode=%u duplicate=%u\n",ok?"PASS":"FAIL",mode,duplicate_releases);
        failures+=!ok;
    }
    printf("BUFFERRELEASE failures=%u\n",failures);
    return failures?1:0;
}
'''


def function(source, name):
    start = source.index('static void ' + name + '(')
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    relative = Path('modules/video_output/wayland/shm.c')
    original = (args.source / relative).read_text()
    with tempfile.TemporaryDirectory(prefix='brook-vlc-release-') as tmp:
        directory = Path(tmp)
        target = directory / relative
        target.parent.mkdir(parents=True)
        target.write_text(original)
        with (ROOT / 'buffer-release.patch').open('rb') as patch:
            subprocess.run(['patch', '-p1'], stdin=patch, cwd=directory, check=True)
        for label, source in [('upstream', original), ('patched', target.read_text())]:
            text = PREAMBLE + '\n' + function(source, 'buffer_release_cb') + '\n' + function(source, 'PictureDetach') + MAIN
            harness = directory / (label + '.c')
            harness.write_text(text)
            for release in [False, True]:
                binary = directory / (label + ('-release' if release else '-debug'))
                subprocess.run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                                *(['-DNDEBUG'] if release else []), str(harness), '-o', str(binary)], check=True)
                result = subprocess.run([str(binary)], text=True, stdout=subprocess.PIPE, check=False)
                print(binary.name + ':\n' + result.stdout, end='')
                expected = 2 if label == 'upstream' and release else 0
                if result.returncode != bool(expected) or f'BUFFERRELEASE failures={expected}\n' not in result.stdout:
                    raise RuntimeError('Unexpected callback ownership results')
    print('PASS: upstream release defect reproduced; patched debug/release ownership passed')


if __name__ == '__main__':
    main()
