// PCH input header for c2go-typeinfo.c: a managed record and a
// `static inline` helper that uses __c2go_typeinfo(struct Node392PCH). The
// helper is `static inline` so the consumer TU re-codegens it from the
// deserialized AST, requiring the C2GoTypeInfoAttr to survive the PCH round
// trip.

typedef __SIZE_TYPE__ size_t_t;
extern void *gc_malloc(const void *type_info, size_t_t n);

struct __attribute__((c2go_managed)) Node392PCH {
  struct Node392PCH *next;
  long               tag;
};

static inline void *prime392_pch(void) {
  return gc_malloc(__c2go_typeinfo(struct Node392PCH),
                   sizeof(struct Node392PCH));
}
