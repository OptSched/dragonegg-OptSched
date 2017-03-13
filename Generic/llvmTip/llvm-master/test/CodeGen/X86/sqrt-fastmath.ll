; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: llc < %s -mtriple=x86_64-unknown-unknown -mattr=avx | FileCheck %s --check-prefix=AVX

declare double @__sqrt_finite(double)
declare float @__sqrtf_finite(float)
declare x86_fp80 @__sqrtl_finite(x86_fp80)
declare float @llvm.sqrt.f32(float)
declare <4 x float> @llvm.sqrt.v4f32(<4 x float>)
declare <8 x float> @llvm.sqrt.v8f32(<8 x float>)


define double @finite_f64_no_estimate(double %d) #0 {
; AVX-LABEL: finite_f64_no_estimate:
; AVX:       # BB#0:
; AVX-NEXT:    vsqrtsd %xmm0, %xmm0, %xmm0
; AVX-NEXT:    retq
;
  %call = tail call double @__sqrt_finite(double %d) #2
  ret double %call
}

; No estimates for doubles.

define double @finite_f64_estimate(double %d) #1 {
; AVX-LABEL: finite_f64_estimate:
; AVX:       # BB#0:
; AVX-NEXT:    vsqrtsd %xmm0, %xmm0, %xmm0
; AVX-NEXT:    retq
;
  %call = tail call double @__sqrt_finite(double %d) #2
  ret double %call
}

define float @finite_f32_no_estimate(float %f) #0 {
; AVX-LABEL: finite_f32_no_estimate:
; AVX:       # BB#0:
; AVX-NEXT:    vsqrtss %xmm0, %xmm0, %xmm0
; AVX-NEXT:    retq
;
  %call = tail call float @__sqrtf_finite(float %f) #2
  ret float %call
}

define float @finite_f32_estimate(float %f) #1 {
; AVX-LABEL: finite_f32_estimate:
; AVX:       # BB#0:
; AVX-NEXT:    vrsqrtss %xmm0, %xmm0, %xmm1
; AVX-NEXT:    vmulss %xmm1, %xmm0, %xmm2
; AVX-NEXT:    vmulss %xmm1, %xmm2, %xmm1
; AVX-NEXT:    vaddss {{.*}}(%rip), %xmm1, %xmm1
; AVX-NEXT:    vmulss {{.*}}(%rip), %xmm2, %xmm2
; AVX-NEXT:    vmulss %xmm1, %xmm2, %xmm1
; AVX-NEXT:    vxorps %xmm2, %xmm2, %xmm2
; AVX-NEXT:    vcmpeqss %xmm2, %xmm0, %xmm0
; AVX-NEXT:    vandnps %xmm1, %xmm0, %xmm0
; AVX-NEXT:    retq
;
  %call = tail call float @__sqrtf_finite(float %f) #2
  ret float %call
}

define x86_fp80 @finite_f80_no_estimate(x86_fp80 %ld) #0 {
; AVX-LABEL: finite_f80_no_estimate:
; AVX:       # BB#0:
; AVX-NEXT:    fldt {{[0-9]+}}(%rsp)
; AVX-NEXT:    fsqrt
; AVX-NEXT:    retq
;
  %call = tail call x86_fp80 @__sqrtl_finite(x86_fp80 %ld) #2
  ret x86_fp80 %call
}

; Don't die on the impossible.

define x86_fp80 @finite_f80_estimate_but_no(x86_fp80 %ld) #1 {
; AVX-LABEL: finite_f80_estimate_but_no:
; AVX:       # BB#0:
; AVX-NEXT:    fldt {{[0-9]+}}(%rsp)
; AVX-NEXT:    fsqrt
; AVX-NEXT:    retq
;
  %call = tail call x86_fp80 @__sqrtl_finite(x86_fp80 %ld) #2
  ret x86_fp80 %call
}

define float @f32_no_estimate(float %x) #0 {
; AVX-LABEL: f32_no_estimate:
; AVX:       # BB#0:
; AVX-NEXT:    vsqrtss %xmm0, %xmm0, %xmm0
; AVX-NEXT:    vmovss {{.*#+}} xmm1 = mem[0],zero,zero,zero
; AVX-NEXT:    vdivss %xmm0, %xmm1, %xmm0
; AVX-NEXT:    retq
;
  %sqrt = tail call float @llvm.sqrt.f32(float %x)
  %div = fdiv fast float 1.0, %sqrt
  ret float %div
}

define float @f32_estimate(float %x) #1 {
; AVX-LABEL: f32_estimate:
; AVX:       # BB#0:
; AVX-NEXT:    vrsqrtss %xmm0, %xmm0, %xmm1
; AVX-NEXT:    vmulss %xmm1, %xmm1, %xmm2
; AVX-NEXT:    vmulss %xmm2, %xmm0, %xmm0
; AVX-NEXT:    vaddss {{.*}}(%rip), %xmm0, %xmm0
; AVX-NEXT:    vmulss {{.*}}(%rip), %xmm1, %xmm1
; AVX-NEXT:    vmulss %xmm0, %xmm1, %xmm0
; AVX-NEXT:    retq
;
  %sqrt = tail call float @llvm.sqrt.f32(float %x)
  %div = fdiv fast float 1.0, %sqrt
  ret float %div
}

