/*
 * cpu_x86.c: 32-bit x86 user mode, as a Win32 program sees it: the 80486 instruction set
 * with the x87 floating point unit, and CMOV/FCMOV/FCOMI of the Pentium Pro. Flat memory;
 * the FS segment is the TEB. No MMX, no SSE.
 *
 * The x87 is complete: 80-bit registers, the three precisions, the four rounding modes, the
 * exception flags, and exceptions that the program did not mask.
 */
#include <stdio.h>
#include <stdlib.h>
#include "ntrun.h"

enum { EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI };

#define CF 0x001u
#define PF 0x004u
#define AF 0x010u
#define ZF 0x040u
#define SF 0x080u
#define DF 0x400u
#define OF 0x800u
#define FLAGS_USER 0x00040dd5u /* the flags a program can change: CF PF AF ZF SF TF DF OF AC (no ID: no CPUID, as an 80486) */

/* an x87 register, as the 80 bits are in memory: 64 bits of the number, then the sign and 15 bits exponent */
typedef struct {
    u64 m;
    u16 se;
} xr;

static struct {
    u32 r[8];
    u32 eip;    /* the instruction that runs now; in a Windows function: where it returns to */
    u32 fl;     /* EFLAGS */
    u32 fsbase; /* the TEB */
    /* x87 */
    xr st[8];       /* by physical register number */
    u8 empty[8];
    int top;
    u16 cw, sw;     /* sw without the 3 bits of top */
    u32 fip, fdp;   /* the last x87 instruction and its memory operand */
    u16 fop;
} X;

static u8 parity[256]; /* PF for a low byte */

