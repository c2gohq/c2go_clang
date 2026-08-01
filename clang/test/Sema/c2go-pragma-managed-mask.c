// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -fsyntax-only -verify %s
// RUN: %clang_cc1 -triple aarch64-unknown-none-goabi -fc2go -std=c2go23 \
// RUN:   -ast-dump %s | FileCheck %s

// expected-no-diagnostics

#include <c2go.h>

#define C2GO_DATA (C2GO_PTR | C2GO_RECORD)
#define C2GO_NESTED ((C2GO_PTR) | (C2GO_RECORD))

#pragma c2go managed(C2GO_DATA) push
struct MacroMask {
  int *next;
};
#pragma c2go pop

#pragma c2go managed(C2GO_NESTED) push
struct NestedMask {
  int *next;
};
#pragma c2go pop

#pragma c2go managed(C2GO_FUNC | C2GO_PTR | C2GO_RECORD) push
int current_package_function(int);
#pragma c2go pop
int outside_package_function(int);

// CHECK-LABEL: RecordDecl {{.*}} struct MacroMask definition
// CHECK: C2GoStructAttr {{.*}} Implicit
// CHECK: FieldDecl {{.*}} next 'int *'
// CHECK-NEXT: C2GoManagedAttr {{.*}} Implicit

// CHECK-LABEL: RecordDecl {{.*}} struct NestedMask definition
// CHECK: C2GoStructAttr {{.*}} Implicit
// CHECK: FieldDecl {{.*}} next 'int *'
// CHECK-NEXT: C2GoManagedAttr {{.*}} Implicit

// CHECK-LABEL: FunctionDecl {{.*}} current_package_function 'int (int)'
// CHECK-NOT: C2GoUnmanagedAttr
// CHECK-LABEL: FunctionDecl {{.*}} outside_package_function 'int (int)'
// CHECK: C2GoUnmanagedAttr {{.*}} Implicit
