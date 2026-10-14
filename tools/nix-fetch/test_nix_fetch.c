/* Host regression checks for long dependency lists and failed closure walks. */
#define main nix_fetch_main
#include "nix-fetch.c"
#undef main
#include <assert.h>

int main(void) {
    char metadata[MAX_NARINFO] = "References: ";
    char refs[MAX_REFERENCES];
    for (int i = 0; i < 92; ++i) {
        char ref[128];
        snprintf(ref, sizeof(ref), "%032d-dependency-with-a-long-package-name ", i);
        strcat(metadata, ref);
    }
    strcat(metadata, "\n");
    assert(strlen(metadata) > 4096);
    assert(narinfo_field(metadata, "References", refs, sizeof(refs)));
    assert(strstr(refs, "00000000000000000000000000000091-dependency"));
    char short_buf[16];
    assert(!narinfo_field(metadata, "References", short_buf, sizeof(short_buf)));

    char temp[] = "/tmp/brook-nix-fetch-test-XXXXXX";
    assert(mkdtemp(temp));
    g_store_dir = temp;
    g_narinfo_cache_dir = temp;
    g_cache_disabled = 0;
    g_curl_path = "/bin/false";
    g_seen_count = 0;
    /* A bad cached narinfo fails without network or archive extraction. */
    char hash[33] = "00000000000000000000000000000001";
    char path[MAX_PATH];
    narinfo_cache_path(hash, path, sizeof(path));
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("StorePath: /nix/store/invalid\n", f);
    assert(fclose(f) == 0);
    assert(fetch_dependencies("00000000000000000000000000000001-missing") < 0);
    g_seen_count = 0;
    char root_hash[33] = "00000000000000000000000000000002";
    char root_path[MAX_PATH], root_metadata[MAX_PATH];
    snprintf(root_path, sizeof(root_path), "%s/%s-existing", temp, root_hash);
    assert(mkdir(root_path, 0755) == 0);
    narinfo_cache_path(root_hash, root_metadata, sizeof(root_metadata));
    f = fopen(root_metadata, "w");
    assert(f);
    fprintf(f, "StorePath: /nix/store/%s-existing\nURL: nar/unused\nReferences: %s-missing\n", root_hash, hash);
    assert(fclose(f) == 0);
    g_fetch_deps = 1;
    assert(fetch_package(root_hash, 0) < 0);
    /* Content-addressed leaf narinfos can omit References completely. */
    f = fopen(root_metadata, "w");
    assert(f);
    fprintf(f, "StorePath: /nix/store/%s-existing\nURL: nar/unused\n", root_hash);
    assert(fclose(f) == 0);
    g_seen_count = 0;
    assert(fetch_package(root_hash, 0) == 0);
    assert(unlink(root_metadata) == 0);
    assert(rmdir(root_path) == 0);
    assert(unlink(path) == 0);
    assert(rmdir(temp) == 0);

    g_seen_count = 0;
    for (int i = 0; i < 300; ++i) {
        snprintf(hash, sizeof(hash), "%032d", i);
        hash_mark_seen(hash);
    }
    assert(hash_is_seen("00000000000000000000000000000299"));
    g_seen_count = MAX_SEEN;
    assert(fetch_package("99999999999999999999999999999999", 0) < 0);

    char pipeline_temp[] = "/tmp/brook-nix-pipeline-test-XXXXXX";
    assert(mkdtemp(pipeline_temp));
    snprintf(path, sizeof(path), "%s/curl", pipeline_temp);
    f = fopen(path, "w");
    assert(f);
    fputs("#!/bin/sh\nexec sleep 60\n", f);
    assert(fclose(f) == 0);
    assert(chmod(path, 0755) == 0);
    g_curl_path = path;
    char unpack_binary[MAX_PATH];
    g_unpack_path = find_binary("BROOK_TEST_FALSE", "false", NULL, 0,
                                unpack_binary, sizeof(unpack_binary));
    assert(g_unpack_path);
    g_cacert_path = NULL;
    long began = now_ms();
    assert(download_and_extract("unused", "none", "/unused") < 0);
    assert(now_ms() - began < 2000);
    assert(unlink(path) == 0);
    assert(rmdir(pipeline_temp) == 0);
    puts("nix-fetch closure regression checks passed");
    return 0;
}
