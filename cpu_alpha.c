/* cpu_alpha.c: Alpha AXP user mode (EV4 with the byte/word extension), as Windows NT uses it:
   64-bit registers, 32-bit addresses kept sign-extended. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <fenv.h>
#include <setjmp.h>
#include "ntrun.h"

static struct {
    u64 r[32];
    u64 f[32];
    u64 fpcr;
    u32 pc;
    u32 teb;
    u32 lock;
} A;

#define V0 0
#define FP 15
#define A0 16
#define RA 26
#define PV 27
#define GP 29
#define SP 30

static int host_rm = 2;

static void setrm(int m) /* 0 chopped, 1 minus, 2 nearest, 3 plus */
{
    static const int mode[4] = { FE_TOWARDZERO, FE_DOWNWARD, FE_TONEAREST, FE_UPWARD };
    if (m != host_rm) {
        fesetround(mode[m]);
        host_rm = m;
    }
}

static inline double f2d(u64 v) { double d; memcpy(&d, &v, 8); return d; }
static inline u64 d2f(double d) { u64 v; memcpy(&v, &d, 8); return v; }

/* a single in memory <-> its register form */
static u64 s_to_reg(u32 s)
{
    u64 sign = (u64)(s >> 31) << 63, exp = (s >> 23) & 0xff, frac = s & 0x7fffff;
    if (exp == 0xff)
        exp = 0x7ff;
    else if (exp)
        exp += 0x3ff - 0x7f;
    return sign | (exp << 52) | (frac << 29);
}

static u32 reg_to_s(u64 r) { return (u32)(((r >> 32) & 0xc0000000u) | ((r >> 29) & 0x3fffffffu)); }

/* a call_pal that is not a service of ours: bpt, or gentrap with its code in a0 */
static u32 alpha_pal_code(u32 fn, s32 a0)
{
    if (fn == 0x80 || fn == 0xac)
        return STATUS_BREAKPOINT;
    if (fn == 0xaa) {
        switch (a0) {
        case -1: return STATUS_INTEGER_OVERFLOW;
        case -2: return STATUS_INTEGER_DIVIDE_BY_ZERO;
        case -3: return 0xc0000091; /* floating overflow */
        case -4: return 0xc000008e; /* floating divide by zero */
        case -5: return 0xc0000093; /* floating underflow */
        case -6: return 0xc0000090; /* floating invalid operand */
        case -7: return 0xc000008f; /* floating inexact result */
        }
        return 0xc000008c; /* array bounds exceeded and the rest */
    }
    return STATUS_ILLEGAL_INSTRUCTION;
}

static void alpha_dump(void)
{
    static const char *n[32] = { "v0", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4",
        "s5", "fp", "a0", "a1", "a2", "a3", "a4", "a5", "t8", "t9", "t10", "t11", "ra", "pv", "at", "gp", "sp", "zr" };
    fprintf(stderr, "pc=%08x fpcr=%016llx\n", A.pc, (unsigned long long)A.fpcr);
    for (int i = 0; i < 32; i++)
        fprintf(stderr, "%3s=%016llx%s", n[i], (unsigned long long)A.r[i], (i & 3) == 3 ? "\n" : " ");
}

static inline u64 bytemask(int size) { return size == 8 ? ~0ull : (1ull << (size * 8)) - 1; }

static u64 zap(u64 v, u32 m)
{
    for (int k = 0; k < 8; k++)
        if (m & (1u << k))
            v &= ~(0xffull << (k * 8));
    return v;
}

