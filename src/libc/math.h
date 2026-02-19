/*
 * math.h - Header file for mathematical functions based on fdlibm
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* Utility Functions */
double fabs(double x);
double copysign(double x, double y);
double scalbn(double x, int n);

/* Rounding and Remainder */
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double fmod(double x, double y);

/* Exponential and Logarithmic */
double exp(double x);
double log(double x);
double log1p(double x);

/* Power and Absolute Value */
double pow(double x, double y);
double sqrt(double x);

/* Trigonometric */
double sin(double x);
double cos(double x);
double tan(double x);

/* Inverse Trigonometric */
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);

/* Hyperbolic */
double sinh(double x);
double cosh(double x);
double tanh(double x);

/* Inverse Hyperbolic */
double asinh(double x);
double acosh(double x);
double atanh(double x);

