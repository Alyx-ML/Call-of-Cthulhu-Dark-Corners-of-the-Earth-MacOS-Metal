// The <math.h> of the translated game code (gen/*.c) when the app compiles it on the player's Mac. The app has no
// macOS SDK: its compiler uses -nostdlibinc, clang's own headers (stdint.h, stddef.h) and this folder. The functions
// have the prototypes of the C standard; libSystem gives them when the game library loads. A function that is not
// here stops the compile with an error (implicit declaration).
#pragma once

double sqrt(double);
float sqrtf(float);
double fabs(double);
float fabsf(float);
double floor(double);
double ceil(double);
double trunc(double);
double round(double);
double rint(double);
double nearbyint(double);
double sin(double);
double cos(double);
double tan(double);
double asin(double);
double acos(double);
double atan(double);
double atan2(double, double);
double exp(double);
double exp2(double);
double log(double);
double log2(double);
double log10(double);
double pow(double, double);
double fmod(double, double);
double remainder(double, double);
double ldexp(double, int);
double frexp(double, int *);
double copysign(double, double);

#define signbit(x) __builtin_signbit(x)
#define isinf(x) __builtin_isinf(x)
#define isnan(x) __builtin_isnan(x)
#define isfinite(x) __builtin_isfinite(x)
#define NAN __builtin_nanf("")
#define INFINITY __builtin_inff()

// the x87 constants of the translator (fldpi, fldl2e, fldln2), with the values of the macOS SDK
#define M_PI 3.14159265358979323846264338327950288
#define M_LOG2E 1.44269504088896340735992468100189214
#define M_LN2 0.693147180559945309417232121458176568
