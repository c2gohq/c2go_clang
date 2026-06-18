// Modules input header: a managed record Node434Mod and a `static inline`
// helper that uses __c2go_typeinfo(struct Node434Mod). When a consumer TU
// calls prime434_mod, module deserialization re-emits the helper body in
// the consumer's IR, requiring the C2GoTypeInfoAttr to survive the round
// trip so the typeinfo descriptor is emitted with an initializer.

typedef __SIZE_TYPE__ size_t_mod;
extern void *gc_malloc(const void *type_info, size_t_mod n);

struct __attribute__((c2go_managed)) Node434Mod {
  struct Node434Mod *next;
  long               tag;
};

static inline void *prime434_mod(void) {
  return gc_malloc(__c2go_typeinfo(struct Node434Mod),
                   sizeof(struct Node434Mod));
}