static void alpha_run(void)
{
    u64 *r = A.r;
    u32 pc = A.pc;

    for (;;) {
        u32 i = r32(pc);
        u32 op = i >> 26, ra = (i >> 21) & 31, rb = (i >> 16) & 31, rc = i & 31;
        s32 disp = (s16)i;
        u32 ea;
        u64 av, bv;

        icount++;
        A.pc = pc;
        if (trace_insn)
            fprintf(stderr, "%08x: %08x\n", pc, i);
        pc += 4;
#define EA() (ea = (u32)(r[rb] + (u64)(s64)disp))
#define BRANCH() (pc = pc + (u32)((s32)(i << 11) >> 9))
        switch (op) {
        case 0x00: /* call_pal */
            if (A.pc >= THUNK_BASE && A.pc < THUNK_BASE + THUNK_MAX * THUNK_SLOT) {
                u32 idx = (A.pc - THUNK_BASE) / THUNK_SLOT, a[16], ret = (u32)r[RA], sp = (u32)r[SP];
                if (idx == API_RETURN)
                    return;
                for (int k = 0; k < 6; k++) a[k] = (u32)r[A0 + k];
                for (int k = 6; k < 16; k++) a[k] = r32(sp + 8 * (u32)(k - 6));
                A.pc = ret;
                r[V0] = (u64)(s64)(s32)api_dispatch(idx, a);
                pc = ret;
                break;
            }
            switch (i & 0x3ffffff) {
            case 0x86: break;                                 /* imb */
            case 0xab: r[V0] = (u64)(s64)(s32)A.teb; break;  /* rdteb */
            default: cpu_fault(alpha_pal_code(i & 0x3ffffff, (s32)r[A0]), A.pc, i);
            }
            break;
        case 0x08: r[ra] = r[rb] + (u64)(s64)disp; break;
        case 0x09: r[ra] = r[rb] + (u64)((s64)disp * 65536); break;
        case 0x0a: EA(); r[ra] = r8(ea); break;
        case 0x0b: EA(); r[ra] = r64(ea & ~7u); break;
        case 0x0c: EA(); r[ra] = r16(ea); break;
        case 0x0d: EA(); w16(ea, (u16)r[ra]); break;
        case 0x0e: EA(); w8(ea, (u8)r[ra]); break;
        case 0x0f: EA(); w64(ea & ~7u, r[ra]); break;
        case 0x10:
        case 0x11:
        case 0x12:
        case 0x13:
            av = r[ra];
            bv = (i & 0x1000) ? ((i >> 13) & 0xff) : r[rb];
            switch (((op & 3) << 8) | ((i >> 5) & 0x7f)) {
            /* 0x10 */
            case 0x000: case 0x040: r[rc] = (u64)(s64)(s32)(av + bv); break;
            case 0x002: r[rc] = (u64)(s64)(s32)(av * 4 + bv); break;
            case 0x009: case 0x049: r[rc] = (u64)(s64)(s32)(av - bv); break;
            case 0x00b: r[rc] = (u64)(s64)(s32)(av * 4 - bv); break;
            case 0x00f: {
                u64 m = 0;
                for (int k = 0; k < 8; k++)
                    if (((av >> (k * 8)) & 0xff) >= ((bv >> (k * 8)) & 0xff))
                        m |= 1u << k;
                r[rc] = m;
                break;
            }
            case 0x012: r[rc] = (u64)(s64)(s32)(av * 8 + bv); break;
            case 0x01b: r[rc] = (u64)(s64)(s32)(av * 8 - bv); break;
            case 0x01d: r[rc] = av < bv; break;
            case 0x020: case 0x060: r[rc] = av + bv; break;
            case 0x022: r[rc] = av * 4 + bv; break;
            case 0x029: case 0x069: r[rc] = av - bv; break;
            case 0x02b: r[rc] = av * 4 - bv; break;
            case 0x02d: r[rc] = av == bv; break;
            case 0x032: r[rc] = av * 8 + bv; break;
            case 0x03b: r[rc] = av * 8 - bv; break;
            case 0x03d: r[rc] = av <= bv; break;
            case 0x04d: r[rc] = (s64)av < (s64)bv; break;
            case 0x06d: r[rc] = (s64)av <= (s64)bv; break;
            /* 0x11 */
            case 0x100: r[rc] = av & bv; break;
            case 0x108: r[rc] = av & ~bv; break;
            case 0x114: if (av & 1) r[rc] = bv; break;
            case 0x116: if (!(av & 1)) r[rc] = bv; break;
            case 0x120: r[rc] = av | bv; break;
            case 0x124: if (av == 0) r[rc] = bv; break;
            case 0x126: if (av != 0) r[rc] = bv; break;
            case 0x128: r[rc] = av | ~bv; break;
            case 0x140: r[rc] = av ^ bv; break;
            case 0x144: if ((s64)av < 0) r[rc] = bv; break;
            case 0x146: if ((s64)av >= 0) r[rc] = bv; break;
            case 0x148: r[rc] = av ^ ~bv; break;
            case 0x161: r[rc] = bv & ~1ull; break; /* amask: we have the byte/word extension */
            case 0x164: if ((s64)av <= 0) r[rc] = bv; break;
            case 0x166: if ((s64)av > 0) r[rc] = bv; break;
            case 0x16c: r[rc] = 0; break; /* implver: EV4 */
            /* 0x12 */
            case 0x202: r[rc] = av & ~(bytemask(1) << ((bv & 7) * 8)); break;
            case 0x206: r[rc] = (av >> ((bv & 7) * 8)) & bytemask(1); break;
            case 0x20b: r[rc] = (av & bytemask(1)) << ((bv & 7) * 8); break;
            case 0x212: r[rc] = av & ~(bytemask(2) << ((bv & 7) * 8)); break;
            case 0x216: r[rc] = (av >> ((bv & 7) * 8)) & bytemask(2); break;
            case 0x21b: r[rc] = (av & bytemask(2)) << ((bv & 7) * 8); break;
            case 0x222: r[rc] = av & ~(bytemask(4) << ((bv & 7) * 8)); break;
            case 0x226: r[rc] = (av >> ((bv & 7) * 8)) & bytemask(4); break;
            case 0x22b: r[rc] = (av & bytemask(4)) << ((bv & 7) * 8); break;
            case 0x230: r[rc] = zap(av, (u32)bv & 0xff); break;
            case 0x231: r[rc] = zap(av, ~(u32)bv & 0xff); break;
            case 0x232: r[rc] = av & ~(bytemask(8) << ((bv & 7) * 8)); break;
            case 0x234: r[rc] = av >> (bv & 63); break;
            case 0x236: r[rc] = av >> ((bv & 7) * 8); break;
            case 0x239: r[rc] = av << (bv & 63); break;
            case 0x23b: r[rc] = av << ((bv & 7) * 8); break;
            case 0x23c: r[rc] = (u64)((s64)av >> (bv & 63)); break;
#define HSH ((64 - (bv & 7) * 8) & 63)
            case 0x252: r[rc] = (bv & 7) ? av & ~(bytemask(2) >> HSH) : av; break;
            case 0x257: r[rc] = (bv & 7) ? (av & bytemask(2)) >> HSH : 0; break;
            case 0x25a: r[rc] = (av << HSH) & bytemask(2); break;
            case 0x262: r[rc] = (bv & 7) ? av & ~(bytemask(4) >> HSH) : av; break;
            case 0x267: r[rc] = (bv & 7) ? (av & bytemask(4)) >> HSH : 0; break;
            case 0x26a: r[rc] = (av << HSH) & bytemask(4); break;
            case 0x272: r[rc] = (bv & 7) ? av & ~(bytemask(8) >> HSH) : av; break;
            case 0x277: r[rc] = (bv & 7) ? av >> HSH : 0; break;
            case 0x27a: r[rc] = av << HSH; break;
            /* 0x13 */
            case 0x300: case 0x340: r[rc] = (u64)(s64)(s32)((u32)av * (u32)bv); break;
            case 0x320: case 0x360: r[rc] = av * bv; break;
            case 0x330: r[rc] = (u64)(((unsigned __int128)av * bv) >> 64); break;
            default: cpu_fault(STATUS_ILLEGAL_INSTRUCTION, A.pc, i);
            }
            break;
        case 0x16: { /* IEEE floating point */
            u32 fn = (i >> 5) & 0x7ff, rm = (fn >> 6) & 3, op6 = fn & 0x3f;
            double a = f2d(A.f[ra]), b = f2d(A.f[rb]);
            /* The qualifier /S (software completion): Windows NT finishes the operation by the
               IEEE rules and the program goes on. Without it the CPU does only finite numbers:
               all else is an arithmetic trap, and NT gives the program an exception. */
            int nos = !(fn & 0x400), single = op6 < 4;
            double tiny = single ? 1.17549435082228750797e-38 : 2.2250738585072014e-308;
            if (nos && op6 != 0x3c && op6 != 0x3e) {
                int bad;
                if (op6 >= 0x24 && op6 < 0x28) {
                    /* a compare: infinity is good (the compilers' code compares with it), a NaN is not */
                    bad = isnan(a) || isnan(b);
                } else {
                    bad = !isfinite(b) || (b != 0 && fabs(b) < tiny);
                    if (op6 < 0x24)
                        bad |= !isfinite(a) || (a != 0 && fabs(a) < tiny);
                }
                if (bad)
                    cpu_fault(STATUS_FLOAT_INVALID_OPERATION, A.pc, i);
                if ((op6 == 0x03 || op6 == 0x23) && b == 0)
                    cpu_fault(a == 0 ? STATUS_FLOAT_INVALID_OPERATION : STATUS_FLOAT_DIVIDE_BY_ZERO, A.pc, i);
            }
            if (rm == 3)
                rm = (u32)((A.fpcr >> 58) & 3);
            if ((fn & 0x3f) < 0x24 || (fn & 0x3f) >= 0x2c)
                setrm((int)rm);
            switch (fn & 0x3f) {
            case 0x00: A.f[rc] = d2f((double)((float)a + (float)b)); break;
            case 0x01: A.f[rc] = d2f((double)((float)a - (float)b)); break;
            case 0x02: A.f[rc] = d2f((double)((float)a * (float)b)); break;
            case 0x03: A.f[rc] = d2f((double)((float)a / (float)b)); break;
            case 0x20: A.f[rc] = d2f(a + b); break;
            case 0x21: A.f[rc] = d2f(a - b); break;
            case 0x22: A.f[rc] = d2f(a * b); break;
            case 0x23: A.f[rc] = d2f(a / b); break;
            case 0x24: A.f[rc] = (isnan(a) || isnan(b)) ? 0x4000000000000000ull : 0; break;
            case 0x25: A.f[rc] = a == b ? 0x4000000000000000ull : 0; break;
            case 0x26: A.f[rc] = a < b ? 0x4000000000000000ull : 0; break;
            case 0x27: A.f[rc] = a <= b ? 0x4000000000000000ull : 0; break;
            case 0x2c:
                if ((fn & 0x3ff) == 0x2ac)
                    A.f[rc] = A.f[rb]; /* cvtst: the register form is a double already */
                else
                    A.f[rc] = d2f((double)(float)b);
                break;
            case 0x2f: { /* cvttq */
                double x = nearbyint(b);
                if (x >= -9223372036854775808.0 && x < 9223372036854775808.0) {
                    A.f[rc] = (u64)(s64)x;
                } else {
                    if (nos) /* too big for 64 bits: an "invalid operation" trap */
                        cpu_fault(STATUS_FLOAT_INVALID_OPERATION, A.pc, i);
                    A.f[rc] = 0;
                }
                break;
            }
            case 0x3c: A.f[rc] = d2f((double)(float)(s64)A.f[rb]); break;
            case 0x3e: A.f[rc] = d2f((double)(s64)A.f[rb]); break;
            default: cpu_fault(STATUS_ILLEGAL_INSTRUCTION, A.pc, i);
            }
            if (nos && (op6 < 4 || (op6 >= 0x20 && op6 < 0x24) || (op6 == 0x2c && (fn & 0x3ff) != 0x2ac))) {
                /* the result of + - * / or of a conversion to single: too big is a trap; too small is
                   a trap with /U, else it is 0 */
                double res = f2d(A.f[rc]);
                if (op6 == 0x2c)
                    tiny = 1.17549435082228750797e-38;
                if (isinf(res))
                    cpu_fault(STATUS_FLOAT_OVERFLOW, A.pc, i);
                if (res != 0 && fabs(res) < tiny) {
                    if (fn & 0x100)
                        cpu_fault(STATUS_FLOAT_UNDERFLOW, A.pc, i);
                    A.f[rc] = d2f(0.0);
                }
            }
            break;
        }
        case 0x17: {
            u32 fn = (i >> 5) & 0x7ff;
            u64 fa = A.f[ra], fb = A.f[rb];
            switch (fn) {
            case 0x010: A.f[rc] = (u64)(s64)(s32)reg_to_s(fb); break; /* cvtlq */
            case 0x020: A.f[rc] = (fa & 0x8000000000000000ull) | (fb & 0x7fffffffffffffffull); break;
            case 0x021: A.f[rc] = (~fa & 0x8000000000000000ull) | (fb & 0x7fffffffffffffffull); break;
            case 0x022: A.f[rc] = (fa & 0xfff0000000000000ull) | (fb & 0x000fffffffffffffull); break;
            case 0x024: A.fpcr = fa; break;
            case 0x025: A.f[ra] = A.fpcr; break;
            case 0x02a: if ((fa & 0x7fffffffffffffffull) == 0) A.f[rc] = fb; break;
            case 0x02b: if ((fa & 0x7fffffffffffffffull) != 0) A.f[rc] = fb; break;
            case 0x02c: if ((fa >> 63) && (fa & 0x7fffffffffffffffull)) A.f[rc] = fb; break;
            case 0x02d: if (!(fa >> 63) || !(fa & 0x7fffffffffffffffull)) A.f[rc] = fb; break;
            case 0x02e: if ((fa >> 63) || !(fa & 0x7fffffffffffffffull)) A.f[rc] = fb; break;
            case 0x02f: if (!(fa >> 63) && (fa & 0x7fffffffffffffffull)) A.f[rc] = fb; break;
            case 0x030: case 0x130: case 0x530: /* cvtql */
                A.f[rc] = ((fb & 0xc0000000ull) << 32) | ((fb & 0x3fffffffull) << 29);
                break;
            default: cpu_fault(STATUS_ILLEGAL_INSTRUCTION, A.pc, i);
            }
            break;
        }
        case 0x18: /* trapb, excb, mb, wmb, fetch: nothing to do */
            if ((i & 0xffff) == 0xc000)
                r[ra] = icount; /* rpcc */
            break;
        case 0x1a: { u64 t = r[rb]; r[ra] = (u64)(s64)(s32)pc; pc = (u32)t & ~3u; break; }
        case 0x1c:
            bv = (i & 0x1000) ? ((i >> 13) & 0xff) : r[rb];
            switch ((i >> 5) & 0x7f) {
            case 0x00: r[rc] = (u64)(s64)(s8)bv; break;
            case 0x01: r[rc] = (u64)(s64)(s16)bv; break;
            default: cpu_fault(STATUS_ILLEGAL_INSTRUCTION, A.pc, i);
            }
            break;
        case 0x22: EA(); A.f[ra] = s_to_reg(r32(ea)); break;
        case 0x23: EA(); A.f[ra] = r64(ea); break;
        case 0x26: EA(); w32(ea, reg_to_s(A.f[ra])); break;
        case 0x27: EA(); w64(ea, A.f[ra]); break;
        case 0x28: EA(); r[ra] = (u64)(s64)(s32)r32(ea); break;
        case 0x29: EA(); r[ra] = r64(ea); break;
        case 0x2a: EA(); r[ra] = (u64)(s64)(s32)r32(ea); A.lock = 1; break;
        case 0x2b: EA(); r[ra] = r64(ea); A.lock = 1; break;
        case 0x2c: EA(); w32(ea, (u32)r[ra]); break;
        case 0x2d: EA(); w64(ea, r[ra]); break;
        case 0x2e: EA(); w32(ea, (u32)r[ra]); r[ra] = 1; break;
        case 0x2f: EA(); w64(ea, r[ra]); r[ra] = 1; break;
        case 0x30: case 0x34: r[ra] = (u64)(s64)(s32)pc; BRANCH(); break;
#define FZ(x) (((x) & 0x7fffffffffffffffull) == 0)
        case 0x31: if (FZ(A.f[ra])) BRANCH(); break;
        case 0x32: if ((A.f[ra] >> 63) && !FZ(A.f[ra])) BRANCH(); break;
        case 0x33: if ((A.f[ra] >> 63) || FZ(A.f[ra])) BRANCH(); break;
        case 0x35: if (!FZ(A.f[ra])) BRANCH(); break;
        case 0x36: if (!(A.f[ra] >> 63) || FZ(A.f[ra])) BRANCH(); break;
        case 0x37: if (!(A.f[ra] >> 63) && !FZ(A.f[ra])) BRANCH(); break;
        case 0x38: if (!(r[ra] & 1)) BRANCH(); break;
        case 0x39: if (r[ra] == 0) BRANCH(); break;
        case 0x3a: if ((s64)r[ra] < 0) BRANCH(); break;
        case 0x3b: if ((s64)r[ra] <= 0) BRANCH(); break;
        case 0x3c: if (r[ra] & 1) BRANCH(); break;
        case 0x3d: if (r[ra] != 0) BRANCH(); break;
        case 0x3e: if ((s64)r[ra] >= 0) BRANCH(); break;
        case 0x3f: if ((s64)r[ra] > 0) BRANCH(); break;
        default: cpu_fault(STATUS_ILLEGAL_INSTRUCTION, A.pc, i);
        }
        r[31] = 0;
        A.f[31] = 0;
    }
}

