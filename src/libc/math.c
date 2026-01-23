/*
 * math.c - Implementation of math functions based on fdlibm
 */

#include <stdint.h>
#include <libc/math.h>

typedef union { double f; uint64_t u; } dbl_cast;

/* ========================================================================
 * Common Constants
 * ======================================================================== */

static const double one = 1.0;
static const double zero = 0.0;
static const double two24 = 1.67772160000000000000e+07;
static const double twon24 = 5.96046447753906250000e-08;
static const double huge = 1.000e+300;
static const double tiny = 1.0e-300;

/* ln2 constants */
static const double ln2_hi = 6.93147180369123816490e-01;
static const double ln2_lo = 1.90821492927058770002e-10;
static const double Lg1 = 6.666666666666735130e-01;
static const double Lg2 = 3.999999999940941908e-01;
static const double Lg3 = 2.857142874366239149e-01;
static const double Lg4 = 2.222219843214978396e-01;
static const double Lg5 = 1.818357216161805012e-01;
static const double Lg6 = 1.531383769920937332e-01;
static const double Lg7 = 1.479819860511658591e-01;

/* exp constants */
static const double halF[2] = {0.5, -0.5};
static const double o_threshold = 7.09782712893383973096e+02;
static const double u_threshold = -7.45133219101941108420e+02;
static const double ln2HI[2] = {6.93147180369123816490e-01, -6.93147180369123816490e-01};
static const double ln2LO[2] = {1.90821492927058770002e-10, -1.90821492927058770002e-10};
static const double invln2 = 1.44269504088896338700e+00;
static const double P1 = 1.66666666666666019037e-01;
static const double P2 = -2.77777777770155933842e-03;
static const double P3 = 6.61375632143793436117e-05;
static const double P4 = -1.65339022054652515390e-06;
static const double P5 = 4.13813679705723846039e-08;

/* sqrt constants */
static const double sqrt_one = 1.0;
static const double sqrt_tiny = 1.0e-300;

/* pi constants */
static const double pi = 3.14159265358979311600e+00;
static const double pio2_hi = 1.57079632679489655800e+00;
static const double pio2_lo = 6.12323399573676603587e-17;
static const double pio4 = 7.85398163397448278999e-01;
static const double pio4lo = 3.06161699786838301793e-17;
static const double pi_lo = 1.2246467991473531772E-16;

/* inverse trig polynomial coefficients */
static const double pS0 = 1.66666666666666657415e-01;
static const double pS1 = -3.25565818622400915405e-01;
static const double pS2 = 2.01212532134862925881e-01;
static const double pS3 = -4.00555345006794114027e-02;
static const double pS4 = 7.91534994289814532176e-04;
static const double pS5 = 3.47933107596021167570e-05;
static const double qS1 = -2.40339491173441421878e+00;
static const double qS2 = 2.02094576023350569471e+00;
static const double qS3 = -6.88283971605453293030e-01;
static const double qS4 = 7.70381505559019352791e-02;

/* atan constants */
static const double atanhi[] = {
  4.63647609000806093515e-01,
  7.85398163397448278999e-01,
  9.82793723247329054082e-01,
  1.57079632679489655800e+00,
};

static const double atanlo[] = {
  2.26987774529616870924e-17,
  3.06161699786838301793e-17,
  1.39033110312309984516e-17,
  6.12323399573676603587e-17,
};

static const double aT[] = {
  3.33333333333329318027e-01,
 -1.99999999998764832476e-01,
  1.42857142725034663711e-01,
 -1.11111104054623557880e-01,
  9.09088713343650656196e-02,
 -7.69187620504482999495e-02,
  6.66107313738753120669e-02,
 -5.83357013379057348645e-02,
  4.97687799461593236017e-02,
 -3.65315727442169155270e-02,
  1.62858201153657823623e-02,
};

/* kernel cos/sin/tan constants */
static const double C1 = 4.16666666666666019037e-02;
static const double C2 = -1.38888888888741095749e-03;
static const double C3 = 2.48015872894767294178e-05;
static const double C4 = -2.75573143513906633035e-07;
static const double C5 = 2.08757232129817482790e-09;
static const double C6 = -1.13596475577881948265e-11;

static const double S1 = -1.66666666666666324348e-01;
static const double S2 = 8.33333333332248946124e-03;
static const double S3 = -1.98412698298579493134e-04;
static const double S4 = 2.75573137070700676789e-06;
static const double S5 = -2.50507602534068634195e-08;
static const double S6 = 1.58969099521155010221e-10;

static const double T[] = {
    3.33333333333334091986e-01,
    1.33333333333201242699e-01,
    5.39682539762260521377e-02,
    2.18694882948595424599e-02,
    8.86323982359930005737e-03,
    3.59207910759131235356e-03,
    1.45620945432529025516e-03,
    5.88041240820264096874e-04,
    2.46463134818469906812e-04,
    7.81794442939557092300e-05,
    7.14072491382608190305e-05,
    -1.85586374855275456654e-05,
    2.59073051863633712884e-05,
};

/* rem_pio2 constants */
static const double invpio2 = 6.36619772367581382433e-01;
static const double pio2_1 = 1.57079632673412561417e+00;
static const double pio2_1t = 6.07710050650619224932e-11;
static const double pio2_2 = 6.07710050630396597660e-11;
static const double pio2_2t = 2.02226624879595063154e-21;
static const double pio2_3 = 2.02226624871116645580e-21;
static const double pio2_3t = 8.47842766036889956997e-32;

static const int two_over_pi[] = {
0xA2F983, 0x6E4E44, 0x1529FC, 0x2757D1, 0xF534DD, 0xC0DB62, 
0x95993C, 0x439041, 0xFE5163, 0xABDEBB, 0xC561B7, 0x246E3A, 
0x424DD2, 0xE00649, 0x2EEA09, 0xD1921C, 0xFE1DEB, 0x1CB129, 
0xA73EE8, 0x8235F5, 0x2EBB44, 0x84E99C, 0x7026B4, 0x5F7E41, 
0x3991D6, 0x398353, 0x39F49C, 0x845F8B, 0xBDF928, 0x3B1FF8, 
0x97FFDE, 0x05980F, 0xEF2F11, 0x8B5A0A, 0x6D1F6D, 0x367ECF, 
0x27CB09, 0xB74F46, 0x3F669E, 0x5FEA2D, 0x7527BA, 0xC7EBE5, 
0xF17B3D, 0x0739F7, 0x8A5292, 0xEA6BFB, 0x5FB11F, 0x8D5D08, 
0x560330, 0x46FC7B, 0x6BABF0, 0xCFBC20, 0x9AF436, 0x1DA9E3, 
0x91615E, 0xE61B08, 0x659985, 0x5F14A0, 0x68408D, 0xFFD880, 
0x4D7327, 0x310606, 0x1556CA, 0x73A8C9, 0x60E27B, 0xC08C6B, 
};

static const int npio2_hw[] = {
0x3FF921FB, 0x400921FB, 0x4012D97C, 0x401921FB, 0x401F6A7A, 0x4022D97C,
0x4025FDBB, 0x402921FB, 0x402C463A, 0x402F6A7A, 0x4031475C, 0x4032D97C,
0x40346B9C, 0x4035FDBB, 0x40378FDB, 0x403921FB, 0x403AB41B, 0x403C463A,
0x403DD85A, 0x403F6A7A, 0x40407E4C, 0x4041475C, 0x4042106C, 0x4042D97C,
0x4043A28C, 0x40446B9C, 0x404534AC, 0x4045FDBB, 0x4046C6CB, 0x40478FDB,
0x404858EB, 0x404921FB,
};

static const int init_jk[] = {2,3,4,6};
static const double PIo2[] = {
  1.57079625129699707031e+00,
  7.54978941586159635335e-08,
  5.39030252995776476554e-15,
  3.28200341580791294123e-22,
  1.27065575308067607349e-29,
  1.22933308981111328932e-36,
  2.73370053816464559624e-44,
  2.16741683877804819444e-51,
};

