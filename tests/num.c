/* num.c: integer and floating point arithmetic; the same text must come out on every CPU */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static unsigned long seed = 12345;

static unsigned long rnd(void)
{
    seed = seed * 1103515245UL + 12345UL;
    return seed;
}

/* the compiler must not do the work at compile time */
static volatile int vi;
static volatile double vd;

static void integers(void)
{
    unsigned long sum = 0, a, b;
    long sa, sb;
    __int64 q = 0;
    unsigned __int64 uq = 0;
    unsigned short us;
    signed char sc;
    int i;

    for (i = 0; i < 20000; i++) {
        a = rnd();
        b = rnd() | 1;
        sa = (long)a;
        sb = (long)b;
        us = (unsigned short)a;
        sc = (signed char)b;
        sum += a + b;
        sum ^= a - b;
        sum += a * b;
        sum ^= a / b;
        sum += a % b;
        sum ^= (unsigned long)(sa / sb);
        sum += (unsigned long)(sa % sb);
        sum ^= a << (b & 31);
        sum += a >> (b & 31);
        sum ^= (unsigned long)(sa >> (b & 31));
        sum += (a < b) + 2 * (sa < sb) + 4 * (a == b) + 8 * (sa >= sb);
        sum ^= (unsigned long)us * 3 + (unsigned long)(sc * 5);
        sum += (unsigned long)(us >> (b & 15)) ^ (unsigned long)(sc >> (b & 7));
        sum ^= ~a & (b | 0xff00ff00UL);
        sum += (a & b) | (a ^ b);
        q += (__int64)sa * sb;
        q ^= (__int64)sa << (b & 63);
        q += q >> (a & 31);
        uq += (unsigned __int64)a * b;
        uq ^= uq / (b + 7);
        uq += uq % (a | 1);
        uq ^= uq >> (b & 63);
    }
    printf("1 integers %08lx\n", sum);
    printf("2 64-bit %08lx%08lx %08lx%08lx\n", (unsigned long)(q >> 32), (unsigned long)q,
        (unsigned long)(uq >> 32), (unsigned long)uq);
    vi = 7;
    printf("3 divide %d %d %d %d\n", -17 / vi, -17 % vi, 17 / -vi, 17 % -vi);
    printf("4 shifts %08lx %08lx %d\n", 0x80000001UL >> vi, 0x80000001UL << vi, -256 >> vi);
    printf("5 atoi %d %ld %lu\n", atoi("-12345"), strtol("0x7fffffff", NULL, 0), strtoul("4294967295", NULL, 10));
}

static void floats(void)
{
    double x = 0, y, z;
    float f;
    long n;
    int i;

    for (i = 1; i <= 2000; i++) {
        y = (double)(rnd() >> 8) / 65536.0 + 1.0;
        z = (double)(rnd() >> 12) / 4096.0 + 0.5;
        x += y * z - y / z;
        x = x * 0.999 + sqrt(y) - (double)(float)z;
        f = (float)(y / z);
        x += f;
        n = (long)(y * 3.7);
        x -= (double)n * 0.25;
    }
    printf("6 double sum %.17g\n", x);
    vd = 3.0;
    printf("7 exact %.17g %.17g %.17g %.17g\n", 1.0 / vd, sqrt(2.0 * vd), 1e300 * vd / vd, 1e-300 * vd);
    printf("8 convert %d %d %d %u %.1f %.1f\n", (int)(vd * 1.9), (int)(-vd * 1.9), (int)floor(-vd / 2), (unsigned)(vd * 1e9),
        (double)(long)-7, (double)(unsigned long)4000000000UL);
    printf("9 printf %g %e %f %.3f %10.4f|%-10.2e|%+.0f\n", 0.1, 123456.789, 1.0 / 3, 2.0005, 3.14159265, 0.000123456, 2.5);
    printf("10 printf %g %g %g %g %.15g\n", 1e21, 1e-7, 123456789.0, 0.0001, 0.1 + 0.2);
    printf("11 strtod %.17g %.17g %.17g\n", atof("3.14159265358979323846"), strtod("1e-320", NULL), atof("-0.000001234567890123456789"));
    printf("12 math %.12g %.12g %.12g %.12g\n", sin(vd), cos(vd), tan(vd / 4), atan2(vd, 4.0));
    printf("13 math %.12g %.12g %.12g %.12g\n", exp(vd), log(vd), log10(vd * 100), pow(vd, 2.5));
    printf("14 math %.12g %.12g %.12g %.12g\n", fmod(vd * 3.7, 2.0), ceil(-vd / 2), fabs(-vd), ldexp(vd, 10));
    f = (float)vd / 7.0f;
    printf("15 float %.9g %.9g\n", (double)f, (double)(f * f));
    printf("16 compare %d %d %d %d\n", vd > 2.9, vd < 2.9, vd == 3.0, vd != vd);
}

int main(void)
{
    integers();
    floats();
    printf("done\n");
    return 0;
}
