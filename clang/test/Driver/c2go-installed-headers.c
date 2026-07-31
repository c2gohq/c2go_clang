// Verify the relocatable C2Go SDK include contract.  The driver accepts an
// install-relative libc include directory only when the compiler resource
// c2go.h and the libc type header are both present, and only in C2Go mode.

// RUN: split-file %s %t
// RUN: %clang --target=x86_64-pc-windows-goabi -fc2go -fc2go-package=t \
// RUN:   -ccc-install-dir %t/sdk/bin -resource-dir=%t/resource \
// RUN:   -fsyntax-only -### %t/input.c 2>&1 | FileCheck %s --check-prefix=C2GO
// RUN: %clang --target=x86_64-pc-windows-goabi \
// RUN:   -ccc-install-dir %t/sdk/bin -resource-dir=%t/resource \
// RUN:   -fsyntax-only -### %t/input.c 2>&1 | FileCheck %s --check-prefix=PLAIN
// RUN: %clang --target=x86_64-pc-windows-goabi -fc2go -fc2go-package=t \
// RUN:   -ccc-install-dir %t/incomplete/bin -resource-dir=%t/resource \
// RUN:   -fsyntax-only -### %t/input.c 2>&1 | FileCheck %s --check-prefix=INCOMPLETE
// RUN: %clang --target=x86_64-pc-windows-goabi -fc2go -fc2go-package=t \
// RUN:   -ccc-install-dir %t/sdk/bin -resource-dir=%t/no-core-resource \
// RUN:   -fsyntax-only -### %t/input.c 2>&1 | FileCheck %s --check-prefix=NOCORE
// RUN: %clang --target=x86_64-pc-windows-goabi -fc2go -fc2go-package=t \
// RUN:   -ccc-install-dir %t/sdk/bin -resource-dir=%t/resource \
// RUN:   -nostdlibinc -fsyntax-only -### %t/input.c 2>&1 | \
// RUN:   FileCheck %s --check-prefix=NOSTDLIBINC
// RUN: %clang --target=x86_64-pc-windows-goabi -fc2go -fc2go-package=t \
// RUN:   -ccc-install-dir %t/sdk/bin -resource-dir=%t/resource \
// RUN:   -nostdinc -fsyntax-only -### %t/input.c 2>&1 | \
// RUN:   FileCheck %s --check-prefix=NOSTDINC

// C2GO: "-internal-isystem" "{{.*}}c2go-installed-headers.c.tmp{{[/\\]}}resource{{[/\\]}}include"
// C2GO-SAME: "-internal-isystem" "{{.*}}c2go-installed-headers.c.tmp{{[/\\]}}sdk{{[/\\]}}include"
// PLAIN-NOT: c2go-installed-headers.c.tmp{{[/\\]}}sdk{{[/\\]}}include
// INCOMPLETE-NOT: c2go-installed-headers.c.tmp{{[/\\]}}incomplete{{[/\\]}}include
// NOCORE-NOT: c2go-installed-headers.c.tmp{{[/\\]}}sdk{{[/\\]}}include
// NOSTDLIBINC-NOT: c2go-installed-headers.c.tmp{{[/\\]}}sdk{{[/\\]}}include
// NOSTDINC-NOT: "-internal-isystem"

//--- input.c
int value;
//--- resource/include/c2go.h
// C2Go compiler resource header sentinel.
//--- no-core-resource/include/.keep
//--- sdk/bin/.keep
//--- sdk/include/bits/alltypes.h
// C2Go libc SDK sentinel.
//--- incomplete/bin/.keep
//--- incomplete/include/stdio.h
// Missing bits/alltypes.h: this must not be accepted as C2Go libc.
