#include "../../drivers/virtio_gpu/virgl_submit.h"
#include <stdio.h>
#include <stdlib.h>
#include <vector>

static void check(bool ok) { if (!ok) abort(); }
static void command(std::vector<uint32_t>& words, uint32_t id, uint32_t payload)
{
    words.push_back((payload << 16) | id);
    words.insert(words.end(), payload, 0);
}
int main()
{
    // The cap would fall immediately after the second vertex-buffer bind.
    // Rewind to the first completed draw rather than orphaning its successor.
    std::vector<uint32_t> words;
    command(words, 6, 3); command(words, 8, 3);
    command(words, 6, 3); command(words, 8, 3);
    check(VirglStreamValid(words.data(), words.size(), 12));
    check(VirglChunkEnd(words.data(), words.size(), 0, 12) == 8);
    check(VirglChunkEnd(words.data(), words.size(), 8, 12) == 16);
    check(VirglChunkEnd(words.data(), words.size(), 0, 16) == 16);
    // A state/upload-only batch still splits at complete command boundaries.
    words.clear(); command(words, 43, 3); command(words, 43, 3); command(words, 43, 3);
    check(VirglChunkEnd(words.data(), words.size(), 0, 9) == 8);
    // Reject truncated and individually oversized commands before submission.
    check(!VirglStreamValid(words.data(), words.size() - 1, 9));
    check(!VirglStreamValid(words.data(), words.size(), 3));
    check(!VirglStreamValid(words.data(), 0, 9));
    puts("virgl submit boundaries: PASS");
}
