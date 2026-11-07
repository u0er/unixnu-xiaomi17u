/* Candidate A ptrauth semantics probe; it does not implement a compatibility shim. */
#include <ptrauth.h>

struct label;
struct four_argument {
    struct label * __ptrauth(ptrauth_key_process_independent_data, 1,
        ptrauth_string_discriminator("ucred.cr_label"), "authenticates-null-values") cr_label;
};
struct three_argument {
    struct label * __ptrauth(ptrauth_key_process_independent_data, 1,
        ptrauth_string_discriminator("ucred.cr_label")) cr_label;
};

#ifdef __cplusplus
extern "C" {
#endif
struct four_argument four_global = { 0 };
struct label *four_load(struct four_argument *x) { return x->cr_label; }
void four_store(struct four_argument *x, struct label *v) { x->cr_label = v; }
void four_clear(struct four_argument *x) { x->cr_label = 0; }
struct label *three_load(struct three_argument *x) { return x->cr_label; }
void three_store(struct three_argument *x, struct label *v) { x->cr_label = v; }
void three_clear(struct three_argument *x) { x->cr_label = 0; }
#ifdef __cplusplus
}
#endif