static void alpha_init(u32 sp, u32 teb, u32 gp)
{
    memset(&A, 0, sizeof A);
    A.r[SP] = (u64)(s64)(s32)(sp - 64);
    A.r[GP] = (u64)(s64)(s32)gp;
    A.r[RA] = (u64)(s64)(s32)THUNK_BASE;
    A.teb = teb;
    A.fpcr = 2ull << 58; /* round to nearest */
}

static void alpha_thunk(u32 addr)
{
    w32(addr, 0x00000100);     /* call_pal 0x100: only our thunks have it */
    w32(addr + 4, 0x6bfa8001); /* ret zero,(ra) (never runs) */
}

static u32 alpha_thunk_ptr(u32 addr) { return addr; }

static void alpha_call_begin(u32 fn, int n, const u32 *args, u64 *save)
{
    u32 sp = ((u32)A.r[SP] - 256) & ~15u;

    save[0] = A.r[RA];
    save[1] = A.r[SP];
    save[2] = A.pc;
    for (int k = 0; k < n; k++) {
        if (k < 6) A.r[A0 + k] = (u64)(s64)(s32)args[k];
        else w64(sp + 8 * (u32)(k - 6), (u64)(s64)(s32)args[k]);
    }
    A.r[SP] = (u64)(s64)(s32)sp;
    A.r[RA] = (u64)(s64)(s32)THUNK_BASE;
    A.r[PV] = (u64)(s64)(s32)fn;
    A.pc = fn;
}

