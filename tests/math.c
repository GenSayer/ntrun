/* math.c: the bits of the results of the C library's math functions, for many numbers.
   Each line is one function: the number of results and a sum of all their bits.
   x86: no lines for sin, cos and tan (see the README: the x86 reference machine is not exact there). */
#include <stdio.h>
#include <string.h>
#include <math.h>

static unsigned long seed = 12345;
static unsigned long sum, count;

static double rnd(void) /* 0 <= result < 1 */
{
    unsigned long a, b;
    seed = seed * 1103515245UL + 12345UL;
    a = (seed >> 8) & 0xffffff;
    seed = seed * 1103515245UL + 12345UL;
    b = (seed >> 8) & 0xffffff;
    return ((double)a * 16777216.0 + (double)b) / 281474976710656.0;
}

static void add(double r)
{
    unsigned char b[8];
    int i;
    memcpy(b, &r, 8);
    for (i = 0; i < 8; i++)
        sum = (sum ^ b[i]) * 16777619UL;
    count++;
}

static void start(void)
{
    sum = 2166136261UL;
    count = 0;
}

static void result(const char *name)
{
    printf("%-6s %4lu results %08lx\n", name, count, sum);
}

/* numbers with an exact result or near a limit */
static const double special[] = {
    0.0, 1.0, 2.0, 0.5, 0.25, 4.0, 8.0, 1024.0, 0.125, 3.0, 10.0, 100.0, 1000.0, 1e10, 1e-10, 1e100, 1e-100,
    0.1, 0.2, 0.3, 0.7, 1.5, 2.5, 7.0, 0.75, 1e-5, 1e-20, 0.99999999999999989, 1.0000000000000002, 65536.0, 1e15,
};
#define NSPECIAL (sizeof special / sizeof special[0])
#define N 200

