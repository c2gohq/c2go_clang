// Spelling-drift lock for the c2go.* metadata names that a minimal c2go C
// source can elicit. Producers (clang frontend) and consumers (the late c2go
// passes) must agree on these names byte-for-byte; any drift silently drops
// the contract and corrupts GC far downstream, so pin it as a build-time
// failure here. This is NOT a full enumeration - only the spellings reachable
// from this source through the frontend and the three pipelines below.
//
// Three RUN lines cover three emit windows:
//   * RAW    (frontend only, -disable-llvm-passes): module flags, named MD,
//            instruction MD the optimizer later consumes, reserved GV prefixes.
//   * LATE   (-O0, runs the late c2go passes): spellings written by the
//            safepoint and write-barrier passes (c2go.zeroinit, c2go.wb.done,
//            c2go.safepoint.next.id).
//   * ESCAPE (-mllvm -c2go-escape-check): the c2go.escape.site site-string
//            global, emitted only when the escape-check gate is on.
//
// REQUIRES: aarch64-registered-target
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -mllvm -c2go-managed-addrspace -Wno-c2go-managed-as1 \
// RUN:   -disable-llvm-passes -emit-llvm -o %t.raw.ll %s
// RUN: FileCheck %s --check-prefix=RAW --input-file=%t.raw.ll
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -mllvm -c2go-managed-addrspace -Wno-c2go-managed-as1 \
// RUN:   -O0 -emit-llvm -o %t.late.ll %s
// RUN: FileCheck %s --check-prefix=LATE --input-file=%t.late.ll
//
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -mllvm -c2go-managed-addrspace -Wno-c2go-managed-as1 \
// RUN:   -mllvm -c2go-escape-check \
// RUN:   -O0 -emit-llvm -o %t.escape.ll %s
// RUN: FileCheck %s --check-prefix=ESCAPE --input-file=%t.escape.ll

#if !defined(__C2GO__)
#  error "needs -fc2go"
#endif

#define c2go_extern  __attribute__((c2go_extern))

#pragma c2go managed(6) push
struct __attribute__((c2go_managed)) Node {
  struct Node *next;
  long         tag;
};
struct __attribute__((c2go_managed)) TwoPtrs {
  struct Node *a;
  struct Node *b;
};
// UAlt holds an anonymous union whose alternatives are BOTH pointers at the
// same offset (a pure pointer slot). It stays GC-tracked: the heap typeinfo
// scans that single word precisely and the synthesised `c2go.anon.<hash>`
// record name is emitted. A punning union (pointer aliased with a scalar)
// inside a GC-tracked record is a hard error (see c2go-union-punning-error.c),
// so the pure-slot form is used here. The stack ambig-words path is exercised
// separately by `mk_alt`.
struct __attribute__((c2go_managed)) UAlt {
  union {
    struct Node *p;
    void        *q;
  } u;
};
#pragma c2go pop

static struct Node    *gA;
static struct TwoPtrs  gB;

// c2go_extern global -> triggers `!c2go.var` GV metadata.
c2go_extern long ext_global;
long ext_global;

c2go_extern long ref_globals(void);
long ref_globals(void) {
  gA = 0;
  gB.a = 0;
  gB.b = 0;
  return 0;
}

// Boundary-symbol variadic -> emits `c2go.func.log_msg` manifest MD.
c2go_extern void log_msg(const char *fmt, ...);
void log_msg(const char *fmt, ...) {}

// Internal (non-extern) variadic -> drives the void** vararg pack lowering:
// c2go.va.argptrs alloca + c2go.va.slot storage + c2go.va.elt GEP +
// `!c2go.ptr.managed !{!"c2go.va"}` tag + `!c2go.va.pack` tag.
int int_log(const char *fmt, ...);
int int_log(const char *fmt, ...) { return 0; }
void caller_to_internal(void) { int_log("%d %d", 1, 2); }

// Local managed struct alloca -> `!c2go.ptr.managed !{!"Node"}`.
long mk_local_node(void) {
  struct Node n = {0, 7};
  return n.tag;
}

// A plain (untracked) struct whose anonymous union puns a pointer word with a
// scalar. It gets no heap typeinfo (so no force-scan error), but a stack local
// of this type still drives the `union.ambig.words` mark-skip, keeping
// copystack from treating the ambiguous word as a live pointer.
struct UPun {
  union {
    struct Node *p;
    long         i;
  } u;
};

// Local punning union -> `!c2go.union.ambig.words`. UAlt (above) provides the
// `c2go.anon.<hash>` synthesised record name via its pure-slot anon union.
long mk_alt(long i) {
  struct UPun a; a.u.i = i;
  return a.u.i;
}

// Aggregate copy of c2go_managed -> frontend attaches `!c2go.elem.type` on
// the @llvm.memcpy/memmove intrinsic. The memcpy-typing pass consumes the tag
// (replacing the intrinsic with a typed runtime helper), so the RAW path is
// the only place this spelling is observable on a sealed .ll.
void copy_node(struct Node *d, struct Node *s) { *d = *s; }
void copy_array(struct Node *d, struct Node *s) {
  __builtin_memmove(d, s, sizeof(struct Node) * 10);
}

