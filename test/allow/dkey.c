/* dkey.c: each row's key as DynamoRIO decodes it. Columns: hash, class, text; "-" when DR has no decoding. */
#include <stdio.h>
#include "dr_api.h"
#include "allow.h"

int main(void) {
    void *dc = dr_standalone_init();
    static char line[1024];
    while (fgets(line, sizeof line, stdin)) {
        byte b[32] = {0};
        int n = 0;
        for (const char *s = line; s[0] != '\t' && s[0] != '\n' && s[0] && s[1]; s += 2) {
            unsigned v;
            sscanf(s, "%2x", &v);
            b[n++] = (byte)v;
        }
        instr_t in;
        instr_init(dc, &in);
        char text[ALLOW_TEXT_MAX];
        if (decode(dc, b, &in) == NULL || !instr_valid(&in)) puts("-\t-\t-");
        else {
            uint64 k = allow_key(dc, &in, text);
            printf("%016llx\t%d\t%s\n", (unsigned long long)k, classify(dc, &in), text);
        }
        instr_free(dc, &in);
    }
    return 0;
}
