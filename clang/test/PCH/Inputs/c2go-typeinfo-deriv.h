// Chained-PCH derived layer, built on top of c2go-typeinfo-base.h. Adds a
// `static inline` helper whose body uses __c2go_typeinfo(struct
// Node434Chain). The implicit typeinfo VarDecl is created here but
// references a record deserialized from the base layer, so both must survive
// the chained PCH round-trip with the C2GoTypeInfoAttr intact.

static inline void *prime434_chain(void) {
  return gc_malloc(__c2go_typeinfo(struct Node434Chain),
                   sizeof(struct Node434Chain));
}