static u32 alpha_call_end(const u64 *save)
{
    u32 ret = (u32)A.r[V0];
    A.r[RA] = save[0];
    A.r[SP] = save[1];
    A.pc = (u32)save[2];
    return ret;
}

static u32 alpha_pc(void) { return A.pc; }
static u32 alpha_sp(void) { return (u32)A.r[SP]; }
static u32 alpha_ra(void) { return (u32)A.r[RA]; }
static void alpha_set_sp(u32 sp) { A.r[SP] = (u64)(s64)(s32)sp; }

/* ---- exceptions: the CONTEXT of Windows NT for Alpha: 32 floating, 32 integer registers of 64 bits ---- */

#define AX_FLT 0x000
#define AX_INT 0x100
#define AX_FPCR 0x200
#define AX_FIR 0x210
#define AX_PSR 0x218
#define AX_FLAGS 0x21c
#define AX_SIZE 0x230
#define IR(c, n) ((c) + AX_INT + 8 * (n))
#define FR(c, n) ((c) + AX_FLT + 8 * (n))

static void alpha_capture(u32 c, u32 fir)
{
    memset(G + c, 0, AX_SIZE);
    for (int i = 0; i < 32; i++) {
        w64(FR(c, i), A.f[i]);
        w64(IR(c, i), A.r[i]);
    }
    w64(c + AX_FPCR, A.fpcr);
    w64(c + AX_FIR, (u64)(s64)(s32)fir);
    w32(c + AX_PSR, 1);
    w32(c + AX_FLAGS, 0x00020007);
}

