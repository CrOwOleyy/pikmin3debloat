/* Offline harness: run ssbu-recomp's Maxwell -> GLSL translator (runtime/mx_translate.c, GPL-3.0) on the
 * code blobs extracted from Pikmin 3 Deluxe's AGL shader archives (scripts/bahs_extract.py).
 *
 *   mxharness <list.txt> <outdir>
 *     list.txt: one blob path per line. Blob layout: +0x00 NVN header (0x30), +0x30 SPH (0x50), +0x80 SASS.
 *     Writes <outdir>/<n>.<vert|frag|...> for each translated stage (Vulkan GLSL, target 1) and prints one
 *     line per blob: index, status (OK / FAIL), stage, failing opcode, reason, path.
 * No constant buffer 1 contents are passed (cdata = NULL): BRX jump tables cannot be resolved. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mx_translate.h"

static const char *ext[] = { "vert", "frag", "geom", "tesc", "tese", "comp" };

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: mxharness <list.txt> <outdir>\n"); return 2; }
    FILE *lf = fopen(argv[1], "r");
    if (!lf) { perror(argv[1]); return 2; }
    char path[1024];
    int n = 0, ok = 0;
    while (fgets(path, sizeof path, lf)) {
        path[strcspn(path, "\r\n")] = 0;
        if (!path[0]) continue;
        FILE *f = fopen(path, "rb");
        if (!f) { printf("%d\tFAIL\t-\topen\tcannot open\t%s\n", n++, path); continue; }
        fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *blob = malloc(len);
        fread(blob, 1, len, f); fclose(f);
        int sph_type = blob[0x30] & 0x1f;       /* 1 = VTG (vertex here), 2 = PS */
        MxIn in; memset(&in, 0, sizeof in);
        in.stage = sph_type == 2 ? MX_FRAGMENT : MX_VERTEX;
        in.code = blob + 0x30;
        in.code_len = (int)(len - 0x30);
        in.target = 1;
        in.binding_base = 0;
        in.ssbo_base = 32;
        in.tex_base = 0;
        MxOut out; memset(&out, 0, sizeof out);
        if (mx_translate(&in, &out)) {
            char op[1100];
            snprintf(op, sizeof op, "%s/%04d.%s", argv[2], n, ext[in.stage]);
            FILE *o = fopen(op, "w");
            if (o) { fputs(out.glsl, o); fclose(o); }
            printf("%d\tOK\t%s\t-\t%d insns\t%s\n", n, ext[in.stage], out.insns, path);
            free(out.glsl);
            ok++;
        } else {
            printf("%d\tFAIL\t%s\t%s\t%s\t%s\n", n, ext[in.stage], out.op, out.why, path);
        }
        free(blob);
        n++;
    }
    fprintf(stderr, "%d blobs, %d translated\n", n, ok);
    return 0;
}