static void x86_dump(void)
{
    static const char *n[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    fprintf(stderr, "eip=%08x eflags=%08x fs=%08x\n", X.eip, X.fl, X.fsbase);
    for (int i = 0; i < 8; i++)
        fprintf(stderr, "%s=%08x%s", n[i], X.r[i], i == 7 ? "\n" : " ");
}

/* ---- registers and operands ---- */

static inline u32 getr8(int n) { return n < 4 ? X.r[n] & 0xff : (X.r[n - 4] >> 8) & 0xff; }
static inline void setr8(int n, u32 v)
{
    if (n < 4)
        X.r[n] = (X.r[n] & ~0xffu) | (v & 0xff);
    else
        X.r[n - 4] = (X.r[n - 4] & ~0xff00u) | ((v & 0xff) << 8);
}
static inline u32 getrv(int n, int osz) { return osz == 32 ? X.r[n] : X.r[n] & 0xffff; }
static inline void setrv(int n, int osz, u32 v)
{
    if (osz == 32)
        X.r[n] = v;
    else
        X.r[n] = (X.r[n] & 0xffff0000u) | (v & 0xffff);
}

/* the address of a memory operand; *pe is the place in the code after the ModRM byte */
static inline u32 decode_ea(u32 *pe, int mod, int rm)
{
    u32 e = *pe, a;

    if (rm == 4) {
        u8 sib = r8(e++);
        int idx = (sib >> 3) & 7, base = sib & 7;
        if (base == 5 && mod == 0) {
            a = r32(e);
            e += 4;
        } else {
            a = X.r[base];
        }
        if (idx != 4)
            a += X.r[idx] << (sib >> 6);
    } else if (rm == 5 && mod == 0) {
        a = r32(e);
        e += 4;
    } else {
        a = X.r[rm];
    }
    if (mod == 1) {
        a += (u32)(s8)r8(e);
        e++;
    } else if (mod == 2) {
        a += r32(e);
        e += 4;
    }
    *pe = e;
    return a;
}

/* ---- flags ---- */

static inline void set_szp(u32 r, int bits)
{
    u32 f = X.fl & ~(ZF | SF | PF);
    u32 m = bits == 32 ? 0xffffffffu : (1u << bits) - 1;

    if (!(r & m))
        f |= ZF;
    if (r & (1u << (bits - 1)))
        f |= SF;
    if (parity[r & 0xff])
        f |= PF;
    X.fl = f;
}

/* ADD OR ADC SBB AND SUB XOR CMP (op 0 to 7) on operands of 8, 16 or 32 bits */
static u32 alu(int op, u32 a, u32 b, int bits)
{
    u32 m = bits == 32 ? 0xffffffffu : (1u << bits) - 1, sign = 1u << (bits - 1), r, f, cin = 0;
    u64 t;

    a &= m;
    b &= m;
    f = X.fl & ~(CF | AF | OF);
    switch (op) {
    case 2: /* ADC */
        cin = X.fl & CF;
        /* fall through */
    case 0: /* ADD */
        t = (u64)a + b + cin;
        r = (u32)t & m;
        if ((t >> bits) & 1)
            f |= CF;
        if (~(a ^ b) & (a ^ r) & sign)
            f |= OF;
        if ((a ^ b ^ r) & 0x10)
            f |= AF;
        break;
    case 3: /* SBB */
        cin = X.fl & CF;
        /* fall through */
    case 5: /* SUB */
    case 7: /* CMP */
        t = (u64)a - b - cin;
        r = (u32)t & m;
        if ((t >> bits) & 1)
            f |= CF;
        if ((a ^ b) & (a ^ r) & sign)
            f |= OF;
        if ((a ^ b ^ r) & 0x10)
            f |= AF;
        break;
    case 1: r = a | b; break;
    case 4: r = a & b; break;
    default: r = a ^ b; break;
    }
    X.fl = f;
    set_szp(r, bits);
    return r;
}

static u32 inc_dec(u32 a, int dec, int bits)
{
    u32 cf = X.fl & CF, r = alu(dec ? 5 : 0, a, 1, bits);
    X.fl = (X.fl & ~CF) | cf; /* INC and DEC keep CF */
    return r;
}

/* ROL ROR RCL RCR SHL SHR SAL SAR (op 0 to 7) */
static u32 shift(int op, u32 a, u32 n, int bits)
{
    u32 m = bits == 32 ? 0xffffffffu : (1u << bits) - 1, sign = 1u << (bits - 1), r = a & m, cf = X.fl & CF, of;

    n &= 31;
    if (!n)
        return r;
    switch (op) {
    case 0: /* ROL */
        n %= (u32)bits;
        if (n)
            r = ((r << n) | (r >> ((u32)bits - n))) & m;
        cf = r & 1;
        of = ((r & sign) != 0) ^ cf;
        X.fl = (X.fl & ~(CF | OF)) | cf | (of ? OF : 0);
        return r;
    case 1: /* ROR */
        n %= (u32)bits;
        if (n)
            r = ((r >> n) | (r << ((u32)bits - n))) & m;
        cf = (r & sign) != 0;
        of = ((r & sign) != 0) ^ ((r & (sign >> 1)) != 0);
        X.fl = (X.fl & ~(CF | OF)) | cf | (of ? OF : 0);
        return r;
    case 2: /* RCL */
        n %= (u32)bits + 1;
        for (u32 k = 0; k < n; k++) {
            u32 c = (r & sign) != 0;
            r = ((r << 1) | cf) & m;
            cf = c;
        }
        of = ((r & sign) != 0) ^ cf;
        X.fl = (X.fl & ~(CF | OF)) | cf | (of ? OF : 0);
        return r;
    case 3: /* RCR */
        n %= (u32)bits + 1;
        of = ((r & sign) != 0) ^ cf;
        for (u32 k = 0; k < n; k++) {
            u32 c = r & 1;
            r = (r >> 1) | (cf ? sign : 0);
            cf = c;
        }
        X.fl = (X.fl & ~(CF | OF)) | cf | (of ? OF : 0);
        return r;
    case 4:
    case 6: /* SHL */
        if (n <= (u32)bits) {
            cf = (r >> ((u32)bits - n)) & 1;
            r = n == (u32)bits ? 0 : (r << n) & m;
        } else {
            cf = 0;
            r = 0;
        }
        of = ((r & sign) != 0) ^ cf;
        break;
    case 5: /* SHR */
        of = (r & sign) != 0;
        if (n <= (u32)bits) {
            cf = (r >> (n - 1)) & 1;
            r = n == (u32)bits ? 0 : r >> n;
        } else {
            cf = 0;
            r = 0;
        }
        break;
    default: /* SAR */
        {
            s32 v = bits == 32 ? (s32)r : bits == 16 ? (s16)r : (s8)r;
            if (n >= (u32)bits) { /* all the bits are the sign */
                cf = v < 0;
                v = v < 0 ? -1 : 0;
            } else {
                cf = ((u32)v >> (n - 1)) & 1;
                v >>= n;
            }
            r = (u32)v & m;
        }
        of = 0;
        break;
    }
    X.fl = (X.fl & ~(CF | OF | AF)) | cf | (of ? OF : 0);
    set_szp(r, bits);
    return r;
}

/* the condition of Jcc, SETcc and CMOVcc: the low 4 bits of the opcode */
static inline int cond(int c)
{
    u32 f = X.fl;
    int r;

    switch (c >> 1) {
    case 0: r = (f & OF) != 0; break;
    case 1: r = (f & CF) != 0; break;
    case 2: r = (f & ZF) != 0; break;
    case 3: r = (f & (CF | ZF)) != 0; break;
    case 4: r = (f & SF) != 0; break;
    case 5: r = (f & PF) != 0; break;
    case 6: r = ((f & SF) != 0) != ((f & OF) != 0); break;
    default: r = (f & ZF) || ((f & SF) != 0) != ((f & OF) != 0); break;
    }
    return r ^ (c & 1);
}

/* ---- the stack ---- */

static inline void push32(u32 v)
{
    w32(X.r[ESP] - 4, v);
    X.r[ESP] -= 4;
}
static inline u32 pop32(void)
{
    u32 v = r32(X.r[ESP]);
    X.r[ESP] += 4;
    return v;
}

/* ---- x87 ---- */

/*
 * The registers are the 80-bit numbers of the x87. All the arithmetic is integer arithmetic
 * here (no floating point of the host), so the results are the same on each host: the same
 * bits as a real x87 for + - * / square root, remainder and all the conversions, in each
 * precision and rounding mode. SIN, COS, TAN, ATAN, LOG and EXP are calculated with 128 bits
 * and then rounded; a real x87 can be wrong by one unit in the last (64th) bit there.
 */

typedef unsigned __int128 u128;

#define ST(i) ((X.top + (i)) & 7)

/* the status word */
#define FS_IE 0x0001u /* an operation with no result (invalid) */
#define FS_DE 0x0002u
#define FS_ZE 0x0004u /* division by zero */
#define FS_OE 0x0008u /* too big */
#define FS_UE 0x0010u /* too small */
#define FS_PE 0x0020u /* the result is not exact */
#define FS_SF 0x0040u /* the register stack: push to a full one, use of an empty register */
#define FS_ES 0x0080u /* there is an exception that the program did not mask */
#define FS_C0 0x0100u
#define FS_C1 0x0200u
#define FS_C2 0x0400u
#define FS_C3 0x4000u
#define FS_B 0x8000u

#ifndef STATUS_FLOAT_INEXACT_RESULT
#define STATUS_FLOAT_INEXACT_RESULT 0xc000008fu
#endif
#ifndef STATUS_FLOAT_STACK_CHECK
#define STATUS_FLOAT_STACK_CHECK 0xc0000092u
#endif

#define XM_ONE 0x8000000000000000ull /* the first bit of the 64: a number has it */
#define XM_QUIET 0x4000000000000000ull

/* what a register holds */
enum { K_ZERO, K_NUM, K_INF, K_NAN, K_BAD };

/* A register taken apart. For K_NUM the value is m * 2^(e - 63), and bit 63 of m is 1. */
typedef struct {
    int k, s;
    s32 e;
    u64 m;
} un;

static int x87_de; /* an operand of this instruction was below the smallest normal number: see operand() */

static inline int clz128(u128 v) /* v != 0 */
{
    u64 h = (u64)(v >> 64);
    return h ? __builtin_clzll(h) : 64 + __builtin_clzll((u64)v);
}

static inline xr xr_make(int s, int ex, u64 m)
{
    xr r;
    r.m = m;
    r.se = (u16)((s ? 0x8000 : 0) | ex);
    return r;
}
static inline xr xr_zero(int s) { return xr_make(s, 0, 0); }
static inline xr xr_inf(int s) { return xr_make(s, 0x7fff, XM_ONE); }
static inline xr xr_one(void) { return xr_make(0, 0x3fff, XM_ONE); }
/* the NaN that the x87 gives when an operation has no result: its sign bit is 1 */
static inline xr xr_indef(void) { return xr_make(1, 0x7fff, XM_ONE | XM_QUIET); }
static xr xr_invalid(void)
{
    x87_de = 0;
    X.sw |= FS_IE;
    return xr_indef();
}

static un unpack(xr x)
{
    un u;
    int ex = x.se & 0x7fff;

    u.s = x.se >> 15;
    u.m = x.m;
    u.e = 0;
    if (ex == 0x7fff) {
        u.k = !(x.m >> 63) ? K_BAD : (x.m << 1) ? K_NAN : K_INF;
    } else if (!x.m) {
        u.k = ex ? K_BAD : K_ZERO;
    } else if (!ex) { /* a number below 2^-16382: its first bits are 0 */
        int sh = __builtin_clzll(x.m);
        u.k = K_NUM;
        u.m = x.m << sh;
        u.e = -16382 - sh;
    } else if (!(x.m >> 63)) {
        u.k = K_BAD; /* a format of the 8087 that later ones do not take */
    } else {
        u.k = K_NUM;
        u.e = ex - 16383;
    }
    return u;
}

/* An operand of an operation. When it is below the smallest normal number, the x87 says so
   (DE) if the operation gives a number: not when an operand is a NaN, not when the operation
   is invalid, not for a division by zero. x87() sets the flag at the end of the instruction. */
static un operand(xr x)
{
    if (!(x.se & 0x7fff) && x.m)
        x87_de = 1;
    return unpack(x);
}

static inline int is_nan(xr x) { return (x.se & 0x7fff) == 0x7fff && (x.m >> 63) && (x.m << 1); }
static inline int is_snan(xr x) { return is_nan(x) && !(x.m & XM_QUIET); }
static inline xr quiet(xr x)
{
    x.m |= XM_QUIET;
    return x;
}

/* The result when an operand is a NaN, as the x87 selects it: a quiet NaN before a
   signaling one, then the one with the larger 64 bits. */
static xr nan2(xr a, xr b)
{
    int as = is_snan(a), bs = is_snan(b), aq = is_nan(a) && !as, bq = is_nan(b) && !bs;
    int larger = a.m != b.m ? a.m > b.m : a.se < b.se, pick;

    x87_de = 0;
    if (as || bs)
        X.sw |= FS_IE;
    if (as)
        pick = bs ? larger : !bq;
    else if (aq)
        pick = bs || !bq ? 1 : larger;
    else
        pick = 0;
    return quiet(pick ? a : b);
}

/* The same for FPATAN, FYL2X and FYL2XP1 (a is ST(0), b is ST(1)): there a signaling NaN
   is before a quiet one, and ST(0) before ST(1). */
static xr nan2t(xr a, xr b)
{
    x87_de = 0;
    if (is_snan(a) || is_snan(b))
        X.sw |= FS_IE;
    return quiet(is_snan(a) ? a : is_snan(b) ? b : is_nan(a) ? a : b);
}

static inline int fp_rc(void) { return (X.cw >> 10) & 3; } /* 0 nearest, 1 down, 2 up, 3 to zero */
static inline int fp_prec(void)
{
    static const int p[4] = { 24, 64, 53, 64 };
    return p[(X.cw >> 8) & 3];
}

/* Round the number v * 2^(e - 127) with the sign s to p bits. A number below 2^emin gets
   fewer bits: its last bit is 2^(emin - p + 1). Bit 127 of v is 1. sticky: there is more
   below v. Result: 0 = it became 0; 1 = a number, *m (bit 63 is 1) and *e; 2 = too big.
   *fl gets FS_PE, FS_UE, FS_OE and FS_C1 (the size went up). */
static int fp_round(int s, s32 *e, u128 v, int sticky, int p, s32 emin, s32 emax, u64 *m, u16 *fl)
{
    s32 q = *e - p + 1; /* the exponent of the last bit that stays */
    int tiny = 0, drop, rb, st, top;
    u128 kept;

    if (*e < emin) {
        q = emin - p + 1;
        tiny = 1;
    }
    drop = 127 - (*e - q);
    if (drop > 128) {
        kept = 0;
        rb = 0;
        st = 1;
    } else if (drop == 128) {
        kept = 0;
        rb = 1;
        st = (v << 1) != 0 || sticky;
    } else {
        kept = v >> drop;
        rb = (int)(v >> (drop - 1)) & 1;
        st = (v & (((u128)1 << (drop - 1)) - 1)) != 0 || sticky;
    }
    if (rb || st) {
        int up = 0;
        switch (fp_rc()) {
        case 0: up = rb && (st || (kept & 1)); break;
        case 1: up = s; break;
        case 2: up = !s; break;
        }
        *fl |= FS_PE;
        if (tiny)
            *fl |= FS_UE;
        if (up) {
            kept++;
            *fl |= FS_C1;
        }
    }
    if (!kept)
        return 0;
    top = 127 - clz128(kept); /* at most 64 */
    *e = q + top;
    *m = top >= 63 ? (u64)(kept >> (top - 63)) : (u64)kept << (63 - top);
    if (*e > emax) {
        *fl |= FS_OE | FS_PE;
        return 2;
    }
    return 1;
}

/* a result that is too big: an infinity, or the largest number when the rounding mode goes away from the infinity */
static inline int ovf_inf(int s)
{
    int rc = fp_rc();
    return rc == 0 || (rc == 1 && s) || (rc == 2 && !s);
}

/* the number v * 2^(e - 127) with the sign s as a register, rounded to p bits; v != 0 */
static xr xr_round_p(int s, s32 e, u128 v, int sticky, int p)
{
    u64 m = 0;
    u16 fl = 0;
    int sh = clz128(v), c;

    v <<= sh;
    e -= sh;
    c = fp_round(s, &e, v, sticky, p, -16382, 16383, &m, &fl);
    X.sw = (X.sw & ~FS_C1) | fl;
    if (c == 0)
        return xr_zero(s);
    if (c == 2)
        return ovf_inf(s) ? xr_inf(s) : xr_make(s, 0x7ffe, ~0ull << (64 - p));
    if (e < -16382)
        return xr_make(s, 0, m >> (-16382 - e));
    return xr_make(s, e + 16383, m);
}

/* the same, in the precision that the control word has: the results of + - * / and square root */
static inline xr xr_round(int s, s32 e, u128 v, int sticky) { return xr_round_p(s, e, v, sticky, fp_prec()); }

static xr xr_addsub(xr a, xr b, int sub)
{
    un x = operand(a), y = operand(b);
    u128 vx, vy, v;
    s32 d;

    if (x.k == K_BAD || y.k == K_BAD)
        return xr_invalid();
    if (x.k == K_NAN || y.k == K_NAN)
        return nan2(a, b);
    if (sub)
        y.s ^= 1;
    if (x.k == K_INF || y.k == K_INF) {
        if (x.k == y.k && x.s != y.s)
            return xr_invalid();
        return xr_inf(x.k == K_INF ? x.s : y.s);
    }
    if (x.k == K_ZERO && y.k == K_ZERO)
        return xr_zero(x.s == y.s ? x.s : fp_rc() == 1);
    if (x.k == K_ZERO) {
        un t = x;
        x = y;
        y = t;
    }
    if (y.k == K_ZERO)
        return xr_round(x.s, x.e, (u128)x.m << 64, 0);
    if (x.e < y.e || (x.e == y.e && x.m < y.m)) {
        un t = x;
        x = y;
        y = t;
    }
    /* |x| >= |y|. The smaller one moves down; the bits that it loses stay as one bit. */
    d = x.e - y.e;
    vx = (u128)x.m << 63;
    vy = (u128)y.m << 63;
    if (d > 126)
        vy = 1;
    else if (d)
        vy = vy >> d | ((vy & (((u128)1 << d) - 1)) != 0);
    if (x.s == y.s) {
        v = vx + vy;
    } else {
        v = vx - vy;
        if (!v)
            return xr_zero(fp_rc() == 1);
    }
    return xr_round(x.s, x.e + 1, v, 0);
}

static xr xr_mul(xr a, xr b)
{
    un x = operand(a), y = operand(b);
    int s = x.s ^ y.s;

    if (x.k == K_BAD || y.k == K_BAD)
        return xr_invalid();
    if (x.k == K_NAN || y.k == K_NAN)
        return nan2(a, b);
    if (x.k == K_INF || y.k == K_INF)
        return x.k == K_ZERO || y.k == K_ZERO ? xr_invalid() : xr_inf(s);
    if (x.k == K_ZERO || y.k == K_ZERO)
        return xr_zero(s);
    return xr_round(s, x.e + y.e + 1, (u128)x.m * y.m, 0);
}

static xr xr_div(xr a, xr b)
{
    un x = operand(a), y = operand(b);
    int s = x.s ^ y.s, q1;
    u64 r, qh, ql;
    u128 t, v;

    if (x.k == K_BAD || y.k == K_BAD)
        return xr_invalid();
    if (x.k == K_NAN || y.k == K_NAN)
        return nan2(a, b);
    if (x.k == K_INF)
        return y.k == K_INF ? xr_invalid() : xr_inf(s);
    if (y.k == K_INF)
        return xr_zero(s);
    if (y.k == K_ZERO) {
        if (x.k == K_ZERO)
            return xr_invalid();
        x87_de = 0;
        X.sw |= FS_ZE;
        return xr_inf(s);
    }
    if (x.k == K_ZERO)
        return xr_zero(s);
    /* the first bit of the quotient, then 128 more bits */
    q1 = x.m >= y.m;
    r = q1 ? x.m - y.m : x.m;
    t = (u128)r << 64;
    qh = (u64)(t / y.m);
    r = (u64)(t % y.m);
    t = (u128)r << 64;
    ql = (u64)(t / y.m);
    r = (u64)(t % y.m);
    v = (u128)qh << 64 | ql;
    if (q1)
        return xr_round(s, x.e - y.e, v >> 1 | (u128)1 << 127, (int)(v & 1) | (r != 0));
    return xr_round(s, x.e - y.e - 1, v, r != 0);
}

static u64 isqrt128(u128 n) /* the whole part of the square root; n >= 2^126 */
{
    u128 r = ((u128)1 << 64) - 1, t;

    for (;;) {
        t = (r + n / r) >> 1;
        if (t >= r)
            return (u64)r;
        r = t;
    }
}

static xr xr_sqrt(xr a)
{
    un x = operand(a);
    u128 n, rem;
    u64 r;
    s32 e;

    if (a.se & 0x8000) /* the x87 of the reference machine sets C2 for an operand with a minus sign */
        X.sw = (X.sw & ~(FS_C0 | FS_C1 | FS_C3)) | FS_C2;
    if (x.k == K_BAD)
        return xr_invalid();
    if (x.k == K_NAN)
        return nan2(a, a);
    if (x.k == K_ZERO)
        return a;
    if (x.s)
        return xr_invalid();
    if (x.k == K_INF)
        return a;
    if (x.e & 1) {
        n = (u128)x.m << 64;
        e = (x.e - 1) / 2;
    } else {
        n = (u128)x.m << 63;
        e = x.e / 2;
    }
    r = isqrt128(n);
    rem = n - (u128)r * r;
    /* the bit after the 64, and a bit that says the root is not exact */
    return xr_round(0, e, (u128)r << 64 | (rem > r ? (u128)1 << 63 : 0) | (rem != 0), 0);
}

/* -1: a < b, 0: equal, 1: a > b, 2: no order (a NaN). FUCOM is quiet: a quiet NaN is not an error there. */
static int xr_cmp(xr a, xr b, int quiet_cmp)
{
    un x = operand(a), y = operand(b);
    int c;

    if (x.k >= K_NAN || y.k >= K_NAN) {
        x87_de = 0;
        if (!quiet_cmp || x.k == K_BAD || y.k == K_BAD || is_snan(a) || is_snan(b))
            X.sw |= FS_IE;
        return 2;
    }
    if (x.k == K_ZERO && y.k == K_ZERO)
        return 0;
    if (x.k == K_ZERO)
        return y.s ? 1 : -1;
    if (y.k == K_ZERO)
        return x.s ? -1 : 1;
    if (x.s != y.s)
        return x.s ? -1 : 1;
    if (x.k == K_INF || y.k == K_INF)
        c = x.k == y.k ? 0 : x.k == K_INF ? 1 : -1;
    else if (x.e != y.e)
        c = x.e > y.e ? 1 : -1;
    else
        c = x.m == y.m ? 0 : x.m > y.m ? 1 : -1;
    return x.s ? -c : c;
}

/* ---- the formats in memory ---- */

static xr xr_from_f64(u64 v)
{
    int s = (int)(v >> 63), ex = (int)(v >> 52) & 0x7ff;
    u64 f = v & 0xfffffffffffffull;

    if (ex == 0x7ff) {
        if (!f)
            return xr_inf(s);
        if (!(f >> 51))
            X.sw |= FS_IE; /* a signaling NaN */
        return xr_make(s, 0x7fff, XM_ONE | XM_QUIET | f << 11);
    }
    if (!ex) {
        int sh;
        if (!f)
            return xr_zero(s);
        x87_de = 1;
        sh = __builtin_clzll(f);
        return xr_make(s, -1074 + 63 - sh + 16383, f << sh);
    }
    return xr_make(s, ex - 1023 + 16383, XM_ONE | f << 11);
}

static xr xr_from_f32(u32 v)
{
    int s = (int)(v >> 31), ex = (int)(v >> 23) & 0xff;
    u64 f = v & 0x7fffffu;

    if (ex == 0xff) {
        if (!f)
            return xr_inf(s);
        if (!(f >> 22))
            X.sw |= FS_IE;
        return xr_make(s, 0x7fff, XM_ONE | XM_QUIET | f << 40);
    }
    if (!ex) {
        int sh;
        if (!f)
            return xr_zero(s);
        x87_de = 1;
        sh = __builtin_clzll(f);
        return xr_make(s, -149 + 63 - sh + 16383, f << sh);
    }
    return xr_make(s, ex - 127 + 16383, XM_ONE | f << 40);
}

static xr xr_from_int(s64 n)
{
    u64 a = n < 0 ? 0 - (u64)n : (u64)n;
    int sh;

    if (!a)
        return xr_zero(0);
    sh = __builtin_clzll(a);
    return xr_make(n < 0, 63 - sh + 16383, a << sh);
}

static u64 xr_to_f64(xr x)
{
    un u = unpack(x);
    u64 sign = (u64)u.s << 63, m = 0;
    u16 fl = 0;
    s32 e = u.e;
    int c;

    switch (u.k) {
    case K_ZERO: return sign;
    case K_INF: return sign | 0x7ff0000000000000ull;
    case K_NAN:
        if (!(u.m & XM_QUIET))
            X.sw |= FS_IE;
        return sign | 0x7ff8000000000000ull | (u.m << 1) >> 12;
    case K_BAD:
        X.sw |= FS_IE;
        return 0xfff8000000000000ull;
    }
    c = fp_round(u.s, &e, (u128)u.m << 64, 0, 53, -1022, 1023, &m, &fl);
    X.sw = (X.sw & ~FS_C1) | fl;
    if (c == 0)
        return sign;
    if (c == 2)
        return sign | (ovf_inf(u.s) ? 0x7ff0000000000000ull : 0x7fefffffffffffffull);
    if (e < -1022)
        return sign | m >> (11 + (-1022 - e));
    return sign | (u64)(e + 1023) << 52 | ((m >> 11) & 0xfffffffffffffull);
}

static u32 xr_to_f32(xr x)
{
    un u = unpack(x);
    u32 sign = (u32)u.s << 31;
    u64 m = 0;
    u16 fl = 0;
    s32 e = u.e;
    int c;

    switch (u.k) {
    case K_ZERO: return sign;
    case K_INF: return sign | 0x7f800000u;
    case K_NAN:
        if (!(u.m & XM_QUIET))
            X.sw |= FS_IE;
        return sign | 0x7fc00000u | (u32)((u.m << 1) >> 41);
    case K_BAD:
        X.sw |= FS_IE;
        return 0xffc00000u;
    }
    c = fp_round(u.s, &e, (u128)u.m << 64, 0, 24, -126, 127, &m, &fl);
    X.sw = (X.sw & ~FS_C1) | fl;
    if (c == 0)
        return sign;
    if (c == 2)
        return sign | (ovf_inf(u.s) ? 0x7f800000u : 0x7f7fffffu);
    if (e < -126)
        return sign | (u32)(m >> (40 + (-126 - e)));
    return sign | (u32)(e + 127) << 23 | ((u32)(m >> 40) & 0x7fffffu);
}

/* The size of the number as a whole number, in the rounding mode rc. *big: it does not go
   into 64 bits. *fl gets FS_PE and FS_C1. For K_NUM and K_ZERO only. */
static u64 un_whole(un u, int rc, int *big, u16 *fl)
{
    u64 n;
    int rb, st, up = 0;

    *big = 0;
    if (u.k == K_ZERO)
        return 0;
    if (u.e > 63) {
        *big = 1;
        return 0;
    }
    if (u.e == 63)
        return u.m;
    if (u.e >= 0) {
        u64 low = u.m << (u.e + 1);
        n = u.m >> (63 - u.e);
        rb = (int)(low >> 63);
        st = (low << 1) != 0;
    } else {
        n = 0;
        rb = u.e == -1;
        st = u.e < -1 || (u.m << 1) != 0;
    }
    if (rb || st) {
        switch (rc) {
        case 0: up = rb && (st || (n & 1)); break;
        case 1: up = u.s; break;
        case 2: up = !u.s; break;
        }
        *fl |= FS_PE;
        if (up) {
            n++;
            *fl |= FS_C1;
        }
    }
    return n;
}

/* FIST: the register as an integer of 16, 32 or 64 bits. A number that does not go into it,
   an infinity or a NaN gives the "indefinite" integer: the most negative one. */
static s64 xr_to_int(xr x, int bits, int rc)
{
    un u = unpack(x);
    u64 lim = 1ull << (bits - 1), n;
    u16 fl = 0;
    int big = 1;

    n = u.k <= K_NUM ? un_whole(u, rc, &big, &fl) : 0;
    if (big || n > lim || (n == lim && !u.s)) {
        X.sw = (X.sw & ~FS_C1) | FS_IE;
        return (s64)(0 - lim);
    }
    X.sw = (X.sw & ~FS_C1) | fl;
    return u.s ? (s64)(0 - n) : (s64)n;
}

/* FRNDINT */
static xr xr_rndint(xr a)
{
    un u = operand(a);
    u16 fl = 0;
    u64 n;
    int big, sh;

    if (u.k == K_BAD)
        return xr_invalid();
    if (u.k == K_NAN)
        return nan2(a, a);
    if (u.k != K_NUM || u.e >= 63)
        return a;
    n = un_whole(u, fp_rc(), &big, &fl);
    X.sw = (X.sw & ~FS_C1) | fl;
    if (!n)
        return xr_zero(u.s);
    sh = __builtin_clzll(n);
    return xr_make(u.s, 63 - sh + 16383, n << sh);
}

/* FSCALE: a * 2^(the whole part of b) */
static xr xr_scale(xr a, xr b)
{
    un x = operand(a), y = unpack(b);
    s32 n;

    if (x.k == K_BAD || y.k == K_BAD)
        return xr_invalid();
    if (y.k == K_NAN) {
        if (is_snan(a) || is_snan(b))
            X.sw |= FS_IE;
        return quiet(b);
    }
    if (x.k == K_NAN)
        return nan2(a, a);
    if (y.k == K_INF) {
        if (y.s)
            return x.k == K_INF ? xr_invalid() : xr_zero(x.s);
        return x.k == K_ZERO ? xr_invalid() : xr_inf(x.s);
    }
    if (x.k != K_NUM)
        return a;
    n = y.k == K_ZERO || y.e < 0 ? 0 : y.e > 20 ? 0x100000 : (s32)(y.m >> (63 - y.e));
    return xr_round_p(x.s, y.s ? x.e - n : x.e + n, (u128)x.m << 64, 0, 64);
}

/* FPREM (nearest = 0: the quotient loses its fraction) and FPREM1 (nearest = 1: the quotient
   is the nearest whole number). The x87 takes off at most 63 bits of the quotient in one
   instruction; then C2 says that the remainder is not complete. */
static xr xr_rem(xr a, xr b, int nearest)
{
    un x = operand(a), y = operand(b);
    s32 d, i;
    u128 r;
    u64 q = 0;
    int s, sh;

    X.sw &= ~(FS_C0 | FS_C1 | FS_C2 | FS_C3);
    if (x.k == K_BAD || y.k == K_BAD)
        return xr_invalid();
    if (x.k == K_NAN || y.k == K_NAN)
        return nan2(a, b);
    if (x.k == K_INF || y.k == K_ZERO)
        return xr_invalid();
    if (y.k == K_INF)
        return x.k == K_NUM ? xr_make(x.s, x.e < -16382 ? 0 : x.e + 16383, x.e < -16382 ? x.m >> (-16382 - x.e) : x.m) : a;
    if (x.k == K_ZERO)
        return a;
    d = x.e - y.e;
    s = x.s;
    if (d >= 64) {
        /* a part of the work only: the divisor is b * 2^(d - n) */
        s32 n = 32 + d % 32;
        y.e += d - n;
        d = n;
        nearest = 0;
        X.sw |= FS_C2;
    }
    if (d < -1 || (d == -1 && !nearest))
        return a; /* |a| < |b| (FPREM), |a| < |b| / 2 (FPREM1) */
    /* r and b as whole numbers of 2^(y.e - 63), or of the half of that when d is -1 */
    if (d == -1) {
        if (x.m <= y.m)
            return a; /* a <= b / 2: the quotient is 0 */
        r = ((u128)y.m << 1) - x.m; /* the quotient is 1 */
        sh = clz128(r);
        a = xr_round_p(s ^ 1, y.e - 1 - 63 + 127 - sh, r << sh, 0, 64);
        X.sw |= FS_C1;
        return a;
    }
    r = x.m;
    for (i = d;; i--) {
        q <<= 1;
        if (r >= y.m) {
            r -= y.m;
            q |= 1;
        }
        if (!i)
            break;
        r <<= 1;
    }
    if (nearest && r) {
        u128 r2 = r << 1;
        if (r2 > y.m || (r2 == y.m && (q & 1))) {
            q++;
            r = y.m - r;
            s ^= 1;
        }
    }
    if (!r) {
        a = xr_zero(x.s);
    } else {
        sh = clz128(r);
        a = xr_round_p(s, y.e - 63 + 127 - sh, r << sh, 0, 64);
    }
    /* the last 3 bits of the quotient */
    if (!(X.sw & FS_C2))
        X.sw |= (q & 4 ? FS_C0 : 0) | (q & 2 ? FS_C3 : 0) | (q & 1 ? FS_C1 : 0);
    return a;
}

/* ---- numbers of 128 bits, for SIN, COS, TAN, ATAN, LOG and EXP ---- */

/* m * 2^(e - 127); bit 127 of m is 1, or m is 0 */
typedef struct {
    int s;
    s32 e;
    u128 m;
} bf;

#define BF_ZERO_E (-0x40000000)
#define BFC(e, hi, lo) { 0, e, (u128)(hi) << 64 | (lo) }

static const bf bf_one = BFC(0, 0x8000000000000000ull, 0);
static const bf bf_two = BFC(1, 0x8000000000000000ull, 0);
static const bf bf_pi = BFC(1, 0xc90fdaa22168c234ull, 0xc4c6628b80dc1cd1ull);
static const bf bf_pio2 = BFC(0, 0xc90fdaa22168c234ull, 0xc4c6628b80dc1cd1ull);
static const bf bf_pio4 = BFC(-1, 0xc90fdaa22168c234ull, 0xc4c6628b80dc1cd1ull);
static const bf bf_ln2 = BFC(-1, 0xb17217f7d1cf79abull, 0xc9e3b39803f2f6afull);
static const bf bf_log2e = BFC(0, 0xb8aa3b295c17f0bbull, 0xbe87fed0691d3e89ull);

/* 2 / pi: 448 bits after the point, the first ones in the last element */
static const u64 two_over_pi[7] = {
    0xfe1deb1cb129a73eull, 0x06492eea09d1921cull, 0xb7246e3a424dd2e0ull, 0xfe5163abdebbc561ull,
    0xdb6295993c439041ull, 0xfc2757d1f534ddc0ull, 0xa2f9836e4e441529ull,
};

static bf bf_make(int s, s32 e, u128 m)
{
    bf r;
    int sh;

    r.s = s;
    if (!m) {
        r.e = BF_ZERO_E;
        r.m = 0;
        return r;
    }
    sh = clz128(m);
    r.m = m << sh;
    r.e = e - sh;
    return r;
}

static inline bf bf_u64(int s, s32 e, u64 m) { return bf_make(s, e, (u128)m << 64); } /* m * 2^(e - 63) */
static inline bf bf_int(u32 n) { return bf_make(0, 127, n); }
static inline bf bf_un(un u) { return bf_u64(u.s, u.e, u.m); }

static bf bf_add(bf a, bf b)
{
    s32 d;
    u128 bm, sum;

    if (!a.m)
        return b;
    if (!b.m)
        return a;
    if (a.e < b.e || (a.e == b.e && a.m < b.m)) {
        bf t = a;
        a = b;
        b = t;
    }
    d = a.e - b.e;
    if (a.s != b.s) {
        /* a - b. When bits of b are lost (b is much smaller), the result is a little less
           than a - (what is left of b): one unit less keeps it on the correct side for the
           rounding. So sin(x) of a very small x is a little less than x, not x. */
        int lost = d > 127 || (d && (b.m & (((u128)1 << d) - 1)) != 0);
        bm = d > 127 ? 0 : b.m >> d;
        return bf_make(a.s, a.e, a.m - bm - (u128)lost);
    }
    if (d > 127)
        return a;
    bm = b.m >> d;
    sum = a.m + bm;
    if (sum < a.m) {
        sum = sum >> 1 | (u128)1 << 127;
        a.e++;
    }
    a.m = sum;
    return a;
}

static inline bf bf_sub(bf a, bf b)
{
    b.s ^= 1;
    return bf_add(a, b);
}

static bf bf_mul(bf a, bf b)
{
    u64 ah = (u64)(a.m >> 64), al = (u64)a.m, bh = (u64)(b.m >> 64), bl = (u64)b.m;
    u128 hh, hl, lh, ll, mid, hi;
    bf r;

    if (!a.m || !b.m)
        return bf_make(a.s ^ b.s, 0, 0);
    hh = (u128)ah * bh;
    hl = (u128)ah * bl;
    lh = (u128)al * bh;
    ll = (u128)al * bl;
    mid = (u128)(u64)hl + (u64)lh + (u64)(ll >> 64);
    hi = hh + (hl >> 64) + (lh >> 64) + (mid >> 64); /* the first 128 bits of the 256 */
    r.s = a.s ^ b.s;
    if (hi >> 127) {
        r.m = hi;
        r.e = a.e + b.e + 1;
    } else {
        r.m = hi << 1 | (u64)mid >> 63;
        r.e = a.e + b.e;
    }
    return r;
}

static bf bf_div(bf a, bf b) /* b is not 0 */
{
    u128 r, q = 0;
    int q0, i;
    bf z;

    if (!a.m)
        return bf_make(a.s ^ b.s, 0, 0);
    q0 = a.m >= b.m;
    r = q0 ? a.m - b.m : a.m;
    for (i = 0; i < 128; i++) {
        int c = (int)(r >> 127);
        r <<= 1;
        q <<= 1;
        if (c || r >= b.m) {
            r -= b.m;
            q |= 1;
        }
    }
    z.s = a.s ^ b.s;
    if (q0) {
        z.m = q >> 1 | (u128)1 << 127;
        z.e = a.e - b.e;
    } else {
        z.m = q;
        z.e = a.e - b.e - 1;
    }
    return z;
}

static bf bf_div_u(bf a, u32 n) /* a / n; n is not 0 */
{
    u128 q, r;
    int sh;

    if (!a.m)
        return a;
    q = a.m / n;
    r = a.m % n;
    sh = clz128(q);
    a.m = q << sh | (r << sh) / n;
    a.e -= sh;
    return a;
}

static inline int bf_small(bf t, bf sum) { return !t.m || t.e < sum.e - 132; } /* t does not change sum */

/* a result of 128 bits as a register: 64 bits, in the rounding mode. It is not exact. */
static xr bf_xr(bf r)
{
    if (!r.m)
        return xr_zero(r.s);
    return xr_round_p(r.s, r.e, r.m, 1, 64);
}

/* sine and cosine of r; |r| is not much more than pi / 4 */
static void bf_sincos(bf r, bf *sn, bf *cs)
{
    bf r2 = bf_mul(r, r), t, sum;
    u32 k;

    sum = t = r;
    for (k = 2; k < 120 && !bf_small(t, sum); k += 2) {
        t = bf_div_u(bf_mul(t, r2), k * (k + 1));
        t.s ^= 1;
        sum = bf_add(sum, t);
    }
    *sn = sum;
    sum = t = bf_one;
    for (k = 1; k < 120 && !bf_small(t, sum); k += 2) {
        t = bf_div_u(bf_mul(t, r2), k * (k + 1));
        t.s ^= 1;
        sum = bf_add(sum, t);
    }
    *cs = sum;
}

/* m * 2^(e - 63) = q * pi / 2 + r, |r| <= pi / 4, for a number from pi / 4 to 2^63.
   The result is q modulo 4. */
static int bf_reduce(u64 m, s32 e, bf *r)
{
    u64 p[9], q, hi, lo;
    u128 c = 0;
    int i, sh = e + 1, neg, z;

    /* m * (2 / pi), then the point goes to the top of p[7] */
    for (i = 0; i < 7; i++) {
        c += (u128)m * two_over_pi[i];
        p[i] = (u64)c;
        c >>= 64;
    }
    p[7] = (u64)c;
    p[8] = 0;
    if (sh) {
        for (i = 8; i > 0; i--)
            p[i] = p[i] << sh | p[i - 1] >> (64 - sh);
        p[0] <<= sh;
    }
    q = p[8];
    neg = (int)(p[7] >> 63);
    if (neg) { /* the fraction is more than a half: go to the next whole number */
        int carry = 1;
        q++;
        for (i = 0; i < 8; i++) {
            p[i] = ~p[i] + (u64)carry;
            carry = carry && !p[i];
        }
    }
    for (i = 7; i > 0 && !p[i]; i--)
        ;
    if (!p[i]) {
        *r = bf_make(0, 0, 0);
        return (int)(q & 3);
    }
    z = __builtin_clzll(p[i]);
    hi = p[i] << z;
    lo = i >= 1 ? p[i - 1] << z : 0;
    if (z) {
        hi |= i >= 1 ? p[i - 1] >> (64 - z) : 0;
        lo |= i >= 2 ? p[i - 2] >> (64 - z) : 0;
    }
    /* the first bit is bit 64 * i + 63 - z of the 512 bits after the point */
    r->s = neg;
    r->e = 64 * i + 63 - z - 512;
    r->m = (u128)hi << 64 | lo;
    *r = bf_mul(*r, bf_pio2);
    return (int)(q & 3);
}

/* SIN, COS, TAN of a register. which: 0 sine, 1 cosine, 2 tangent. The result is 0 when
   the number is too big (2^63 or more): then the x87 does nothing but set C2. */
static int xr_trig(xr a, int which, xr *r1, xr *r2)
{
    un u = operand(a);
    bf r, sn, cs, t;
    int q = 0;

    X.sw &= ~FS_C2;
    if (u.k == K_BAD || u.k == K_INF) {
        *r1 = *r2 = xr_invalid();
        return 1;
    }
    if (u.k == K_NAN) {
        *r1 = *r2 = nan2(a, a);
        return 1;
    }
    if (u.k == K_ZERO) {
        *r1 = which == 1 ? xr_one() : a;
        *r2 = xr_one();
        return 1;
    }
    if (u.e > 62) {
        X.sw |= FS_C2;
        return 0;
    }
    if (which == 2 && u.e < -60) { /* the tangent of a very small x is a little more than x */
        *r1 = xr_round_p(u.s, u.e, (u128)u.m << 64, 1, 64);
        *r2 = xr_one();
        return 1;
    }
    r = bf_un(u);
    r.s = 0;
    if (u.e > -1 || (u.e == -1 && u.m > 0xc90fdaa22168c234ull))
        q = bf_reduce(u.m, u.e, &r);
    bf_sincos(r, &sn, &cs);
    /* |x| = q * pi / 2 + r */
    if (q & 1) {
        t = sn;
        sn = cs;
        cs = t;
        cs.s ^= 1;
    }
    if (q & 2) {
        sn.s ^= 1;
        cs.s ^= 1;
    }
    sn.s ^= u.s;
    if (which == 2) {
        *r1 = bf_xr(bf_div(sn, cs));
        *r2 = xr_one();
    } else {
        *r1 = bf_xr(which ? cs : sn);
        *r2 = bf_xr(cs);
    }
    return 1;
}

static bf bf_atan(bf t) /* 0 <= t <= 1 */
{
    bf t2, pw, sum;
    int big = 0;
    u32 k;

    if (!t.m)
        return t;
    if (t.e > -2 || (t.e == -2 && (u64)(t.m >> 64) > 0xd413cccfe7799211ull)) {
        /* more than the square root of 2 less 1: atan(t) = pi / 4 + atan((t - 1) / (t + 1)) */
        t = bf_div(bf_sub(t, bf_one), bf_add(t, bf_one));
        big = 1;
    }
    t2 = bf_mul(t, t);
    sum = pw = t;
    for (k = 3; k < 400 && !bf_small(pw, sum); k += 2) {
        pw = bf_mul(pw, t2);
        pw.s ^= 1;
        sum = bf_add(sum, bf_div_u(pw, k));
    }
    return big ? bf_add(sum, bf_pio4) : sum;
}

/* FPATAN: the angle of the point (x, y), -pi to pi */
static xr xr_atan2(xr ya, xr xa)
{
    un y = operand(ya), x = operand(xa);
    bf ay, ax, r;

    if (y.k == K_BAD || x.k == K_BAD)
        return xr_invalid();
    if (y.k == K_NAN || x.k == K_NAN)
        return nan2t(xa, ya);
    if (y.k == K_ZERO || (x.k == K_INF && y.k != K_INF)) {
        if (!x.s)
            return xr_zero(y.s);
        r = bf_pi;
    } else if (x.k == K_ZERO || (y.k == K_INF && x.k != K_INF)) {
        r = bf_pio2;
    } else if (y.k == K_INF) {
        r = bf_pio4;
        if (x.s)
            r = bf_sub(bf_pi, r);
    } else {
        ay = bf_un(y);
        ax = bf_un(x);
        ay.s = ax.s = 0;
        if (ay.e > ax.e || (ay.e == ax.e && ay.m > ax.m))
            r = bf_sub(bf_pio2, bf_atan(bf_div(ax, ay)));
        else
            r = bf_atan(bf_div(ay, ax));
        if (x.s)
            r = bf_sub(bf_pi, r);
    }
    r.s = y.s;
    return bf_xr(r);
}

/* ln((1 + z) / (1 - z)) = 2 * (z + z^3 / 3 + z^5 / 5 ...), for a small z */
static bf bf_atanh2(bf z)
{
    bf z2 = bf_mul(z, z), pw = z, sum = z;
    u32 k;

    for (k = 3; k < 400 && !bf_small(pw, sum); k += 2) {
        pw = bf_mul(pw, z2);
        sum = bf_add(sum, bf_div_u(pw, k));
    }
    if (sum.m)
        sum.e++;
    return sum;
}

/* FYL2X: y * log2(x) */
static xr xr_yl2x(xr ya, xr xa)
{
    un y = operand(ya), x = operand(xa);
    bf f, l;
    s32 k;
    int lt1;

    if (y.k == K_BAD || x.k == K_BAD)
        return xr_invalid();
    if (y.k == K_NAN || x.k == K_NAN)
        return nan2t(xa, ya);
    if (x.s && x.k != K_ZERO)
        return xr_invalid();
    lt1 = x.k == K_ZERO || (x.k == K_NUM && x.e < 0); /* x < 1 */
    if (y.k == K_INF) {
        if (x.k == K_NUM && x.e == 0 && x.m == XM_ONE)
            return xr_invalid();
        return xr_inf(y.s ^ lt1);
    }
    if (x.k == K_INF)
        return y.k == K_ZERO ? xr_invalid() : xr_inf(y.s);
    if (x.k == K_ZERO) {
        if (y.k == K_ZERO)
            return xr_invalid();
        X.sw |= FS_ZE;
        return xr_inf(!y.s);
    }
    if (y.k == K_ZERO)
        return xr_zero(y.s ^ lt1);
    if (x.m == XM_ONE) { /* a power of 2: the logarithm is exact */
        if (!x.e)
            return xr_zero(y.s);
        return xr_round_p(y.s ^ (x.e < 0), y.e + 64, (u128)y.m * (u64)(x.e < 0 ? -x.e : x.e), 0, 64);
    }
    /* x = f * 2^k, f from 0.707 to 1.414; ln(f) from (f - 1) / (f + 1) */
    f = bf_u64(0, 0, x.m);
    k = x.e;
    if (x.m > 0xb504f333f9de6484ull) {
        f.e = -1;
        k++;
    }
    l = bf_mul(bf_atanh2(bf_div(bf_sub(f, bf_one), bf_add(f, bf_one))), bf_log2e);
    if (k) {
        bf kk = bf_int((u32)(k < 0 ? -k : k));
        kk.s = k < 0;
        l = bf_add(kk, l);
    }
    return bf_xr(bf_mul(l, bf_un(y)));
}

/* FYL2XP1: y * log2(x + 1), for x from -0.29 to 0.41 */
static xr xr_yl2xp1(xr ya, xr xa)
{
    un y = operand(ya), x = operand(xa);
    bf bx;

    if (y.k == K_BAD || x.k == K_BAD)
        return xr_invalid();
    if (y.k == K_NAN || x.k == K_NAN)
        return nan2t(xa, ya);
    if (x.k == K_INF || (x.k == K_NUM && (x.e > -2 || (x.e == -2 && x.m > (x.s ? 0x95f619980c4336f7ull : 0xd413cccfe7799211ull)))))
        return xr_invalid();
    if (x.k == K_ZERO || y.k != K_NUM)
        return xr_mul(xa, ya);
    bx = bf_un(x);
    return bf_xr(bf_mul(bf_mul(bf_atanh2(bf_div(bx, bf_add(bx, bf_two))), bf_log2e), bf_un(y)));
}

/* F2XM1: 2^x - 1, for x from -1 to 1 */
static xr xr_2xm1(xr a)
{
    un x = operand(a);
    bf y, t, sum;
    u32 k;

    if (x.k == K_BAD)
        return xr_invalid();
    if (x.k == K_NAN)
        return nan2(a, a);
    if (x.k == K_ZERO)
        return a;
    if (x.k == K_INF || x.e > 0 || (x.e == 0 && x.m != XM_ONE))
        return xr_invalid();
    if (x.e == 0)
        return x.s ? xr_make(1, 0x3ffe, XM_ONE) : a; /* -1: -0.5; 1: 1 */
    /* e^y - 1 = y + y^2 / 2 + y^3 / 6 ..., y = x * ln(2) */
    y = bf_mul(bf_un(x), bf_ln2);
    sum = t = y;
    for (k = 2; k < 200 && !bf_small(t, sum); k++) {
        t = bf_div_u(bf_mul(t, y), k);
        sum = bf_add(sum, t);
    }
    return bf_xr(sum);
}

/* ---- the register stack ---- */

static inline void fset(int i, xr v)
{
    int p = ST(i);
    X.st[p] = v;
    X.empty[p] = 0;
}

static xr fget(int i) /* ST(i); an empty register is an error and gives the NaN */
{
    int p = ST(i);
    if (X.empty[p]) {
        X.sw = (X.sw & ~FS_C1) | FS_IE | FS_SF;
        return xr_indef();
    }
    return X.st[p];
}

static void fpush(xr v)
{
    X.top = (X.top - 1) & 7;
    if (!X.empty[X.top]) { /* the stack is full */
        X.sw |= FS_IE | FS_SF | FS_C1;
        v = xr_indef();
    }
    X.st[X.top] = v;
    X.empty[X.top] = 0;
}

static inline void fpop(void)
{
    X.empty[X.top] = 1;
    X.top = (X.top + 1) & 7;
}

static void fcompare(xr a, xr b, int quiet_cmp) /* FCOM: C3 C2 C0 */
{
    int c = xr_cmp(a, b, quiet_cmp);

    X.sw &= ~(FS_C0 | FS_C1 | FS_C2 | FS_C3);
    if (c == 2)
        X.sw |= FS_C0 | FS_C2 | FS_C3;
    else if (c < 0)
        X.sw |= FS_C0;
    else if (c == 0)
        X.sw |= FS_C3;
}

static void fcomi(xr a, xr b, int quiet_cmp) /* FCOMI: ZF PF CF */
{
    int c = xr_cmp(a, b, quiet_cmp);

    X.sw &= ~FS_C1;
    X.fl &= ~(ZF | PF | CF | OF | SF | AF);
    if (c == 2)
        X.fl |= ZF | PF | CF;
    else if (c < 0)
        X.fl |= CF;
    else if (c == 0)
        X.fl |= ZF;
}

static u16 x87_tags(void)
{
    u16 t = 0;
    for (int p = 0; p < 8; p++) {
        int ex = X.st[p].se & 0x7fff;
        int v = X.empty[p] ? 3 : !ex && !X.st[p].m ? 1 : !ex || ex == 0x7fff || !(X.st[p].m >> 63) ? 2 : 0;
        t |= (u16)(v << (2 * p));
    }
    return t;
}

/* ES and B say that there is an exception that the control word does not mask */
static void x87_es(void)
{
    if (X.sw & ~X.cw & 0x3f)
        X.sw |= FS_ES | FS_B;
    else
        X.sw &= ~(FS_ES | FS_B);
}

static u16 x87_sw(void) { return (u16)((X.sw & ~0x3800u) | (u32)(X.top << 11)); }

static void x87_init(void)
{
    X.cw = 0x037f;
    X.sw = 0;
    X.top = 0;
    X.fip = X.fdp = X.fop = 0;
    for (int p = 0; p < 8; p++) {
        X.empty[p] = 1;
        X.st[p] = xr_zero(0);
    }
}

/* An exception that the program did not mask: the x87 gives it to the program at the next
   x87 instruction (or FWAIT). The codes are those of Windows NT. */
static void x87_wait(u32 insn)
{
    u32 f = X.sw & ~X.cw & 0x3f, code;

    if (!f)
        return;
    if (f & FS_IE)
        code = X.sw & FS_SF ? STATUS_FLOAT_STACK_CHECK : STATUS_FLOAT_INVALID_OPERATION;
    else if (f & FS_ZE)
        code = STATUS_FLOAT_DIVIDE_BY_ZERO;
    else if (f & FS_DE)
        code = STATUS_FLOAT_INVALID_OPERATION;
    else if (f & FS_OE)
        code = STATUS_FLOAT_OVERFLOW;
    else if (f & FS_UE)
        code = STATUS_FLOAT_UNDERFLOW;
    else
        code = STATUS_FLOAT_INEXACT_RESULT;
    cpu_fault(code, X.eip, insn);
}

/* FNSTENV (28 bytes) and FLDENV; FNSAVE and FRSTOR have the 8 registers after that */
static void x87_env_put(u32 a)
{
    w32(a, 0xffff0000u | X.cw);
    w32(a + 4, 0xffff0000u | x87_sw());
    w32(a + 8, 0xffff0000u | x87_tags());
    w32(a + 12, X.fip);
    w32(a + 16, (X.fip ? 0x1bu : 0) | (u32)(X.fop & 0x7ff) << 16);
    w32(a + 20, X.fdp);
    w32(a + 24, 0xffff0000u | (X.fdp ? 0x23u : 0));
}

static void x87_env_get(u32 a)
{
    u16 t = r16(a + 8);
    X.cw = r16(a);
    X.sw = r16(a + 4);
    X.top = (X.sw >> 11) & 7;
    X.fip = r32(a + 12);
    X.fop = (u16)(r32(a + 16) >> 16) & 0x7ff;
    X.fdp = r32(a + 20);
    for (int p = 0; p < 8; p++)
        X.empty[p] = ((t >> (2 * p)) & 3) == 3;
    x87_es();
}

static void x87_reg_put(int i, u32 a) /* ST(i) as 80 bits */
{
    int p = ST(i);
    w64(a, X.st[p].m);
    w16(a + 8, X.st[p].se);
}

static void x87_reg_get(int p, u32 a) /* the physical register p from 80 bits */
{
    X.st[p].m = r64(a);
    X.st[p].se = r16(a + 8);
}

/* + * compare compare-and-pop - reverse- / reverse/ : the 8 operations of D8, DA, DC and DE.
   The result is 1 when there is a value for the destination. */
static int x87_arith(int reg, xr a, xr b, xr *r)
{
    switch (reg) {
    case 0: *r = xr_addsub(a, b, 0); return 1;
    case 1: *r = xr_mul(a, b); return 1;
    case 2: fcompare(a, b, 0); return 0;
    case 3: fcompare(a, b, 0); fpop(); return 0;
    case 4: *r = xr_addsub(a, b, 1); return 1;
    case 5: *r = xr_addsub(b, a, 1); return 1;
    case 6: *r = xr_div(a, b); return 1;
    default: *r = xr_div(b, a); return 1;
    }
}

/* one x87 instruction: OP is the first byte (0xd8 to 0xdf), then the ModRM byte and its operand */
static void x87_do(u32 op, int mod, int reg, int rm, u32 ea)
{
    xr a, b, r, r2;
    int k = op - 0xd8, p;

    if (mod != 3) {
        /* memory operands */
        switch (k) {
        case 0: /* D8: m32real arithmetic */
        case 4: /* DC: m64real arithmetic */
        case 2: /* DA: m32int arithmetic */
        case 6: /* DE: m16int arithmetic */
            a = fget(0);
            b = k == 0 ? xr_from_f32(r32(ea)) : k == 4 ? xr_from_f64(r64(ea)) : k == 2 ? xr_from_int((s32)r32(ea)) : xr_from_int((s16)r16(ea));
            if (x87_arith(reg, a, b, &r))
                fset(0, r);
            return;
        case 1: /* D9 */
            switch (reg) {
            case 0: fpush(xr_from_f32(r32(ea))); return;
            case 2:
            case 3: w32(ea, xr_to_f32(fget(0))); if (reg == 3) fpop(); return;
            case 4: x87_env_get(ea); return;
            case 5: X.cw = r16(ea); x87_es(); return;
            case 6: x87_env_put(ea); return;
            case 7: w16(ea, X.cw); return;
            }
            break;
        case 3: /* DB */
            switch (reg) {
            case 0: fpush(xr_from_int((s32)r32(ea))); return;
            case 2:
            case 3: w32(ea, (u32)xr_to_int(fget(0), 32, fp_rc())); if (reg == 3) fpop(); return;
            case 5: r.m = r64(ea); r.se = r16(ea + 8); fpush(r); return;
            case 7: a = fget(0); w64(ea, a.m); w16(ea + 8, a.se); fpop(); return;
            }
            break;
        case 5: /* DD */
            switch (reg) {
            case 0: fpush(xr_from_f64(r64(ea))); return;
            case 2:
            case 3: w64(ea, xr_to_f64(fget(0))); if (reg == 3) fpop(); return;
            case 4: /* FRSTOR */
                x87_env_get(ea);
                for (int i = 0; i < 8; i++)
                    x87_reg_get(ST(i), ea + 28 + 10 * (u32)i);
                return;
            case 6: /* FNSAVE */
                x87_env_put(ea);
                for (int i = 0; i < 8; i++)
                    x87_reg_put(i, ea + 28 + 10 * (u32)i);
                x87_init();
                return;
            case 7: w16(ea, x87_sw()); return;
            }
            break;
        case 7: /* DF */
            switch (reg) {
            case 0: fpush(xr_from_int((s16)r16(ea))); return;
            case 2:
            case 3: w16(ea, (u16)xr_to_int(fget(0), 16, fp_rc())); if (reg == 3) fpop(); return;
            case 4: { /* FBLD: 18 decimal digits, 4 bits each, and a sign byte */
                u64 v = 0;
                for (int i = 8; i >= 0; i--) {
                    u8 dg = r8(ea + (u32)i);
                    v = v * 100 + (u64)(dg >> 4) * 10 + (dg & 15);
                }
                r = xr_from_int((s64)v);
                if (r8(ea + 9) & 0x80)
                    r.se |= 0x8000;
                fpush(r);
                return;
            }
            case 5: fpush(xr_from_int((s64)r64(ea))); return;
            case 6: { /* FBSTP */
                un u = unpack(fget(0));
                u16 fl = 0;
                int big = 1;
                u64 n = u.k <= K_NUM ? un_whole(u, fp_rc(), &big, &fl) : 0;
                if (big || n > 999999999999999999ull) { /* no such decimal number */
                    X.sw = (X.sw & ~FS_C1) | FS_IE;
                    w64(ea, 0xc000000000000000ull);
                    w16(ea + 8, 0xffff);
                } else {
                    X.sw = (X.sw & ~FS_C1) | fl;
                    for (int i = 0; i < 9; i++) {
                        w8(ea + (u32)i, (u8)((n % 10) | ((n / 10 % 10) << 4)));
                        n /= 100;
                    }
                    w8(ea + 9, u.s ? 0x80 : 0);
                }
                fpop();
                return;
            }
            case 7: w64(ea, (u64)xr_to_int(fget(0), 64, fp_rc())); fpop(); return;
            }
            break;
        }
        cpu_fault(STATUS_ILLEGAL_INSTRUCTION, X.eip, op << 8 | (u32)(mod << 6 | reg << 3 | rm));
    }

    /* register forms; rm is the number of ST(i) */
    switch (k) {
    case 0: /* D8: ST(0) = ST(0) op ST(i) */
        a = fget(0);
        b = fget(rm);
        if (x87_arith(reg, a, b, &r))
            fset(0, r);
        return;
    case 1: /* D9 */
        switch (reg) {
        case 0: a = fget(rm); fpush(a); return; /* FLD ST(i) */
        case 1: /* FXCH */
            a = fget(0);
            b = fget(rm);
            fset(0, b);
            fset(rm, a);
            X.sw &= ~FS_C1;
            return;
        case 2: if (rm == 0) return; break; /* FNOP */
        case 3: a = fget(0); fset(rm, a); fpop(); return; /* FSTP ST(i), another code for it */
        case 4:
            switch (rm) {
            case 0: /* FCHS */
            case 1: /* FABS */
                a = fget(0);
                a.se = rm ? a.se & 0x7fff : a.se ^ 0x8000;
                fset(0, a);
                X.sw &= ~FS_C1;
                return;
            case 4: fcompare(fget(0), xr_zero(0), 0); return; /* FTST */
            case 5: { /* FXAM */
                un u;
                p = X.top;
                a = X.st[p];
                u = unpack(a);
                X.sw &= ~(FS_C0 | FS_C1 | FS_C2 | FS_C3);
                if (a.se & 0x8000)
                    X.sw |= FS_C1;
                if (X.empty[p])
                    X.sw |= FS_C3 | FS_C0;
                else if (u.k == K_NAN)
                    X.sw |= FS_C0;
                else if (u.k == K_INF)
                    X.sw |= FS_C2 | FS_C0;
                else if (u.k == K_ZERO)
                    X.sw |= FS_C3;
                else if (u.k == K_NUM)
                    X.sw |= a.se & 0x7fff ? FS_C2 : FS_C3 | FS_C2;
                return;
            }
            }
            break;
        case 5: { /* the constants; the last bit goes with the rounding mode */
            int rc = fp_rc();
            switch (rm) {
            case 0: fpush(xr_one()); return;
            case 1: fpush(xr_make(0, 0x4000, rc == 2 ? 0xd49a784bcd1b8affull : 0xd49a784bcd1b8afeull)); return; /* log2(10) */
            case 2: fpush(xr_make(0, 0x3fff, rc & 1 ? 0xb8aa3b295c17f0bbull : 0xb8aa3b295c17f0bcull)); return; /* log2(e) */
            case 3: fpush(xr_make(0, 0x4000, rc & 1 ? 0xc90fdaa22168c234ull : 0xc90fdaa22168c235ull)); return; /* pi */
            case 4: fpush(xr_make(0, 0x3ffd, rc & 1 ? 0x9a209a84fbcff798ull : 0x9a209a84fbcff799ull)); return; /* log10(2) */
            case 5: fpush(xr_make(0, 0x3ffe, rc & 1 ? 0xb17217f7d1cf79abull : 0xb17217f7d1cf79acull)); return; /* ln(2) */
            case 6: fpush(xr_zero(0)); return;
            }
            break;
        }
        case 6:
            switch (rm) {
            case 0: fset(0, xr_2xm1(fget(0))); return; /* F2XM1 */
            case 1: a = fget(0); b = fget(1); r = xr_yl2x(b, a); fpop(); fset(0, r); return; /* FYL2X */
            case 2: /* FPTAN */
                a = fget(0);
                if (xr_trig(a, 2, &r, &r2)) {
                    fset(0, r);
                    fpush(r2);
                }
                return;
            case 3: a = fget(0); b = fget(1); r = xr_atan2(b, a); fpop(); fset(0, r); return; /* FPATAN */
            case 4: { /* FXTRACT: the exponent, then the number without it */
                un u;
                a = fget(0);
                u = operand(a);
                if (u.k == K_BAD) {
                    r = xr_invalid();
                    fset(0, r);
                    fpush(r);
                } else if (u.k == K_NAN) {
                    r = nan2(a, a);
                    fset(0, r);
                    fpush(r);
                } else if (u.k == K_INF) {
                    fset(0, xr_inf(0));
                    fpush(a);
                } else if (u.k == K_ZERO) {
                    X.sw |= FS_ZE;
                    fset(0, xr_inf(1));
                    fpush(a);
                } else {
                    fset(0, xr_from_int(u.e));
                    fpush(xr_make(u.s, 0x3fff, u.m));
                }
                return;
            }
            case 5: a = fget(0); b = fget(1); fset(0, xr_rem(a, b, 1)); return; /* FPREM1 */
            case 6: X.top = (X.top - 1) & 7; X.sw &= ~FS_C1; return;
            case 7: X.top = (X.top + 1) & 7; X.sw &= ~FS_C1; return;
            }
            break;
        case 7:
            switch (rm) {
            case 0: a = fget(0); b = fget(1); fset(0, xr_rem(a, b, 0)); return; /* FPREM */
            case 1: a = fget(0); b = fget(1); r = xr_yl2xp1(b, a); fpop(); fset(0, r); return; /* FYL2XP1 */
            case 2: fset(0, xr_sqrt(fget(0))); return;
            case 3: /* FSINCOS */
                a = fget(0);
                if (xr_trig(a, 0, &r, &r2)) {
                    fset(0, r);
                    fpush(r2);
                }
                return;
            case 4: fset(0, xr_rndint(fget(0))); return;
            case 5: a = fget(0); b = fget(1); fset(0, xr_scale(a, b)); return;
            case 6: /* FSIN */
            case 7: /* FCOS */
                a = fget(0);
                if (xr_trig(a, rm == 7, &r, &r2))
                    fset(0, r);
                return;
            }
            break;
        }
        break;
    case 2: /* DA: FCMOVcc, FUCOMPP */
        if (reg < 4) {
            static const u32 fl[4] = { CF, ZF, CF | ZF, PF };
            if (X.fl & fl[reg])
                fset(0, fget(rm));
            return;
        }
        if (reg == 5 && rm == 1) {
            a = fget(0);
            b = fget(1);
            fcompare(a, b, 1);
            fpop();
            fpop();
            return;
        }
        break;
    case 3: /* DB: FCMOVNcc, FNCLEX, FNINIT, FUCOMI, FCOMI */
        if (reg < 4) {
            static const u32 fl[4] = { CF, ZF, CF | ZF, PF };
            if (!(X.fl & fl[reg]))
                fset(0, fget(rm));
            return;
        }
        if (reg == 4) {
            if (rm == 2) { X.sw &= 0x7f00; return; }
            if (rm == 3) { x87_init(); return; }
            if (rm < 2 || rm == 4) return; /* FENI, FDISI, FSETPM: nothing */
            break;
        }
        if (reg == 5 || reg == 6) {
            a = fget(0);
            b = fget(rm);
            fcomi(a, b, reg == 5);
            return;
        }
        break;
    case 4: /* DC: ST(i) = ST(0) op ST(i) for - and /, in the other order than D8 */
        a = fget(0);
        b = fget(rm);
        if (x87_arith(reg, a, b, &r))
            fset(rm, r);
        return;
    case 5: /* DD */
        switch (reg) {
        case 0: X.empty[ST(rm)] = 1; return; /* FFREE */
        case 1: /* FXCH, another code for it */
            a = fget(0);
            b = fget(rm);
            fset(0, b);
            fset(rm, a);
            return;
        case 2:
        case 3: /* FST ST(i), FSTP ST(i) */
            a = fget(0);
            fset(rm, a);
            if (reg == 3)
                fpop();
            return;
        case 4:
        case 5: /* FUCOM, FUCOMP */
            a = fget(0);
            b = fget(rm);
            fcompare(a, b, 1);
            if (reg == 5)
                fpop();
            return;
        }
        break;
    case 6: /* DE: as DC, and pop */
        if (reg == 3 && rm != 1)
            break;
        a = fget(0);
        b = fget(rm);
        if (reg == 2 || reg == 3) { /* FCOMP, FCOMPP */
            fcompare(a, b, 0);
            if (reg == 3)
                fpop();
        } else if (x87_arith(reg, a, b, &r)) {
            fset(rm, r);
        }
        fpop();
        return;
    case 7: /* DF */
        if (reg == 4 && rm == 0) { /* FNSTSW AX */
            X.r[EAX] = (X.r[EAX] & 0xffff0000u) | x87_sw();
            return;
        }
        if (reg == 5 || reg == 6) {
            a = fget(0);
            b = fget(rm);
            fcomi(a, b, reg == 5);
            fpop();
            return;
        }
        if (reg == 0) { /* FFREEP */
            X.empty[ST(rm)] = 1;
            fpop();
            return;
        }
        if (reg == 2 || reg == 3) { /* FSTP ST(i), other codes for it */
            a = fget(0);
            fset(rm, a);
            fpop();
            return;
        }
        break;
    }
    cpu_fault(STATUS_ILLEGAL_INSTRUCTION, X.eip, op << 8 | (u32)(mod << 6 | reg << 3 | rm));
}

static void x87(u32 op, int mod, int reg, int rm, u32 ea)
{
    /* the instructions that do not look for an exception first: FNSTENV FNSTCW FNSAVE FNSTSW FNCLEX FNINIT */
    int k = op - 0xd8;
    int nowait = mod != 3 ? (k == 1 && reg >= 6) || (k == 5 && reg >= 6) : (k == 3 && reg == 4) || (k == 7 && reg == 4 && rm == 0);
    xr st[8];
    u8 empty[8];
    int top = X.top, guard;
    u16 before = X.sw;

    if (!nowait) {
        if (X.sw & FS_ES)
            x87_wait(op << 8 | (u32)(mod << 6 | reg << 3 | rm));
        X.fip = X.eip;
        X.fop = (u16)((k << 8) | (mod << 6 | reg << 3 | rm));
        X.fdp = mod != 3 ? ea : 0;
    }
    /* When the program did not mask "invalid" or "division by zero", an instruction with
       such an error (or a too small operand) does not change the registers. */
    x87_de = 0;
    guard = (X.cw & (FS_IE | FS_ZE | FS_DE)) != (FS_IE | FS_ZE | FS_DE);
    if (guard) {
        memcpy(st, X.st, sizeof st);
        memcpy(empty, X.empty, sizeof empty);
    }
    x87_do(op, mod, reg, rm, ea);
    if (x87_de)
        X.sw |= FS_DE;
    if (guard && (X.sw & ~before & ~X.cw & (FS_IE | FS_ZE | FS_DE)) && !nowait) {
        memcpy(X.st, st, sizeof st);
        memcpy(X.empty, empty, sizeof empty);
        X.top = top;
    }
    x87_es();
}

/* ---- the interpreter ---- */

#define MODRM() do { \
        u8 b_ = r8(eip++); \
        mod = b_ >> 6; \
        reg = (b_ >> 3) & 7; \
        rm = b_ & 7; \
        if (mod != 3) \
            ea = decode_ea(&eip, mod, rm) + seg; \
    } while (0)