static void alpha_restore(u32 c)
{
    for (int i = 0; i < 32; i++) {
        A.f[i] = r64(FR(c, i));
        A.r[i] = r64(IR(c, i));
    }
    A.r[31] = 0;
    A.f[31] = 0;
    A.fpcr = r64(c + AX_FPCR);
    A.pc = (u32)r64(c + AX_FIR);
}

static u32 alpha_lookup(u32 pc)
{
    u32 size, tab = find_pdata(pc, &size);
    s32 lo = 0, hi = (s32)(size / 20) - 1;

    if (!tab)
        return 0;
    while (hi >= lo) {
        s32 mid = (lo + hi) >> 1;
        u32 fe = tab + (u32)mid * 20;
        if (pc < r32(fe))
            hi = mid - 1;
        else if (pc >= r32(fe + 4))
            lo = mid + 1;
        else {
            u32 pe = r32(fe + 16);
            if (pe < r32(fe) || pe > r32(fe + 4))
                fe = pe; /* a second entry for the same function: go to the first */
            return fe;
        }
    }
    return 0;
}

#define IS_RET(i) (((i) & 0xffe0ffffu) == 0x6be08001u) /* ret zero,(rb),1 */

/* Undo the prologue of the function that has `pc`, in the context at c. Returns the address
   where control left the caller. virt: the stack pointer at the entry of the function;
   real: the stack pointer in the function. */