/* Forward declarations */
static int __kernel_rem_pio2(double *x, double *y, int e0, int nx, int prec, const int *ipio2);
static double __kernel_cos(double x, double y);
static double __kernel_sin(double x, double y, int iy);
static double __kernel_tan(double x, double y, int iy);
static int __ieee754_rem_pio2(double x, double *y);

/* ========================================================================
 * Utility Functions
 * ======================================================================== */

static int isnan(double x)
{
    dbl_cast dc = {.f = x};
    return ((dc.u & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) &&
           ((dc.u & 0x000FFFFFFFFFFFFFULL) != 0);
}

double fabs(double x)
{
    dbl_cast dc = {.f = x};
    dc.u &= 0x7FFFFFFFFFFFFFFFULL;
    return dc.f;
}

double copysign(double x, double y)
{
    dbl_cast dcx, dcy;
    dcx.f = x;
    dcy.f = y;
    dcx.u = (dcx.u & 0x7FFFFFFFFFFFFFFFULL) | (dcy.u & 0x8000000000000000ULL);
    return dcx.f;
}

double scalbn(double x, int n)
{
    int k;
    dbl_cast dc;
    dc.f = x;
    int32_t hx = (int32_t)(dc.u >> 32);
    uint32_t lx = (uint32_t)dc.u;
    k = (hx & 0x7ff00000) >> 20;
    
    if (k == 0) {
        if ((lx | (hx & 0x7fffffff)) == 0) return x;
        x *= 1.80143985094819840000e+16;
        dc.f = x;
        hx = (int32_t)(dc.u >> 32);
        k = ((hx & 0x7ff00000) >> 20) - 54;
        if (n < -50000) return tiny * x;
    }
    if (k == 0x7ff) return x + x;
    k = k + n;
    if (k > 0x7fe) return huge * copysign(huge, x);
    if (k > 0) {
        dc.u = (dc.u & 0x800fffff00000000ULL) | ((uint64_t)k << 52);
        return dc.f;
    }
    if (k <= -54) {
        if (n > 50000) return huge * copysign(huge, x);
        else return tiny * copysign(tiny, x);
    }
    k += 54;
    dc.u = (dc.u & 0x800fffff00000000ULL) | ((uint64_t)k << 52);
    return dc.f * 5.55111512312578270212e-17;
}

double floor(double x)
{
    dbl_cast dc;
    dc.f = x;
    int32_t i0 = dc.u >> 32;
    uint32_t i1 = dc.u;
    int32_t j0 = ((i0 >> 20) & 0x7ff) - 0x3ff;
    
    if (j0 < 20) {
        if (j0 < 0) {
            if (i0 >= 0) {
                i0 = i1 = 0;
            } else if (((i0 & 0x7fffffff) | i1) != 0) {
                i0 = 0xbff00000;
                i1 = 0;
            }
        } else {
            uint32_t i = (0x000fffff) >> j0;
            if (((i0 & i) | i1) == 0) return x;
            if (i0 < 0) i0 += (0x00100000) >> j0;
            i0 &= (~i);
            i1 = 0;
        }
    } else if (j0 > 51) {
        if (j0 == 0x400) return x + x;
        else return x;
    } else {
        uint32_t i = ((uint32_t)(0xffffffff)) >> (j0 - 20);
        if ((i1 & i) == 0) return x;
        if (i0 < 0) {
            if (j0 == 20) i0 += 1;
            else {
                uint32_t j = i1 + (1 << (52 - j0));
                if (j < i1) i0 += 1;
                i1 = j;
            }
        }
        i1 &= (~i);
    }
    dc.u = ((uint64_t)i0 << 32) | i1;
    return dc.f;
}

/* ========================================================================
 * Logarithm and Exponential Functions
 * ======================================================================== */

double log(double x)
{
    if (x == 0.0) return -1.0 / 0.0; // Return -inf
    if (x < 0.0) {
        return 0.0 / 0.0; 
    }
    dbl_cast dc = {.f = x};
    uint64_t bits = dc.u;
    
    int32_t hx = (int32_t)(bits >> 32);
    int32_t k = (hx >> 20) - 1023;
    hx &= 0x000fffff;
    
    int32_t i = (hx + 0x95f64) & 0x100000;
    dc.u = (bits & 0x000fffffffffffffULL) | ((uint64_t)(i ^ 0x3ff00000) << 32);
    k += (i >> 20);
    
    double f = dc.f - 1.0;
    double dk = (double)k;
    
    if ((hx + 2) < 5) {
        if (f == 0.0) return dk * ln2_hi + dk * ln2_lo;
        double R = f * f * (0.5 - 0.33333333333333333 * f);
        return dk * ln2_hi - ((R - dk * ln2_lo) - f);
    }
    
    double s = f / (2.0 + f);
    double z = s * s;
    double w = z * z;
    double t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    double t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    double R = t1 + t2;
    double hfsq = 0.5 * f * f;
    
    i = hx - 0x6147a;
    int32_t j = 0x6b851 - hx;
    
    if ((i | j) > 0) {
        return dk * ln2_hi - ((hfsq - (s * (hfsq + R) + dk * ln2_lo)) - f);
    } else {
        return dk * ln2_hi - ((s * (f - R) - dk * ln2_lo) - f);
    }
}

double exp(double x)
{
    dbl_cast dc;
    dc.f = x;
    
    uint32_t hx = (uint32_t)(dc.u >> 32);
    int xsb = (hx >> 31) & 1;
    hx &= 0x7fffffff;
    
    if (hx >= 0x40862E42) {
        if (hx >= 0x7ff00000) {
            uint32_t lx = (uint32_t)(dc.u & 0xFFFFFFFF);
            if (((hx & 0xfffff) | lx) != 0) return x + x;
            return (xsb == 0) ? x : 0.0;
        }
        if (x > o_threshold) return 1e300 * 1e300;
        if (x < u_threshold) return 1e-300 * 1e-300;
    }
    
    int k;
    double hi, lo, c, t;
    
    if (hx > 0x3fd62e42) {
        if (hx < 0x3FF0A2B2) {
            hi = x - ln2HI[xsb];
            lo = ln2LO[xsb];
            k = 1 - xsb - xsb;
        } else {
            k = (int)(invln2 * x + halF[xsb]);
            t = k;
            hi = x - t * ln2HI[0];
            lo = t * ln2LO[0];
        }
        x = hi - lo;
    } else if (hx < 0x3e300000) {
        return one + x;
    } else {
        k = 0;
    }
    
    t = x * x;
    c = x - t * (P1 + t * (P2 + t * (P3 + t * (P4 + t * P5))));
    
    double y;
    if (k == 0) {
        return one - ((x * c) / (c - 2.0) - x);
    } else {
        y = one - ((lo - (x * c) / (2.0 - c)) - hi);
    }
    
    if (k >= -1021) {
        dc.f = y;
        dc.u += ((uint64_t)k << 52);
        return dc.f;
    } else {
        dc.f = y;
        dc.u += ((uint64_t)(k + 1000) << 52);
        return dc.f * 9.33263618503218878990e-302;
    }
}

double log1p(double x)
{
    double hfsq, f, c, s, z, R, u;
    int k, hx, hu, ax;
    dbl_cast dc;
    dc.f = x;
    int32_t hx_ = (int32_t)(dc.u >> 32);
    ax = hx_ & 0x7fffffff;

    k = 1;
    if (hx_ < 0x3FDA827A) {
        if (ax >= 0x3ff00000) {
            if (x == -1.0) return -1.80143985094819840000e+16 / zero;
            else return (x - x) / (x - x);
        }
        if (ax < 0x3e200000) {
            if (1.80143985094819840000e+16 + x > zero && ax < 0x3c900000)
                return x;
            else
                return x - x * x * 0.5;
        }
        if (hx_ > 0 || hx_ <= ((int)0xbfd2bec3)) {
            k = 0;
            f = x;
            hu = 1;
        }
    }
    if (hx_ >= 0x7ff00000) return x + x;
    if (k != 0) {
        if (hx_ < 0x43400000) {
            u = 1.0 + x;
            dbl_cast dcu;
            dcu.f = u;
            hu = (int32_t)(dcu.u >> 32);
            k = (hu >> 20) - 1023;
            c = (k > 0) ? 1.0 - (u - x) : x - (u - 1.0);
            c /= u;
        } else {
            u = x;
            dbl_cast dcu;
            dcu.f = u;
            hu = (int32_t)(dcu.u >> 32);
            k = (hu >> 20) - 1023;
            c = 0;
        }
        hu &= 0x000fffff;
        if (hu < 0x6a09e) {
            dbl_cast dcu;
            dcu.f = u;
            dcu.u = (dcu.u & 0x800fffff00000000ULL) | 0x3ff0000000000000ULL;
            u = dcu.f;
        } else {
            k += 1;
            dbl_cast dcu;
            dcu.f = u;
            dcu.u = (dcu.u & 0x800fffff00000000ULL) | 0x3fe0000000000000ULL;
            u = dcu.f;
            hu = (0x00100000 - hu) >> 2;
        }
        f = u - 1.0;
    }
    hfsq = 0.5 * f * f;
    if (hu == 0) {
        if (f == zero) {
            if (k == 0) return zero;
            else {
                c += k * ln2_lo;
                return k * ln2_hi + c;
            }
        }
        R = hfsq * (1.0 - 0.66666666666666666 * f);
        if (k == 0) return f - R;
        else return k * ln2_hi - ((R - (k * ln2_lo + c)) - f);
    }
    s = f / (2.0 + f);
    z = s * s;
    R = z * (Lg1 + z * (Lg2 + z * (Lg3 + z * (Lg4 + z * (Lg5 + z * (Lg6 + z * Lg7))))));
    if (k == 0) return f - (hfsq - s * (hfsq + R));
    else return k * ln2_hi - ((hfsq - (s * (hfsq + R) + (k * ln2_lo + c))) - f);
}

/* ========================================================================
 * Square Root
 * ======================================================================== */

static double __ieee754_sqrt(double x)
{
    double z;
    int32_t sign = (int32_t)0x80000000;
    uint32_t r, t1, s1, ix1, q1;
    int32_t ix0, s0, q, m, t, i;

    dbl_cast dc;
    dc.f = x;
    ix0 = (int32_t)(dc.u >> 32);
    ix1 = (uint32_t)dc.u;

    if ((ix0 & 0x7ff00000) == 0x7ff00000) {
        return x * x + x;
    }
    if (ix0 <= 0) {
        if (((ix0 & (~sign)) | ix1) == 0) return x;
        else if (ix0 < 0)
            return (x - x) / (x - x);
    }
    m = (ix0 >> 20);
    if (m == 0) {
        while (ix0 == 0) {
            m -= 21;
            ix0 |= (ix1 >> 11);
            ix1 <<= 21;
        }
        for (i = 0; (ix0 & 0x00100000) == 0; i++) ix0 <<= 1;
        m -= i - 1;
        ix0 |= (ix1 >> (32 - i));
        ix1 <<= i;
    }
    m -= 1023;
    ix0 = (ix0 & 0x000fffff) | 0x00100000;
    if (m & 1) {
        ix0 += ix0 + ((ix1 & sign) >> 31);
        ix1 += ix1;
    }
    m >>= 1;

    ix0 += ix0 + ((ix1 & sign) >> 31);
    ix1 += ix1;
    q = q1 = s0 = s1 = 0;
    r = 0x00200000;

    while (r != 0) {
        t = s0 + r;
        if (t <= ix0) {
            s0 = t + r;
            ix0 -= t;
            q += r;
        }
        ix0 += ix0 + ((ix1 & sign) >> 31);
        ix1 += ix1;
        r >>= 1;
    }

    r = sign;
    while (r != 0) {
        t1 = s1 + r;
        t = s0;
        if ((t < ix0) || ((t == ix0) && (t1 <= ix1))) {
            s1 = t1 + r;
            if (((t1 & sign) == (uint32_t)sign) && (s1 & sign) == 0) s0 += 1;
            ix0 -= t;
            if (ix1 < t1) ix0 -= 1;
            ix1 -= t1;
            q1 += r;
        }
        ix0 += ix0 + ((ix1 & sign) >> 31);
        ix1 += ix1;
        r >>= 1;
    }

    if ((ix0 | ix1) != 0) {
        z = sqrt_one - sqrt_tiny;
        if (z >= sqrt_one) {
            z = sqrt_one + sqrt_tiny;
            if (q1 == (uint32_t)0xffffffff) {
                q1 = 0;
                q += 1;
            } else if (z > sqrt_one) {
                if (q1 == (uint32_t)0xfffffffe) q += 1;
                q1 += 2;
            } else
                q1 += (q1 & 1);
        }
    }
    ix0 = (q >> 1) + 0x3fe00000;
    ix1 = q1 >> 1;
    if ((q & 1) == 1) ix1 |= sign;
    ix0 += (m << 20);

    dc.u = ((uint64_t)ix0 << 32) | ix1;
    z = dc.f;
    return z;
}

double sqrt(double x)
{
    double z;
    z = __ieee754_sqrt(x);
    if (isnan(x)) return z;
    if (x < 0.0) {
        return (0.0 / 0.0);
    } else
        return z;
}

/* ========================================================================
 * Kernel Functions for Trigonometry
 * ======================================================================== */

static int __kernel_rem_pio2(double *x, double *y, int e0, int nx, int prec, const int *ipio2)
{
    int jz, jx, jv, jp, jk, carry, n, iq[20], i, j, k, m, q0, ih;
    double z, fw, f[20], fq[20], q[20];

    jk = init_jk[prec];
    jp = jk;
    jx = nx - 1;
    jv = (e0 - 3) / 24;
    if (jv < 0) jv = 0;
    q0 = e0 - 24 * (jv + 1);

    j = jv - jx;
    m = jx + jk;
    for (i = 0; i <= m; i++, j++)
        f[i] = (j < 0) ? zero : (double)ipio2[j];

    for (i = 0; i <= jk; i++) {
        for (j = 0, fw = 0.0; j <= jx; j++)
            fw += x[j] * f[jx + i - j];
        q[i] = fw;
    }

    jz = jk;
recompute:
    for (i = 0, j = jz, z = q[jz]; j > 0; i++, j--) {
        fw = (double)((int)(twon24 * z));
        iq[i] = (int)(z - two24 * fw);
        z = q[j - 1] + fw;
    }

    z = scalbn(z, q0);
    z -= 8.0 * floor(z * 0.125);
    n = (int)z;
    z -= (double)n;
    ih = 0;
    if (q0 > 0) {
        i = (iq[jz - 1] >> (24 - q0));
        n += i;
        iq[jz - 1] -= i << (24 - q0);
        ih = iq[jz - 1] >> (23 - q0);
    } else if (q0 == 0)
        ih = iq[jz - 1] >> 23;
    else if (z >= 0.5)
        ih = 2;

    if (ih > 0) {
        n += 1;
        carry = 0;
        for (i = 0; i < jz; i++) {
            j = iq[i];
            if (carry == 0) {
                if (j != 0) {
                    carry = 1;
                    iq[i] = 0x1000000 - j;
                }
            } else
                iq[i] = 0xffffff - j;
        }
        if (q0 > 0) {
            switch (q0) {
            case 1:
                iq[jz - 1] &= 0x7fffff;
                break;
            case 2:
                iq[jz - 1] &= 0x3fffff;
                break;
            }
        }
        if (ih == 2) {
            z = one - z;
            if (carry != 0)
                z -= scalbn(one, q0);
        }
    }

    if (z == zero) {
        j = 0;
        for (i = jz - 1; i >= jk; i--)
            j |= iq[i];
        if (j == 0) {
            for (k = 1; iq[jk - k] == 0; k++)
                ;
            for (i = jz + 1; i <= jz + k; i++) {
                f[jx + i] = (double)ipio2[jv + i];
                for (j = 0, fw = 0.0; j <= jx; j++)
                    fw += x[j] * f[jx + i - j];
                q[i] = fw;
            }
            jz += k;
            goto recompute;
        }
    }

    if (z == 0.0) {
        jz -= 1;
        q0 -= 24;
        while (iq[jz] == 0) {
            jz--;
            q0 -= 24;
        }
    } else {
        z = scalbn(z, -q0);
        if (z >= two24) {
            fw = (double)((int)(twon24 * z));
            iq[jz] = (int)(z - two24 * fw);
            jz += 1;
            q0 += 24;
            iq[jz] = (int)fw;
        } else
            iq[jz] = (int)z;
    }

    fw = scalbn(one, q0);
    for (i = jz; i >= 0; i--) {
        q[i] = fw * (double)iq[i];
        fw *= twon24;
    }

    for (i = jz; i >= 0; i--) {
        for (fw = 0.0, k = 0; k <= jp && k <= jz - i; k++)
            fw += PIo2[k] * q[i + k];
        fq[jz - i] = fw;
    }

    switch (prec) {
    case 0:
        fw = 0.0;
        for (i = jz; i >= 0; i--)
            fw += fq[i];
        y[0] = (ih == 0) ? fw : -fw;
        break;
    case 1:
    case 2:
        fw = 0.0;
        for (i = jz; i >= 0; i--)
            fw += fq[i];
        y[0] = (ih == 0) ? fw : -fw;
        fw = fq[0] - fw;
        for (i = 1; i <= jz; i++)
            fw += fq[i];
        y[1] = (ih == 0) ? fw : -fw;
        break;
    case 3:
        for (i = jz; i > 0; i--) {
            fw = fq[i - 1] + fq[i];
            fq[i] += fq[i - 1] - fw;
            fq[i - 1] = fw;
        }
        for (i = jz; i > 1; i--) {
            fw = fq[i - 1] + fq[i];
            fq[i] += fq[i - 1] - fw;
            fq[i - 1] = fw;
        }
        for (fw = 0.0, i = jz; i >= 2; i--)
            fw += fq[i];
        if (ih == 0) {
            y[0] = fq[0];
            y[1] = fq[1];
            y[2] = fw;
        } else {
            y[0] = -fq[0];
            y[1] = -fq[1];
            y[2] = -fw;
        }
    }
    return n & 7;
}

static int __ieee754_rem_pio2(double x, double *y)
{
    double z, w, t, r, fn;
    double tx[3];
    int e0, i, j, nx, n;
    dbl_cast dc;
    dc.f = x;
    int32_t hx = (int32_t)(dc.u >> 32);
    int32_t ix = hx & 0x7fffffff;

    if (ix <= 0x3fe921fb) {
        y[0] = x;
        y[1] = 0;
        return 0;
    }
    if (ix < 0x4002d97c) {
        if (hx > 0) {
            z = x - pio2_1;
            if (ix != 0x3ff921fb) {
                y[0] = z - pio2_1t;
                y[1] = (z - y[0]) - pio2_1t;
            } else {
                z -= pio2_2;
                y[0] = z - pio2_2t;
                y[1] = (z - y[0]) - pio2_2t;
            }
            return 1;
        } else {
            z = x + pio2_1;
            if (ix != 0x3ff921fb) {
                y[0] = z + pio2_1t;
                y[1] = (z - y[0]) + pio2_1t;
            } else {
                z += pio2_2;
                y[0] = z + pio2_2t;
                y[1] = (z - y[0]) + pio2_2t;
            }
            return -1;
        }
    }
    if (ix <= 0x413921fb) {
        t = fabs(x);
        n = (int)(t * invpio2 + 0.5);
        fn = (double)n;
        r = t - fn * pio2_1;
        w = fn * pio2_1t;
        if (n < 32 && ix != npio2_hw[n - 1]) {
            y[0] = r - w;
        } else {
            j = ix >> 20;
            y[0] = r - w;
            dc.f = y[0];
            i = j - (((int32_t)(dc.u >> 32)) & 0x7ff);
            if (i > 16) {
                t = r;
                w = fn * pio2_2;
                r = t - w;
                w = fn * pio2_2t - ((t - r) - w);
                y[0] = r - w;
                dc.f = y[0];
                i = j - (((int32_t)(dc.u >> 32)) & 0x7ff);
                if (i > 49) {
                    t = r;
                    w = fn * pio2_3;
                    r = t - w;
                    w = fn * pio2_3t - ((t - r) - w);
                    y[0] = r - w;
                }
            }
        }
        y[1] = (r - y[0]) - w;
        if (hx < 0) {
            y[0] = -y[0];
            y[1] = -y[1];
            return -n;
        } else
            return n;
    }
    if (ix >= 0x7ff00000) {
        y[0] = y[1] = x - x;
        return 0;
    }
    dc.f = x;
    e0 = (ix >> 20) - 1046;
    dc.u = ((uint64_t)(ix - ((int32_t)e0 << 20)) << 32) | ((uint32_t)dc.u);
    z = dc.f;
    for (i = 0; i < 2; i++) {
        tx[i] = (double)((int)(z));
        z = (z - tx[i]) * two24;
    }
    tx[2] = z;
    nx = 3;
    while (tx[nx - 1] == zero)
        nx--;
    n = __kernel_rem_pio2(tx, y, e0, nx, 2, two_over_pi);
    if (hx < 0) {
        y[0] = -y[0];
        y[1] = -y[1];
        return -n;
    }
    return n;
}

static double __kernel_cos(double x, double y)
{
    double a, hz, z, r, qx;
    dbl_cast dc;
    dc.f = x;
    int32_t ix = (int32_t)(dc.u >> 32) & 0x7fffffff;

    if (ix < 0x3e400000) {
        if (((int)x) == 0)
            return one;
    }
    z = x * x;
    r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
    if (ix < 0x3FD33333)
        return one - (0.5 * z - (z * r - x * y));
    else {
        if (ix > 0x3fe90000) {
            qx = 0.28125;
        } else {
            dc.u = (uint64_t)(ix - 0x00200000) << 32;
            qx = dc.f;
        }
        hz = 0.5 * z - qx;
        a = one - qx;
        return a - (hz - (z * r - x * y));
    }
}

static double __kernel_sin(double x, double y, int iy)
{
    double z, r, v;
    dbl_cast dc;
    dc.f = x;
    int32_t ix = (int32_t)(dc.u >> 32) & 0x7fffffff;

    if (ix < 0x3e400000) {
        if ((int)x == 0)
            return x;
    }
    z = x * x;
    v = z * x;
    r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    if (iy == 0)
        return x + v * (S1 + z * r);
    else
        return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

static double __kernel_tan(double x, double y, int iy)
{
    double z, r, v, w, s;
    dbl_cast dc;
    dc.f = x;
    int32_t hx = (int32_t)(dc.u >> 32);
    int32_t ix = hx & 0x7fffffff;

    if (ix < 0x3e300000) {
        if ((int)x == 0) {
            if (((ix | (uint32_t)dc.u) | (iy + 1)) == 0)
                return one / fabs(x);
            else {
                if (iy == 1)
                    return x;
                else {
                    double a, t;
                    z = w = x + y;
                    dbl_cast dcz;
                    dcz.f = z;
                    dcz.u &= 0xFFFFFFFF00000000ULL;
                    z = dcz.f;
                    v = y - (z - x);
                    t = a = -one / w;
                    dcz.f = t;
                    dcz.u &= 0xFFFFFFFF00000000ULL;
                    t = dcz.f;
                    s = one + t * z;
                    return t + a * (s + t * v);
                }
            }
        }
    }
    if (ix >= 0x3FE59428) {
        if (hx < 0) {
            x = -x;
            y = -y;
        }
        z = pio4 - x;
        w = pio4lo - y;
        x = z + w;
        y = 0.0;
    }
    z = x * x;
    w = z * z;
    r = T[1] + w * (T[3] + w * (T[5] + w * (T[7] + w * (T[9] + w * T[11]))));
    v = z * (T[2] + w * (T[4] + w * (T[6] + w * (T[8] + w * (T[10] + w * T[12])))));
    s = z * x;
    r = y + z * (s * (r + v) + y);
    r += T[0] * s;
    w = x + r;
    if (ix >= 0x3FE59428) {
        v = (double)iy;
        return (double)(1 - ((hx >> 30) & 2)) * (v - 2.0 * (x - (w * w / (w + v) - r)));
    }
    if (iy == 1)
        return w;
    else {
        double a, t;
        z = w;
        dbl_cast dcz;
        dcz.f = z;
        dcz.u &= 0xFFFFFFFF00000000ULL;
        z = dcz.f;
        v = r - (z - x);
        t = a = -1.0 / w;
        dcz.f = t;
        dcz.u &= 0xFFFFFFFF00000000ULL;
        t = dcz.f;
        s = 1.0 + t * z;
        return t + a * (s + t * v);
    }
}

/* ========================================================================
 * Trigonometric Functions
 * ======================================================================== */

double sin(double x)
{
    double y[2], z = 0.0;
    int n;
    dbl_cast dc;
    dc.f = x;
    int32_t ix = (int32_t)(dc.u >> 32) & 0x7fffffff;

    if (ix <= 0x3fe921fb)
        return __kernel_sin(x, z, 0);
    else if (ix >= 0x7ff00000)
        return x - x;
    else {
        n = __ieee754_rem_pio2(x, y);
        switch (n & 3) {
        case 0:
            return __kernel_sin(y[0], y[1], 1);
        case 1:
            return __kernel_cos(y[0], y[1]);
        case 2:
            return -__kernel_sin(y[0], y[1], 1);
        default:
            return -__kernel_cos(y[0], y[1]);
        }
    }
}

double cos(double x)
{
    double y[2], z = 0.0;
    int n;
    dbl_cast dc;
    dc.f = x;
    int32_t ix = (int32_t)(dc.u >> 32) & 0x7fffffff;

    if (ix <= 0x3fe921fb)
        return __kernel_cos(x, z);
    else if (ix >= 0x7ff00000)
        return x - x;
    else {
        n = __ieee754_rem_pio2(x, y);
        switch (n & 3) {
        case 0:
            return __kernel_cos(y[0], y[1]);
        case 1:
            return -__kernel_sin(y[0], y[1], 1);
        case 2:
            return -__kernel_cos(y[0], y[1]);
        default:
            return __kernel_sin(y[0], y[1], 1);
        }
    }
}

double tan(double x)
{
    double y[2], z = 0.0;
    int n;
    dbl_cast dc;
    dc.f = x;
    int32_t ix = (int32_t)(dc.u >> 32) & 0x7fffffff;

    if (ix <= 0x3fe921fb)
        return __kernel_tan(x, z, 1);
    else if (ix >= 0x7ff00000)
        return x - x;
    else {
        n = __ieee754_rem_pio2(x, y);
        return __kernel_tan(y[0], y[1], 1 - ((n & 1) << 1));
    }
}

/* ========================================================================
 * Inverse Trigonometric Functions
 * ======================================================================== */

double asin(double x)
{
    double t, w, p, q, c, r, s;
    dbl_cast dc;
    dc.f = x;
    int32_t hx = (int32_t)(dc.u >> 32);
    uint32_t lx = (uint32_t)dc.u;
    int32_t ix = hx & 0x7fffffff;

    if (ix >= 0x3ff00000) {
        if (((ix - 0x3ff00000) | lx) == 0)
            return x * pio2_hi + x * pio2_lo;
        return (x - x) / (x - x);
    } else if (ix < 0x3fe00000) {
        if (ix < 0x3e400000) {
            if (huge + x > one)
                return x;
        } else {
            t = x * x;
            p = t * (pS0 + t * (pS1 + t * (pS2 + t * (pS3 + t * (pS4 + t * pS5)))));
            q = one + t * (qS1 + t * (qS2 + t * (qS3 + t * qS4)));
            w = p / q;
            return x + x * w;
        }
    }
    w = one - fabs(x);
    t = w * 0.5;
    p = t * (pS0 + t * (pS1 + t * (pS2 + t * (pS3 + t * (pS4 + t * pS5)))));
    q = one + t * (qS1 + t * (qS2 + t * (qS3 + t * qS4)));
    s = sqrt(t);
    if (ix >= 0x3FEF3333) {
        w = p / q;
        t = pio2_hi - (2.0 * (s + s * w) - pio2_lo);
    } else {
        dbl_cast ws;
        ws.f = s;
        ws.u &= 0xFFFFFFFF00000000ULL;
        w = ws.f;
        c = (t - w * w) / (s + w);
        r = p / q;
        p = 2.0 * s * r - (pio2_lo - 2.0 * c);
        q = pio4 - 2.0 * w;
        t = pio4 - (p - q);
    }
    if (hx > 0)
        return t;
    else
        return -t;
}

double acos(double x)
{
    double z, p, q, r, w, s, c, df;
    dbl_cast dc;
    dc.f = x;
    int32_t hx = (int32_t)(dc.u >> 32);
    uint32_t lx = (uint32_t)dc.u;
    int32_t ix = hx & 0x7fffffff;

    if (ix >= 0x3ff00000) {
        if (((ix - 0x3ff00000) | lx) == 0) {
            if (hx > 0)
                return 0.0;
            else
                return pi + 2.0 * pio2_lo;
        }
        return (x - x) / (x - x);
    }
    if (ix < 0x3fe00000) {
        if (ix <= 0x3c600000)
            return pio2_hi + pio2_lo;
        z = x * x;
        p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
        q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
        r = p / q;
        return pio2_hi - (x - (pio2_lo - x * r));
    } else if (hx < 0) {
        z = (one + x) * 0.5;
        p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
        q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
        s = sqrt(z);
        r = p / q;
        w = r * s - pio2_lo;
        return pi - 2.0 * (s + w);
    } else {
        z = (one - x) * 0.5;
        s = sqrt(z);
        dbl_cast dfs;
        dfs.f = s;
        dfs.u &= 0xFFFFFFFF00000000ULL;
        df = dfs.f;
        c = (z - df * df) / (s + df);
        p = z * (pS0 + z * (pS1 + z * (pS2 + z * (pS3 + z * (pS4 + z * pS5)))));
        q = one + z * (qS1 + z * (qS2 + z * (qS3 + z * qS4)));
        r = p / q;
        w = r * s + c;
        return 2.0 * (df + w);
    }
}

double atan(double x)
{
    double w, s1, s2, z;
    dbl_cast dc;
    dc.f = x;
    int32_t hx = (int32_t)(dc.u >> 32);
    int32_t ix = hx & 0x7fffffff;
    int id;

    if (ix >= 0x44100000) {
        if (ix > 0x7ff00000 || (ix == 0x7ff00000 && ((uint32_t)dc.u != 0)))
            return x + x;
        if (hx > 0)
            return atanhi[3] + atanlo[3];
        else
            return -atanhi[3] - atanlo[3];
    }
    if (ix < 0x3fdc0000) {
        if (ix < 0x3e200000) {
            if (huge + x > one)
                return x;
        }
        id = -1;
    } else {
        x = fabs(x);
        if (ix < 0x3ff30000) {
            if (ix < 0x3fe60000) {
                id = 0;
                x = (2.0 * x - one) / (2.0 + x);
            } else {
                id = 1;
                x = (x - one) / (x + one);
            }
        } else {
            if (ix < 0x40038000) {
                id = 2;
                x = (x - 1.5) / (one + 1.5 * x);
            } else {
                id = 3;
                x = -1.0 / x;
            }
        }
    }
    z = x * x;
    w = z * z;
    s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
    s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
    if (id < 0)
        return x - x * (s1 + s2);
    else {
        z = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
        return (hx < 0) ? -z : z;
    }
}

double atan2(double y, double x)
{
    double z;
    int k, m;
    dbl_cast dcx, dcy;
    dcx.f = x;
    dcy.f = y;
    int32_t hx = (int32_t)(dcx.u >> 32);
    int32_t hy = (int32_t)(dcy.u >> 32);
    uint32_t lx = (uint32_t)dcx.u;
    uint32_t ly = (uint32_t)dcy.u;
    int32_t ix = hx & 0x7fffffff;
    int32_t iy = hy & 0x7fffffff;

    if (((ix | ((lx | -lx) >> 31)) > 0x7ff00000) ||
        ((iy | ((ly | -ly) >> 31)) > 0x7ff00000))
        return x + y;
    if ((hx - 0x3ff00000 | lx) == 0)
        return atan(y);
    m = ((hy >> 31) & 1) | ((hx >> 30) & 2);

    if ((iy | ly) == 0) {
        switch (m) {
        case 0:
        case 1:
            return y;
        case 2:
            return pi + tiny;
        case 3:
            return -pi - tiny;
        }
    }
    if ((ix | lx) == 0)
        return (hy < 0) ? -pio2_hi - tiny : pio2_hi + tiny;

    if (ix == 0x7ff00000) {
        if (iy == 0x7ff00000) {
            switch (m) {
            case 0:
                return pio4 + tiny;
            case 1:
                return -pio4 - tiny;
            case 2:
                return 3.0 * pio4 + tiny;
            case 3:
                return -3.0 * pio4 - tiny;
            }
        } else {
            switch (m) {
            case 0:
                return zero;
            case 1:
                return -zero;
            case 2:
                return pi + tiny;
            case 3:
                return -pi - tiny;
            }
        }
    }
    if (iy == 0x7ff00000)
        return (hy < 0) ? -pio2_hi - tiny : pio2_hi + tiny;

    k = (iy - ix) >> 20;
    if (k > 60)
        z = pio2_hi + 0.5 * pio2_lo;
    else if (hx < 0 && k < -60)
        z = 0.0;
    else
        z = atan(fabs(y / x));
    switch (m) {
    case 0:
        return z;
    case 1: {
        dbl_cast dcz;
        dcz.f = z;
        dcz.u |= 0x8000000000000000ULL;
        return dcz.f;
    }
    case 2:
        return pi - (z - pi_lo);
    default:
        return (z - pi_lo) - pi;
    }
}

/* ========================================================================
 * Hyperbolic Functions
 * ======================================================================== */

double asinh(double x)
{
    double t, w;
    dbl_cast dc;
    dc.f = x;
    int32_t hx = (int32_t)(dc.u >> 32);
    int32_t ix = hx & 0x7fffffff;

    if (ix >= 0x7ff00000)
        return x + x;
    if (ix < 0x3e300000) {
        if (huge + x > one)
            return x;
    }
    if (ix > 0x41b00000) {
        w = log(fabs(x)) + ln2_hi;
    } else if (ix > 0x40000000) {
        t = fabs(x);
        w = log(2.0 * t + one / (sqrt(x * x + one) + t));
    } else {
        t = x * x;
        w = log1p(fabs(x) + t / (one + sqrt(one + t)));
    }
    if (hx > 0)
        return w;
    else
        return -w;
}

double acosh(double x)
{
    double t;
    dbl_cast dc;
    dc.f = x;
    int32_t hx = (int32_t)(dc.u >> 32);
    uint32_t lx = (uint32_t)dc.u;

    if (hx < 0x3ff00000) {
        return (x - x) / (x - x);
    } else if (hx >= 0x41b00000) {
        if (hx >= 0x7ff00000) {
            return x + x;
        } else
            return log(x) + ln2_hi;
    } else if (((hx - 0x3ff00000) | lx) == 0) {
        return 0.0;
    } else if (hx > 0x40000000) {
        t = x * x;
        return log(2.0 * x - one / (x + sqrt(t - one)));
    } else {
        t = x - one;
        return log1p(t + sqrt(2.0 * t + t * t));
    }
}

double atanh(double x)
{
    double t;
    dbl_cast dc;
    dc.f = x;
    int32_t hx = (int32_t)(dc.u >> 32);
    uint32_t lx = (uint32_t)dc.u;
    int32_t ix = hx & 0x7fffffff;

    if ((ix | ((lx | (-lx)) >> 31)) > 0x3ff00000)
        return (x - x) / (x - x);
    if (ix == 0x3ff00000)
        return x / zero;
    if (ix < 0x3e300000 && (huge + x) > zero)
        return x;
    dc.u &= 0x7FFFFFFFFFFFFFFFULL;
    x = dc.f;
    if (ix < 0x3fe00000) {
        t = x + x;
        t = 0.5 * log1p(t + t * x / (one - x));
    } else
        t = 0.5 * log1p((x + x) / (one - x));
    if (hx >= 0)
        return t;
    else
        return -t;
}

double sinh(double x)
{
    double t, w, h;
    dbl_cast dc;
    dc.f = x;
    int32_t jx = (int32_t)(dc.u >> 32);
    int32_t ix = jx & 0x7fffffff;

    if (ix >= 0x7ff00000)
        return x + x;

    h = 0.5;
    if (jx < 0)
        h = -h;
    if (ix < 0x40862E42) {
        t = fabs(x);
        if (ix < 0x3e300000)
            if (huge + x > one)
                return x;
        if (ix < 0x3ff00000) {
            if (ix < 0x3FD62E43) {
                return h * (2.0 * t - 2.0 * t * t / (t + sqrt(one + t * t)));
            } else {
                return h * (2.0 * t - 2.0 * (sqrt(one + t * t) - one) / t);
            }
        }
        w = exp(t);
        return h * (w - one / w);
    }
    if (ix < 0x40862E42)
        return h * exp(fabs(x));
    if (ix <= 0x408633CE)
        return h * 2.0 * exp(fabs(x) - ln2_hi);
    return x * huge * huge;
}

double cosh(double x)
{
    double t, w;
    dbl_cast dc;
    dc.f = x;
    int32_t ix = (int32_t)(dc.u >> 32) & 0x7fffffff;

    if (ix >= 0x7ff00000)
        return x * x;

    if (ix < 0x3fd62e43) {
        t = fabs(x);
        if (ix < 0x3c800000)
            return one;
        return one + t * t / (2.0 * one);
    }

    if (ix < 0x40862E42) {
        t = exp(fabs(x));
        return 0.5 * t + 0.5 / t;
    }

    if (ix < 0x40862E42)
        return 0.5 * exp(fabs(x));

    if (ix <= 0x408633CE) {
        w = exp(fabs(x) - ln2_hi);
        return w + w;
    }

    return huge * huge;
}

double tanh(double x)
{
    double t, z;
    dbl_cast dc;
    dc.f = x;
    int32_t jx = (int32_t)(dc.u >> 32);
    int32_t ix = jx & 0x7fffffff;

    if (ix >= 0x7ff00000) {
        if (jx >= 0)
            return one / x + one;
        else
            return one / x - one;
    }

    if (ix < 0x40360000) {
        if (ix < 0x3c800000)
            return x;
        if (ix >= 0x3ff00000) {
            t = exp(2.0 * fabs(x));
            z = one - 2.0 / (t + 2.0);
        } else {
            t = -fabs(x);
            z = -t;
            t = exp(t + t);
            z = z + 2.0 / (2.0 - t);
        }
    } else {
        z = one - tiny;
    }
    return (jx >= 0) ? z : -z;
}

/* ========================================================================
 * Power and Related Functions
 * ======================================================================== */

double pow(double x, double y)
{
    double z, ax, z_h, z_l, p_h, p_l;
    double y1, t1, t2, r, s, t, u, v, w;
    int i, j, k, yisint, n;
    int hx, hy, ix, iy;
    unsigned lx, ly;

    /* Constants needed throughout the function */
    static const double bp[] = {1.0, 1.5};
    static const double dp_h[] = {0.0, 5.84962487220764160156e-01};
    static const double dp_l[] = {0.0, 1.35003920212974897128e-08};
    static const double L1 = 5.99999999999994648725e-01;
    static const double L2 = 4.28571428578550184252e-01;
    static const double L3 = 3.33333329818377432918e-01;
    static const double L4 = 2.72728123808534006489e-01;
    static const double L5 = 2.30660745775561754067e-01;
    static const double L6 = 2.06975017800338417784e-01;
    static const double pow_P1 = 1.66666666666666019037e-01;
    static const double pow_P2 = -2.77777777770155933842e-03;
    static const double pow_P3 = 6.61375632143793436117e-05;
    static const double pow_P4 = -1.65339022054652515390e-06;
    static const double pow_P5 = 4.13813679705723846039e-08;
    static const double lg2 = 6.93147180559945286227e-01;
    static const double lg2_h = 6.93147182464599609375e-01;
    static const double lg2_l = -1.90465429995776804525e-09;
    static const double ovt = 8.0085662595372944372e-17;
    static const double cp = 9.61796693925975554329e-01;
    static const double cp_h = 9.61796700954437255859e-01;
    static const double cp_l = -7.02846165095275826516e-09;

    dbl_cast dcx, dcy;
    dcx.f = x;
    dcy.f = y;
    hx = (int)(dcx.u >> 32);
    lx = (unsigned)dcx.u;
    hy = (int)(dcy.u >> 32);
    ly = (unsigned)dcy.u;
    ix = hx & 0x7fffffff;
    iy = hy & 0x7fffffff;

    if ((iy | ly) == 0)
        return one;
    if (hx == 0x3ff00000 && lx == 0)
        return one;
    if (ix > 0x7ff00000 || ((ix == 0x7ff00000) && (lx != 0)) ||
        iy > 0x7ff00000 || ((iy == 0x7ff00000) && (ly != 0)))
        return x + y;

    yisint = 0;
    if (hx < 0) {
        if (iy >= 0x43400000)
            yisint = 2;
        else if (iy >= 0x3ff00000) {
            k = (iy >> 20) - 0x3ff;
            if (k > 20) {
                j = ly >> (52 - k);
                if ((j << (52 - k)) == ly)
                    yisint = 2 - (j & 1);
            } else if (ly == 0) {
                j = iy >> (20 - k);
                if ((j << (20 - k)) == iy)
                    yisint = 2 - (j & 1);
            }
        }
    }

    if (ly == 0) {
        if (iy == 0x7ff00000) {
            if (((ix - 0x3ff00000) | lx) == 0)
                return y - y;
            else if (ix >= 0x3ff00000)
                return (hy >= 0) ? y : zero;
            else
                return (hy < 0) ? -y : zero;
        }
        if (iy == 0x3ff00000) {
            if (hy < 0)
                return one / x;
            else
                return x;
        }
        if (hy == 0x40000000)
            return x * x;
        if (hy == 0x3fe00000) {
            if (hx >= 0)
                return sqrt(x);
        }
    }

    ax = fabs(x);
    if (lx == 0) {
        if (ix == 0x7ff00000 || ix == 0 || ix == 0x3ff00000) {
            z = ax;
            if (hy < 0)
                z = one / z;
            if (hx < 0) {
                if (((ix - 0x3ff00000) | yisint) == 0) {
                    z = (z - z) / (z - z);
                } else if (yisint == 1)
                    z = -z;
            }
            return z;
        }
    }

    if (((((unsigned)hx >> 31) - 1) | yisint) == 0)
        return (x - x) / (x - x);

    if (iy > 0x41e00000) {
        if (iy > 0x43f00000) {
            if (ix <= 0x3fefffff)
                return (hy < 0) ? huge * huge : tiny * tiny;
            if (ix >= 0x3ff00000)
                return (hy > 0) ? huge * huge : tiny * tiny;
        }
        if (ix < 0x3fefffff)
            return (hy < 0) ? huge * huge : tiny * tiny;
        if (ix > 0x3ff00000)
            return (hy > 0) ? huge * huge : tiny * tiny;
        t = ax - one;
        w = (t * t) * (0.5 - t * (0.3333333333333333333333 - t * 0.25));
        u = ln2_hi * t;
        v = t * ln2_lo - w * invln2;
        t1 = u + v;
        dcx.f = t1;
        dcx.u &= 0xFFFFFFFF00000000ULL;
        t1 = dcx.f;
        t2 = v - (t1 - u);
    } else {
        double s2, s_h, s_l, t_h, t_l;
        n = 0;
        if (ix < 0x00100000) {
            ax *= 1.80143985094819840000e+16;
            n -= 54;
            dcx.f = ax;
            ix = (int)(dcx.u >> 32);
        }
        n += ((ix) >> 20) - 0x3ff;
        j = ix & 0x000fffff;
        ix = j | 0x3ff00000;
        if (j <= 0x3988E)
            k = 0;
        else if (j < 0xBB67A)
            k = 1;
        else {
            k = 0;
            n += 1;
            ix -= 0x00100000;
        }
        dcx.u = ((unsigned long long)ix << 32) | lx;
        ax = dcx.f;

        u = (ax - bp[k]) / (ax + bp[k]);
        v = u * u;
        s = v * v;
        r = v * (L1 + s * (L2 + s * (L3 + s * (L4 + s * (L5 + s * L6)))));
        s = u + u * r;
        s_h = s;
        dcx.f = s_h;
        dcx.u &= 0xFFFFFFFF00000000ULL;
        s_h = dcx.f;
        t_h = 0.0;
        dcx.f = t_h;
        dcx.u = ((unsigned long long)((ix >> 1) | 0x20000000) << 32);
        t_h = dcx.f;
        t_l = ax - (t_h - bp[k]);
        s_l = v * ((u - s_h * t_h) - s_h * t_l) / (ax + bp[k]);
        s2 = s_h + s_l;
        dcx.f = s2;
        dcx.u &= 0xFFFFFFFF00000000ULL;
        s2 = dcx.f;
        t_h = 3.0 + s2 + t_h;
        dcx.f = t_h;
        dcx.u &= 0xFFFFFFFF00000000ULL;
        t_h = dcx.f;
        t_l = t_l + (s_l + (s2 - s_h));
        u = t_h * lg2_h;
        v = t_l * lg2 + t_h * lg2_l;
        p_h = u + v;
        dcx.f = p_h;
        dcx.u &= 0xFFFFFFFF00000000ULL;
        p_h = dcx.f;
        p_l = v - (p_h - u);
        z_h = cp_h * p_h;
        z_l = cp_l * p_h + p_l * cp + dp_l[k];
        t = (double)n;
        t1 = (((z_h + z_l) + dp_h[k]) + t);
        dcx.f = t1;
        dcx.u &= 0xFFFFFFFF00000000ULL;
        t1 = dcx.f;
        t2 = z_l - (((t1 - t) - dp_h[k]) - z_h);
    }

    s = one;
    if ((((unsigned)hy >> 31) - 1 | (yisint - 1)) == 0)
        s = -one;

    y1 = y;
    dcy.f = y1;
    dcy.u &= 0xFFFFFFFF00000000ULL;
    y1 = dcy.f;
    p_l = (y - y1) * t1 + y * t2;
    p_h = y1 * t1;
    z = p_l + p_h;
    dcx.f = z;
    j = (int)(dcx.u >> 32);
    i = (int)dcx.u;
    if (j >= 0x40900000) {
        if (((j - 0x40900000) | i) != 0)
            return s * huge * huge;
        else {
            if (p_l + ovt > z - p_h)
                return s * huge * huge;
        }
    } else if ((j & 0x7fffffff) >= 0x4090cc00) {
        if (((j - 0xc090cc00) | i) != 0)
            return s * tiny * tiny;
        else {
            if (p_l <= z - p_h)
                return s * tiny * tiny;
        }
    }

    i = j & 0x7fffffff;
    k = (i >> 20) - 0x3ff;
    n = 0;
    if (i > 0x3fe00000) {
        n = j + (0x00100000 >> (k + 1));
        k = ((n & 0x7fffffff) >> 20) - 0x3ff;
        t = 0.0;
        dcx.f = t;
        dcx.u = ((unsigned long long)(n & ~(0x000fffff >> k)) << 32);
        t = dcx.f;
        n = ((n & 0x000fffff) | 0x00100000) >> (20 - k);
        if (j < 0)
            n = -n;
        p_h -= t;
    }
    t = p_l + p_h;
    dcx.f = t;
    dcx.u &= 0xFFFFFFFF00000000ULL;
    t = dcx.f;
    u = t * lg2_h;
    v = (p_l - (t - p_h)) * lg2 + t * lg2_l;
    z = u + v;
    w = v - (z - u);
    t = z * z;
    t1 = z - t * (pow_P1 + t * (pow_P2 + t * (pow_P3 + t * (pow_P4 + t * pow_P5))));
    r = (z * t1) / (t1 - 2.0) - (w + z * w);
    z = one - (r - z);
    dcx.f = z;
    j = (int)(dcx.u >> 32);
    j += (n << 20);
    if ((j >> 20) <= 0)
        z = scalbn(z, n);
    else {
        dcx.u = ((unsigned long long)j << 32) | (dcx.u & 0xFFFFFFFF);
        z = dcx.f;
    }
    return s * z;
}

double fmod(double x, double y)
{
    int n, hx, hy, hz, ix, iy, sx, i;
    unsigned lx, ly, lz;

    dbl_cast dcx, dcy;
    dcx.f = x;
    dcy.f = y;
    hx = (int)(dcx.u >> 32);
    lx = (unsigned)dcx.u;
    hy = (int)(dcy.u >> 32);
    ly = (unsigned)dcy.u;
    sx = hx & 0x80000000;
    hx ^= sx;
    hy &= 0x7fffffff;

    if ((hy | ly) == 0 || (hx >= 0x7ff00000) ||
        ((hy | ((ly | -ly) >> 31)) > 0x7ff00000))
        return (x * y) / (x * y);
    if (hx <= hy) {
        if ((hx < hy) || (lx < ly))
            return x;
        if (lx == ly)
            return zero * x;
    }

    if (hx < 0x00100000) {
        if (hx == 0) {
            for (ix = -1043, i = lx; i > 0; i <<= 1)
                ix -= 1;
        } else {
            for (ix = -1022, i = (hx << 11); i > 0; i <<= 1)
                ix -= 1;
        }
    } else
        ix = (hx >> 20) - 1023;

    if (hy < 0x00100000) {
        if (hy == 0) {
            for (iy = -1043, i = ly; i > 0; i <<= 1)
                iy -= 1;
        } else {
            for (iy = -1022, i = (hy << 11); i > 0; i <<= 1)
                iy -= 1;
        }
    } else
        iy = (hy >> 20) - 1023;

    if (ix >= -1022)
        hx = 0x00100000 | (0x000fffff & hx);
    else {
        n = -1022 - ix;
        if (n <= 31) {
            hx = (hx << n) | (lx >> (32 - n));
            lx <<= n;
        } else {
            hx = lx << (n - 32);
            lx = 0;
        }
    }
    if (iy >= -1022)
        hy = 0x00100000 | (0x000fffff & hy);
    else {
        n = -1022 - iy;
        if (n <= 31) {
            hy = (hy << n) | (ly >> (32 - n));
            ly <<= n;
        } else {
            hy = ly << (n - 32);
            ly = 0;
        }
    }

    n = ix - iy;
    while (n--) {
        hz = hx - hy;
        lz = lx - ly;
        if (lx < ly)
            hz -= 1;
        if (hz < 0) {
            hx = hx + hx + (lx >> 31);
            lx = lx + lx;
        } else {
            if ((hz | lz) == 0)
                return zero * x;
            hx = hz + hz + (lz >> 31);
            lx = lz + lz;
        }
    }
    hz = hx - hy;
    lz = lx - ly;
    if (lx < ly)
        hz -= 1;
    if (hz >= 0) {
        hx = hz;
        lx = lz;
    }

    if ((hx | lx) == 0)
        return zero * x;
    while (hx < 0x00100000) {
        hx = hx + hx + (lx >> 31);
        lx = lx + lx;
        iy -= 1;
    }
    if (iy >= -1022) {
        hx = ((hx - 0x00100000) | ((iy + 1023) << 20));
        dcx.u = ((unsigned long long)hx << 32) | lx;
    } else {
        n = -1022 - iy;
        if (n <= 20) {
            lx = (lx >> n) | ((unsigned)hx << (32 - n));
            hx >>= n;
        } else if (n <= 31) {
            lx = (hx << (32 - n)) | (lx >> n);
            hx = sx;
        } else {
            lx = hx >> (n - 32);
            hx = sx;
        }
        dcx.u = ((unsigned long long)hx << 32) | lx;
        x = dcx.f;
        x *= one;
        dcx.f = x;
    }
    dcx.u |= ((unsigned long long)sx << 32);
    return dcx.f;
}

/* ========================================================================
 * Rounding and Remainder Functions
 * ======================================================================== */

double ceil(double x)
{
    dbl_cast dc;
    dc.f = x;
    int i0 = dc.u >> 32;
    unsigned i1 = dc.u;
    int j0 = ((i0 >> 20) & 0x7ff) - 0x3ff;

    if (j0 < 20) {
        if (j0 < 0) {
            if (huge + x > 0.0) {
                if (i0 < 0) {
                    i0 = 0x80000000;
                    i1 = 0;
                } else if ((i0 | i1) != 0) {
                    i0 = 0x3ff00000;
                    i1 = 0;
                }
            }
        } else {
            unsigned i = (0x000fffff) >> j0;
            if (((i0 & i) | i1) == 0)
                return x;
            if (huge + x > 0.0) {
                if (i0 > 0)
                    i0 += (0x00100000) >> j0;
                i0 &= (~i);
                i1 = 0;
            }
        }
    } else if (j0 > 51) {
        if (j0 == 0x400)
            return x + x;
        else
            return x;
    } else {
        unsigned i = ((unsigned)(0xffffffff)) >> (j0 - 20);
        if ((i1 & i) == 0)
            return x;
        if (huge + x > 0.0) {
            if (i0 > 0) {
                if (j0 == 20)
                    i0 += 1;
                else {
                    unsigned j = i1 + (1 << (52 - j0));
                    if (j < i1)
                        i0 += 1;
                    i1 = j;
                }
            }
            i1 &= (~i);
        }
    }
    dc.u = ((unsigned long long)i0 << 32) | i1;
    return dc.f;
}

double trunc(double x)
{
    dbl_cast dc;
    dc.f = x;
    int i0 = dc.u >> 32;
    unsigned i1 = dc.u;
    int j0 = ((i0 >> 20) & 0x7ff) - 0x3ff;
    int sx = (unsigned)i0 >> 31;

    if (j0 < 20) {
        if (j0 < 0)
            return sx ? -0.0 : 0.0;
        i0 &= ~((0x000fffff) >> j0);
        i1 = 0;
    } else if (j0 > 51) {
        if (j0 == 0x400)
            return x + x;
        return x;
    } else {
        i1 &= ~((unsigned)(0xffffffff) >> (j0 - 20));
    }
    dc.u = ((unsigned long long)i0 << 32) | i1;
    return dc.f;
}

double round(double x)
{
    dbl_cast dc;
    dc.f = x;
    int i0 = dc.u >> 32;
    unsigned i1 = dc.u;
    int j0 = ((i0 >> 20) & 0x7ff) - 0x3ff;
    int sx = (unsigned)i0 >> 31;

    if (j0 < 20) {
        if (j0 < 0) {
            if (huge + x > 0.0) {
                i0 &= 0x80000000;
                if (j0 == -1)
                    i0 |= 0x3ff00000;
                i1 = 0;
            }
        } else {
            unsigned i = (0x000fffff) >> j0;
            if (((i0 & i) | i1) == 0)
                return x;
            if (huge + x > 0.0) {
                i0 += (0x00080000) >> j0;
                i0 &= (~i);
                i1 = 0;
            }
        }
    } else if (j0 > 51) {
        if (j0 == 0x400)
            return x + x;
        else
            return x;
    } else {
        unsigned i = ((unsigned)(0xffffffff)) >> (j0 - 20);
        if ((i1 & i) == 0)
            return x;
        if (huge + x > 0.0) {
            unsigned j = i1 + (0x80000000 >> (j0 - 20));
            if (j < i1)
                i0 += 1;
            i1 = j & (~i);
        }
    }
    dc.u = ((unsigned long long)i0 << 32) | i1;
    return dc.f;
}