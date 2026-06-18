// AST merge input #2: mirrors typeinfo1.c with a distinct managed record
// NodeB434, so the merge exercises two independent record + typeinfo pairs.

typedef __SIZE_TYPE__ size_t;
extern void *gc_malloc(const void *type_info, size_t n);

struct __attribute__((c2go_managed)) NodeB434 {
  struct NodeB434 *prev;
  long             tag;
};

void *alloc_node_b434(void) {
  return gc_malloc(__c2go_typeinfo(struct NodeB434), sizeof(struct NodeB434));
}
