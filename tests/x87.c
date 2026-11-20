/* x87.c: the bits that the x87 gives for many operands, in each precision and rounding mode.
   For x86 only (the instructions are in __asm lines). Each line is one instruction: the
   number of cases and a sum of all the result bits and status bits.
   Build: cl -Od x87.c */
#include <stdio.h>
#include <string.h>
#include <excpt.h>

static unsigned char A[10], B[10], R1[10], R2[10];
static unsigned short CW, SW;

static unsigned long seed = 20261120;
static unsigned long sum, cases;

static unsigned long rnd15(void)
{
    seed = seed * 1103515245UL + 12345UL;
    return (seed >> 16) & 0x7fff;
}

static unsigned long rnd32(void)
{
    unsigned long a = rnd15(), b = rnd15(), c = rnd15();
    return a << 17 | b << 2 | (c & 3);
}

static void put(unsigned char *t, unsigned long hi, unsigned long lo, unsigned se)
{
    t[0] = (unsigned char)lo; t[1] = (unsigned char)(lo >> 8); t[2] = (unsigned char)(lo >> 16); t[3] = (unsigned char)(lo >> 24);
    t[4] = (unsigned char)hi; t[5] = (unsigned char)(hi >> 8); t[6] = (unsigned char)(hi >> 16); t[7] = (unsigned char)(hi >> 24);
    t[8] = (unsigned char)se; t[9] = (unsigned char)(se >> 8);
}

/* a number of one kind */
static void gen(unsigned char *t, int kind)
{
    unsigned long hi = rnd32() | 0x80000000UL, lo = rnd32();
    unsigned se = 0x3fff, sign = (rnd15() & 1) << 15;

    switch (kind) {
    case 0: se = 0x3fff - 40 + rnd15() % 80; break;                 /* a number near 1 */
    case 1: se = 1 + rnd15() % 0x7ffe; break;                       /* a number of any size */
    case 2: lo &= 0xfffff800UL; se = 0x3fff - 300 + rnd15() % 600; break; /* as a double has it */
    case 3: hi = (rnd15() % 1000 + 1); lo = 0; se = 0x3fff + 31;    /* a small whole number */
        while (!(hi & 0x80000000UL)) { hi <<= 1; se--; }
        break;
    case 4: hi &= 0x7fffffffUL; if (rnd15() & 1) hi >>= rnd15() % 31; se = 0; break; /* below the smallest normal number */
    case 5: hi = lo = 0; se = 0; break;                             /* 0 */
    case 6: hi = 0x80000000UL; lo = 0; se = 0x7fff; break;          /* infinity */
    case 7: hi |= 0x40000000UL; se = 0x7fff; break;                 /* a quiet NaN */
    case 8: hi &= 0xbfffffffUL; lo |= 1; se = 0x7fff; break;        /* a signaling NaN */
    case 9: se = 0x7ffe - rnd15() % 70; break;                      /* very big */
    case 10: se = 1 + rnd15() % 70; break;                          /* very small */
    case 11: se = 0x3fff - 8 + rnd15() % 16; lo = 0; hi &= 0xff000000UL; break; /* few bits */
    case 12: se = 0x3fff - 2 - rnd15() % 70; break;                 /* less than 0.25 */
    case 13: se = 0x3fff - 1 - rnd15() % 30; break;                 /* less than 1 */
    case 14: se = 0x3fff + rnd15() % 62; break;                     /* 1 to 2^62 */
    case 15: se = 0x3fff + rnd15() % 16; lo = 0; hi &= 0xffff0000UL; break; /* a whole part of up to 16 bits */
    case 16: hi = 0x80000000UL; lo = 0; se = 0x3fff - 20 + rnd15() % 40; break; /* a power of 2 */
    case 17: hi = 0x80000000UL; lo = 0; se = 0x3fff - 1 + rnd15() % 3; break;   /* 0.5, 1 or 2 */
    }
    put(t, hi, lo, se | sign);
}

