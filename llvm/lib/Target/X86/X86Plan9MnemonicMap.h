//===-- X86Plan9MnemonicMap.h - Plan-9 x86_64 mnemonic/reg mapping --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// c2go (Wave Y / #298) — 纯数据头, 由 Track C 的 X86Plan9InstPrinter 消费.
//
//   * 任务背景
//     Go runtime 的 amd64 汇编采用 Plan-9 风格 (Go obj/x86), 与 LLVM 默认的
//     AT&T 语法在 mnemonic / register 命名两个维度上系统性偏离. Wave Y 的
//     Track B 负责把这套偏离 "记账" 成单一可信表, Track C 据此实现 .s 发射.
//
//   * 参考 (reference source)
//       - gosrc/src/cmd/internal/obj/x86/anames.go    : 全部 Plan-9 mnemonic
//       - gosrc/src/cmd/internal/obj/x86/list6.go     : 全部 Plan-9 register
//       - gosrc/src/cmd/internal/obj/x86/asm6.go      : 编码 / 别名规则
//       - gosrc/src/runtime/asm_amd64.s               : 真用例 (preamble / call)
//       - /Users/dexter/Downloads/abitest_amd64/asm_amd64.s
//                                                     : #485 hand-written 8 用例
//
//   * Plan-9 x86_64 风格 — 高层规则速记
//     1. 操作数顺序: src, dst  (与 AT&T 一致, 与 Intel 反).
//     2. 立即数前缀: `$N`        (与 AT&T 一致, 不带 `#`).
//     3. mnemonic 自带 size suffix (B/W/L/Q), e.g. `MOVQ` / `ADDL` /
//        `CMPB`, 因此 register 不区分 64/32/16 名 (AX 既是 RAX 也是 EAX/AX,
//        靠 mnemonic 选定 width). 低 8 位另起一组 (AL/BL/CL/DL/SPB/BPB/SIB/
//        DIB/R8B..R15B).
//     4. AT&T 的 register 前缀 `%`  在 Plan-9 中**去掉**: `%rax` -> `AX`,
//        `%rsp` -> `SP`, `%xmm0` -> `X0`.
//     5. 跳转 / 条件助记符自成体系 (signed: JLT/JLE/JGT/JGE;
//        unsigned: JCS/JCC/JHI/JLS; 等). 看 `kPlan9CondSuffix` 区段.
//     6. CMOV / SET 的 size 与条件位置与 AT&T 相反:
//          AT&T  `cmovgq`            Plan-9 `CMOVQGT`
//          AT&T  `setl`              Plan-9 `SETLT`
//     7. 内存寻址语法 (本 header 不展开):
//          AT&T  `disp(%base,%idx,scale)`
//          Plan-9 `disp(BASE)(IDX*scale)`
//        (注意 base / index 间用第二组括号, 不是逗号; 详见 Track C 的
//         printMemReference 实现.)
//     8. CALL/RET 的 `q` 后缀去掉: `callq` -> `CALL`, `retq` -> `RET`.
//
//   * 本 header 只是 **mapping data** (constexpr table). 不含转换函数;
//     Track C 自行做 lookup (建议: lambda + StringSwitch 包装), 也可以
//     直接迭代 `kPlan9MnemonicMap[]`.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_X86PLAN9MNEMONICMAP_H
#define LLVM_LIB_TARGET_X86_X86PLAN9MNEMONICMAP_H

#include <cstddef>

