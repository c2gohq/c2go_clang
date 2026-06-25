/*===---- c2go.h - c2go-mode core macros and built-ins ---------------------===
 *
 * Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
 * See https://llvm.org/LICENSE.txt for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===-----------------------------------------------------------------------===
 *
 * Included by c2go-managed C source files. Defines the world attributes,
 * pragma helpers, and a small built-in surface (gc_malloc, errno
 * accessor). The bulk of libc is in <c2go-libc/include/*.h> headers; this
 * file is the c2go-mode core.
 *
 * Reference: docs/c2go_design.md (v14).
 */

#ifndef __C2GO_H
#define __C2GO_H

#if !defined(__clang_c2go__) && !defined(__C2GO__)
#error "<c2go.h> requires a c2go-mode clang (--target=<arch>-<vendor>-<os>-goabi)"
#endif

#ifdef __cplusplus
#error "<c2go.h> is C-only; c2go-mode does not support C++."
#endif

/*===-- World attributes ----------------------------------------------------
 * v14 §3 world system. Attach to Records, fields, params, vars, functions.
 * Default world per c2go-mode = `managed`; pragma push/pop overrides.
 *===-----------------------------------------------------------------------*/

/* The underlying clang attributes are spelled `c2go_managed` /
 * `c2go_unmanaged` to avoid collision with HIPManaged's existing
 * `__attribute__((managed))`. User code uses the short macros. */
#define managed         __attribute__((c2go_managed))
#define unmanaged       __attribute__((c2go_unmanaged))
#define c2go_extern     __attribute__((c2go_extern))

/* Named selector values (use instead of bare 0 / 1). */
/* c2go_extern Go-name casing (its optional int arg): the bare `c2go_extern`
 * macro is the default (C2GO_EXPORTED). Passing C2GO_KEEPCASE needs the raw
 * attribute (`__attribute__((c2go_extern(C2GO_KEEPCASE)))`) because the
 * object-like `c2go_extern` macro shadows the attribute spelling, so a
 * convenience wrapper macro can't carry the arg. Keep-case is currently
 * unused; these names document the values. */
#define C2GO_EXPORTED   1   /* default: Go name upper-first (foo -> Foo) */
#define C2GO_KEEPCASE   0   /* keep the C symbol's casing */

/* c2go_linkname target-ABI selector (its optional 2nd arg): */
#define C2GO_GOABI0     1   /* target provides an ABI0 entry -> direct reference */
/* (omit the 2nd arg) -> external Go symbol (ABIInternal) -> alias-then-wrap stub */

/* c2go_linkname takes a Go symbol name and an OPTIONAL 2nd selector. Variadic
 * so both forms work:
 *   c2go_linkname("pkg.Sym")               // external Go symbol -> stub
 *   c2go_linkname("pkg.Sym", C2GO_GOABI0)  // target has ABI0 -> direct
 */
#define c2go_linkname(...) __attribute__((c2go_linkname(__VA_ARGS__)))

/* c2go_returntype(struct X): marks a c2go_linkname/c2go_extern function as
 * returning a Go multi-value tuple. The named C struct's fields correspond
 * 1:1 (order + type) to the called Go function's return values; the call
 * lowers each field as its own Go ABI0 result slot (placed after the args
 * on the caller's frame) instead of a hidden sret pointer. The function's
 * declared return type must be that same struct. See design.md §P5. */
#define c2go_returntype(T) __attribute__((c2go_returntype(T)))

/*===-- C heap -------------------------------------------------------------
 * Use plain malloc/free from <stdlib.h> for unmanaged C-heap memory
 * (default-unmanaged; not GC-tracked). The old cmalloc/ccalloc/crealloc/cfree
 * escape hatch is removed — stdlib.h malloc is the unmanaged allocator now.
 *===-----------------------------------------------------------------------*/

/*===-- GC allocation (v15 §P4) -------------------------------------------
 * malloc/calloc/realloc/free are NOT rewritten to runtime.mallocgc; they
 * lower to ordinary libc calls. GC-tracked allocation is now explicit.
 *
 * gc_malloc allocates `n` zeroed bytes on the Go heap, tracked by the GC
 * according to `type_info` (a *runtime._type; null = noscan blob). The
 * type_info pointer is obtained via the c2go_typeinfo() RTTI surface below.
 * Routes to runtime.mallocgc(n, type_info, needzero=true).
 * See design.md §6.1 / §4.6.5.
 *===-----------------------------------------------------------------------*/

void *gc_malloc(const void *type_info, __SIZE_TYPE__ n)
    c2go_linkname("github.com/c2gohq/c2go_libc.GCMalloc", C2GO_GOABI0);

/*===-- RTTI: *runtime._type for a managed type (v15 §P4 / §4.6.5) ---------
 * The Go-runtime `*_type` for a managed Record type T lives in a per-type
 * Go variable `_typeinfo_<T>` that c2gobind emits in the generated `.go`
 * (extracted from reflect.TypeFor[T]() via the iface-header data word).
 * C type identity is erased at runtime, so there is no generic runtime
 * function that can compute it.
 *
 * `c2go_typeinfo(T)` resolves the descriptor automatically: clang's
 * `__c2go_typeinfo` built-in (which takes a *type*, like sizeof) emits an
 * extern reference to the `_typeinfo_<T>` symbol c2gobind defines, and
 * yields the *runtime._type pointer value. No manual extern declaration is
 * needed — pass the struct/union type directly.
 *
 * Usage:
 *   struct Node { ... };
 *   void *p = gc_malloc(c2go_typeinfo(struct Node), sizeof(struct Node));
 *===-----------------------------------------------------------------------*/

#define c2go_typeinfo(T) (__c2go_typeinfo(T))

