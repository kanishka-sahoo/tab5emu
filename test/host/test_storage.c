#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "retro_os.h"
#include "retro_storage.h"
#include "test_util.h"

static char s_root[RETRO_PATH_MAX];

/* Native path of a logical one. */
static const char *native(const char *path)
{
    static char buf[RETRO_PATH_MAX];
    if (!retro_storage_resolve(path, buf, sizeof(buf))) {
        return "";
    }
    return buf;
}

/* Write a file directly, bypassing the atomic path (to fake a crash). */
static void put_raw(const char *path, const char *text)
{
    FILE *f = fopen(native(path), "wb");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static bool native_exists(const char *path)
{
    struct stat st;
    return stat(native(path), &st) == 0;
}

/* Contents of a file, "" if missing. */
static const char *contents(const char *path)
{
    static char buf[256];
    size_t len = 0;
    char *p = retro_storage_read_file(path, &len, RETRO_MEM_ANY);
    if (!p) {
        return "";
    }
    snprintf(buf, sizeof(buf), "%s", p);
    retro_mem_free(p);
    return buf;
}

static void test_volumes(void)
{
    CHECK(retro_storage_volume_count() == 1);
    retro_volume_info_t v;
    CHECK(retro_storage_volume_info(0, &v));
    CHECK_STR(v.name, "sd");
    CHECK_STR(v.mount, RETRO_STORAGE_SD);
    CHECK(v.mounted);
    CHECK(!retro_storage_volume_info(1, &v));
    CHECK(!retro_storage_volume_info(-1, &v));
    CHECK(retro_storage_mounted("sd"));
    CHECK(!retro_storage_mounted("usb0"));
}

static void test_resolve(void)
{
    char out[RETRO_PATH_MAX], want[RETRO_PATH_MAX + 16];
    CHECK(retro_storage_resolve("/storage/sd/retro/a.txt", out, sizeof(out)));
    snprintf(want, sizeof(want), "%s/retro/a.txt", s_root);
    CHECK_STR(out, want);
    CHECK(retro_storage_resolve("/storage/sd", out, sizeof(out)));
    CHECK_STR(out, s_root);

    CHECK(!retro_storage_resolve("/storage/sdx/a", out, sizeof(out))); /* prefix only */
    CHECK(!retro_storage_resolve("/storage/usb0/a", out, sizeof(out)));
    CHECK(!retro_storage_resolve("/storage", out, sizeof(out)));
    CHECK(!retro_storage_resolve("/sd/a", out, sizeof(out)));
    CHECK(!retro_storage_resolve("storage/sd/a", out, sizeof(out)));
    CHECK(!retro_storage_resolve(NULL, out, sizeof(out)));
    /* Doesn't fit. */
    CHECK(!retro_storage_resolve("/storage/sd/a", out, strlen(s_root) + 2));
    CHECK(retro_storage_resolve("/storage/sd/a", out, strlen(s_root) + 3));
}

static void test_mkdirs(void)
{
    CHECK(retro_storage_mkdirs("/storage/sd/m1/m2/m3"));
    struct stat st;
    CHECK(stat(native("/storage/sd/m1/m2/m3"), &st) == 0 && S_ISDIR(st.st_mode));
    /* Again: already there is fine. So is the volume root, and a trailing
     * slash. */
    CHECK(retro_storage_mkdirs("/storage/sd/m1/m2/m3"));
    CHECK(retro_storage_mkdirs("/storage/sd"));
    CHECK(retro_storage_mkdirs("/storage/sd/m1/t/"));
    CHECK(stat(native("/storage/sd/m1/t"), &st) == 0 && S_ISDIR(st.st_mode));
    /* A file in the way. */
    put_raw("/storage/sd/m1/file", "x");
    CHECK(!retro_storage_mkdirs("/storage/sd/m1/file"));
    CHECK(!retro_storage_mkdirs("/storage/sd/m1/file/sub"));
    CHECK(!retro_storage_mkdirs("/storage/usb0/x"));
}

static void test_exists_size_remove(void)
{
    const char *p = "/storage/sd/esr.bin";
    CHECK(!retro_storage_exists(p));
    CHECK(retro_storage_size(p) == -1);
    put_raw(p, "hello");
    CHECK(retro_storage_exists(p));
    CHECK(retro_storage_size(p) == 5);
    CHECK(retro_storage_remove(p));
    CHECK(!retro_storage_exists(p));
    CHECK(!retro_storage_remove(p));
    CHECK(!retro_storage_exists("/storage/nope/x"));
}

static void test_fopen(void)
{
    FILE *f = retro_storage_fopen("/storage/sd/fo.txt", "w");
    CHECK(f != NULL);
    if (f) {
        fputs("abc", f);
        fclose(f);
    }
    CHECK(retro_storage_size("/storage/sd/fo.txt") == 3);
    CHECK(retro_storage_fopen("/storage/usb0/fo.txt", "w") == NULL);
}

static void test_write_atomic_and_read(void)
{
    const char *p = "/storage/sd/at/deep/er/cfg.json";
    CHECK(retro_storage_write_atomic(p, "first", 5)); /* creates parents */
    size_t len = 0;
    char *d = retro_storage_read_file(p, &len, RETRO_MEM_ANY);
    CHECK(d != NULL && len == 5);
    if (d) {
        CHECK_STR(d, "first"); /* NUL-terminated */
        retro_mem_free(d);
    }
    CHECK(retro_storage_write_atomic(p, "2nd", 3)); /* replaces, shorter */
    CHECK_STR(contents(p), "2nd");
    CHECK(retro_storage_size(p) == 3);
    /* No leftovers. */
    CHECK(!native_exists("/storage/sd/at/deep/er/cfg.json.tmp"));
    CHECK(!native_exists("/storage/sd/at/deep/er/cfg.json.new"));
    /* Empty file. */
    CHECK(retro_storage_write_atomic(p, "", 0));
    d = retro_storage_read_file(p, &len, RETRO_MEM_ANY);
    CHECK(d != NULL && len == 0 && d[0] == '\0');
    retro_mem_free(d);
    /* Missing file, len untouched. */
    len = 77;
    CHECK(retro_storage_read_file("/storage/sd/at/none", &len, RETRO_MEM_ANY) == NULL);
    CHECK(len == 77);
    CHECK(!retro_storage_write_atomic("/storage/usb0/x", "a", 1));
}

static void test_recover_new_without_target(void)
{
    /* Crash after unlinking the target, before the final rename. */
    const char *p = "/storage/sd/r1.json";
    put_raw("/storage/sd/r1.json.new", "new");
    CHECK(retro_storage_recover(p));
    CHECK(!native_exists("/storage/sd/r1.json.new"));
    CHECK_STR(contents(p), "new");
}

static void test_recover_new_beats_old(void)
{
    /* Crash before unlinking the old target: .new is complete and newer. */
    const char *p = "/storage/sd/r2.json";
    put_raw(p, "old");
    put_raw("/storage/sd/r2.json.new", "new");
    CHECK(retro_storage_recover(p));
    CHECK_STR(contents(p), "new");
    CHECK(!native_exists("/storage/sd/r2.json.new"));
}

static void test_recover_tmp_is_discarded(void)
{
    /* Crash while writing .tmp: partial, so the old file stays. */
    const char *p = "/storage/sd/r3.json";
    put_raw(p, "old");
    put_raw("/storage/sd/r3.json.tmp", "partial");
    CHECK(retro_storage_recover(p));
    CHECK_STR(contents(p), "old");
    CHECK(!native_exists("/storage/sd/r3.json.tmp"));

    /* .tmp with no target: nothing to recover. */
    put_raw("/storage/sd/r4.json.tmp", "partial");
    CHECK(!retro_storage_recover("/storage/sd/r4.json"));
    CHECK(!native_exists("/storage/sd/r4.json.tmp"));
}

static void test_read_file_recovers_first(void)
{
    const char *p = "/storage/sd/r5.json";
    put_raw(p, "old");
    put_raw("/storage/sd/r5.json.new", "fresh");
    put_raw("/storage/sd/r5.json.tmp", "junk");
    CHECK_STR(contents(p), "fresh");
    CHECK(!native_exists("/storage/sd/r5.json.new"));
    CHECK(!native_exists("/storage/sd/r5.json.tmp"));
}

static void test_write_atomic_over_leftovers(void)
{
    const char *p = "/storage/sd/r6.json";
    put_raw("/storage/sd/r6.json.new", "stale new");
    put_raw("/storage/sd/r6.json.tmp", "stale tmp");
    CHECK(retro_storage_write_atomic(p, "mine", 4));
    CHECK_STR(contents(p), "mine");
    CHECK(!native_exists("/storage/sd/r6.json.new"));
    CHECK(!native_exists("/storage/sd/r6.json.tmp"));
}

static void test_recover_dir(void)
{
    retro_storage_mkdirs("/storage/sd/rd/sub/subsub");
    put_raw("/storage/sd/rd/a.json.new", "a");                 /* promoted */
    put_raw("/storage/sd/rd/b.json", "b-old");
    put_raw("/storage/sd/rd/b.json.new", "b");                 /* promoted */
    put_raw("/storage/sd/rd/c.json", "c-old");
    put_raw("/storage/sd/rd/c.json.tmp", "junk");              /* deleted */
    put_raw("/storage/sd/rd/sub/d.json.new", "d");             /* promoted */
    put_raw("/storage/sd/rd/sub/subsub/e.json.new", "e");      /* promoted */
    put_raw("/storage/sd/rd/sub/subsub/f.json.tmp", "junk");   /* deleted */
    put_raw("/storage/sd/rd/plain.txt", "p");

    /* Non-recursive: only the top level. */
    CHECK(retro_storage_recover_dir("/storage/sd/rd", false) == 2);
    CHECK_STR(contents("/storage/sd/rd/a.json"), "a");
    CHECK_STR(contents("/storage/sd/rd/b.json"), "b");
    CHECK(!native_exists("/storage/sd/rd/c.json.tmp"));
    CHECK_STR(contents("/storage/sd/rd/c.json"), "c-old");
    CHECK(native_exists("/storage/sd/rd/sub/d.json.new"));

    CHECK(retro_storage_recover_dir("/storage/sd/rd", true) == 2);
    CHECK_STR(contents("/storage/sd/rd/sub/d.json"), "d");
    CHECK_STR(contents("/storage/sd/rd/sub/subsub/e.json"), "e");
    CHECK(!native_exists("/storage/sd/rd/sub/subsub/f.json.tmp"));
    CHECK_STR(contents("/storage/sd/rd/plain.txt"), "p");

    /* Nothing left to do. */
    CHECK(retro_storage_recover_dir("/storage/sd/rd", true) == 0);
    CHECK(retro_storage_recover_dir("/storage/sd/missing", true) == 0);
    CHECK(retro_storage_recover_dir("/storage/usb0", true) == 0);
}

static void test_recover_dir_many_leftovers(void)
{
    /* More leftovers than one scan batch, with a subdirectory among them:
     * every file is repaired and the subdirectory is still visited. */
    retro_storage_mkdirs("/storage/sd/many/zsub");
    char p[64];
    for (int i = 0; i < 40; i++) {
        snprintf(p, sizeof(p), "/storage/sd/many/f%02d.json.new", i);
        put_raw(p, "n");
        snprintf(p, sizeof(p), "/storage/sd/many/g%02d.json.tmp", i);
        put_raw(p, "t");
    }
    put_raw("/storage/sd/many/zsub/z.json.new", "z");
    CHECK(retro_storage_recover_dir("/storage/sd/many", true) == 41);
    for (int i = 0; i < 40; i++) {
        snprintf(p, sizeof(p), "/storage/sd/many/f%02d.json", i);
        CHECK(retro_storage_exists(p));
        snprintf(p, sizeof(p), "/storage/sd/many/g%02d.json.tmp", i);
        CHECK(!native_exists(p));
    }
    CHECK_STR(contents("/storage/sd/many/zsub/z.json"), "z");
}

int main(void)
{
    /* A fresh SD root, before the first storage call. */
    snprintf(s_root, sizeof(s_root), "%s/retro_sd_XXXXXX",
             getenv("TMPDIR") && *getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    /* TMPDIR on macOS ends in '/'; collapse the "//". */
    char *dbl = strstr(s_root, "//");
    if (dbl) {
        memmove(dbl, dbl + 1, strlen(dbl));
    }
    if (!mkdtemp(s_root)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("RETRO_SD_ROOT", s_root, 1);
    if (!retro_storage_init()) {
        fprintf(stderr, "storage init failed\n");
        return 1;
    }

    RUN(test_volumes);
    RUN(test_resolve);
    RUN(test_mkdirs);
    RUN(test_exists_size_remove);
    RUN(test_fopen);
    RUN(test_write_atomic_and_read);
    RUN(test_recover_new_without_target);
    RUN(test_recover_new_beats_old);
    RUN(test_recover_tmp_is_discarded);
    RUN(test_read_file_recovers_first);
    RUN(test_write_atomic_over_leftovers);
    RUN(test_recover_dir);
    RUN(test_recover_dir_many_leftovers);

    if (g_test_failures == 0) {
        char cmd[RETRO_PATH_MAX + 16];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", s_root);
        if (system(cmd) != 0) {
            fprintf(stderr, "couldn't remove %s\n", s_root);
        }
    }
    return TEST_EXIT();
}
