; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mtriple=i686-pc-linux-gnu -mcpu=corei7 | FileCheck %s
; RUN: llc < %s -mtriple=i686-pc-linux-gnu -mcpu=core-avx-i | FileCheck %s --check-prefix=AVX

define <1 x float> @test1(<1 x double> %x) nounwind {
; CHECK-LABEL: test1:
; CHECK:       # BB#0:
; CHECK-NEXT:    pushl %eax
; CHECK-NEXT:    movsd {{.*#+}} xmm0 = mem[0],zero
; CHECK-NEXT:    cvtsd2ss %xmm0, %xmm0
; CHECK-NEXT:    movss %xmm0, (%esp)
; CHECK-NEXT:    flds (%esp)
; CHECK-NEXT:    popl %eax
; CHECK-NEXT:    retl
;
; AVX-LABEL: test1:
; AVX:       # BB#0:
; AVX-NEXT:    pushl %eax
; AVX-NEXT:    vmovsd {{.*#+}} xmm0 = mem[0],zero
; AVX-NEXT:    vcvtsd2ss %xmm0, %xmm0, %xmm0
; AVX-NEXT:    vmovss %xmm0, (%esp)
; AVX-NEXT:    flds (%esp)
; AVX-NEXT:    popl %eax
; AVX-NEXT:    retl
  %y = fptrunc <1 x double> %x to <1 x float>
  ret <1 x float> %y
}

define <2 x float> @test2(<2 x double> %x) nounwind {
; CHECK-LABEL: test2:
; CHECK:       # BB#0:
; CHECK-NEXT:    cvtpd2ps %xmm0, %xmm0
; CHECK-NEXT:    retl
;
; AVX-LABEL: test2:
; AVX:       # BB#0:
; AVX-NEXT:    vcvtpd2ps %xmm0, %xmm0
; AVX-NEXT:    retl
  %y = fptrunc <2 x double> %x to <2 x float>
  ret <2 x float> %y
}

define <4 x float> @test3(<4 x double> %x) nounwind {
; CHECK-LABEL: test3:
; CHECK:       # BB#0:
; CHECK-NEXT:    cvtpd2ps %xmm1, %xmm1
; CHECK-NEXT:    cvtpd2ps %xmm0, %xmm0
; CHECK-NEXT:    unpcklpd {{.*#+}} xmm0 = xmm0[0],xmm1[0]
; CHECK-NEXT:    retl
;
; AVX-LABEL: test3:
; AVX:       # BB#0:
; AVX-NEXT:    vcvtpd2psy %ymm0, %xmm0
; AVX-NEXT:    vzeroupper
; AVX-NEXT:    retl
  %y = fptrunc <4 x double> %x to <4 x float>
  ret <4 x float> %y
}

define <8 x float> @test4(<8 x double> %x) nounwind {
; CHECK-LABEL: test4:
; CHECK:       # BB#0:
; CHECK-NEXT:    subl $12, %esp
; CHECK-NEXT:    cvtpd2ps %xmm1, %xmm1
; CHECK-NEXT:    cvtpd2ps %xmm0, %xmm0
; CHECK-NEXT:    unpcklpd {{.*#+}} xmm0 = xmm0[0],xmm1[0]
; CHECK-NEXT:    cvtpd2ps %xmm2, %xmm1
; CHECK-NEXT:    cvtpd2ps {{[0-9]+}}(%esp), %xmm2
; CHECK-NEXT:    unpcklpd {{.*#+}} xmm1 = xmm1[0],xmm2[0]
; CHECK-NEXT:    addl $12, %esp
; CHECK-NEXT:    retl
;
; AVX-LABEL: test4:
; AVX:       # BB#0:
; AVX-NEXT:    vcvtpd2psy %ymm0, %xmm0
; AVX-NEXT:    vcvtpd2psy %ymm1, %xmm1
; AVX-NEXT:    vinsertf128 $1, %xmm1, %ymm0, %ymm0
; AVX-NEXT:    retl
  %y = fptrunc <8 x double> %x to <8 x float>
  ret <8 x float> %y
}