static u32 alpha_vunwind(u32 pc, u32 fe, u32 c, int *infunc, u32 *virt, u32 *real, u32 ptrs)
{
    u32 insn = r32(pc), fol, next, dreg = 31, doff = 0;
    int got_ra = 0, got_sp = 0;

    if (IS_RET(insn)) { /* at the return: all is restored */
        next = (u32)r64(IR(c, (insn >> 16) & 31)) - 4;
        *real = 0;
        *virt = (u32)r64(IR(c, SP));
        *infunc = 0;
        return next;
    }
    fol = r32(pc + 4);
    if (IS_RET(fol)) { /* one instruction before the return: it may free the frame */
        u32 op = insn >> 26, ra = (insn >> 21) & 31, rb = (insn >> 16) & 31, rc = insn & 31;
        next = (u32)r64(IR(c, (fol >> 16) & 31)) - 4;
        if (op == 0x08 && ra == SP) {
            w64(IR(c, SP), (u64)(s64)(s16)insn + r64(IR(c, rb)));
            *real = 0;
            *virt = (u32)r64(IR(c, SP));
            *infunc = 0;
            return next;
        }
        if (op == 0x10 && ((insn >> 5) & 0x7f) == 0x20 && rc == SP && !(insn & 0x1000)) {
            w64(IR(c, SP), r64(IR(c, ra)) + r64(IR(c, rb)));
            *real = 0;
            *virt = (u32)r64(IR(c, SP));
            *infunc = 0;
            return next;
        }
    }
    *real = *virt = (u32)r64(IR(c, SP));
    if (pc < r32(fe) || pc >= r32(fe + 16)) {
        *infunc = 1;
        pc = r32(fe + 16) & ~3u;
    } else {
        *infunc = 0;
    }
    next = (u32)r64(IR(c, RA)) - 4;
    while (pc > r32(fe)) {
        u32 op, ra, rb, rc, fn, lit, addr, frame = 0;
        s32 off;
        int alloc = 0;

        pc -= 4;
        insn = r32(pc);
        op = insn >> 26;
        ra = (insn >> 21) & 31;
        rb = (insn >> 16) & 31;
        rc = insn & 31;
        fn = (insn >> 5) & 0x7f;
        lit = (insn >> 13) & 0xff;
        off = (s16)insn;
        switch (op) {
        case 0x2d: /* stq: a saved register */
            if (rb == SP && ra != 31) {
                addr = (u32)off + (u32)r64(IR(c, SP));
                w64(IR(c, ra), r64(addr));
                if (ra == RA) {
                    if (!got_ra) { next = (u32)r64(IR(c, RA)) - 4; got_ra = 1; }
                    else next += 4;
                } else if (ra == SP && !got_sp) {
                    *virt = *real = (u32)r64(IR(c, SP));
                    got_sp = 1;
                }
                if (ptrs)
                    w32(ptrs + 128 + 4 * ra, addr);
            }
            break;
        case 0x09: /* ldah */
            off = (s32)((u32)off << 16);
            /* fall through */
        case 0x08: /* lda */
            if (ra == SP) {
                if (rb == SP) {
                    frame = (u32)-off;
                    alloc = 1;
                } else {
                    dreg = rb;
                    doff = (u32)off;
                }
            } else if (ra == dreg) {
                if (rb == dreg) {
                    doff += (u32)off;
                } else if (rb == 31) {
                    frame = (u32)off + doff;
                    alloc = 1;
                } else if (rb == SP) {
                    frame = (u32)-(off + (s32)doff);
                    alloc = 1;
                }
            }
            break;
        case 0x10:
            if (fn == 0x20 && (insn & 0x1000)) { /* addq with a literal */
                if (ra == 31 && rc == dreg) {
                    frame = lit;
                    alloc = 1;
                }
            } else if (fn == 0x29 && !(insn & 0x1000)) { /* subq sp, reg, sp */
                if (ra == SP && rc == SP) {
                    dreg = rb;
                    doff = 0;
                }
            }
            break;
        case 0x11:
            if (fn == 0x20 && rc != 31) { /* bis: a move */
                if (!(insn & 0x1000)) {
                    if (ra == 31)
                        ra = rb;
                    else if (rb == 31)
                        rb = ra;
                    if (ra == rb && ra != 31) {
                        w64(IR(c, ra), r64(IR(c, rc)));
                        if (ra == RA) {
                            if (!got_ra) { next = (u32)r64(IR(c, RA)) - 4; got_ra = 1; }
                            else next += 4;
                        } else if (ra == SP && !got_sp) {
                            *virt = *real = (u32)r64(IR(c, SP));
                            got_sp = 1;
                        }
                    }
                } else if (ra == 31 && rc == dreg) {
                    frame = lit;
                    alloc = 1;
                }
            }
            break;
        case 0x27: /* stt: a saved floating register */
            if (rb == SP && ra != 31) {
                addr = (u32)off + (u32)r64(IR(c, SP));
                w64(FR(c, ra), r64(addr));
                if (ptrs)
                    w32(ptrs + 4 * ra, addr);
            }
            break;
        case 0x17:
            if (((insn >> 5) & 0x7ff) == 0x020 && ra == rb && ra != 31) /* cpys: a move */
                w64(FR(c, ra), r64(FR(c, rc)));
            break;
        }
        if (alloc) { /* the instruction made the frame: take it back */
            w64(IR(c, SP), r64(IR(c, SP)) + frame);
            *virt = (u32)r64(IR(c, SP));
            dreg = 31;
        }
    }
    return next;
}