int main(void)
{
    static double x[N], y[N], u[N], w[N]; /* -10 to 10; 0.000001 to 100; -1 to 1; small */
    int i, e;
    double ip;

    for (i = 0; i < N; i++) {
        x[i] = (rnd() - 0.5) * 20.0;
        y[i] = rnd() * 100.0 + 1e-6;
        u[i] = rnd() * 2.0 - 1.0;
        w[i] = (rnd() - 0.5) * 1e-5;
    }
#ifndef _M_IX86
    start();
    for (i = 0; i < N; i++) { add(sin(x[i])); add(sin(w[i])); add(sin(x[i] * 5e4)); }
    for (i = 0; i < NSPECIAL; i++) { add(sin(special[i])); add(sin(-special[i])); }
    result("sin");
    start();
    for (i = 0; i < N; i++) { add(cos(x[i])); add(cos(w[i])); add(cos(x[i] * 5e4)); }
    for (i = 0; i < NSPECIAL; i++) add(cos(special[i]));
    result("cos");
    start();
    for (i = 0; i < N; i++) { add(tan(x[i])); add(tan(w[i])); }
    for (i = 0; i < NSPECIAL; i++) add(tan(special[i]));
    result("tan");
#endif
    start();
    for (i = 0; i < N; i++) { add(atan(x[i])); add(atan(w[i])); add(atan(y[i] * 1e6)); }
    for (i = 0; i < NSPECIAL; i++) { add(atan(special[i])); add(atan(-special[i])); }
    result("atan");
    start();
    for (i = 0; i < N; i++) { add(atan2(x[i], y[i])); add(atan2(y[i], x[i])); add(atan2(x[i], -y[i])); add(atan2(w[i], x[i])); }
    for (i = 0; i + 1 < NSPECIAL; i++) { add(atan2(special[i], special[i + 1])); add(atan2(special[i + 1], -special[i])); }
    result("atan2");
    start();
    for (i = 0; i < N; i++) { add(asin(u[i])); add(asin(w[i])); }
    add(asin(1.0)); add(asin(-1.0)); add(asin(0.5)); add(asin(0.0));
    result("asin");
    start();
    for (i = 0; i < N; i++) { add(acos(u[i])); add(acos(w[i])); }
    add(acos(1.0)); add(acos(-1.0)); add(acos(0.5)); add(acos(0.0));
    result("acos");
    start();
    for (i = 0; i < N; i++) { add(exp(x[i])); add(exp(w[i])); add(exp(x[i] * 50.0)); }
    for (i = 0; i < NSPECIAL; i++) { if (special[i] < 700) { add(exp(special[i])); add(exp(-special[i])); } }
    result("exp");
    start();
    for (i = 0; i < N; i++) { add(log(y[i])); add(log(1.0 + w[i])); add(log(y[i] * 1e200)); add(log(y[i] * 1e-200)); }
    for (i = 1; i < NSPECIAL; i++) add(log(special[i]));
    result("log");
    start();
    for (i = 0; i < N; i++) { add(log10(y[i])); add(log10(1.0 + w[i])); }
    for (i = 1; i < NSPECIAL; i++) add(log10(special[i]));
    result("log10");
    start();
    for (i = 0; i < N; i++) { add(pow(y[i], x[i] / 4.0)); add(pow(y[i], w[i])); add(pow(1.0 + w[i], x[i] * 1e4)); }
    for (i = 1; i < NSPECIAL; i++) {
        if (special[i] < 1e11 && special[i] > 1e-11) {
            add(pow(special[i], 2.0)); add(pow(special[i], 0.5)); add(pow(special[i], -3.0)); add(pow(2.0, special[i] < 1000 ? special[i] : 10.0));
            add(pow(special[i], 1.0 / 3.0)); add(pow(10.0, special[i] < 300 ? special[i] : 3.0)); add(pow(-special[i], 3.0));
        }
    }
    result("pow");
    start();
    for (i = 0; i < N; i++) { add(sqrt(y[i])); add(sqrt(y[i] * 1e300)); add(sqrt(y[i] * 1e-300)); add(sqrt(fabs(w[i]))); }
    for (i = 0; i < NSPECIAL; i++) add(sqrt(special[i]));
    result("sqrt");
    start();
    for (i = 0; i < N; i++) { add(sinh(x[i])); add(sinh(w[i])); add(sinh(u[i])); }
    for (i = 0; i < NSPECIAL; i++) { if (special[i] < 700) add(sinh(special[i])); }
    result("sinh");
    start();
    for (i = 0; i < N; i++) { add(cosh(x[i])); add(cosh(w[i])); add(cosh(u[i])); }
    for (i = 0; i < NSPECIAL; i++) { if (special[i] < 700) add(cosh(special[i])); }
    result("cosh");
    start();
    for (i = 0; i < N; i++) { add(tanh(x[i])); add(tanh(w[i])); add(tanh(u[i])); }
    for (i = 0; i < NSPECIAL; i++) add(tanh(special[i]));
    result("tanh");
    start();
    for (i = 0; i < N; i++) { add(fmod(y[i], fabs(x[i]) + 0.1)); add(fmod(x[i] * 1e15, y[i])); add(fmod(x[i], y[i] * 1e-200)); add(fmod(w[i], y[i])); }
    for (i = 1; i < NSPECIAL; i++) { add(fmod(1e15, special[i])); add(fmod(-special[i], 0.3)); }
    result("fmod");
    start();
    for (i = 0; i < N; i++) { add(floor(x[i])); add(floor(w[i])); add(floor(x[i] * 1e15)); add(ceil(x[i])); add(ceil(w[i])); add(ceil(x[i] * 1e15)); }
    for (i = 0; i < NSPECIAL; i++) { add(floor(special[i])); add(floor(-special[i])); add(ceil(special[i])); add(ceil(-special[i])); }
    result("floor");
    start();
    for (i = 0; i < N; i++) {
        add(ldexp(x[i], i - 100)); add(ldexp(y[i], i * 12 - 1200));
        add(frexp(y[i] * 1e-200, &e)); add((double)e);
        add(modf(x[i] * 1000.0, &ip)); add(ip);
        add(fabs(x[i]));
    }
    result("parts");
    return 0;
}