namespace llvm {
namespace X86Plan9 {

//===----------------------------------------------------------------------===//
// 1. Mnemonic mapping (LLVM AT&T -> Plan-9)
//
//   每项 { att, plan9 } 一对.
//   att   : LLVM AT&T InstPrinter 实际输出的小写 mnemonic
//           (含 `q/l/w/b` size suffix, 含 `callq/retq` 等长形).
//   plan9 : Go obj/x86 anames.go 中的大写 mnemonic.
//
//   覆盖范围: leaf-NOSPLIT 必需 + 常见 GPR/FP/SIMD 编排 + 控制流 + cond
//   suffix 全集 (signed + unsigned + flag-bit). 约 95 条.
//
//   NOT 覆盖 (Track C 需另行处理):
//     - 内存寻址语法 (operand-level, 非 mnemonic-level)
//     - AVX-512 mask / EVEX broadcast (VEX/EVEX 前缀)
//     - x87 浮点 (Plan-9 用 F0..F7 + FADDD/FMULD 系列, c2go 不发)
//     - 同 mnemonic 不同 size 的特殊别名 (e.g. CWD/CDQ/CQO 已在表中)
//===----------------------------------------------------------------------===//

struct MnemonicEntry {
  const char *Att;    // LLVM AT&T printer 输出, 不带 '%' / '$' / suffix 之外
  const char *Plan9;  // Plan-9 (Go obj/x86) mnemonic
};

inline constexpr MnemonicEntry kPlan9MnemonicMap[] = {
    //
    // -- 数据移动 (mov family) -----------------------------------------------
    //
    {"movb",        "MOVB"},
    {"movw",        "MOVW"},
    {"movl",        "MOVL"},
    {"movq",        "MOVQ"},
    {"movabsq",     "MOVQ"},        // 64-bit imm 在 Plan-9 仍写 MOVQ $imm, reg
    // sign / zero extending mov: Plan-9 用 "MOV<src><dst><S|Z>X" 形
    {"movsbl",      "MOVBLSX"},     // sign-extend byte -> long
    {"movsbq",      "MOVBQSX"},     // sign-extend byte -> quad
    {"movzbl",      "MOVBLZX"},
    {"movzbq",      "MOVBQZX"},
    {"movswl",      "MOVWLSX"},
    {"movswq",      "MOVWQSX"},
    {"movzwl",      "MOVWLZX"},
    {"movzwq",      "MOVWQZX"},
    {"movslq",      "MOVLQSX"},     // sign-extend long -> quad
    {"movsxd",      "MOVLQSX"},     // Intel 老助记符 (LLVM 偶用)
    // NOTE: cltq / cdqe 在 "杂项" 段统一处理, 避免重复 key.
    // FP / SIMD scalar mov
    {"movss",       "MOVSS"},
    {"movsd",       "MOVSD"},
    // FP / SIMD packed mov
    {"movaps",      "MOVAPS"},
    {"movapd",      "MOVAPD"},
    {"movups",      "MOVUPS"},
    {"movupd",      "MOVUPD"},
    {"movdqa",      "MOVO"},        // Plan-9: MOVO/MOVOA (aligned 128)
    {"movdqu",      "MOVOU"},       // Plan-9: MOVOU       (unaligned 128)

    //
    // -- 加 / 减 / 比较 -------------------------------------------------------
    //
    {"addb",        "ADDB"},
    {"addw",        "ADDW"},
    {"addl",        "ADDL"},
    {"addq",        "ADDQ"},
    {"subb",        "SUBB"},
    {"subw",        "SUBW"},
    {"subl",        "SUBL"},
    {"subq",        "SUBQ"},
    {"adcq",        "ADCQ"},
    {"adcl",        "ADCL"},
    {"sbbq",        "SBBQ"},
    {"sbbl",        "SBBL"},
    {"cmpb",        "CMPB"},
    {"cmpw",        "CMPW"},
    {"cmpl",        "CMPL"},
    {"cmpq",        "CMPQ"},
    {"testb",       "TESTB"},
    {"testw",       "TESTW"},
    {"testl",       "TESTL"},
    {"testq",       "TESTQ"},
    {"incq",        "INCQ"},
    {"incl",        "INCL"},
    {"decq",        "DECQ"},
    {"decl",        "DECL"},
    {"negq",        "NEGQ"},
    {"negl",        "NEGL"},
    {"notq",        "NOTQ"},
    {"notl",        "NOTL"},

    //
    // -- 乘 / 除 / 位运算 -----------------------------------------------------
    //
    {"imull",       "IMULL"},
    {"imulq",       "IMULQ"},
    {"mull",        "MULL"},
    {"mulq",        "MULQ"},
    {"idivl",       "IDIVL"},
    {"idivq",       "IDIVQ"},
    {"divl",        "DIVL"},
    {"divq",        "DIVQ"},
    {"andb",        "ANDB"},
    {"andw",        "ANDW"},
    {"andl",        "ANDL"},
    {"andq",        "ANDQ"},
    {"orl",         "ORL"},
    {"orq",         "ORQ"},
    {"xorl",        "XORL"},
    {"xorq",        "XORQ"},
    {"shll",        "SHLL"},
    {"shlq",        "SHLQ"},
    {"shrl",        "SHRL"},
    {"shrq",        "SHRQ"},
    {"sarl",        "SARL"},
    {"sarq",        "SARQ"},
    {"roll",        "ROLL"},
    {"rolq",        "ROLQ"},
    {"rorl",        "RORL"},
    {"rorq",        "RORQ"},
    {"bswapl",      "BSWAPL"},
    {"bswapq",      "BSWAPQ"},

    //
    // -- 栈 / 调用 / 返回 -----------------------------------------------------
    //
    {"pushq",       "PUSHQ"},
    {"pushl",       "PUSHL"},
    {"popq",        "POPQ"},
    {"popl",        "POPL"},
    {"leaq",        "LEAQ"},
    {"leal",        "LEAL"},
    {"callq",       "CALL"},        // 关键: AT&T 64-bit 显式 callq, Plan-9 裸 CALL
    {"call",        "CALL"},
    {"retq",        "RET"},
    {"ret",         "RET"},
    {"jmpq",        "JMP"},         // indirect 64-bit jmp 在 AT&T 写 jmpq
    {"jmp",         "JMP"},
    {"nop",         "NOP"},
    {"nopl",        "NOPL"},
    {"nopw",        "NOPW"},
    {"ud2",         "UD2"},
    {"int3",        "INT $3"},      // Plan-9 用 INT $3, 非独立 mnemonic; 占位
    {"syscall",     "SYSCALL"},
    {"hlt",         "HLT"},
    {"lfence",      "LFENCE"},
    {"sfence",      "SFENCE"},
    {"mfence",      "MFENCE"},

    //
    // -- 条件跳转 (signed / unsigned / flag) ----------------------------------
    //   关键差异:
    //     AT&T   jl / jge / jg / jle          (signed; L=less)
    //     Plan-9 JLT / JGE / JGT / JLE        (T=than, GO 不要单字母)
    //     AT&T   jb / jae / ja / jbe          (unsigned)
    //     Plan-9 JCS / JCC / JHI / JLS        (CS=carry-set, HI=higher)
    //     AT&T   je / jne                     -> Plan-9 JEQ / JNE
    //     AT&T   js / jns                     -> Plan-9 JMI / JPL
    //     AT&T   jo / jno                     -> Plan-9 JOS / JOC
    //     AT&T   jp / jnp                     -> Plan-9 JPS / JPC
    //
    {"je",          "JEQ"},
    {"jz",          "JEQ"},
    {"jne",         "JNE"},
    {"jnz",         "JNE"},
    {"jl",          "JLT"},
    {"jnge",        "JLT"},
    {"jge",         "JGE"},
    {"jnl",         "JGE"},
    {"jg",          "JGT"},
    {"jnle",        "JGT"},
    {"jle",         "JLE"},
    {"jng",         "JLE"},
    {"jb",          "JCS"},
    {"jc",          "JCS"},
    {"jnae",        "JCS"},
    {"jae",         "JCC"},
    {"jnc",         "JCC"},
    {"jnb",         "JCC"},
    {"ja",          "JHI"},
    {"jnbe",        "JHI"},
    {"jbe",         "JLS"},
    {"jna",         "JLS"},
    {"js",          "JMI"},
    {"jns",         "JPL"},
    {"jo",          "JOS"},
    {"jno",         "JOC"},
    {"jp",          "JPS"},
    {"jpe",         "JPS"},
    {"jnp",         "JPC"},
    {"jpo",         "JPC"},

    //
    // -- SET<cc> ----------------------------------------------------------------
    //
    {"sete",        "SETEQ"},
    {"setz",        "SETEQ"},
    {"setne",       "SETNE"},
    {"setnz",       "SETNE"},
    {"setl",        "SETLT"},
    {"setge",       "SETGE"},
    {"setg",        "SETGT"},
    {"setle",       "SETLE"},
    {"setb",        "SETCS"},
    {"setc",        "SETCS"},
    {"setae",       "SETCC"},
    {"seta",        "SETHI"},
    {"setbe",       "SETLS"},
    {"sets",        "SETMI"},
    {"setns",       "SETPL"},
    {"seto",        "SETOS"},
    {"setno",       "SETOC"},
    {"setp",        "SETPS"},
    {"setnp",       "SETPC"},

    //
    // -- CMOV<cc> (size 在中间, cond 在尾) -------------------------------------
    //   AT&T cmovel / cmoveq / cmovew  -> Plan-9 CMOVLEQ / CMOVQEQ / CMOVWEQ
    //   这里只列 quadword 主路径; long/word 用同规则但前缀换 L/W.
    //   Track C 实现时建议拆 helper: "CMOV" + size + cond.
    //
    {"cmoveq",      "CMOVQEQ"},     // mov-if-equal
    {"cmovneq",     "CMOVQNE"},
    {"cmovlq",      "CMOVQLT"},
    {"cmovgeq",     "CMOVQGE"},
    {"cmovgq",      "CMOVQGT"},
    {"cmovleq",     "CMOVQLE"},
    {"cmovbq",      "CMOVQCS"},
    {"cmovaeq",     "CMOVQCC"},
    {"cmovaq",      "CMOVQHI"},
    {"cmovbeq",     "CMOVQLS"},
    {"cmovsq",      "CMOVQMI"},
    {"cmovnsq",     "CMOVQPL"},
    {"cmovoq",      "CMOVQOS"},
    {"cmovnoq",     "CMOVQOC"},

    //
    // -- FP scalar 算术 -------------------------------------------------------
    //
    {"addss",       "ADDSS"},
    {"addsd",       "ADDSD"},
    {"subss",       "SUBSS"},
    {"subsd",       "SUBSD"},
    {"mulss",       "MULSS"},
    {"mulsd",       "MULSD"},
    {"divss",       "DIVSS"},
    {"divsd",       "DIVSD"},
    {"sqrtss",      "SQRTSS"},
    {"sqrtsd",      "SQRTSD"},
    {"maxss",       "MAXSS"},
    {"maxsd",       "MAXSD"},
    {"minss",       "MINSS"},
    {"minsd",       "MINSD"},
    {"comiss",      "COMISS"},
    {"comisd",      "COMISD"},
    {"ucomiss",     "UCOMISS"},
    {"ucomisd",     "UCOMISD"},

    //
    // -- FP packed / XOR ------------------------------------------------------
    //
    {"addps",       "ADDPS"},
    {"addpd",       "ADDPD"},
    {"xorps",       "XORPS"},
    {"xorpd",       "XORPD"},
    {"andps",       "ANDPS"},
    {"andpd",       "ANDPD"},

    //
    // -- FP <-> int 转换 ------------------------------------------------------
    //   abitest T7 用了 CVTTSD2SQ 把 X0 转成 int64 -> 关键路径.
    //
    {"cvtsi2ssl",   "CVTSL2SS"},    // int32  -> f32
    {"cvtsi2ssq",   "CVTSQ2SS"},    // int64  -> f32
    {"cvtsi2sdl",   "CVTSL2SD"},    // int32  -> f64
    {"cvtsi2sdq",   "CVTSQ2SD"},    // int64  -> f64
    {"cvtss2sd",    "CVTSS2SD"},
    {"cvtsd2ss",    "CVTSD2SS"},
    {"cvttss2sil",  "CVTTSS2SL"},   // f32 -> int32 (truncate)
    {"cvttss2siq",  "CVTTSS2SQ"},   // f32 -> int64
    {"cvttsd2sil",  "CVTTSD2SL"},   // f64 -> int32
    {"cvttsd2siq",  "CVTTSD2SQ"},   // f64 -> int64 (abitest leafRegSIMD)

    //
    // -- 杂项 (sign-extend 系 / xchg) -----------------------------------------
    //   LLVM AT&T 默认 spelling 是 BSD/SysV (cbtw/cwtl/cwtd/cltd/cltq/cqto),
    //   *不是* Intel (cbw/cwde/cwd/cdq/cdqe/cqo). 两套都列, 给 Track C 兜底.
    //
    {"cbtw",        "CBW"},          // AL  -> AX           (sign extend byte->word)
    {"cbw",         "CBW"},
    {"cwtl",        "CWDE"},         // AX  -> EAX          (sign extend word->long)
    {"cwde",        "CWDE"},
    {"cwtd",        "CWD"},          // AX  -> DX:AX
    {"cwd",         "CWD"},
    {"cltd",        "CDQ"},          // EAX -> EDX:EAX
    {"cdq",         "CDQ"},
    {"cltq",        "MOVLQSX"},      // EAX -> RAX (sext); Plan-9 显式用 MOVLQSX
    {"cdqe",        "MOVLQSX"},
    {"cqto",        "CQO"},          // RAX -> RDX:RAX
    {"cqo",         "CQO"},
    {"xchgb",       "XCHGB"},
    {"xchgw",       "XCHGW"},
    {"xchgl",       "XCHGL"},
    {"xchgq",       "XCHGQ"},
};

inline constexpr std::size_t kPlan9MnemonicMapSize =
    sizeof(kPlan9MnemonicMap) / sizeof(kPlan9MnemonicMap[0]);

//===----------------------------------------------------------------------===//
// 2. Register name mapping (LLVM AT&T -> Plan-9)
//
//   AT&T printer 输出形如  `%rax` / `%eax` / `%ax` / `%al` / `%xmm0`.
//   Plan-9 寄存器名 size-agnostic (AX 同时表示 64/32/16 位; 8 位另起 AL/BL/
//   CL/DL/SPB/BPB/SIB/DIB/R8B..R15B; 高 8 位 AH/CH/DH/BH 与 AT&T 同名).
//
//   入表的 att 字段 **不含** 前导 `%` (Track C 在 lookup 前自行剥离).
//   Plan-9 字段直接是输出形.
//===----------------------------------------------------------------------===//

struct RegEntry {
  const char *Att;    // 不含 '%'; 大小写按 AT&T 输出 (小写)
  const char *Plan9;  // Plan-9 register (大写)
};

inline constexpr RegEntry kPlan9RegMap[] = {
    // -- 通用整数寄存器 64-bit (RAX..R15) -> AX..R15 (size-agnostic) ---------
    {"rax", "AX"},   {"rbx", "BX"},   {"rcx", "CX"},   {"rdx", "DX"},
    {"rsi", "SI"},   {"rdi", "DI"},   {"rsp", "SP"},   {"rbp", "BP"},
    {"r8",  "R8"},   {"r9",  "R9"},   {"r10", "R10"},  {"r11", "R11"},
    {"r12", "R12"},  {"r13", "R13"},  {"r14", "R14"},  {"r15", "R15"},

    // -- 32-bit (EAX..R15D) — Plan-9 同名 (size 由 MOVL/ADDL 等 mnemonic 决定)
    {"eax", "AX"},   {"ebx", "BX"},   {"ecx", "CX"},   {"edx", "DX"},
    {"esi", "SI"},   {"edi", "DI"},   {"esp", "SP"},   {"ebp", "BP"},
    {"r8d",  "R8"},  {"r9d",  "R9"},  {"r10d", "R10"}, {"r11d", "R11"},
    {"r12d", "R12"}, {"r13d", "R13"}, {"r14d", "R14"}, {"r15d", "R15"},

    // -- 16-bit (AX..R15W) — Plan-9 同名 (MOVW 区分 size)
    {"ax", "AX"},    {"bx", "BX"},    {"cx", "CX"},    {"dx", "DX"},
    {"si", "SI"},    {"di", "DI"},    {"sp", "SP"},    {"bp", "BP"},
    {"r8w",  "R8"},  {"r9w",  "R9"},  {"r10w", "R10"}, {"r11w", "R11"},
    {"r12w", "R12"}, {"r13w", "R13"}, {"r14w", "R14"}, {"r15w", "R15"},

    // -- 低 8 位 — Plan-9 另起一组 (SPB/BPB/SIB/DIB; R8B..R15B 同名)
    {"al", "AL"},    {"bl", "BL"},    {"cl", "CL"},    {"dl", "DL"},
    {"sil", "SIB"},  {"dil", "DIB"},  {"spl", "SPB"},  {"bpl", "BPB"},
    {"r8b",  "R8B"}, {"r9b",  "R9B"}, {"r10b", "R10B"},{"r11b", "R11B"},
    {"r12b", "R12B"},{"r13b", "R13B"},{"r14b", "R14B"},{"r15b", "R15B"},

    // -- 高 8 位 — Plan-9/AT&T 完全同名
    {"ah", "AH"},    {"bh", "BH"},    {"ch", "CH"},    {"dh", "DH"},

    // -- SSE/AVX 128-bit (XMM0..XMM15) -> X0..X15 -----------------------------
    //   去掉 'XMM' 前缀, 留 X + idx. abitest T5/T6/T7 全在此 16 个内.
    {"xmm0",  "X0"},  {"xmm1",  "X1"},  {"xmm2",  "X2"},  {"xmm3",  "X3"},
    {"xmm4",  "X4"},  {"xmm5",  "X5"},  {"xmm6",  "X6"},  {"xmm7",  "X7"},
    {"xmm8",  "X8"},  {"xmm9",  "X9"},  {"xmm10", "X10"}, {"xmm11", "X11"},
    {"xmm12", "X12"}, {"xmm13", "X13"}, {"xmm14", "X14"}, {"xmm15", "X15"},
    // AVX-512 扩展 XMM16..31 (Plan-9 也覆盖, 见 list6.go L115)
    {"xmm16", "X16"}, {"xmm17", "X17"}, {"xmm18", "X18"}, {"xmm19", "X19"},
    {"xmm20", "X20"}, {"xmm21", "X21"}, {"xmm22", "X22"}, {"xmm23", "X23"},
    {"xmm24", "X24"}, {"xmm25", "X25"}, {"xmm26", "X26"}, {"xmm27", "X27"},
    {"xmm28", "X28"}, {"xmm29", "X29"}, {"xmm30", "X30"}, {"xmm31", "X31"},

    // -- AVX 256-bit (YMM0..YMM31) -> Y0..Y31 ---------------------------------
    {"ymm0",  "Y0"},  {"ymm1",  "Y1"},  {"ymm2",  "Y2"},  {"ymm3",  "Y3"},
    {"ymm4",  "Y4"},  {"ymm5",  "Y5"},  {"ymm6",  "Y6"},  {"ymm7",  "Y7"},
    {"ymm8",  "Y8"},  {"ymm9",  "Y9"},  {"ymm10", "Y10"}, {"ymm11", "Y11"},
    {"ymm12", "Y12"}, {"ymm13", "Y13"}, {"ymm14", "Y14"}, {"ymm15", "Y15"},

    // -- AVX-512 512-bit (ZMM0..ZMM31) -> Z0..Z31 -----------------------------
    {"zmm0",  "Z0"},  {"zmm1",  "Z1"},  {"zmm2",  "Z2"},  {"zmm3",  "Z3"},
    {"zmm4",  "Z4"},  {"zmm5",  "Z5"},  {"zmm6",  "Z6"},  {"zmm7",  "Z7"},
    {"zmm8",  "Z8"},  {"zmm9",  "Z9"},  {"zmm10", "Z10"}, {"zmm11", "Z11"},
    {"zmm12", "Z12"}, {"zmm13", "Z13"}, {"zmm14", "Z14"}, {"zmm15", "Z15"},

    // -- AVX-512 mask 寄存器 (K0..K7) -- Plan-9 同名
    {"k0", "K0"}, {"k1", "K1"}, {"k2", "K2"}, {"k3", "K3"},
    {"k4", "K4"}, {"k5", "K5"}, {"k6", "K6"}, {"k7", "K7"},

    // -- 段 / 控制 (Plan-9 大写, 同名) ---------------------------------------
    {"cs", "CS"}, {"ss", "SS"}, {"ds", "DS"},
    {"es", "ES"}, {"fs", "FS"}, {"gs", "GS"},
};

inline constexpr std::size_t kPlan9RegMapSize =
    sizeof(kPlan9RegMap) / sizeof(kPlan9RegMap[0]);

//===----------------------------------------------------------------------===//
// 3. 示例对照 (LLVM AT&T 整行 -> Plan-9 整行)
//
//   下面 12 个对照覆盖 abitest_amd64 八个用例最常见的发射模式. 仅作注释
//   存档, Track C 可直接拿来当 LIT 测试期望值.
//
//   操作数顺序 (src, dst) 两种语法都一致. 主要差异:
//     - register   :  %rax     -> AX        ; %xmm0 -> X0
//     - immediate  :  $100     -> $100      (一致)
//     - memory     :  8(%rsp)  -> 8(SP)     ; 16(%rsp,%rax,8) -> 16(SP)(AX*8)
//                                              (基址加变址改第二组括号)
//     - mnemonic   :  callq    -> CALL      ; retq -> RET
//
//   ┌─────────────────────────────────────────────┬───────────────────────────┐
//   │  AT&T                                       │  Plan-9                   │
//   ├─────────────────────────────────────────────┼───────────────────────────┤
//   │  movq   %rax, %rbx                          │  MOVQ  AX, BX             │
//   │  movq   8(%rsp), %rax                       │  MOVQ  8(SP), AX          │
//   │  movq   $100, %rcx                          │  MOVQ  $100, CX           │
//   │  addq   %rbx, %rax                          │  ADDQ  BX, AX             │
//   │  subq   $16, %rsp                           │  SUBQ  $16, SP            │
//   │  cmpq   %rax, %rbx                          │  CMPQ  AX, BX             │
//   │  jl     .Lbb0_1                             │  JLT   .Lbb0_1            │
//   │  jae    .Lloop                              │  JCC   .Lloop             │
//   │  callq  Add(SB)                             │  CALL  Add(SB)            │
//   │  retq                                       │  RET                      │
//   │  movsd  16(%rsp), %xmm0                     │  MOVSD 16(SP), X0         │
//   │  cvttsd2siq %xmm0, %rcx                     │  CVTTSD2SQ X0, CX         │
//   │  movups 16(%rsp), %xmm0                     │  MOVUPS 16(SP), X0        │
//   │  movups %xmm0, 32(%rsp)                     │  MOVUPS X0, 32(SP)        │
//   └─────────────────────────────────────────────┴───────────────────────────┘
//
//   (再贴一组 abitest T7 中带 SIMD/reg 的整段对照)
//
//     AT&T:                                      Plan-9:
//       cvttsd2siq %xmm0, %rcx                     CVTTSD2SQ X0, CX
//       addq   %rbx, %rax                          ADDQ      BX, AX
//       addq   %rcx, %rax                          ADDQ      CX, AX
//       retq                                       RET
//
//===----------------------------------------------------------------------===//

} // namespace X86Plan9
} // namespace llvm

#endif // LLVM_LIB_TARGET_X86_X86PLAN9MNEMONICMAP_H