static u32 alpha_handler(u32 fe)
{
    u32 h = r32(fe + 8);
    return (h & ~3u) ? h : 0;
}

static u32 alpha_cget(u32 c, int what)
{
    switch (what) {
    case C_SP: return (u32)r64(IR(c, SP));
    case C_NEXT: return (u32)r64(IR(c, RA)) - 4;
    case C_FIR: return (u32)r64(c + AX_FIR);
    }
    return 0;
}

static void alpha_cset(u32 c, int what, u32 v)
{
    if (what == C_FIR)
        w64(c + AX_FIR, (u64)(s64)(s32)v);
    else if (what == C_RETVAL)
        w64(IR(c, V0), (u64)(s64)(s32)v);
}

/* the jump buffer: fp, pc, seb, type (4 words); f2-f9; s0-s5, s6 (fp); sp; fir (quadwords) */
#define AJ_FP 0
#define AJ_PC 4
#define AJ_SEB 8
#define AJ_TYPE 12
#define AJ_FLT 0x10
#define AJ_S0 0x50
#define AJ_SP 0x88
#define AJ_FIR 0x90

u32 guest_alloc(u32 n);

/* setjmp of NTDLL: it keeps the frame of the caller, for an unwind */
static u32 alpha_nt_setjmp(u32 jb)
{
    u32 ra = (u32)A.r[RA], tmp = ((u32)A.r[SP] - AX_SIZE - 64) & ~15u, virt = 0, real = 0, fe;
    int infunc;

    w32(jb + AJ_TYPE, 2);
    w32(jb + AJ_PC, ra);
    alpha_capture(tmp, ra);
    fe = alpha_lookup(ra - 4);
    if (fe)
        alpha_vunwind(ra - 4, fe, tmp, &infunc, &virt, &real, 0);
    else
        virt = (u32)A.r[SP];
    w32(jb + AJ_FP, virt);
    return 0;
}

