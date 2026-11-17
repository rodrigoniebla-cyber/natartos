/* setperm <in-policy> <out-policy> <domain> : mark <domain> permissive (additive). */
#include <stdio.h>
#include <stdlib.h>
#include <sepol/policydb/policydb.h>
#include <sepol/policydb/ebitmap.h>
#include <sepol/policydb/hashtab.h>

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: %s in out domain\n", argv[0]); return 2; }
    FILE *fin = fopen(argv[1], "r");
    if (!fin) { perror("open in"); return 1; }

    policydb_t pol;
    policy_file_t pf;
    policy_file_init(&pf);
    pf.type = PF_USE_STDIO;
    pf.fp = fin;
    if (policydb_init(&pol)) { fprintf(stderr, "policydb_init\n"); return 1; }
    if (policydb_read(&pol, &pf, 0)) { fprintf(stderr, "policydb_read failed\n"); return 1; }
    fclose(fin);

    type_datum_t *t = hashtab_search(pol.p_types.table, argv[3]);
    if (!t) { fprintf(stderr, "type '%s' not found\n", argv[3]); return 1; }
    printf("domain '%s' value=%u flavor=%d\n", argv[3], t->s.value, t->flavor);

    if (ebitmap_set_bit(&pol.permissive_map, t->s.value, 1)) {
        fprintf(stderr, "ebitmap_set_bit failed\n"); return 1;
    }
    printf("set permissive bit for value %u\n", t->s.value);

    FILE *fout = fopen(argv[2], "w");
    if (!fout) { perror("open out"); return 1; }
    policy_file_t pfo;
    policy_file_init(&pfo);
    pfo.type = PF_USE_STDIO;
    pfo.fp = fout;
    if (policydb_write(&pol, &pfo)) { fprintf(stderr, "policydb_write failed\n"); return 1; }
    fclose(fout);
    policydb_destroy(&pol);
    printf("wrote %s\n", argv[2]);
    return 0;
}
