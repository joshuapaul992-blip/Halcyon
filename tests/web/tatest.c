/* Host test of tools/mktrust.py: the trust anchors it generated from the CA
 * bundle must equal what BearSSL's own decoders make of the same PEM file
 * (web_session_add_ca_pem), certificate by certificate.
 *   tatest cacert.pem */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "webint.h"

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    static char buf[1 << 20];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    web_session_t *s = web_session_new();
    int added = web_session_add_ca_pem(s, buf, n);
    int fails = 0;
    if ((unsigned)added != web_trust_anchors_count) {
        printf("FAIL: %d anchors from PEM, %u generated\n", added, web_trust_anchors_count);
        fails++;
    }
    for (int i = 0; i < added && (unsigned)i < web_trust_anchors_count; i++) {
        const br_x509_trust_anchor *a = &web_trust_anchors[i], *b = &s->extra[i];
        int bad = a->dn.len != b->dn.len || memcmp(a->dn.data, b->dn.data, a->dn.len) ||
                  a->pkey.key_type != b->pkey.key_type || !(b->flags & BR_X509_TA_CA);
        if (!bad && a->pkey.key_type == BR_KEYTYPE_RSA)
            bad = a->pkey.key.rsa.nlen != b->pkey.key.rsa.nlen || memcmp(a->pkey.key.rsa.n, b->pkey.key.rsa.n, a->pkey.key.rsa.nlen) ||
                  a->pkey.key.rsa.elen != b->pkey.key.rsa.elen || memcmp(a->pkey.key.rsa.e, b->pkey.key.rsa.e, a->pkey.key.rsa.elen);
        else if (!bad)
            bad = a->pkey.key.ec.curve != b->pkey.key.ec.curve || a->pkey.key.ec.qlen != b->pkey.key.ec.qlen ||
                  memcmp(a->pkey.key.ec.q, b->pkey.key.ec.q, a->pkey.key.ec.qlen);
        if (bad) { printf("FAIL: anchor %d differs\n", i); fails++; }
    }
    web_session_free(s);
    printf("trust anchors: %u compared with BearSSL's decoder, %d failures\n", web_trust_anchors_count, fails);
    return fails != 0;
}