/*===-- Callback: c2go function -> unmanaged native fn-ptr ----------------
 * c2go_callback(fn) yields an unmanaged, native-callable function pointer for
 * the c2go function `fn`, suitable to hand to an unmanaged_extern library that
 * expects a C callback (e.g. qsort's comparator). clang's `__c2go_callback`
 * built-in synthesizes a per-fn cdecl trampoline (emitted by c2gobind) that
 * marshals the native C-ABI arguments into a Go ABI0 frame and re-enters `fn`
 * through runtime.cgocallback, so a foreign thread's call lands safely in the
 * Go world. The result type is `fn`'s signature as an unmanaged function
 * pointer.
 *
 * Usage:
 *   extern void qsort(void *, __SIZE_TYPE__, __SIZE_TYPE__,
 *                     int (*)(const void *, const void *));
 *   int cmp(const void *a, const void *b) { ... }
 *   qsort(base, n, sz, c2go_callback(cmp));
 *===-----------------------------------------------------------------------*/

#define c2go_callback(fn) (__c2go_callback(fn))

/*===-- Callout: extern-import host fn -> internal-callable fn-ptr ---------
 * c2go_callout(fn) yields an internal-callable function pointer to the
 * `unmanaged extern` import `fn` — the symmetric dual of c2go_callback. Where
 * c2go_callback hands a c2go function to the host, c2go_callout hands a host
 * function to c2go: the result is a normal callable/storable c2go function
 * pointer that bridges to the host (through the import's generated wrapper) when
 * called.
 *
 * A direct call `fn(...)` ALREADY uses that wrapper automatically — you only
 * need c2go_callout when you want the function POINTER itself (to store it or
 * pass it around within c2go). It takes the function NAME, not an address.
 * (`&fn` is different: that is the RAW host address, of type
 * `unmanaged import func ptr`, which is NOT callable from c2go and is for
 * handing back to the host.)
 *
 * For an import with a scalar/pointer/float signature the result is a plain
 * function pointer (assign it directly). For a record-by-value or variadic
 * signature the result carries the import's stack-ABI calling convention, so
 * hold it via __typeof__(c2go_callout(fn)).
 *
 * Usage:
 *   extern int host_cmp(const void *, const void *);  // unmanaged extern
 *   int (*p)(const void *, const void *) = c2go_callout(host_cmp);
 *   int r = p(a, b);                                  // bridges to the host
 *===-----------------------------------------------------------------------*/

#define c2go_callout(fn) (__c2go_callout(fn))

/*===-- GC array allocation (v15 §P4) -------------------------------------
 * gc_malloc_array(type_info, elem_size, count) allocates a contiguous,
 * GC-tracked array of `count` elements, each `elem_size` bytes, scanned per
 * `type_info`. It forwards `elem_size * count` zeroed bytes to GCMalloc /
 * runtime.mallocgc; the Go runtime repeats the element type's pointer bitmap
 * across the whole block (the same mechanism `make([]T, n)` uses), so the GC
 * scans every element precisely as long as the total is an exact multiple of
 * the element size. Pass c2go_typeinfo(struct T) for type_info and
 * sizeof(struct T) for elem_size.
 *===-----------------------------------------------------------------------*/

static inline void *gc_malloc_array(const void *type_info,
                                    __SIZE_TYPE__ elem_size,
                                    __SIZE_TYPE__ count) {
  return gc_malloc(type_info, elem_size * count);
}

/*===-- errno accessor (per-goroutine via GLS) ----------------------------*/

extern int *__c2go_errno_ptr(void)
    c2go_linkname("github.com/c2gohq/c2go_libc.ErrnoPtr", C2GO_GOABI0);

#define errno (*__c2go_errno_ptr())

/*===-- Go built-in types (v15 §P5) ---------------------------------------
 * C representations of Go's built-in aggregate types, matching their exact
 * in-memory layout on 64-bit so they can be passed and returned across the
 * C↔Go (Go ABI0) boundary. `__c2go_intptr` is Go's `int`/`intptr` (signed,
 * pointer-width). These are plain structs by value:
 *
 *   c2go_slice   ↔ Go `[]T`     : {data, len, cap}        (3 words)
 *   c2go_string  ↔ Go `string`  : {data, len}             (2 words)
 *   c2go_iface   ↔ Go interface : {tab, data}             (2 words)
 *   c2go_error   ↔ Go `error`   : a non-empty interface   (== c2go_iface)
 *
 * The interface/error layout is the (itab, data) word pair; the `__c2go_iface`
 * tag is also what the c2go front end recognizes to reject interface-method
 * calls (dynamic dispatch through an interface value is not a static Go ABI0
 * symbol and cannot be lowered as a c2go_linkname call). See design.md §P5.
 *===-----------------------------------------------------------------------*/

typedef __INTPTR_TYPE__ __c2go_intptr;

typedef struct __c2go_slice {
  void *data;
  __c2go_intptr len;
  __c2go_intptr cap;
} c2go_slice;

typedef struct __c2go_string {
  const char *data;
  __c2go_intptr len;
} c2go_string;

typedef struct __c2go_iface {
  void *tab;
  void *data;
} c2go_iface;

/* Go's `error` is a (non-empty) interface — same (itab, data) layout. */
typedef c2go_iface c2go_error;

/*===-- Predefined macros (set by driver) ---------------------------------
 *   __C2GO__             — 1 when in c2go-mode
 *   __clang_c2go__       — clang's c2go-mode marker
 *   __C2GO_TARGET_GO__   — Go runtime version range, e.g. "1.22-1.25"
 *===-----------------------------------------------------------------------*/

#endif /* __C2GO_H */
