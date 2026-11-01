/* SPDX-License-Identifier: GPL-3.0-only */
/* (#76) The stock firmware reads what we write: built by run_tests.sh against the sources of the tag v2.1.1 (copied to
 * build/stock211/), it reads the records tests/ui_test.c wrote with this firmware (build/host/fm6x_*.bin): a user sound
 * and a project, each once with the FM6X block (PHASE and WAVE) and once without (the same sound at PHASE 0 SINE).
 * Stock must take both the same (the block passed over: the DX7 patch, the values, the patch's tag as they are), and
 * write the one without byte for byte as we did. Built twice: -DSTOCK_USER the user sound (upreset.c's -DUP_HOST part,
 * up_fm6.c's upf_tag cut out to build/stock211/upf_tag.h), else the project (project.c's -DPROJ_HOST part, SKF4) */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef STOCK_USER
#define __attribute__(x)
#define UP_HOST 1
#include "../firmware/src/core.h"
static int st_read(uint32_t off, void *dst, uint32_t n) { (void)off; memset(dst, 0xFF, n); return 0; }
static int st_erase(uint32_t off) { (void)off; return 0; }
static int st_prog(uint32_t off, const void *src, uint32_t n) { (void)off; (void)src; (void)n; return 0; }
#include "../firmware/src/storage.c"
#include "../firmware/src/upreset.c"
#include "upf_tag.h"
#define ENGI_FM6 12u                                     /* (eng_fm6.c: not in the -DUP_HOST part) */
#else
#define main hostsim_main
#include "hostsim.c"
#undef main
#define PROJ_HOST 1
#include "../firmware/src/project.c"
#endif

static int check(const char *what, int ok)
{
    printf("%-100s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

static int load(const char *dir, const char *name, void *b, size_t n)
{
    char path[256];
    FILE *f;
    size_t got;
    snprintf(path, sizeof path, "%s/%s", dir, name);
    if (!(f = fopen(path, "rb")))
        return 0;
    got = fread(b, 1, n, f);
    fclose(f);
    return got == n;
}

#ifdef STOCK_USER
static int test(const char *dir)
{
    static up_rec_t r[2], again;
    static const int16_t def[P_COUNT];
    int16_t v[2][P_COUNT];
    uint32_t tag;
    uint8_t in[2 * sizeof(up_rec_t) + 4u];
    int bad = 0;
    if (!load(dir, "fm6x_user.bin", in, sizeof in))
        return check("stock v2.1.1: build/host/fm6x_user.bin (tests/ui_test.c writes it)", 0);
    memcpy(r, in, sizeof r);
    memcpy(&tag, in + sizeof r, 4);
    bad += check("stock v2.1.1: our user sound with the FM6X block and without: valid FM6 records, the block there",
                 up_valid(&r[0]) && up_valid(&r[1]) && r[0].engine == ENGI_FM6 && memcmp(&r[0], &r[1], sizeof r[0]));
    up_params(&r[0], v[0], def);
    up_params(&r[1], v[1], def);
    bad += check("  .. the same values", !memcmp(v[0], v[1], sizeof v[0]));
    bad += check("  .. our patch's tag is stock's upf_tag of the record with the block: the DX7 patch loads",
                 upf_tag(&r[0]) == tag);
    again = r[1];
    up_set_sound(&again, v[1]);
    bad += check("  .. stock writes the one without byte for byte as we did", !memcmp(&again, &r[1], sizeof again));
    again = r[0];
    up_set_sound(&again, v[0]);
    bad += check("  .. and saved again on stock, the one with the block becomes it (the block dropped)",
                 !memcmp(&again, &r[1], sizeof again));
    return bad;
}
#else
static int test(const char *dir)
{
    static uint8_t in[2][PROJ_STORE4];
    static project_t q[2];
    static project_store_t again;
    int bad = 0;
    if (!load(dir, "fm6x_proj.bin", in, sizeof in))
        return check("stock v2.1.1: build/host/fm6x_proj.bin (tests/ui_test.c writes it)", 0);
    bad += check("stock v2.1.1: our SKF4 project with the FM6X block and without: both load (hash, layout)",
                 proj_unpack4(&q[0], in[0]) && proj_unpack4(&q[1], in[1]) && memcmp(in[0], in[1], PROJ_STORE4));
    bad += check("  .. into the same project: the FM6 patches, every track and value", !memcmp(&q[0], &q[1], sizeof q[0]));
    bad += check("  .. stock writes the one without byte for byte as we did",
                 proj_pack4(&again, &q[1]) && !memcmp(again.raw, in[1], PROJ_STORE4));
    bad += check("  .. and saved again on stock, the one with the block becomes it (the block dropped)",
                 proj_pack4(&again, &q[0]) && !memcmp(again.raw, in[1], PROJ_STORE4));
    return bad;
}
#endif

int main(int argc, char **argv)
{
    int bad = test(argc > 1 ? argv[1] : "build/host");
    if (bad)
        printf("stock_compat: %d FAILED\n", bad);
    return bad != 0;
}
