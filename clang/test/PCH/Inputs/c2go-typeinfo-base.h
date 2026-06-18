// Chained-PCH base layer: declares the managed record Node434Chain only.
// The deriv layer (c2go-typeinfo-deriv.h) adds a `static inline` helper that
// uses __c2go_typeinfo(struct Node434Chain). Splitting the record and the
// typeinfo use across two PCH layers forces both the record and the implicit
// typeinfo VarDecl through two ASTReader stages.

typedef __SIZE_TYPE__ size_t_chain;
extern void *gc_malloc(const void *type_info, size_t_chain n);

struct __attribute__((c2go_managed)) Node434Chain {
  struct Node434Chain *next;
  long                 tag;
};
