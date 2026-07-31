// Verify the relocatable C2Go SDK include contract.  The driver accepts an
// install-relative include directory only when both identifying headers are
// present, and only in C2Go mode.

// RUN: split-file %s %t
// RUN: %clang --target=x86_64-pc-windows-goabi -fc2go -fc2go-package=t \
// RUN:   -ccc-install-dir %t/sdk/bin -resource-dir=%S/Inputs/resource_dir \
// RUN:   -fsyntax-only -### %t/input.c 2>&1 | FileCheck %s --check-prefix=C2GO
// RUN: %clang --target=x86_64-pc-windows-goabi \
// RUN:   -ccc-install-dir %t/sdk/bin -resource-dir=%S/Inputs/resource_dir \
// RUN:   -fsyntax-only -### %t/input.c 2>&1 | FileCheck %s --check-prefix=PLAIN
// RUN: %clang --target=x86_64-pc-windows-goabi -fc2go -fc2go-package=t \
// RUN:   -ccc-install-dir %t/incomplete/bin -resource-dir=%S/Inputs/resource_dir \
// RUN:   -fsyntax-only -### %t/input.c 2>&1 | FileCheck %s --check-prefix=INCOMPLETE
// RUN: %clang --target=x86_64-pc-windows-goabi -fc2go -fc2go-package=t \
// RUN:   -ccc-install-dir %t/sdk/bin -resource-dir=%S/Inputs/resource_dir \
// RUN:   -nostdlibinc -fsyntax-only -### %t/input.c 2>&1 | \
// RUN:   FileCheck %s --check-prefix=NOSTDLIBINC
// RUN: %clang --target=x86_64-pc-windows-goabi -fc2go -fc2go-package=t \
// RUN:   -ccc-install-dir %t/sdk/bin -resource-dir=%S/Inputs/resource_dir \
// RUN:   -nostdinc -fsyntax-only -### %t/input.c 2>&1 | \
// RUN:   FileCheck %s --check-prefix=NOSTDINC

// C2GO: "-internal-isystem" "{{.*}}Inputs{{[/\\]}}resource_dir{{[/\\]}}include"
// C2GO-SAME: "-internal-isystem" "{{.*}}c2go-installed-headers.c.tmp{{[/\\]}}sdk{{[/\\]}}include"
// PLAIN-NOT: c2go-installed-headers.c.tmp{{[/\\]}}sdk{{[/\\]}}include
// INCOMPLETE-NOT: c2go-installed-headers.c.tmp{{[/\\]}}incomplete{{[/\\]}}include
// NOSTDLIBINC-NOT: c2go-installed-headers.c.tmp{{[/\\]}}sdk{{[/\\]}}include
// NOSTDINC-NOT: "-internal-isystem"

//--- input.c
int value;
//--- sdk/bin/.keep
//--- sdk/include/c2go.h
// C2Go SDK sentinel.
//--- sdk/include/bits/alltypes.h
// C2Go libc SDK sentinel.
//--- incomplete/bin/.keep
//--- incomplete/include/c2go.h
// Missing bits/alltypes.h: this must not be accepted as a C2Go SDK.