// Non-null managed-ptr store -> the write-barrier pass (LATE) attaches
// `!c2go.wb.done` to its fast-path store.
void link_nodes(struct Node *a, struct Node *b) { a->next = b; }

// malloc returning a c2go_managed* -> `!c2go.alloc.target.type` on the call.
extern void *malloc(unsigned long);
struct Node *make_node(void) {
  return (struct Node *)malloc(sizeof(struct Node));
}

// Pointer-typed local alloca -> `!c2go.ptr.slot` for copystack relocation.
extern long g_int;
long ptr_slot(void) {
  long *p = &g_int;
  return *p;
}

// va_start / va_arg -> AArch64 c2go lowering emits c2go.va.base / cur /
// argp / next IR value names on the cursor loads/GEPs.
#include <stdarg.h>
int sum_internal(const char *fmt, ...);
int sum_internal(const char *fmt, ...) {
  va_list ap;
  __builtin_va_start(ap, fmt);
  int s = __builtin_va_arg(ap, int) + __builtin_va_arg(ap, int);
  __builtin_va_end(ap);
  return s;
}

// =========================================================================
//                                RAW assertions
// =========================================================================
// Everything the clang frontend stamps directly: module flags, NamedMD seed
// list, per-global gcmask GVs and var-MD; typeinfo / gcbitmap /
// go_owned_globals / elem.type / alloc.target.type; struct and func manifest
// MD; ptr.managed / ptr.slot / union.ambig.words; va.pack on the argptrs and
// slot allocas; the vararg cursor IR value names; the c2go._gotype struct type
// and the c2go.anon.<hash> synthesised record name.
//
// --- Module flags ---
// RAW-DAG: !"c2go.goabi"
// RAW-DAG: !"c2go.opt-level"
// RAW-DAG: !"c2go.target-cpu"
// RAW-DAG: !"c2go.target-features"
// RAW-DAG: !"c2go.manifest.schema"
// RAW-DAG: !"c2go.pkgpath"
//
// --- NamedMDNode keys ---
// RAW-DAG: !c2go.safepoint.callees =
// RAW-DAG: !c2go.go_owned_globals =
// RAW-DAG: !c2go.func.log_msg =
// RAW-DAG: !c2go.func.ref_globals =
// RAW-DAG: !c2go.struct.Node =
// RAW-DAG: !c2go.struct.Node.fields =
// RAW-DAG: !c2go.struct.Node.meta =
// RAW-DAG: !c2go.struct.Node.godef =
// RAW-DAG: !c2go.struct.TwoPtrs =
// UAlt is a plain (untracked) struct, so it gets no struct.UAlt MD. The
// unique-to-UAlt signals that remain are the stack-local `union.ambig.words`
// (asserted below) and the synthesised `c2go.anon.<hash>` record name.
//
// --- Reserved GlobalVariable name prefixes ---
// RAW-DAG: @c2go.global.gcmask.gA
// RAW-DAG: @c2go.global.gcmask.gB
// RAW-DAG: @c2go.typeinfo.Node
// RAW-DAG: @c2go.typeinfo.TwoPtrs
// RAW-DAG: @c2go.gcbitmap.Node
// RAW-DAG: @c2go.gcbitmap.TwoPtrs
// RAW-DAG: %c2go._gotype = type
//
// --- Instruction-level metadata ---
// RAW-DAG: !c2go.ptr.managed
// RAW-DAG: !c2go.ptr.slot
// RAW-DAG: !c2go.elem.type
// RAW-DAG: !c2go.alloc.target.type
// RAW-DAG: !c2go.union.ambig.words
// RAW-DAG: !c2go.var
// RAW-DAG: !c2go.va.pack
// RAW-DAG: !{!"c2go.va"}
//
// --- Synthesised record name + vararg IR value names ---
// RAW-DAG: c2go.anon.{{[0-9a-f]+}}
// RAW-DAG: %c2go.va.argptrs
// RAW-DAG: %c2go.va.slot
// RAW-DAG: %c2go.va.elt
// RAW-DAG: %c2go.va.base
// RAW-DAG: %c2go.va.cur
// RAW-DAG: %c2go.va.argp
// RAW-DAG: %c2go.va.next

// =========================================================================
//                               LATE assertions
// =========================================================================
// Emitted only when the late c2go passes run: c2go.zeroinit and the
// c2go.safepoint.next.id module flag (safepoint pass); c2go.wb.done
// (write-barrier pass).
//
// LATE-DAG: !c2go.zeroinit
// LATE-DAG: !c2go.wb.done
// LATE-DAG: !"c2go.safepoint.next.id"

// =========================================================================
//                              ESCAPE assertions
// =========================================================================
// The escape-check pass is gated by `-mllvm -c2go-escape-check` (default off).
// Each instrumented stack->heap store gets a private `@c2go.escape.site*`
// site-string operand. The RAW/LATE runs do not enable the gate, so this
// spelling is unique to the ESCAPE run.
//
// ESCAPE-DAG: @c2go.escape.site