/* the usual mix */
static int mix(void)
{
    static const unsigned char k[32] = { 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 2, 2, 2, 3, 3, 3, 4, 4, 5, 6, 7, 8, 9, 9, 10, 10, 11, 11, 11, 16, 17 };
    return k[rnd15() & 31];
}

static void add(const unsigned char *t, int n)
{
    while (n-- > 0)
        sum = (sum ^ *t++) * 16777619UL;
}

/* one case is done: its results and the bits MASK of the status word go into the sum */
static void done(int two, unsigned mask)
{
    unsigned char s[2];
    s[0] = (unsigned char)(SW & mask);
    s[1] = (unsigned char)((SW & mask) >> 8);
    add(R1, 10);
    if (two)
        add(R2, 10);
    add(s, 2);
    cases++;
}

static void start(void)
{
    sum = 2166136261UL;
    cases = 0;
}

static void result(const char *name)
{
    printf("%-8s %4lu cases %08lx\n", name, cases, sum);
}

static const char *binname[] = { "fadd", "fsub", "fmul", "fdiv", "fprem", "fprem1", "fscale", "fpatan", "fyl2x", "fyl2xp1", "fcom", "fucom" };

static void bin(int op)
{
    memset(R1, 0, 10);
    __asm fninit
    __asm fldcw CW
    __asm fld tbyte ptr B
    __asm fld tbyte ptr A
    switch (op) {
    case 0: __asm faddp st(1), st
        break;
    case 1: __asm fsubp st(1), st
        break;
    case 2: __asm fmulp st(1), st
        break;
    case 3: __asm fdivp st(1), st
        break;
    case 4: __asm fprem
        break;
    case 5: __asm fprem1
        break;
    case 6: __asm fscale
        break;
    case 7: __asm fpatan
        break;
    case 8: __asm fyl2x
        break;
    case 9: __asm fyl2xp1
        break;
    case 10: __asm fcom st(1)
        break;
    case 11: __asm fucom st(1)
        break;
    }
    __asm fnstsw SW
    __asm fstp tbyte ptr R1
    __asm fninit
}

static const char *unname[] = { "fsqrt", "frndint", "f2xm1", "fxtract", "fabs", "fchs", "fxam", "ftst" };

static int un(int op)
{
    int two = 0;
    memset(R1, 0, 10);
    memset(R2, 0, 10);
    __asm fninit
    __asm fldcw CW
    __asm fld tbyte ptr A
    switch (op) {
    case 0: __asm fsqrt
        break;
    case 1: __asm frndint
        break;
    case 2: __asm f2xm1
        break;
    case 3: __asm fxtract
        two = 1;
        break;
    case 4: __asm fabs
        break;
    case 5: __asm fchs
        break;
    case 6: __asm fxam
        break;
    case 7: __asm ftst
        break;
    }
    __asm fnstsw SW
    __asm fstp tbyte ptr R1
    if (two) {
        __asm fstp tbyte ptr R2
    }
    __asm fninit
    return two;
}

static const char *cvname[] = { "fst64", "fst32", "fist16", "fist32", "fist64", "fbstp", "fld64", "fld32", "fild64", "fbld", "fiadd32", "fmul64" };

static void cv(int op)
{
    memset(R1, 0, 10);
    __asm fninit
    __asm fldcw CW
    switch (op) {
    case 0: __asm fld tbyte ptr A
        __asm fstp qword ptr R1
        break;
    case 1: __asm fld tbyte ptr A
        __asm fstp dword ptr R1
        break;
    case 2: __asm fld tbyte ptr A
        __asm fistp word ptr R1
        break;
    case 3: __asm fld tbyte ptr A
        __asm fistp dword ptr R1
        break;
    case 4: __asm fld tbyte ptr A
        __asm fistp qword ptr R1
        break;
    case 5: __asm fld tbyte ptr A
        __asm fbstp tbyte ptr R1
        break;
    case 6: __asm fld qword ptr A
        __asm fstp tbyte ptr R1
        break;
    case 7: __asm fld dword ptr A
        __asm fstp tbyte ptr R1
        break;
    case 8: __asm fild qword ptr A
        __asm fstp tbyte ptr R1
        break;
    case 9: __asm fbld tbyte ptr A
        __asm fstp tbyte ptr R1
        break;
    case 10: __asm fld tbyte ptr A
        __asm fiadd dword ptr B
        __asm fstp tbyte ptr R1
        break;
    case 11: __asm fld tbyte ptr A
        __asm fmul qword ptr B
        __asm fstp tbyte ptr R1
        break;
    }
    __asm fnstsw SW
    __asm fninit
}

