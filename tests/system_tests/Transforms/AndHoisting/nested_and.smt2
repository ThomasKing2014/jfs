; RUN: %jfs-opt -and-hoist %s | %FileCheck %s

; CHECK: (declare-fun x () Bool)
; CHECK-NEXT: (declare-fun y () Bool)
; CHECK-NEXT: (declare-fun z () Bool)
(declare-fun x () Bool)
(declare-fun y () Bool)
(declare-fun z () Bool)

; CHECK: (assert x)
; CHECK-NEXT: (assert (or y z))
(assert (and (or y z) x))

; CHECK-NEXT: (assert (= x x))
(assert (= x x))
(check-sat)