#define RM8() (mod == 3 ? getr8(rm) : r8(ea))
#define WM8(x) do { u32 x_ = (x); if (mod == 3) setr8(rm, x_); else w8(ea, (u8)x_); } while (0)
#define RMV() (mod == 3 ? getrv(rm, osz) : osz == 32 ? r32(ea) : (u32)r16(ea))
#define WMV(x) do { u32 x_ = (x); if (mod == 3) setrv(rm, osz, x_); else if (osz == 32) w32(ea, x_); else w16(ea, (u16)x_); } while (0)
#define IMM8() ((u32)r8(eip++))
#define IMMS8() ((u32)(s32)(s8)r8(eip++))
#define IMMV() (osz == 32 ? (eip += 4, r32(eip - 4)) : (eip += 2, (u32)r16(eip - 2)))
#define SEXT(x, bits) ((bits) == 32 ? (s32)(x) : (bits) == 16 ? (s32)(s16)(x) : (s32)(s8)(x))

static void x86_run(void)
{
    for (;;) {
        u32 eip = X.eip, seg = 0, op, ea = 0, v, w;
        int osz = 32, rep = 0, mod = 0, reg = 0, rm = 0, sz, step;

        icount++;
        if (trace_insn)
            fprintf(stderr, "%08x: %02x %02x %02x %02x %02x %02x  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x fl=%03x\n",
                eip, r8(eip), r8(eip + 1), r8(eip + 2), r8(eip + 3), r8(eip + 4), r8(eip + 5), X.r[0], X.r[1], X.r[2], X.r[3],
                X.r[4], X.r[5], X.r[6], X.r[7], X.fl & 0xfff);
    again:
        op = r8(eip++);
        switch (op) {
        /* prefixes */
        case 0x66: osz = 16; goto again;
        case 0xf0: goto again; /* LOCK: one thread runs at a time */
        case 0xf2: rep = 2; goto again;
        case 0xf3: rep = 1; goto again;
        case 0x26: case 0x2e: case 0x36: case 0x3e: case 0x65: goto again; /* ES CS SS DS GS: flat memory */
        case 0x64: seg = X.fsbase; goto again;

        /* ADD OR ADC SBB AND SUB XOR CMP */
        case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
            MODRM();
            v = alu((int)op >> 3, RM8(), getr8(reg), 8);
            if ((op >> 3) != 7)
                WM8(v);
            break;
        case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31: case 0x39:
            MODRM();
            v = alu((int)op >> 3, RMV(), getrv(reg, osz), osz);
            if ((op >> 3) != 7)
                WMV(v);
            break;
        case 0x02: case 0x0a: case 0x12: case 0x1a: case 0x22: case 0x2a: case 0x32: case 0x3a:
            MODRM();
            v = alu((int)op >> 3, getr8(reg), RM8(), 8);
            if ((op >> 3) != 7)
                setr8(reg, v);
            break;
        case 0x03: case 0x0b: case 0x13: case 0x1b: case 0x23: case 0x2b: case 0x33: case 0x3b:
            MODRM();
            v = alu((int)op >> 3, getrv(reg, osz), RMV(), osz);
            if ((op >> 3) != 7)
                setrv(reg, osz, v);
            break;
        case 0x04: case 0x0c: case 0x14: case 0x1c: case 0x24: case 0x2c: case 0x34: case 0x3c:
            v = IMM8();
            v = alu((int)op >> 3, getr8(0), v, 8);
            if ((op >> 3) != 7)
                setr8(0, v);
            break;
        case 0x05: case 0x0d: case 0x15: case 0x1d: case 0x25: case 0x2d: case 0x35: case 0x3d:
            v = IMMV();
            v = alu((int)op >> 3, getrv(EAX, osz), v, osz);
            if ((op >> 3) != 7)
                setrv(EAX, osz, v);
            break;

        case 0x27: case 0x2f: { /* DAA, DAS */
            u32 al = getr8(0), cf = X.fl & CF, f = X.fl & ~(CF | AF);
            if ((al & 15) > 9 || (X.fl & AF)) {
                al = op == 0x27 ? al + 6 : al - 6;
                f |= AF;
            }
            if (getr8(0) > 0x99 || cf) {
                al = op == 0x27 ? al + 0x60 : al - 0x60;
                f |= CF;
            }
            X.fl = f;
            setr8(0, al);
            set_szp(al, 8);
            break;
        }
        case 0x37: case 0x3f: { /* AAA, AAS */
            u32 ax = X.r[EAX] & 0xffff;
            if ((ax & 15) > 9 || (X.fl & AF)) {
                ax = op == 0x37 ? ax + 0x106 : ax - 0x106;
                X.fl |= AF | CF;
            } else {
                X.fl &= ~(AF | CF);
            }
            X.r[EAX] = (X.r[EAX] & 0xffff0000u) | (ax & 0xff0f);
            break;
        }

        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
            setrv((int)op & 7, osz, inc_dec(getrv((int)op & 7, osz), 0, osz));
            break;
        case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
            setrv((int)op & 7, osz, inc_dec(getrv((int)op & 7, osz), 1, osz));
            break;
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
            if (osz == 32) {
                push32(X.r[op & 7]);
            } else {
                w16(X.r[ESP] - 2, (u16)X.r[op & 7]);
                X.r[ESP] -= 2;
            }
            break;
        case 0x58: case 0x59: case 0x5a: case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f:
            if (osz == 32) {
                v = r32(X.r[ESP]);
                X.r[ESP] += 4;
                X.r[op & 7] = v;
            } else {
                v = r16(X.r[ESP]);
                X.r[ESP] += 2;
                setrv((int)op & 7, 16, v);
            }
            break;
        case 0x60: /* PUSHAD */
            v = X.r[ESP];
            for (int i = 0; i < 8; i++)
                w32(v - 4 * (u32)(i + 1), i == ESP ? v : X.r[i]);
            X.r[ESP] = v - 32;
            break;
        case 0x61: /* POPAD */
            v = X.r[ESP];
            for (int i = 7; i >= 0; i--)
                if (i != ESP)
                    X.r[i] = r32(v + 4 * (u32)(7 - i));
            X.r[ESP] = v + 32;
            break;
        case 0x62: /* BOUND */
            MODRM();
            if (mod == 3)
                goto bad;
            if (osz == 32 ? ((s32)X.r[reg] < (s32)r32(ea) || (s32)X.r[reg] > (s32)r32(ea + 4)) :
                    ((s16)X.r[reg] < (s16)r16(ea) || (s16)X.r[reg] > (s16)r16(ea + 2)))
                cpu_fault(STATUS_ARRAY_BOUNDS_EXCEEDED, X.eip, op);
            break;
        case 0x68:
            v = IMMV();
            push32(v);
            break;
        case 0x6a:
            v = IMMS8();
            push32(v);
            break;
        case 0x69: case 0x6b: { /* IMUL Gv, Ev, imm */
            s64 t;
            MODRM();
            v = RMV();
            w = op == 0x69 ? IMMV() : IMMS8();
            t = (s64)SEXT(v, osz) * (s64)SEXT(w, osz);
            setrv(reg, osz, (u32)t);
            if (t != (s64)SEXT((u32)t, osz))
                X.fl |= CF | OF;
            else
                X.fl &= ~(CF | OF);
            break;
        }

        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7a: case 0x7b: case 0x7c: case 0x7d: case 0x7e: case 0x7f:
            v = IMMS8();
            if (cond((int)op & 15))
                eip += v;
            break;

        case 0x80: case 0x82:
            MODRM();
            w = RM8();
            v = IMM8();
            v = alu(reg, w, v, 8);
            if (reg != 7)
                WM8(v);
            break;
        case 0x81: case 0x83:
            MODRM();
            w = RMV();
            v = op == 0x81 ? IMMV() : IMMS8();
            v = alu(reg, w, v, osz);
            if (reg != 7)
                WMV(v);
            break;
        case 0x84:
            MODRM();
            alu(4, RM8(), getr8(reg), 8);
            break;
        case 0x85:
            MODRM();
            alu(4, RMV(), getrv(reg, osz), osz);
            break;
        case 0x86:
            MODRM();
            v = RM8();
            WM8(getr8(reg));
            setr8(reg, v);
            break;
        case 0x87:
            MODRM();
            v = RMV();
            WMV(getrv(reg, osz));
            setrv(reg, osz, v);
            break;
        case 0x88:
            MODRM();
            WM8(getr8(reg));
            break;
        case 0x89:
            MODRM();
            WMV(getrv(reg, osz));
            break;
        case 0x8a:
            MODRM();
            setr8(reg, RM8());
            break;
        case 0x8b:
            MODRM();
            setrv(reg, osz, RMV());
            break;
        case 0x8c: { /* MOV Ev, Sreg: the selectors that Windows NT has in user mode */
            static const u16 sel[8] = { 0x23, 0x1b, 0x23, 0x23, 0x3b, 0, 0, 0 };
            MODRM();
            if (mod == 3)
                setrv(rm, osz, sel[reg]);
            else
                w16(ea, sel[reg]);
            break;
        }
        case 0x8d:
            MODRM();
            if (mod == 3)
                goto bad;
            setrv(reg, osz, ea - seg);
            break;
        case 0x8e: /* MOV Sreg, Ew: the segments are all the same memory */
            MODRM();
            break;
        case 0x8f: /* POP Ev: the address is made with the stack pointer after the pop */
            v = r32(X.r[ESP]);
            X.r[ESP] += 4;
            MODRM();
            if (mod == 3) {
                X.r[rm] = v;
            } else {
                X.r[ESP] -= 4; /* a fault in the write leaves the stack as it was */
                w32(ea, v);
                X.r[ESP] += 4;
            }
            break;

        case 0x90:
            break;
        case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
            v = getrv(EAX, osz);
            setrv(EAX, osz, getrv((int)op & 7, osz));
            setrv((int)op & 7, osz, v);
            break;
        case 0x98:
            if (osz == 32)
                X.r[EAX] = (u32)(s32)(s16)X.r[EAX];
            else
                setrv(EAX, 16, (u32)(s32)(s8)X.r[EAX]);
            break;
        case 0x99:
            if (osz == 32)
                X.r[EDX] = (s32)X.r[EAX] < 0 ? 0xffffffffu : 0;
            else
                setrv(EDX, 16, (s16)X.r[EAX] < 0 ? 0xffff : 0);
            break;
        case 0x9b: /* FWAIT */
            x87_wait(op);
            break;
        case 0x9c:
            push32((X.fl & 0x00fcffffu) | 2);
            break;
        case 0x9d:
            v = pop32();
            X.fl = (X.fl & ~FLAGS_USER) | (v & FLAGS_USER) | 0x202;
            break;
        case 0x9e:
            X.fl = (X.fl & ~0xd5u) | (getr8(4) & 0xd5);
            break;
        case 0x9f:
            setr8(4, (X.fl & 0xd5) | 2);
            break;

        case 0xa0:
            v = r32(eip);
            eip += 4;
            setr8(0, r8(v + seg));
            break;
        case 0xa1:
            v = r32(eip);
            eip += 4;
            setrv(EAX, osz, osz == 32 ? r32(v + seg) : r16(v + seg));
            break;
        case 0xa2:
            v = r32(eip);
            eip += 4;
            w8(v + seg, (u8)X.r[EAX]);
            break;
        case 0xa3:
            v = r32(eip);
            eip += 4;
            if (osz == 32)
                w32(v + seg, X.r[EAX]);
            else
                w16(v + seg, (u16)X.r[EAX]);
            break;

        /* the string instructions; with REP they run ECX times */
        case 0xa4: case 0xa5: case 0xa6: case 0xa7: case 0xaa: case 0xab: case 0xac: case 0xad: case 0xae: case 0xaf:
            sz = (op & 1) ? osz / 8 : 1;
            step = (X.fl & DF) ? -sz : sz;
            while (!rep || X.r[ECX]) {
                switch (op & 0xfe) {
                case 0xa4: /* MOVS */
                    if (sz == 1) w8(X.r[EDI], r8(X.r[ESI] + seg));
                    else if (sz == 2) w16(X.r[EDI], r16(X.r[ESI] + seg));
                    else w32(X.r[EDI], r32(X.r[ESI] + seg));
                    X.r[ESI] += (u32)step;
                    X.r[EDI] += (u32)step;
                    break;
                case 0xa6: /* CMPS */
                    v = sz == 1 ? r8(X.r[ESI] + seg) : sz == 2 ? r16(X.r[ESI] + seg) : r32(X.r[ESI] + seg);
                    w = sz == 1 ? r8(X.r[EDI]) : sz == 2 ? r16(X.r[EDI]) : r32(X.r[EDI]);
                    alu(7, v, w, sz * 8);
                    X.r[ESI] += (u32)step;
                    X.r[EDI] += (u32)step;
                    break;
                case 0xaa: /* STOS */
                    if (sz == 1) w8(X.r[EDI], (u8)X.r[EAX]);
                    else if (sz == 2) w16(X.r[EDI], (u16)X.r[EAX]);
                    else w32(X.r[EDI], X.r[EAX]);
                    X.r[EDI] += (u32)step;
                    break;
                case 0xac: /* LODS */
                    if (sz == 1) setr8(0, r8(X.r[ESI] + seg));
                    else setrv(EAX, sz * 8, sz == 2 ? r16(X.r[ESI] + seg) : r32(X.r[ESI] + seg));
                    X.r[ESI] += (u32)step;
                    break;
                default: /* SCAS */
                    w = sz == 1 ? r8(X.r[EDI]) : sz == 2 ? r16(X.r[EDI]) : r32(X.r[EDI]);
                    alu(7, sz == 1 ? getr8(0) : getrv(EAX, sz * 8), w, sz * 8);
                    X.r[EDI] += (u32)step;
                    break;
                }
                if (!rep)
                    break;
                X.r[ECX]--;
                if ((op & 0xf6) == 0xa6 && ((rep == 1) != ((X.fl & ZF) != 0)))
                    break; /* REPE stops when not equal, REPNE when equal */
            }
            break;

        case 0xa8:
            v = IMM8();
            alu(4, getr8(0), v, 8);
            break;
        case 0xa9:
            v = IMMV();
            alu(4, getrv(EAX, osz), v, osz);
            break;
        case 0xb0: case 0xb1: case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6: case 0xb7:
            v = IMM8();
            setr8((int)op & 7, v);
            break;
        case 0xb8: case 0xb9: case 0xba: case 0xbb: case 0xbc: case 0xbd: case 0xbe: case 0xbf:
            v = IMMV();
            setrv((int)op & 7, osz, v);
            break;

        case 0xc0: case 0xd0: case 0xd2: /* shifts of a byte */
            MODRM();
            w = RM8();
            v = op == 0xc0 ? IMM8() : op == 0xd0 ? 1 : X.r[ECX] & 0xff;
            WM8(shift(reg, w, v, 8));
            break;
        case 0xc1: case 0xd1: case 0xd3:
            MODRM();
            w = RMV();
            v = op == 0xc1 ? IMM8() : op == 0xd1 ? 1 : X.r[ECX] & 0xff;
            WMV(shift(reg, w, v, osz));
            break;
        case 0xc2:
            v = r16(eip);
            eip = r32(X.r[ESP]);
            X.r[ESP] += 4 + v;
            break;
        case 0xc3:
            eip = r32(X.r[ESP]);
            X.r[ESP] += 4;
            break;
        case 0xc6:
            MODRM();
            v = IMM8();
            WM8(v);
            break;
        case 0xc7:
            MODRM();
            v = IMMV();
            WMV(v);
            break;
        case 0xc8: { /* ENTER size, level */
            u32 size = r16(eip), level = r8(eip + 2) & 31, sp = X.r[ESP], frame;
            eip += 3;
            w32(sp - 4, X.r[EBP]);
            sp -= 4;
            frame = sp;
            for (u32 i = 1; i < level; i++) {
                w32(sp - 4, r32(X.r[EBP] - 4 * i));
                sp -= 4;
            }
            if (level) {
                w32(sp - 4, frame);
                sp -= 4;
            }
            X.r[EBP] = frame;
            X.r[ESP] = sp - size;
            break;
        }
        case 0xc9: /* LEAVE */
            v = r32(X.r[EBP]);
            X.r[ESP] = X.r[EBP] + 4;
            X.r[EBP] = v;
            break;
        case 0xcc:
            cpu_fault(STATUS_BREAKPOINT, X.eip, op);
        case 0xcd:
            v = IMM8();
            cpu_fault(v == 3 ? STATUS_BREAKPOINT : STATUS_ACCESS_VIOLATION, X.eip, op << 8 | v);
        case 0xce:
            if (X.fl & OF)
                cpu_fault(STATUS_INTEGER_OVERFLOW, X.eip, op);
            break;

        case 0xd4: /* AAM */
            v = IMM8();
            if (!v)
                cpu_fault(STATUS_INTEGER_DIVIDE_BY_ZERO, X.eip, op);
            w = getr8(0);
            setr8(4, w / v);
            setr8(0, w % v);
            set_szp(w % v, 8);
            break;
        case 0xd5: /* AAD */
            v = IMM8();
            w = (getr8(0) + getr8(4) * v) & 0xff;
            setr8(0, w);
            setr8(4, 0);
            set_szp(w, 8);
            break;
        case 0xd6: /* SALC */
            setr8(0, X.fl & CF ? 0xff : 0);
            break;
        case 0xd7: /* XLAT */
            setr8(0, r8(X.r[EBX] + getr8(0) + seg));
            break;

        case 0xd8: case 0xd9: case 0xda: case 0xdb: case 0xdc: case 0xdd: case 0xde: case 0xdf:
            MODRM();
            x87(op, mod, reg, rm, ea);
            break;

        case 0xe0: case 0xe1: case 0xe2: /* LOOPNE, LOOPE, LOOP */
            v = IMMS8();
            X.r[ECX]--;
            if (X.r[ECX] && (op == 0xe2 || ((op == 0xe1) == ((X.fl & ZF) != 0))))
                eip += v;
            break;
        case 0xe3:
            v = IMMS8();
            if (!X.r[ECX])
                eip += v;
            break;
        case 0xe8:
            v = r32(eip);
            eip += 4;
            push32(eip);
            eip += v;
            break;
        case 0xe9:
            v = r32(eip);
            eip += 4 + v;
            break;
        case 0xeb:
            v = IMMS8();
            eip += v;
            break;

        case 0xf4: /* HLT: the code of a thunk of a Windows function */
            if (IN_THUNKS(X.eip)) {
                u32 idx = (X.eip - THUNK_BASE) / THUNK_SLOT, a[16], sp, ret;
                int nb;
                if (idx == API_RETURN)
                    return;
                guest_level_check(); /* it does not come back when this call level was left behind */
                sp = X.r[ESP];
                ret = r32(sp);
                for (int i = 0; i < 16; i++)
                    a[i] = r32(sp + 4 + 4 * (u32)i);
                nb = api_argbytes(idx);
                /* the function sees the CPU as it is after its return: it took its arguments off the stack */
                X.eip = ret;
                X.r[ESP] = sp + 4 + (u32)(nb > 0 ? nb : 0);
                X.r[EAX] = api_dispatch(idx, a);
                continue;
            }
            cpu_fault(STATUS_PRIVILEGED_INSTRUCTION, X.eip, op);
        case 0xf5:
            X.fl ^= CF;
            break;
        case 0xf6: /* TEST NOT NEG MUL IMUL DIV IDIV on a byte */
            MODRM();
            w = RM8();
            switch (reg) {
            case 0: case 1:
                v = IMM8();
                alu(4, w, v, 8);
                break;
            case 2:
                WM8(~w);
                break;
            case 3:
                WM8(alu(5, 0, w, 8));
                break;
            case 4:
                v = getr8(0) * w;
                setrv(EAX, 16, v);
                X.fl = v & 0xff00 ? X.fl | CF | OF : X.fl & ~(CF | OF);
                break;
            case 5:
                v = (u32)((s32)(s8)getr8(0) * (s32)(s8)w);
                setrv(EAX, 16, v);
                X.fl = (s32)v != (s32)(s8)v ? X.fl | CF | OF : X.fl & ~(CF | OF);
                break;
            case 6: {
                u32 ax = X.r[EAX] & 0xffff;
                if (!w)
                    cpu_fault(STATUS_INTEGER_DIVIDE_BY_ZERO, X.eip, op);
                if (ax / w > 0xff)
                    cpu_fault(STATUS_INTEGER_OVERFLOW, X.eip, op);
                setr8(0, ax / w);
                setr8(4, ax % w);
                break;
            }
            default: {
                s32 ax = (s16)X.r[EAX], d = (s8)w, q;
                if (!d)
                    cpu_fault(STATUS_INTEGER_DIVIDE_BY_ZERO, X.eip, op);
                q = ax / d;
                if (q > 127 || q < -128)
                    cpu_fault(STATUS_INTEGER_OVERFLOW, X.eip, op);
                setr8(0, (u32)q);
                setr8(4, (u32)(ax % d));
                break;
            }
            }
            break;
        case 0xf7: /* the same on 16 or 32 bits */
            MODRM();
            w = RMV();
            switch (reg) {
            case 0: case 1:
                v = IMMV();
                alu(4, w, v, osz);
                break;
            case 2:
                WMV(~w);
                break;
            case 3:
                WMV(alu(5, 0, w, osz));
                break;
            case 4:
                if (osz == 32) {
                    u64 t = (u64)X.r[EAX] * w;
                    X.r[EAX] = (u32)t;
                    X.r[EDX] = (u32)(t >> 32);
                    X.fl = X.r[EDX] ? X.fl | CF | OF : X.fl & ~(CF | OF);
                } else {
                    v = (X.r[EAX] & 0xffff) * w;
                    setrv(EAX, 16, v);
                    setrv(EDX, 16, v >> 16);
                    X.fl = v >> 16 ? X.fl | CF | OF : X.fl & ~(CF | OF);
                }
                break;
            case 5:
                if (osz == 32) {
                    s64 t = (s64)(s32)X.r[EAX] * (s64)(s32)w;
                    X.r[EAX] = (u32)t;
                    X.r[EDX] = (u32)((u64)t >> 32);
                    X.fl = t != (s64)(s32)t ? X.fl | CF | OF : X.fl & ~(CF | OF);
                } else {
                    s32 t = (s32)(s16)X.r[EAX] * (s32)(s16)w;
                    setrv(EAX, 16, (u32)t);
                    setrv(EDX, 16, (u32)t >> 16);
                    X.fl = t != (s32)(s16)t ? X.fl | CF | OF : X.fl & ~(CF | OF);
                }
                break;
            case 6:
                if (!w)
                    cpu_fault(STATUS_INTEGER_DIVIDE_BY_ZERO, X.eip, op);
                if (osz == 32) {
                    u64 n = ((u64)X.r[EDX] << 32) | X.r[EAX], q = n / w;
                    if (q > 0xffffffffull)
                        cpu_fault(STATUS_INTEGER_OVERFLOW, X.eip, op);
                    X.r[EAX] = (u32)q;
                    X.r[EDX] = (u32)(n % w);
                } else {
                    u32 n = ((X.r[EDX] & 0xffff) << 16) | (X.r[EAX] & 0xffff), q = n / w;
                    if (q > 0xffff)
                        cpu_fault(STATUS_INTEGER_OVERFLOW, X.eip, op);
                    setrv(EAX, 16, q);
                    setrv(EDX, 16, n % w);
                }
                break;
            default:
                if (!w)
                    cpu_fault(STATUS_INTEGER_DIVIDE_BY_ZERO, X.eip, op);
                if (osz == 32) {
                    s64 n = (s64)(((u64)X.r[EDX] << 32) | X.r[EAX]), d = (s32)w, q;
                    if (n == INT64_MIN && d == -1)
                        cpu_fault(STATUS_INTEGER_OVERFLOW, X.eip, op);
                    q = n / d;
                    if (q > 0x7fffffffll || q < -0x80000000ll)
                        cpu_fault(STATUS_INTEGER_OVERFLOW, X.eip, op);
                    X.r[EAX] = (u32)q;
                    X.r[EDX] = (u32)(n % d);
                } else {
                    s32 n = (s32)(((X.r[EDX] & 0xffff) << 16) | (X.r[EAX] & 0xffff)), d = (s16)w, q;
                    q = n / d;
                    if (q > 32767 || q < -32768)
                        cpu_fault(STATUS_INTEGER_OVERFLOW, X.eip, op);
                    setrv(EAX, 16, (u32)q);
                    setrv(EDX, 16, (u32)(n % d));
                }
                break;
            }
            break;
        case 0xf8:
            X.fl &= ~CF;
            break;
        case 0xf9:
            X.fl |= CF;
            break;
        case 0xfc:
            X.fl &= ~DF;
            break;
        case 0xfd:
            X.fl |= DF;
            break;
        case 0xfe:
            MODRM();
            if (reg > 1)
                goto bad;
            WM8(inc_dec(RM8(), reg, 8));
            break;
        case 0xff:
            MODRM();
            switch (reg) {
            case 0: case 1:
                WMV(inc_dec(RMV(), reg, osz));
                break;
            case 2: /* CALL Ev */
                v = RMV();
                push32(eip);
                eip = v;
                break;
            case 4: /* JMP Ev */
                eip = RMV();
                break;
            case 6:
                v = RMV();
                push32(v);
                break;
            default:
                goto bad;
            }
            break;

        case 0x0f:
            op = r8(eip++);
            switch (op) {
            case 0x31: /* RDTSC */
                X.r[EAX] = (u32)icount;
                X.r[EDX] = (u32)(icount >> 32);
                break;
            case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
            case 0x48: case 0x49: case 0x4a: case 0x4b: case 0x4c: case 0x4d: case 0x4e: case 0x4f:
                MODRM();
                v = RMV();
                if (cond((int)op & 15))
                    setrv(reg, osz, v);
                break;
            case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
            case 0x88: case 0x89: case 0x8a: case 0x8b: case 0x8c: case 0x8d: case 0x8e: case 0x8f:
                v = r32(eip);
                eip += 4;
                if (cond((int)op & 15))
                    eip += v;
                break;
            case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
            case 0x98: case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e: case 0x9f:
                MODRM();
                WM8((u32)cond((int)op & 15));
                break;
            case 0xa0: /* PUSH FS */
                push32(0x3b);
                break;
            case 0xa8: /* PUSH GS */
                push32(0);
                break;
            case 0xa1: case 0xa9: /* POP FS, POP GS */
                X.r[ESP] += 4;
                break;
            case 0xa2: /* CPUID: an 80486 */
                if (X.r[EAX] == 0) {
                    X.r[EAX] = 1;
                    X.r[EBX] = 0x756e6547; /* "GenuineIntel" */
                    X.r[EDX] = 0x49656e69;
                    X.r[ECX] = 0x6c65746e;
                } else {
                    X.r[EAX] = 0x0480;
                    X.r[EBX] = X.r[ECX] = 0;
                    X.r[EDX] = 1; /* it has an x87 */
                }
                break;
            case 0xa3: case 0xab: case 0xb3: case 0xbb: case 0xba: { /* BT BTS BTR BTC */
                u32 bit, mask, kind;
                MODRM();
                if (op == 0xba) {
                    if (reg < 4)
                        goto bad;
                    bit = IMM8() & (u32)(osz - 1);
                    kind = (u32)reg & 3;
                } else {
                    bit = getrv(reg, osz);
                    kind = (op >> 3) & 3;
                    if (mod != 3) /* a bit of a string of bits: it can be outside the operand */
                        ea += (u32)(SEXT(bit, osz) >> 3) & ~(u32)(osz / 8 - 1);
                    bit &= (u32)(osz - 1);
                }
                mask = 1u << bit;
                v = RMV();
                X.fl = v & mask ? X.fl | CF : X.fl & ~CF;
                if (kind == 1)
                    WMV(v | mask);
                else if (kind == 2)
                    WMV(v & ~mask);
                else if (kind == 3)
                    WMV(v ^ mask);
                break;
            }
            case 0xa4: case 0xa5: case 0xac: case 0xad: { /* SHLD, SHRD */
                u32 n, cf, r, m = osz == 32 ? 0xffffffffu : 0xffff;
                MODRM();
                w = RMV();
                n = ((op & 1) ? X.r[ECX] : IMM8()) & 31;
                v = getrv(reg, osz);
                if (!n || n > (u32)osz)
                    break;
                if (op < 0xa8) {
                    cf = (w >> ((u32)osz - n)) & 1;
                    r = n == (u32)osz ? v : ((w << n) | (v >> ((u32)osz - n))) & m;
                } else {
                    cf = (w >> (n - 1)) & 1;
                    r = n == (u32)osz ? v : ((w >> n) | (v << ((u32)osz - n))) & m;
                }
                X.fl = (X.fl & ~(CF | OF)) | cf | (((r ^ w) >> (osz - 1)) & 1 ? OF : 0);
                set_szp(r, osz);
                WMV(r);
                break;
            }
            case 0xaf: { /* IMUL Gv, Ev */
                s64 t;
                MODRM();
                v = RMV();
                t = (s64)SEXT(getrv(reg, osz), osz) * (s64)SEXT(v, osz);
                setrv(reg, osz, (u32)t);
                if (t != (s64)SEXT((u32)t, osz))
                    X.fl |= CF | OF;
                else
                    X.fl &= ~(CF | OF);
                break;
            }
            case 0xb0: /* CMPXCHG Eb, Gb */
                MODRM();
                w = RM8();
                alu(7, getr8(0), w, 8);
                if (X.fl & ZF)
                    WM8(getr8(reg));
                else
                    setr8(0, w);
                break;
            case 0xb1:
                MODRM();
                w = RMV();
                alu(7, getrv(EAX, osz), w, osz);
                if (X.fl & ZF)
                    WMV(getrv(reg, osz));
                else
                    setrv(EAX, osz, w);
                break;
            case 0xb6:
                MODRM();
                setrv(reg, osz, RM8());
                break;
            case 0xb7:
                MODRM();
                X.r[reg] = mod == 3 ? X.r[rm] & 0xffff : r16(ea);
                break;
            case 0xbe:
                MODRM();
                setrv(reg, osz, (u32)(s32)(s8)RM8());
                break;
            case 0xbf:
                MODRM();
                X.r[reg] = (u32)(s32)(s16)(mod == 3 ? X.r[rm] : r16(ea));
                break;
            case 0xbc: case 0xbd: /* BSF, BSR */
                MODRM();
                v = RMV();
                if (!v) {
                    X.fl |= ZF;
                } else {
                    X.fl &= ~ZF;
                    setrv(reg, osz, op == 0xbc ? (u32)__builtin_ctz(v) : 31u - (u32)__builtin_clz(v));
                }
                break;
            case 0xc0: /* XADD Eb, Gb */
                MODRM();
                w = RM8();
                v = alu(0, w, getr8(reg), 8);
                setr8(reg, w);
                WM8(v);
                break;
            case 0xc1:
                MODRM();
                w = RMV();
                v = alu(0, w, getrv(reg, osz), osz);
                setrv(reg, osz, w);
                WMV(v);
                break;
            case 0xc7: { /* CMPXCHG8B */
                u64 m, d = ((u64)X.r[EDX] << 32) | X.r[EAX];
                MODRM();
                if (mod == 3 || reg != 1)
                    goto bad;
                m = r64(ea);
                if (m == d) {
                    w64(ea, ((u64)X.r[ECX] << 32) | X.r[EBX]);
                    X.fl |= ZF;
                } else {
                    X.r[EAX] = (u32)m;
                    X.r[EDX] = (u32)(m >> 32);
                    X.fl &= ~ZF;
                }
                break;
            }
            case 0xc8: case 0xc9: case 0xca: case 0xcb: case 0xcc: case 0xcd: case 0xce: case 0xcf:
                X.r[op & 7] = __builtin_bswap32(X.r[op & 7]);
                break;
            default:
                op |= 0x0f00;
                goto bad;
            }
            break;

        /* IN, OUT, INS, OUTS, CLI, STI: not for a program */
        case 0x6c: case 0x6d: case 0x6e: case 0x6f: case 0xe4: case 0xe5: case 0xe6: case 0xe7:
        case 0xec: case 0xed: case 0xee: case 0xef: case 0xfa: case 0xfb:
            cpu_fault(STATUS_PRIVILEGED_INSTRUCTION, X.eip, op);
        default:
        bad:
            if (getenv("NTRUN_DUMP"))
                fprintf(stderr, "ntrun: x86 instruction %02x at %08x is not known\n", op, X.eip);
            cpu_fault(STATUS_ILLEGAL_INSTRUCTION, X.eip, op);
        }
        X.eip = eip;
    }
}

