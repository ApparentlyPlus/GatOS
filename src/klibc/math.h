/*
 * math.h - Header file for mathematical functions based on fdlibm
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

/* Utility Functions */
double kfabs(double x);
double kcopysign(double x, double y);
double kscalbn(double x, int n);

/* Rounding and Remainder */
double kfloor(double x);
double kceil(double x);
double ktrunc(double x);
double kround(double x);
double kfmod(double x, double y);

/* Exponential and Logarithmic */
double kexp(double x);
double klog(double x);
double klog1p(double x);

/* Power and Absolute Value */
double kpow(double x, double y);
double ksqrt(double x);

/* Trigonometric */
double ksin(double x);
double kcos(double x);
double ktan(double x);

/* Inverse Trigonometric */
double kasin(double x);
double kacos(double x);
double katan(double x);
double katan2(double y, double x);

/* Hyperbolic */
double ksinh(double x);
double kcosh(double x);
double ktanh(double x);

/* Inverse Hyperbolic */
double kasinh(double x);
double kacosh(double x);
double katanh(double x);
