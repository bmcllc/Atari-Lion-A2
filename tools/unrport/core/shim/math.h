#pragma once
#ifdef __cplusplus
extern "C" {
#endif
double sqrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double exp(double x);
double log(double x);
double pow(double x, double y);
double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double fabs(double x);
#ifdef __cplusplus
}
#endif
#define _isnan(x) __builtin_isnan(x)