/* ---- the interface to the core ---- */

static void x86_init(u32 sp, u32 teb, u32 gp)
{
    (void)gp;
    for (int i = 0; i < 256; i++)
        parity[i] = !(__builtin_popcount((unsigned)i) & 1);
    memset(&X, 0, sizeof X);
    X.r[ESP] = (sp - 64) & ~3u;
    X.fl = 0x202;
    X.fsbase = teb;
    X.eip = THUNK_BASE;
    w32(teb, 0xffffffff); /* the list of exception handlers is empty */
    x87_init();
    X.cw = 0x027f; /* as Windows starts a program: 53-bit precision */
}

static void x86_thunk(u32 addr) { w8(addr, 0xf4); }
static u32 x86_thunk_ptr(u32 addr) { return addr; }

/* a call from the host: the arguments and the return address go on the stack */
static void x86_call_begin(u32 fn, int n, const u32 *args, u64 *save)
{
    u32 sp = X.r[ESP];

    save[0] = sp;
    save[1] = X.eip;
    for (int k = n - 1; k >= 0; k--) {
        sp -= 4;
        w32(sp, args[k]);
    }
    sp -= 4;
    w32(sp, THUNK_BASE);
    X.r[ESP] = sp;
    X.eip = fn;
}

static u32 x86_call_end(const u64 *save)
{
    X.r[ESP] = (u32)save[0];
    X.eip = (u32)save[1];
    return X.r[EAX];
}

