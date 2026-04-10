#include "test_framework.h"
#include "string.h"

TEST_MAIN("string", {
    // -----------------------------------------------------------------------
    // memset
    // -----------------------------------------------------------------------
    {
        unsigned char buf[32];
        memset(buf, 0xAB, sizeof(buf));
        for (unsigned i = 0; i < sizeof(buf); i++)
            ASSERT_EQ(buf[i], (unsigned char)0xAB);

        memset(buf, 0, sizeof(buf));
        for (unsigned i = 0; i < sizeof(buf); i++)
            ASSERT_EQ(buf[i], (unsigned char)0);

        // Zero-length memset should be safe.
        memset(buf, 0xFF, 0);
        ASSERT_EQ(buf[0], (unsigned char)0);
    }

    // -----------------------------------------------------------------------
    // memcpy
    // -----------------------------------------------------------------------
    {
        const char src[] = "Hello, Brook!";
        char dst[32] = {};
        memcpy(dst, src, sizeof(src));
        ASSERT_EQ(memcmp(dst, src, sizeof(src)), 0);

        // Overlapping regions: memcpy is __restrict__, but verify basic behavior.
        // Zero-length copy is safe.
        memcpy(dst, src, 0);
        ASSERT_EQ(dst[0], 'H');
    }

    // -----------------------------------------------------------------------
    // memmove (handles overlap)
    // -----------------------------------------------------------------------
    {
        char buf[16] = "ABCDEFGH";

        // Forward overlap: copy [0..5] -> [2..7]
        memmove(buf + 2, buf, 6);
        ASSERT_EQ(buf[0], 'A');
        ASSERT_EQ(buf[1], 'B');
        ASSERT_EQ(buf[2], 'A');
        ASSERT_EQ(buf[3], 'B');
        ASSERT_EQ(buf[4], 'C');
        ASSERT_EQ(buf[5], 'D');
        ASSERT_EQ(buf[6], 'E');
        ASSERT_EQ(buf[7], 'F');

        // Backward overlap: copy [2..7] -> [0..5]
        char buf2[16] = "ABCDEFGH";
        memmove(buf2, buf2 + 2, 6);
        ASSERT_EQ(buf2[0], 'C');
        ASSERT_EQ(buf2[1], 'D');
        ASSERT_EQ(buf2[2], 'E');
        ASSERT_EQ(buf2[3], 'F');
        ASSERT_EQ(buf2[4], 'G');
        ASSERT_EQ(buf2[5], 'H');

        // Zero-length memmove is safe.
        memmove(buf2, buf2 + 1, 0);
        ASSERT_EQ(buf2[0], 'C');
    }

    // -----------------------------------------------------------------------
    // memcmp
    // -----------------------------------------------------------------------
    {
        const char a[] = "ABCDEF";
        const char b[] = "ABCDEF";
        const char c[] = "ABCDEG";
        const char d[] = "ABCDEE";

        ASSERT_EQ(memcmp(a, b, 6), 0);
        ASSERT_TRUE(memcmp(a, c, 6) < 0);  // 'F' < 'G'
        ASSERT_TRUE(memcmp(a, d, 6) > 0);  // 'F' > 'E'

        // Prefix match with shorter length.
        ASSERT_EQ(memcmp(a, c, 5), 0);

        // Zero-length comparison is always equal.
        ASSERT_EQ(memcmp(a, c, 0), 0);
    }

    // -----------------------------------------------------------------------
    // strlen
    // -----------------------------------------------------------------------
    {
        ASSERT_EQ(strlen(""), (size_t)0);
        ASSERT_EQ(strlen("A"), (size_t)1);
        ASSERT_EQ(strlen("Brook"), (size_t)5);
        ASSERT_EQ(strlen("Hello\0World"), (size_t)5); // stops at first NUL
    }

    // -----------------------------------------------------------------------
    // strcmp
    // -----------------------------------------------------------------------
    {
        ASSERT_EQ(strcmp("abc", "abc"), 0);
        ASSERT_TRUE(strcmp("abc", "abd") < 0);
        ASSERT_TRUE(strcmp("abd", "abc") > 0);
        ASSERT_TRUE(strcmp("abc", "abcd") < 0);  // shorter < longer
        ASSERT_TRUE(strcmp("abcd", "abc") > 0);
        ASSERT_EQ(strcmp("", ""), 0);
        ASSERT_TRUE(strcmp("", "a") < 0);
        ASSERT_TRUE(strcmp("a", "") > 0);
    }

    // -----------------------------------------------------------------------
    // strncmp
    // -----------------------------------------------------------------------
    {
        ASSERT_EQ(strncmp("abcdef", "abcxyz", 3), 0);  // first 3 match
        ASSERT_TRUE(strncmp("abcdef", "abcxyz", 4) < 0); // 'd' < 'x'
        ASSERT_EQ(strncmp("abc", "abc", 10), 0);  // n > length, still equal
        ASSERT_EQ(strncmp("abc", "xyz", 0), 0);   // zero-length always equal
        ASSERT_EQ(strncmp("", "", 5), 0);
    }

    // -----------------------------------------------------------------------
    // strchr
    // -----------------------------------------------------------------------
    {
        const char* s = "Hello, Brook!";
        ASSERT_EQ(strchr(s, 'H'), s);
        ASSERT_EQ(strchr(s, 'B'), s + 7);
        ASSERT_EQ(strchr(s, '!'), s + 12);
        ASSERT_EQ(strchr(s, '\0'), s + 13);  // finds NUL terminator
        ASSERT_EQ(strchr(s, 'Z'), (const char*)0);  // not found

        ASSERT_EQ(strchr("", 'A'), (const char*)0);
        // strchr("", '\0') should return pointer to the NUL terminator.
        {
            const char* empty = "";
            ASSERT_EQ(strchr(empty, '\0'), empty);
        }
    }

    // -----------------------------------------------------------------------
    // Edge cases: large-ish buffers (catch off-by-one in loops)
    // -----------------------------------------------------------------------
    {
        char big[256];
        memset(big, 'X', 256);
        ASSERT_EQ(big[0], 'X');
        ASSERT_EQ(big[255], 'X');

        char big2[256];
        memcpy(big2, big, 256);
        ASSERT_EQ(memcmp(big, big2, 256), 0);

        // Modify one byte and verify memcmp detects it.
        big2[128] = 'Y';
        ASSERT_TRUE(memcmp(big, big2, 256) < 0);  // 'X' < 'Y'
    }

    brook::SerialPrintf("string: all tests passed\n");
})
