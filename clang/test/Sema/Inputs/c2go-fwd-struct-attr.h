// Cross-TU test input. This header carries the forward declaration with the
// c2go_managed attribute; the including source file completes the type without
// restating the attribute on the tag. Sema must propagate the attribute from
// the forward declaration to the completed definition.
struct __attribute__((c2go_managed)) HdrRec;