static u32 x86_pc(void) { return X.eip; }
static u32 x86_sp(void) { return X.r[ESP]; }
static void x86_set_sp(u32 sp) { X.r[ESP] = sp; }

/* the CONTEXT of Windows NT for x86: 0xcc bytes */
#define XC_FLOAT 0x1c
#define XC_SEGGS 0x8c
#define XC_EDI 0x9c
#define XC_EAX 0xb0
#define XC_EBP 0xb4
#define XC_EIP 0xb8
#define XC_EFLAGS 0xc0
#define XC_ESP 0xc4
#define XC_SIZE 0xcc

static void x86_capture(u32 c, u32 fir)
{
    memset(G + c, 0, XC_SIZE);
    w32(c, 0x0001000f); /* control, integer, segments, floating point */
    x87_env_put(c + XC_FLOAT);
    for (int i = 0; i < 8; i++)
        x87_reg_put(i, c + XC_FLOAT + 28 + 10 * (u32)i);
    w32(c + XC_SEGGS, 0);
    w32(c + XC_SEGGS + 4, 0x3b);
    w32(c + XC_SEGGS + 8, 0x23);
    w32(c + XC_SEGGS + 12, 0x23);
    w32(c + XC_EDI, X.r[EDI]);
    w32(c + XC_EDI + 4, X.r[ESI]);
    w32(c + XC_EDI + 8, X.r[EBX]);
    w32(c + XC_EDI + 12, X.r[EDX]);
    w32(c + XC_EDI + 16, X.r[ECX]);
    w32(c + XC_EAX, X.r[EAX]);
    w32(c + XC_EBP, X.r[EBP]);
    w32(c + XC_EIP, fir);
    w32(c + XC_EIP + 4, 0x1b);
    w32(c + XC_EFLAGS, X.fl);
    w32(c + XC_ESP, X.r[ESP]);
    w32(c + XC_ESP + 4, 0x23);
}

