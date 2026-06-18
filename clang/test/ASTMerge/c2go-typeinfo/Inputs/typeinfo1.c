// AST merge input #1: a managed record NodeA434 and a function that uses
// __c2go_typeinfo(struct NodeA434). When this AST is merged into a host TU,
// the C2GoTypeInfoAttr's originating RecordDecl must be re-pointed into the
// host context; otherwise the descriptor degrades into a bare extern.

typedef __SIZE_TYPE__ size_t;
extern void *gc_malloc(const void *type_info, size_t n);

struct __attribute__((c2go_managed)) NodeA434 {
  struct NodeA434 *next;
  long             tag;
};

void *alloc_node_a434(void) {
  return gc_malloc(__c2go_typeinfo(struct NodeA434), sizeof(struct NodeA434));
}
