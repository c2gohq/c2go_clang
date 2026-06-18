// A c2go_managed attribute on a forward declaration in a header must
// propagate to the completing definition in the including TU. Otherwise the
// completed record loses its addrspace(1) marking on managed-pointer fields
// and drops out of the c2go RTTI manifest.
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -I %S/Inputs -ast-dump %s | FileCheck %s
//
// RUN: %clang_cc1 -triple arm64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -I %S/Inputs -emit-llvm -O0 -o - %s | FileCheck %s --check-prefix=IR

#include "c2go-fwd-struct-attr.h"

// Definition without restating the attribute; the header's forward
// declaration is the only place that mentions c2go_managed.
struct HdrRec {
  int v;
  struct HdrRec *p;
};

struct HdrRec *use(struct HdrRec *r) { return r->p; }

// The completed record carries the attribute, and the field pointer plus the
// function signature are lowered in addrspace(1) under the Go ABI0 CC.

// CHECK: RecordDecl {{.*}} struct HdrRec definition
// CHECK-NEXT: C2GoStructAttr

// IR: %struct.HdrRec = type { i32, ptr addrspace(1) }
// IR: define {{.*}}goabi0cc ptr addrspace(1) @use(ptr addrspace(1)