static void x86_restore(u32 c)
{
    x87_env_get(c + XC_FLOAT);
    for (int i = 0; i < 8; i++)
        x87_reg_get(ST(i), c + XC_FLOAT + 28 + 10 * (u32)i);
    X.r[EDI] = r32(c + XC_EDI);
    X.r[ESI] = r32(c + XC_EDI + 4);
    X.r[EBX] = r32(c + XC_EDI + 8);
    X.r[EDX] = r32(c + XC_EDI + 12);
    X.r[ECX] = r32(c + XC_EDI + 16);
    X.r[EAX] = r32(c + XC_EAX);
    X.r[EBP] = r32(c + XC_EBP);
    X.eip = r32(c + XC_EIP);
    X.fl = (X.fl & ~FLAGS_USER) | (r32(c + XC_EFLAGS) & FLAGS_USER) | 0x202;
    X.r[ESP] = r32(c + XC_ESP);
}

static u32 x86_cget(u32 c, int what)
{
    switch (what) {
    case C_SP: return r32(c + XC_ESP);
    case C_FIR: return r32(c + XC_EIP);
    case C_RETVAL: return r32(c + XC_EAX);
    }
    return 0;
}

static void x86_cset(u32 c, int what, u32 v)
{
    switch (what) {
    case C_SP: w32(c + XC_ESP, v); break;
    case C_FIR: w32(c + XC_EIP, v); break;
    case C_RETVAL: w32(c + XC_EAX, v); break;
    }
}

static void x86_state_save(void *p) { memcpy(p, &X, sizeof X); }

static void x86_state_load(const void *p, u32 teb)
{
    (void)teb; /* it is in the state */
    memcpy(&X, p, sizeof X);
}

const struct cpu cpu_x86 = {
    "x86", 4096, 486, 0,
    x86_init, x86_thunk, x86_thunk_ptr, x86_run, x86_call_begin, x86_call_end,
    x86_pc, x86_sp, x86_pc, x86_set_sp, x86_dump,
    XC_SIZE, 3, 0, 0,
    x86_capture, x86_restore, 0, 0, 0, x86_cget, x86_cset,
    0, 0, 0,
    sizeof X, x86_state_save, x86_state_load,
    1,
};
