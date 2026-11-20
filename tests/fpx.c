/* fpx.c: floating point operations with no ordinary result. The CPUs are not the same here:
   x86, MIPS and PowerPC give infinity or a NaN, Alpha gives the program an exception. */
#include <stdio.h>
#include <float.h>
#include <string.h>
#include <windows.h>

static volatile double big = 1e308, tiny = 1e-308, zero = 0.0, three = 3.0, minus = -1.0;
static unsigned long code;

static int filter(unsigned long c)
{
    code = c;
    return EXCEPTION_EXECUTE_HANDLER;
}

static void show(const char *what, double v)
{
    unsigned long w[2];

    memcpy(w, &v, 8); /* the bits of the number too */
    printf("%08lx%08lx ", w[1], w[0]);
    if (_isnan(v))
        printf("%s: not a number\n", what);
    else if (!_finite(v))
        printf("%s: %s infinity\n", what, v > 0 ? "plus" : "minus");
    else
        printf("%s: %.17g\n", what, v);
}

int main(void)
{
    double r;
    long n;

    __try {
        r = big * three;
        show("1 too big", r);
    } __except (filter(GetExceptionCode())) {
        printf("1 too big: exception %08lx\n", code);
    }
    __try {
        r = tiny / big;
        show("2 too small", r);
    } __except (filter(GetExceptionCode())) {
        printf("2 too small: exception %08lx\n", code);
    }
    __try {
        r = three / zero;
        show("3 divide by zero", r);
    } __except (filter(GetExceptionCode())) {
        printf("3 divide by zero: exception %08lx\n", code);
    }
    __try {
        r = zero / zero;
        show("4 zero by zero", r);
    } __except (filter(GetExceptionCode())) {
        printf("4 zero by zero: exception %08lx\n", code);
    }
    __try {
        n = (long)(big * minus);
        printf("5 to integer: %ld\n", n);
    } __except (filter(GetExceptionCode())) {
        printf("5 to integer: exception %08lx\n", code);
    }
    __try {
        r = big + big;
        r = r - r;
        show("6 infinity less infinity", r);
    } __except (filter(GetExceptionCode())) {
        printf("6 infinity less infinity: exception %08lx\n", code);
    }
    printf("done\n");
    return 0;
}