static void alpha_nt_longjmp(u32 jb, u32 val)
{
    u32 type = r32(jb + AJ_TYPE), rec;

    if (!val)
        val = 1;
    if (type == 1) { /* a plain jump */
        for (int i = 0; i < 8; i++)
            A.f[2 + i] = r64(jb + AJ_FLT + 8 * (u32)i);
        for (int i = 0; i < 7; i++)
            A.r[9 + i] = r64(jb + AJ_S0 + 8 * (u32)i);
        A.r[SP] = r64(jb + AJ_SP);
        A.r[V0] = (u64)(s64)(s32)val;
        A.pc = (u32)r64(jb + AJ_FIR);
        guest_resume();
    }
    if (type & ~2u) { /* made by the GEM compiler: unwind by the real frame pointer */
        rec = guest_alloc(80);
        memset(G + rec, 0, 80);
        w32(rec, STATUS_UNWIND);
        w32(rec + 4, 2);
        w32(rec + 16, 1);
        w32(rec + 20, r32(jb + AJ_SEB));
        seh_unwind(UW_RFP, r32(jb + AJ_FP), r32(jb + AJ_PC), rec, val);
    }
    seh_unwind(UW_NORMAL, r32(jb + AJ_FP), r32(jb + AJ_PC), 0, val);
}

static void alpha_state_save(void *p) { memcpy(p, &A, sizeof A); }

static void alpha_state_load(const void *p, u32 teb)
{
    (void)teb; /* it is in the registers */
    memcpy(&A, p, sizeof A);
    host_rm = -1; /* this host thread has its own rounding mode: set it again */
    setrm(2);
}

const struct cpu cpu_alpha = {
    "Alpha", 8192, 21064, 2,
    alpha_init, alpha_thunk, alpha_thunk_ptr, alpha_run, alpha_call_begin, alpha_call_end,
    alpha_pc, alpha_sp, alpha_ra, alpha_set_sp, alpha_dump,
    AX_SIZE, 15, 1, 0,
    alpha_capture, alpha_restore, alpha_lookup, alpha_vunwind, alpha_handler, alpha_cget, alpha_cset,
    0, alpha_nt_setjmp, alpha_nt_longjmp,
    sizeof A, alpha_state_save, alpha_state_load,
    0,
};