static const char *cnname[] = { "fld1", "fldl2t", "fldl2e", "fldpi", "fldlg2", "fldln2", "fldz" };

static void cn(int op)
{
    __asm fninit
    __asm fldcw CW
    switch (op) {
    case 0: __asm fld1
        break;
    case 1: __asm fldl2t
        break;
    case 2: __asm fldl2e
        break;
    case 3: __asm fldpi
        break;
    case 4: __asm fldlg2
        break;
    case 5: __asm fldln2
        break;
    case 6: __asm fldz
        break;
    }
    __asm fnstsw SW
    __asm fstp tbyte ptr R1
    __asm fninit
}

static const char *exname[] = { "divide by zero", "zero by zero", "too big in a store", "too small in a store", "not exact",
                                "too big in a register", "all masked", "square root of -1" };

/* the exception code that Windows gives when the program did not mask an x87 exception */
static unsigned long ex(int which)
{
    static const unsigned short cw[8] = { 0x037b, 0x037e, 0x0377, 0x036f, 0x035f, 0x0377, 0x037f, 0x037e };
    static double big = 1e300, small = 1e-300, three = 3.0, minus = -1.0;
    static float f;
    unsigned long code = 0;
    unsigned short c = cw[which];
    int i;

    __try {
        __asm fninit
        __asm fldcw c
        switch (which) {
        case 0: __asm fld1
            __asm fldz
            __asm fdivp st(1), st
            break;
        case 1:
        case 6: __asm fldz
            __asm fldz
            __asm fdivp st(1), st
            break;
        case 2: __asm fld big
            __asm fstp f
            break;
        case 3: __asm fld small
            __asm fstp f
            break;
        case 4: __asm fld1
            __asm fdiv three
            break;
        case 5: __asm fld big
            for (i = 0; i < 17; i++) {
                __asm fmul big
            }
            break;
        case 7: __asm fld minus
            __asm fsqrt
            break;
        }
        __asm fwait
    } __except (code = _exception_code(), 1) {
    }
    __asm fninit
    return code;
}

/* the control words: precision 24, 53 and 64 bits (first is 0), or 64 bits only (first is 2); 4 rounding modes */
static unsigned short control(int n, int first)
{
    static const unsigned short pc[3] = { 0x0000, 0x0200, 0x0300 };
    return (unsigned short)(0x007f | pc[first + n / 4] | (n % 4) << 10);
}

