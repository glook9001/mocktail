// Single-cycle hardware math builtins for Mocktail's libm stub.
// Directly emits hardware vector/FPU instructions without PLT/glibc overhead.

#include <cmath>

#define MOCKTAIL_UNARY_DOUBLE(name) \
  extern "C" double name(double value) { \
    return __builtin_##name(value); \
  }
#define MOCKTAIL_UNARY_FLOAT(name) \
  extern "C" float name(float value) { \
    return __builtin_##name(value); \
  }
#define MOCKTAIL_BINARY_DOUBLE(name) \
  extern "C" double name(double left, double right) { \
    return __builtin_##name(left, right); \
  }
#define MOCKTAIL_BINARY_FLOAT(name) \
  extern "C" float name(float left, float right) { \
    return __builtin_##name(left, right); \
  }

MOCKTAIL_UNARY_DOUBLE(acos)
MOCKTAIL_UNARY_DOUBLE(asin)
MOCKTAIL_UNARY_DOUBLE(atan)
MOCKTAIL_UNARY_DOUBLE(cbrt)
MOCKTAIL_UNARY_DOUBLE(ceil)
MOCKTAIL_UNARY_DOUBLE(cos)
MOCKTAIL_UNARY_DOUBLE(cosh)
MOCKTAIL_UNARY_DOUBLE(exp)
MOCKTAIL_UNARY_DOUBLE(exp2)
MOCKTAIL_UNARY_DOUBLE(expm1)
MOCKTAIL_UNARY_DOUBLE(fabs)
MOCKTAIL_UNARY_DOUBLE(floor)
MOCKTAIL_UNARY_DOUBLE(log)
MOCKTAIL_UNARY_DOUBLE(log10)
MOCKTAIL_UNARY_DOUBLE(log2)
MOCKTAIL_UNARY_DOUBLE(round)
MOCKTAIL_UNARY_DOUBLE(sin)
MOCKTAIL_UNARY_DOUBLE(sinh)
MOCKTAIL_UNARY_DOUBLE(sqrt)
MOCKTAIL_UNARY_DOUBLE(tan)
MOCKTAIL_UNARY_DOUBLE(tanh)
MOCKTAIL_UNARY_DOUBLE(trunc)
MOCKTAIL_UNARY_FLOAT(acosf)
MOCKTAIL_UNARY_FLOAT(asinf)
MOCKTAIL_UNARY_FLOAT(atanf)
MOCKTAIL_UNARY_FLOAT(cbrtf)
MOCKTAIL_UNARY_FLOAT(ceilf)
MOCKTAIL_UNARY_FLOAT(cosf)
MOCKTAIL_UNARY_FLOAT(coshf)
MOCKTAIL_UNARY_FLOAT(expf)
MOCKTAIL_UNARY_FLOAT(exp2f)
MOCKTAIL_UNARY_FLOAT(erfcf)
MOCKTAIL_UNARY_FLOAT(erff)
MOCKTAIL_UNARY_FLOAT(fabsf)
MOCKTAIL_UNARY_FLOAT(floorf)
MOCKTAIL_UNARY_FLOAT(logf)
MOCKTAIL_UNARY_FLOAT(log10f)
MOCKTAIL_UNARY_FLOAT(log2f)
MOCKTAIL_UNARY_FLOAT(roundf)
MOCKTAIL_UNARY_FLOAT(sinf)
MOCKTAIL_UNARY_FLOAT(sinhf)
MOCKTAIL_UNARY_FLOAT(sqrtf)
MOCKTAIL_UNARY_FLOAT(tanf)
MOCKTAIL_UNARY_FLOAT(tanhf)
MOCKTAIL_UNARY_FLOAT(truncf)
MOCKTAIL_BINARY_DOUBLE(atan2)
MOCKTAIL_BINARY_DOUBLE(copysign)
MOCKTAIL_BINARY_DOUBLE(fdim)
MOCKTAIL_BINARY_DOUBLE(fmax)
MOCKTAIL_BINARY_DOUBLE(fmin)
MOCKTAIL_BINARY_DOUBLE(fmod)
MOCKTAIL_BINARY_DOUBLE(hypot)
MOCKTAIL_BINARY_DOUBLE(nextafter)
MOCKTAIL_BINARY_DOUBLE(pow)
MOCKTAIL_BINARY_DOUBLE(remainder)
MOCKTAIL_BINARY_FLOAT(atan2f)
MOCKTAIL_BINARY_FLOAT(copysignf)
MOCKTAIL_BINARY_FLOAT(fdimf)
MOCKTAIL_BINARY_FLOAT(fmaxf)
MOCKTAIL_BINARY_FLOAT(fminf)
MOCKTAIL_BINARY_FLOAT(fmodf)
MOCKTAIL_BINARY_FLOAT(hypotf)
MOCKTAIL_BINARY_FLOAT(nextafterf)
MOCKTAIL_BINARY_FLOAT(powf)
MOCKTAIL_BINARY_FLOAT(remainderf)

extern "C" double fma(double left, double right, double addend) {
  return __builtin_fma(left, right, addend);
}
extern "C" float fmaf(float left, float right, float addend) {
  return __builtin_fmaf(left, right, addend);
}
extern "C" int finitef(float value) {
  return std::isfinite(value) ? 1 : 0;
}
extern "C" long double fmal(long double left, long double right,
                            long double addend) {
  return __builtin_fmal(left, right, addend);
}
extern "C" int ilogb(double value) {
  return __builtin_ilogb(value);
}
extern "C" long long llround(double value) {
  return __builtin_llround(value);
}
extern "C" long long llroundf(float value) {
  return __builtin_llroundf(value);
}
extern "C" long lround(double value) {
  return __builtin_lround(value);
}
extern "C" long lroundf(float value) {
  return __builtin_lroundf(value);
}
extern "C" double nan(const char* tag) {
  return __builtin_nan(tag);
}
extern "C" long double powl(long double left, long double right) {
  return __builtin_powl(left, right);
}
extern "C" float remquof(float left, float right, int* quotient) {
  return __builtin_remquof(left, right, quotient);
}

extern "C" double frexp(double value, int* exponent) {
  return __builtin_frexp(value, exponent);
}
extern "C" float frexpf(float value, int* exponent) {
  return __builtin_frexpf(value, exponent);
}
extern "C" double ldexp(double value, int exponent) {
  return __builtin_ldexp(value, exponent);
}
extern "C" float ldexpf(float value, int exponent) {
  return __builtin_ldexpf(value, exponent);
}
extern "C" double modf(double value, double* integral) {
  return __builtin_modf(value, integral);
}
extern "C" float modff(float value, float* integral) {
  return __builtin_modff(value, integral);
}
extern "C" void sincos(double value, double* sine, double* cosine) {
  __builtin_sincos(value, sine, cosine);
}
extern "C" void sincosf(float value, float* sine, float* cosine) {
  __builtin_sincosf(value, sine, cosine);
}