define <4 x float> @v4f32_no_estimate(<4 x float> %x) #0 {
; AVX-LABEL: v4f32_no_estimate:
; AVX:       # BB#0:
; AVX-NEXT:    vsqrtps %xmm0, %xmm0
; AVX-NEXT:    vmovaps {{.*#+}} xmm1 = [1.000000e+00,1.000000e+00,1.000000e+00,1.000000e+00]
; AVX-NEXT:    vdivps %xmm0, %xmm1, %xmm0
; AVX-NEXT:    retq
;
  %sqrt = tail call <4 x float> @llvm.sqrt.v4f32(<4 x float> %x)
  %div = fdiv fast <4 x float> <float 1.0, float 1.0, float 1.0, float 1.0>, %sqrt
  ret <4 x float> %div
}

define <4 x float> @v4f32_estimate(<4 x float> %x) #1 {
; AVX-LABEL: v4f32_estimate:
; AVX:       # BB#0:
; AVX-NEXT:    vrsqrtps %xmm0, %xmm1
; AVX-NEXT:    vmulps %xmm1, %xmm1, %xmm2
; AVX-NEXT:    vmulps %xmm2, %xmm0, %xmm0
; AVX-NEXT:    vaddps {{.*}}(%rip), %xmm0, %xmm0
; AVX-NEXT:    vmulps {{.*}}(%rip), %xmm1, %xmm1
; AVX-NEXT:    vmulps %xmm0, %xmm1, %xmm0
; AVX-NEXT:    retq
;
  %sqrt = tail call <4 x float> @llvm.sqrt.v4f32(<4 x float> %x)
  %div = fdiv fast <4 x float> <float 1.0, float 1.0, float 1.0, float 1.0>, %sqrt
  ret <4 x float> %div
}

define <8 x float> @v8f32_no_estimate(<8 x float> %x) #0 {
; AVX-LABEL: v8f32_no_estimate:
; AVX:       # BB#0:
; AVX-NEXT:    vsqrtps %ymm0, %ymm0
; AVX-NEXT:    vmovaps {{.*#+}} ymm1 = [1.000000e+00,1.000000e+00,1.000000e+00,1.000000e+00,1.000000e+00,1.000000e+00,1.000000e+00,1.000000e+00]
; AVX-NEXT:    vdivps %ymm0, %ymm1, %ymm0
; AVX-NEXT:    retq
;
  %sqrt = tail call <8 x float> @llvm.sqrt.v8f32(<8 x float> %x)
  %div = fdiv fast <8 x float> <float 1.0, float 1.0, float 1.0, float 1.0, float 1.0, float 1.0, float 1.0, float 1.0>, %sqrt
  ret <8 x float> %div
}

define <8 x float> @v8f32_estimate(<8 x float> %x) #1 {
; AVX-LABEL: v8f32_estimate:
; AVX:       # BB#0:
; AVX-NEXT:    vrsqrtps %ymm0, %ymm1
; AVX-NEXT:    vmulps %ymm1, %ymm1, %ymm2
; AVX-NEXT:    vmulps %ymm2, %ymm0, %ymm0
; AVX-NEXT:    vaddps {{.*}}(%rip), %ymm0, %ymm0
; AVX-NEXT:    vmulps {{.*}}(%rip), %ymm1, %ymm1
; AVX-NEXT:    vmulps %ymm0, %ymm1, %ymm0
; AVX-NEXT:    retq
;
  %sqrt = tail call <8 x float> @llvm.sqrt.v8f32(<8 x float> %x)
  %div = fdiv fast <8 x float> <float 1.0, float 1.0, float 1.0, float 1.0, float 1.0, float 1.0, float 1.0, float 1.0>, %sqrt
  ret <8 x float> %div
}


attributes #0 = { "unsafe-fp-math"="true" "reciprocal-estimates"="!sqrtf,!vec-sqrtf,!divf,!vec-divf" }
attributes #1 = { "unsafe-fp-math"="true" "reciprocal-estimates"="sqrt,vec-sqrt" }
attributes #2 = { nounwind readnone }