int main(void)
{
    int op, c, i, ka, kb, two, first, modes;

    /* the constants */
    for (op = 0; op < 7; op++) {
        start();
        for (c = 0; c < 4; c++) {
            CW = control(c, 2);
            cn(op);
            done(0, 0);
        }
        result(cnname[op]);
    }
    /* two operands. The precision is for + - * / only: the others are done with 64 bits. */
    for (op = 0; op < 12; op++) {
        first = op < 4 || op >= 10 ? 0 : 2;
        modes = first ? 4 : 12;
        start();
        for (c = 0; c < modes; c++) {
            CW = control(c, first);
            for (i = 0; i < 40; i++) {
                ka = mix();
                kb = mix();
                if (op == 4 || op == 5) { /* remainder: sometimes numbers of very different sizes */
                    if (i % 4 == 1) { ka = 9; kb = 0; }
                    if (i % 4 == 2) { ka = 0; kb = 0; }
                }
                if (op == 6) /* scale: by a small whole number */
                    kb = i % 8 == 7 ? mix() : i % 8 == 6 ? 9 : 15;
                if (op == 8 && i % 5) /* logarithm: of 1 or more, of less than 1, of a power of 2 */
                    ka = i % 8 == 3 ? 16 : i % 2 ? 14 : 13;
                if (op == 9) /* logarithm of 1 + x: a small x */
                    ka = i % 8 == 7 ? mix() : 12;
                if (op == 8) { /* no operands below the smallest normal number there */
                    if (ka == 4) ka = 10;
                    if (kb == 4) kb = 10;
                }
                gen(A, ka);
                gen(B, kb);
                if ((op == 8 && i % 5) || (op >= 10 && i % 6 == 5))
                    A[9] &= 0x7f;
                if (op >= 10 && i % 6 == 4)
                    memcpy(B, A, 10);
                if ((op == 4 || op == 5 || op == 7) && i % 8 == 5) { /* the same size: remainder 0, angle of 45 degrees */
                    memcpy(B, A, 10);
                    if (i & 8)
                        B[9] ^= 0x80;
                }
                bin(op);
                /* the remainder to the nearest: the result only; compares: the condition bits */
                done(0, op == 5 ? 0 : op == 4 ? 0x453d : op >= 10 ? 0x4501 : 0x003d);
            }
        }
        result(binname[op]);
    }
    /* one operand */
    for (op = 0; op < 8; op++) {
        first = op == 0 ? 0 : 2;
        modes = first ? 4 : 12;
        start();
        for (c = 0; c < modes; c++) {
            CW = control(c, first);
            for (i = 0; i < 40; i++) {
                ka = mix();
                if (op == 0 && i % 4)
                    ka = i % 8 == 7 ? 16 : i % 4 == 1 ? 0 : i % 4 == 2 ? 2 : 1;
                if (op == 1)
                    ka = i % 4 == 3 ? mix() : i % 4 == 2 ? 13 : 15;
                if (op == 2)
                    ka = i % 8 == 7 ? mix() : i % 8 == 3 ? 17 : i % 2 ? 12 : 13;
                gen(A, ka);
                if (op == 0 && i % 4)
                    A[9] &= 0x7f;
                two = un(op);
                done(two, op == 6 ? 0x4700 : op == 7 ? 0x4501 : 0x003d);
            }
        }
        result(unname[op]);
    }
    /* to and from memory. Stores have the rounding mode; the others are done with 64 bits. */
    for (op = 0; op < 12; op++) {
        first = op < 6 ? 0 : 2;
        modes = first ? 4 : 12;
        start();
        for (c = 0; c < modes; c++) {
            CW = control(c, first);
            for (i = 0; i < 40; i++) {
                ka = mix();
                if (op >= 2 && op <= 5) /* to an integer */
                    ka = i % 4 == 3 ? mix() : i % 4 == 2 ? 14 : i % 4 == 1 ? 13 : 15;
                gen(A, ka);
                gen(B, mix());
                if (op == 6 || op == 7 || op == 8 || op == 11) { /* any bits as a memory operand */
                    unsigned long h = rnd32(), l = rnd32();
                    unsigned char *t = op == 11 ? B : A;
                    if (i % 5 == 4)
                        h &= 0x800fffffUL; /* a 64-bit number below the smallest normal one */
                    if (i % 5 == 3)
                        h |= 0x7ff00000UL;
                    if (op == 7 && i % 5 == 2)
                        l &= 0x807fffffUL;
                    if (op == 7 && i % 5 == 1)
                        l |= 0x7f800000UL;
                    put(t, h, l, 0);
                }
                if (op == 9) { /* 18 decimal digits */
                    int j;
                    for (j = 0; j < 9; j++)
                        A[j] = (unsigned char)((rnd15() % 10) << 4 | rnd15() % 10);
                    A[9] = (unsigned char)(rnd15() & 0x80);
                }
                cv(op);
                done(0, 0x003d);
            }
        }
        result(cvname[op]);
    }
    /* exceptions */
    for (op = 0; op < 8; op++)
        printf("%-22s exception %08lx\n", exname[op], ex(op));
    return 0;
}
