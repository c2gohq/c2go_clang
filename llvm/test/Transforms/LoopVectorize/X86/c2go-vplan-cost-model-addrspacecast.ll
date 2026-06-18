; The legacy and VPlan cost models must agree on the cost of an
; addrspacecast (managed AS1 -> unmanaged AS0), so loop vectorization
; does not hit the cost-model agreement assertion under x86-64.
;
; X86 declares AS1<->AS0 as a no-op cast, so getCastInstrCost is 0. The
; legacy cost-model switch on cast opcodes must list AddrSpaceCast
; alongside BitCast/ZExt/SExt/... and route to getCastInstrCost, exactly
; like the VPlan path; otherwise it falls through to the default arm,
; estimates the cast as a mul, disagrees with VPlan, and (on an
; assertions build) trips the cost-model agreement assert.

; RUN: opt -passes=loop-vectorize -mtriple=x86_64-apple-darwin -mcpu=penryn -S %s | FileCheck %s

target triple = "x86_64-apple-darwin"

%struct.Node = type { ptr addrspace(1), ptr addrspace(1), ptr addrspace(1), ptr addrspace(1), ptr addrspace(1), ptr addrspace(1), i64 }

@gRing = internal unnamed_addr global ptr addrspace(1) null, align 8
@gCksum = internal unnamed_addr global i64 0, align 8

; CHECK-LABEL: define void @stress_field_store
; Without agreeing cost models this aborted with a cost-model agreement
; assertion failure. With them agreeing, vectorization proceeds (any
; decision is fine); we only need the pass to return a well-formed
; function. The check below confirms that.
; CHECK: ret void
define void @stress_field_store(i32 noundef %iters) local_unnamed_addr {
entry:
  %0 = load ptr addrspace(1), ptr @gRing, align 8
  %cmp36 = icmp sgt i32 %iters, 0
  br i1 %cmp36, label %for.body.preheader, label %for.cond.cleanup

for.body.preheader:                               ; preds = %entry
  %wide.trip.count = zext nneg i32 %iters to i64
  br label %for.body

for.cond.cleanup.loopexit:                        ; preds = %for.body
  %xor13.lcssa = phi i64 [ %xor13, %for.body ]
  br label %for.cond.cleanup

for.cond.cleanup:                                 ; preds = %for.cond.cleanup.loopexit, %entry
  %sum.0.lcssa = phi i64 [ 0, %entry ], [ %xor13.lcssa, %for.cond.cleanup.loopexit ]
  %1 = load i64, ptr @gCksum, align 8
  %xor14 = xor i64 %1, %sum.0.lcssa
  store i64 %xor14, ptr @gCksum, align 8
  ret void

for.body:                                         ; preds = %for.body.preheader, %for.body
  %indvars.iv = phi i64 [ 0, %for.body.preheader ], [ %indvars.iv.next, %for.body ]
  %sum.037 = phi i64 [ 0, %for.body.preheader ], [ %xor13, %for.body ]
  %and = and i64 %indvars.iv, 15
  %add.ptr = getelementptr inbounds nuw %struct.Node, ptr addrspace(1) %0, i64 %and
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %and1 = and i64 %indvars.iv.next, 15
  %add.ptr3 = getelementptr inbounds nuw %struct.Node, ptr addrspace(1) %0, i64 %and1
  %2 = add nuw i64 %indvars.iv, 2
  %and5 = and i64 %2, 15
  %add.ptr7 = getelementptr inbounds nuw %struct.Node, ptr addrspace(1) %0, i64 %and5
  %3 = add nuw i64 %indvars.iv, 3
  %and9 = and i64 %3, 15
  %add.ptr11 = getelementptr inbounds nuw %struct.Node, ptr addrspace(1) %0, i64 %and9
  store ptr addrspace(1) %add.ptr3, ptr addrspace(1) %add.ptr, align 8
  %prev = getelementptr inbounds nuw i8, ptr addrspace(1) %add.ptr, i64 8
  store ptr addrspace(1) %add.ptr7, ptr addrspace(1) %prev, align 8
  %sibling = getelementptr inbounds nuw i8, ptr addrspace(1) %add.ptr, i64 16
  store ptr addrspace(1) %add.ptr11, ptr addrspace(1) %sibling, align 8
  ; AS1->AS0 addrspacecast (managed->unmanaged) - the cost-model path
  ; under test. Three in a row keep the vectorizer interested at VF=2.
  %4 = addrspacecast ptr addrspace(1) %add.ptr3 to ptr
  %label = getelementptr inbounds nuw i8, ptr addrspace(1) %add.ptr, i64 24
  store ptr %4, ptr addrspace(1) %label, align 8
  %5 = addrspacecast ptr addrspace(1) %add.ptr7 to ptr
  %payload = getelementptr inbounds nuw i8, ptr addrspace(1) %add.ptr, i64 32
  store ptr %5, ptr addrspace(1) %payload, align 8
  %6 = addrspacecast ptr addrspace(1) %add.ptr11 to ptr
  %user = getelementptr inbounds nuw i8, ptr addrspace(1) %add.ptr, i64 40
  store ptr %6, ptr addrspace(1) %user, align 8
  %xor = xor i64 %indvars.iv, 0
  %tag = getelementptr inbounds nuw i8, ptr addrspace(1) %add.ptr, i64 48
  store i64 %xor, ptr addrspace(1) %tag, align 8
  %xor13 = xor i64 %xor, %sum.037
  %exitcond.not = icmp eq i64 %indvars.iv.next, %wide.trip.count
  br i1 %exitcond.not, label %for.cond.cleanup.loopexit, label %for.body
}
