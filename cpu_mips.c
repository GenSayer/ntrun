/* cpu_mips.c: MIPS R4000 user mode, little endian, as Windows NT uses it (FR=0). */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <fenv.h>
#include <setjmp.h>
#include "ntrun.h"

static struct {
    s64 r[32];
    s64 hi, lo;
    u32 pc;
    u32 fcr31;
    u32 f[34];
    u32 llbit;
} M;

static inline double getd(int n) { u64 v = M.f[n] | ((u64)M.f[n + 1] << 32); double d; memcpy(&d, &v, 8); return d; }
static inline void setd(int n, double d) { u64 v; memcpy(&v, &d, 8); M.f[n] = (u32)v; M.f[n + 1] = (u32)(v >> 32); }
static inline float gets_(int n) { float f; memcpy(&f, &M.f[n], 4); return f; }
static inline void sets_(int n, float f) { memcpy(&M.f[n], &f, 4); }
static inline u64 getl(int n) { return M.f[n] | ((u64)M.f[n + 1] << 32); }
static inline void setl(int n, u64 v) { M.f[n] = (u32)v; M.f[n + 1] = (u32)(v >> 32); }

/* The NaN of MIPS: the first bit of the fraction is 0 in a quiet NaN (on the other CPUs it is 1).
   An operation that has no number as its result gives 7ff7ffffffffffff (single: 7fbfffff);
   a quiet NaN operand goes through as it is. */
static double nan_d(double r, double a, double b)
{
    u64 v;

    if (!isnan(r))
        return r;
    memcpy(&v, &a, 8);
    if (isnan(a) && !(v & 0x0008000000000000ull))
        return a;
    memcpy(&v, &b, 8);
    if (isnan(b) && !(v & 0x0008000000000000ull))
        return b;
    v = 0x7ff7ffffffffffffull;
    memcpy(&r, &v, 8);
    return r;
}

static float nan_s(float r, float a, float b)
{
    u32 v;

    if (!isnan(r))
        return r;
    memcpy(&v, &a, 4);
    if (isnan(a) && !(v & 0x00400000u))
        return a;
    memcpy(&v, &b, 4);
    if (isnan(b) && !(v & 0x00400000u))
        return b;
    v = 0x7fbfffffu;
    memcpy(&r, &v, 4);
    return r;
}

static void set_round(void)
{
    static const int mode[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };
    fesetround(mode[M.fcr31 & 3]);
}

static s32 to_w(double d, int how) /* how: 0 current mode, 1 trunc, 2 ceil, 3 floor, 4 nearest */
{
    double x;
    switch (how) {
    case 1: x = trunc(d); break;
    case 2: x = ceil(d); break;
    case 3: x = floor(d); break;
    case 4: x = nearbyint(d); break; /* the mode is nearest unless the program changed it */
    default: x = nearbyint(d); break;
    }
    if (!(x >= -2147483648.0 && x <= 2147483647.0)) {
        /* it does not fit: the "invalid operation" flag and cause bits. The compilers make code
           that looks at the flags after a conversion to see if the number was too big. */
        M.fcr31 |= (1u << 6) | (1u << 16);
        return 0x7fffffff;
    }
    return (s32)x;
}

/* the flag bits of the control register (invalid, division by zero, overflow, underflow,
   inexact): the host keeps them for the arithmetic it does */
static u32 fcr31_read(void)
{
    int e = fetestexcept(FE_ALL_EXCEPT);

    if (e & FE_INVALID) M.fcr31 |= 1u << 6;
    if (e & FE_DIVBYZERO) M.fcr31 |= 1u << 5;
    if (e & FE_OVERFLOW) M.fcr31 |= 1u << 4;
    if (e & FE_UNDERFLOW) M.fcr31 |= 1u << 3;
    if (e & FE_INEXACT) M.fcr31 |= 1u << 2;
    return M.fcr31;
}

static int fcmp(double a, double b, int cond)
{
    int un = isnan(a) || isnan(b);
    if (un)
        return cond & 1;
    return ((cond & 2) && a == b) || ((cond & 4) && a < b);
}

/* the code of a break or trap instruction: the compilers use 7 for a division by zero */
static u32 mips_trap_code(u32 code)
{
    switch (code & 0x3ff) {
    case 6: return STATUS_INTEGER_OVERFLOW;
    case 7: return STATUS_INTEGER_DIVIDE_BY_ZERO;
    }
    return STATUS_BREAKPOINT;
}

static void mips_dump(void)
{
    static const char *n[32] = { "zr", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5",
        "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "s8", "ra" };
    fprintf(stderr, "pc=%08x hi=%08x lo=%08x fcr31=%08x\n", M.pc, (u32)M.hi, (u32)M.lo, M.fcr31);
    for (int i = 0; i < 32; i++)
        fprintf(stderr, "%s=%08x%s", n[i], (u32)M.r[i], (i & 7) == 7 ? "\n" : " ");
}

static void mips_run(void)
{
    s64 *r = M.r;
    u32 pc = M.pc, npc = pc + 4;

    for (;;) {
        u32 i = r32(pc);
        u32 nnpc = npc + 4;
        u32 rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31, sa = (i >> 6) & 31;
        s32 simm = (s16)i;
        u32 ea;

        icount++;
        M.pc = pc;
        if (trace_insn)
            fprintf(stderr, "%08x: %08x\n", pc, i);
#define BR(c) do { if (c) nnpc = npc + (simm << 2); } while (0)
#define BRL(c) do { if (c) nnpc = npc + (simm << 2); else { npc = nnpc; nnpc = npc + 4; } } while (0)
#define EA() (ea = (u32)r[rs] + simm)
        switch (i >> 26) {
        case 0:
            switch (i & 63) {
            case 0x00: r[rd] = (s32)((u32)r[rt] << sa); break;
            case 0x02: r[rd] = (s32)((u32)r[rt] >> sa); break;
            case 0x03: r[rd] = (s32)r[rt] >> sa; break;
            case 0x04: r[rd] = (s32)((u32)r[rt] << (r[rs] & 31)); break;
            case 0x06: r[rd] = (s32)((u32)r[rt] >> (r[rs] & 31)); break;
            case 0x07: r[rd] = (s32)r[rt] >> (r[rs] & 31); break;
            case 0x08: nnpc = (u32)r[rs]; break;
            case 0x09: { u32 t = (u32)r[rs]; r[rd] = (s32)(pc + 8); nnpc = t; break; }
            case 0x0c: /* syscall: only our thunks have it */
                if (pc >= THUNK_BASE && pc < THUNK_BASE + THUNK_MAX * THUNK_SLOT) {
                    u32 idx = (pc - THUNK_BASE) / THUNK_SLOT, a[16], ra = (u32)r[31], sp = (u32)r[29];
                    if (idx == API_RETURN) { M.pc = pc; return; }
                    for (int k = 0; k < 4; k++) a[k] = (u32)r[4 + k];
                    for (int k = 4; k < 16; k++) a[k] = r32(sp + 4 * k);
                    M.pc = ra;
                    r[2] = (s32)api_dispatch(idx, a);
                    pc = ra; npc = pc + 4;
                    r[0] = 0;
                    continue;
                }
                M.pc = pc; cpu_fault(STATUS_ILLEGAL_INSTRUCTION, pc, i);
            case 0x0d: M.pc = pc; cpu_fault(mips_trap_code(i >> 16), pc, i);
            case 0x0f: break; /* sync */
            case 0x10: r[rd] = M.hi; break;
            case 0x11: M.hi = r[rs]; break;
            case 0x12: r[rd] = M.lo; break;
            case 0x13: M.lo = r[rs]; break;
            case 0x14: r[rd] = (s64)((u64)r[rt] << (r[rs] & 63)); break;
            case 0x16: r[rd] = (s64)((u64)r[rt] >> (r[rs] & 63)); break;
            case 0x17: r[rd] = r[rt] >> (r[rs] & 63); break;
            case 0x18: { s64 p = (s64)(s32)r[rs] * (s64)(s32)r[rt]; M.lo = (s32)p; M.hi = (s32)(p >> 32); break; }
            case 0x19: { u64 p = (u64)(u32)r[rs] * (u64)(u32)r[rt]; M.lo = (s32)p; M.hi = (s32)(p >> 32); break; }
            case 0x1a: {
                s32 a = (s32)r[rs], b = (s32)r[rt];
                if (b == 0) break;
                if (a == INT32_MIN && b == -1) { M.lo = a; M.hi = 0; break; }
                M.lo = a / b; M.hi = a % b; break;
            }
            case 0x1b: { u32 a = (u32)r[rs], b = (u32)r[rt]; if (b) { M.lo = (s32)(a / b); M.hi = (s32)(a % b); } break; }
            case 0x1c: { __int128 p = (__int128)r[rs] * (__int128)r[rt]; M.lo = (s64)p; M.hi = (s64)(p >> 64); break; }
            case 0x1d: { unsigned __int128 p = (unsigned __int128)(u64)r[rs] * (u64)r[rt]; M.lo = (s64)p; M.hi = (s64)(p >> 64); break; }
            case 0x1e: { s64 a = r[rs], b = r[rt]; if (b == 0) break; if (a == INT64_MIN && b == -1) { M.lo = a; M.hi = 0; break; } M.lo = a / b; M.hi = a % b; break; }
            case 0x1f: { u64 a = r[rs], b = r[rt]; if (b) { M.lo = a / b; M.hi = a % b; } break; }
            case 0x20: { s32 a = (s32)r[rs], b = (s32)r[rt], c; if (__builtin_add_overflow(a, b, &c)) { M.pc = pc; cpu_fault(STATUS_INTEGER_OVERFLOW, pc, i); } r[rd] = c; break; }
            case 0x21: r[rd] = (s32)((u32)r[rs] + (u32)r[rt]); break;
            case 0x22: { s32 a = (s32)r[rs], b = (s32)r[rt], c; if (__builtin_sub_overflow(a, b, &c)) { M.pc = pc; cpu_fault(STATUS_INTEGER_OVERFLOW, pc, i); } r[rd] = c; break; }
            case 0x23: r[rd] = (s32)((u32)r[rs] - (u32)r[rt]); break;
            case 0x24: r[rd] = r[rs] & r[rt]; break;
            case 0x25: r[rd] = r[rs] | r[rt]; break;
            case 0x26: r[rd] = r[rs] ^ r[rt]; break;
            case 0x27: r[rd] = ~(r[rs] | r[rt]); break;
            case 0x2a: r[rd] = r[rs] < r[rt]; break;
            case 0x2b: r[rd] = (u64)r[rs] < (u64)r[rt]; break;
            case 0x2c: case 0x2d: r[rd] = (s64)((u64)r[rs] + (u64)r[rt]); break;
            case 0x2e: case 0x2f: r[rd] = (s64)((u64)r[rs] - (u64)r[rt]); break;
            case 0x30: if (r[rs] >= r[rt]) { M.pc = pc; cpu_fault(mips_trap_code(i >> 6), pc, i); } break;
            case 0x31: if ((u64)r[rs] >= (u64)r[rt]) { M.pc = pc; cpu_fault(mips_trap_code(i >> 6), pc, i); } break;
            case 0x32: if (r[rs] < r[rt]) { M.pc = pc; cpu_fault(mips_trap_code(i >> 6), pc, i); } break;
            case 0x33: if ((u64)r[rs] < (u64)r[rt]) { M.pc = pc; cpu_fault(mips_trap_code(i >> 6), pc, i); } break;
            case 0x34: if (r[rs] == r[rt]) { M.pc = pc; cpu_fault(mips_trap_code(i >> 6), pc, i); } break;
            case 0x36: if (r[rs] != r[rt]) { M.pc = pc; cpu_fault(mips_trap_code(i >> 6), pc, i); } break;
            case 0x38: r[rd] = (s64)((u64)r[rt] << sa); break;
            case 0x3a: r[rd] = (s64)((u64)r[rt] >> sa); break;
            case 0x3b: r[rd] = r[rt] >> sa; break;
            case 0x3c: r[rd] = (s64)((u64)r[rt] << (sa + 32)); break;
            case 0x3e: r[rd] = (s64)((u64)r[rt] >> (sa + 32)); break;
            case 0x3f: r[rd] = r[rt] >> (sa + 32); break;
            default: M.pc = pc; cpu_fault(STATUS_ILLEGAL_INSTRUCTION, pc, i);
            }
            break;
        case 1:
            switch (rt) {
            case 0: BR(r[rs] < 0); break;
            case 1: BR(r[rs] >= 0); break;
            case 2: BRL(r[rs] < 0); break;
            case 3: BRL(r[rs] >= 0); break;
            case 16: { int c = r[rs] < 0; r[31] = (s32)(pc + 8); BR(c); break; }
            case 17: { int c = r[rs] >= 0; r[31] = (s32)(pc + 8); BR(c); break; }
            case 18: { int c = r[rs] < 0; r[31] = (s32)(pc + 8); BRL(c); break; }
            case 19: { int c = r[rs] >= 0; r[31] = (s32)(pc + 8); BRL(c); break; }
            default: M.pc = pc; cpu_fault(STATUS_ILLEGAL_INSTRUCTION, pc, i);
            }
            break;
        case 2: nnpc = (npc & 0xf0000000) | ((i & 0x3ffffff) << 2); break;
        case 3: r[31] = (s32)(pc + 8); nnpc = (npc & 0xf0000000) | ((i & 0x3ffffff) << 2); break;
        case 4: BR(r[rs] == r[rt]); break;
        case 5: BR(r[rs] != r[rt]); break;
        case 6: BR(r[rs] <= 0); break;
        case 7: BR(r[rs] > 0); break;
        case 8: { s32 a = (s32)r[rs], c; if (__builtin_add_overflow(a, simm, &c)) { M.pc = pc; cpu_fault(STATUS_INTEGER_OVERFLOW, pc, i); } r[rt] = c; break; }
        case 9: r[rt] = (s32)((u32)r[rs] + (u32)simm); break;
        case 10: r[rt] = r[rs] < (s64)simm; break;
        case 11: r[rt] = (u64)r[rs] < (u64)(s64)simm; break;
        case 12: r[rt] = r[rs] & (u16)i; break;
        case 13: r[rt] = r[rs] | (u16)i; break;
        case 14: r[rt] = r[rs] ^ (u16)i; break;
        case 15: r[rt] = (s32)((u32)(u16)i << 16); break;
        case 17: {
            u32 fs = rd, ft = rt, fd = sa, fn = i & 63;
            switch (rs) {
            case 0: r[rt] = (s32)M.f[fs]; break;
            case 1: r[rt] = (s64)getl(fs); break;
            case 2: r[rt] = fs == 31 ? (s32)fcr31_read() : fs == 0 ? 0x500 : 0; break;
            case 4: M.f[fs] = (u32)r[rt]; break;
            case 5: setl(fs, (u64)r[rt]); break;
            case 6: if (fs == 31) { M.fcr31 = (u32)r[rt]; feclearexcept(FE_ALL_EXCEPT); set_round(); } break;
            case 8: {
                int c = (M.fcr31 >> 23) & 1;
                if (!(rt & 1)) c = !c;
                if (rt & 2) BRL(c); else BR(c);
                break;
            }
            case 16: { /* single */
                float a = gets_(fs), b = gets_(ft);
                switch (fn) {
                case 0: sets_(fd, nan_s(a + b, a, b)); break;
                case 1: sets_(fd, nan_s(a - b, a, b)); break;
                case 2: sets_(fd, nan_s(a * b, a, b)); break;
                case 3: sets_(fd, nan_s(a / b, a, b)); break;
                case 4: sets_(fd, nan_s(sqrtf(a), a, a)); break;
                case 5: sets_(fd, fabsf(a)); break;
                case 6: M.f[fd] = M.f[fs]; break;
                case 7: sets_(fd, -a); break;
                case 12: M.f[fd] = (u32)to_w(a, 4); break;
                case 13: M.f[fd] = (u32)to_w(a, 1); break;
                case 14: M.f[fd] = (u32)to_w(a, 2); break;
                case 15: M.f[fd] = (u32)to_w(a, 3); break;
                case 33: setd(fd, (double)a); break;
                case 36: M.f[fd] = (u32)to_w(a, 0); break;
                default:
                    if (fn >= 48) { int c = fcmp(a, b, fn & 15); M.fcr31 = (M.fcr31 & ~(1u << 23)) | ((u32)c << 23); break; }
                    M.pc = pc; cpu_fault(STATUS_ILLEGAL_INSTRUCTION, pc, i);
                }
                break;
            }
            case 17: { /* double */
                double a = getd(fs), b = getd(ft);
                switch (fn) {
                case 0: setd(fd, nan_d(a + b, a, b)); break;
                case 1: setd(fd, nan_d(a - b, a, b)); break;
                case 2: setd(fd, nan_d(a * b, a, b)); break;
                case 3: setd(fd, nan_d(a / b, a, b)); break;
                case 4: setd(fd, nan_d(sqrt(a), a, a)); break;
                case 5: setd(fd, fabs(a)); break;
                case 6: M.f[fd] = M.f[fs]; M.f[fd + 1] = M.f[fs + 1]; break;
                case 7: setd(fd, -a); break;
                case 12: M.f[fd] = (u32)to_w(a, 4); break;
                case 13: M.f[fd] = (u32)to_w(a, 1); break;
                case 14: M.f[fd] = (u32)to_w(a, 2); break;
                case 15: M.f[fd] = (u32)to_w(a, 3); break;
                case 32: sets_(fd, (float)a); break;
                case 36: M.f[fd] = (u32)to_w(a, 0); break;
                default:
                    if (fn >= 48) { int c = fcmp(a, b, fn & 15); M.fcr31 = (M.fcr31 & ~(1u << 23)) | ((u32)c << 23); break; }
                    M.pc = pc; cpu_fault(STATUS_ILLEGAL_INSTRUCTION, pc, i);
                }
                break;
            }
            case 20: { /* word */
                s32 a = (s32)M.f[fs];
                switch (fn) {
                case 32: sets_(fd, (float)a); break;
                case 33: setd(fd, (double)a); break;
                default: M.pc = pc; cpu_fault(STATUS_ILLEGAL_INSTRUCTION, pc, i);
                }
                break;
            }
            default: M.pc = pc; cpu_fault(STATUS_ILLEGAL_INSTRUCTION, pc, i);
            }
            break;
        }
        case 20: BRL(r[rs] == r[rt]); break;
        case 21: BRL(r[rs] != r[rt]); break;
        case 22: BRL(r[rs] <= 0); break;
        case 23: BRL(r[rs] > 0); break;
        case 24: case 25: r[rt] = (s64)((u64)r[rs] + (u64)(s64)simm); break;
        case 26: { /* ldl */
            EA(); u32 b = ea & 7; u64 m = r64(ea & ~7u);
            r[rt] = (s64)(b == 7 ? m : (((u64)r[rt] & (0x00ffffffffffffffull >> (b * 8))) | (m << ((7 - b) * 8))));
            break;
        }
        case 27: { /* ldr */
            EA(); u32 b = ea & 7; u64 m = r64(ea & ~7u);
            r[rt] = (s64)(b == 0 ? m : (((u64)r[rt] & (0xffffffffffffff00ull << ((7 - b) * 8))) | (m >> (b * 8))));
            break;
        }
        case 32: EA(); r[rt] = (s8)r8(ea); break;
        case 33: EA(); r[rt] = (s16)r16(ea); break;
        case 34: { /* lwl */
            EA(); u32 b = ea & 3, m = r32(ea & ~3u);
            r[rt] = (s32)(((u32)r[rt] & (0x00ffffffu >> (b * 8))) | (m << ((3 - b) * 8)));
            break;
        }
        case 35: EA(); r[rt] = (s32)r32(ea); break;
        case 36: EA(); r[rt] = r8(ea); break;
        case 37: EA(); r[rt] = r16(ea); break;
        case 38: { /* lwr */
            EA(); u32 b = ea & 3, m = r32(ea & ~3u);
            r[rt] = (s32)(b == 0 ? m : (((u32)r[rt] & (0xffffff00u << ((3 - b) * 8))) | (m >> (b * 8))));
            break;
        }
        case 39: EA(); r[rt] = r32(ea); break;
        case 40: EA(); w8(ea, (u8)r[rt]); break;
        case 41: EA(); w16(ea, (u16)r[rt]); break;
        case 42: { /* swl */
            EA(); u32 b = ea & 3, a = ea & ~3u, m = r32(a);
            w32(a, b == 3 ? (u32)r[rt] : ((m & (0xffffff00u << (b * 8))) | ((u32)r[rt] >> ((3 - b) * 8))));
            break;
        }
        case 43: EA(); w32(ea, (u32)r[rt]); break;
        case 44: { /* sdl */
            EA(); u32 b = ea & 7, a = ea & ~7u; u64 m = r64(a);
            w64(a, b == 7 ? (u64)r[rt] : ((m & (0xffffffffffffff00ull << (b * 8))) | ((u64)r[rt] >> ((7 - b) * 8))));
            break;
        }
        case 45: { /* sdr */
            EA(); u32 b = ea & 7, a = ea & ~7u; u64 m = r64(a);
            w64(a, b == 0 ? (u64)r[rt] : ((m & ((1ull << (b * 8)) - 1)) | ((u64)r[rt] << (b * 8))));
            break;
        }
        case 46: { /* swr */
            EA(); u32 b = ea & 3, a = ea & ~3u, m = r32(a);
            w32(a, b == 0 ? (u32)r[rt] : ((m & ((1u << (b * 8)) - 1)) | ((u32)r[rt] << (b * 8))));
            break;
        }
        case 47: break; /* cache */
        case 48: EA(); r[rt] = (s32)r32(ea); M.llbit = 1; break;
        case 49: EA(); M.f[rt] = r32(ea); break;
        case 52: EA(); r[rt] = (s64)r64(ea); M.llbit = 1; break;
        case 53: EA(); M.f[rt] = r32(ea); M.f[rt + 1] = r32(ea + 4); break;
        case 55: EA(); r[rt] = (s64)r64(ea); break;
        case 56: EA(); w32(ea, (u32)r[rt]); r[rt] = 1; break;
        case 57: EA(); w32(ea, M.f[rt]); break;
        case 60: EA(); w64(ea, (u64)r[rt]); r[rt] = 1; break;
        case 61: EA(); w32(ea, M.f[rt]); w32(ea + 4, M.f[rt + 1]); break;
        case 63: EA(); w64(ea, (u64)r[rt]); break;
        default: M.pc = pc; cpu_fault(STATUS_ILLEGAL_INSTRUCTION, pc, i);
        }
        r[0] = 0;
        pc = npc;
        npc = nnpc;
    }
}

static void mips_init(u32 sp, u32 teb, u32 gp)
{
    memset(&M, 0, sizeof M);
    M.r[29] = (s32)(sp - 64);
    M.r[28] = (s32)gp;
    M.r[31] = (s32)THUNK_BASE;
    w32(0x7ffff4a8, teb); /* the user PCR page: NtCurrentTeb() reads it */
    set_round();
}

static void mips_thunk(u32 addr)
{
    w32(addr, 0x0000000c);     /* syscall */
    w32(addr + 4, 0x03e00008); /* jr ra (never runs) */
    w32(addr + 8, 0);
}

static u32 mips_thunk_ptr(u32 addr) { return addr; }

static void mips_call_begin(u32 fn, int n, const u32 *args, u64 *save)
{
    u32 sp = ((u32)M.r[29] - 128) & ~15u;

    save[0] = (u64)M.r[31];
    save[1] = (u64)M.r[29];
    save[2] = M.pc;
    for (int k = 0; k < n; k++) {
        if (k < 4) M.r[4 + k] = (s32)args[k];
        else w32(sp + 4 * (u32)k, args[k]);
    }
    M.r[29] = (s32)sp;
    M.r[31] = (s32)THUNK_BASE;
    M.pc = fn;
}

static u32 mips_call_end(const u64 *save)
{
    u32 ret = (u32)M.r[2];
    M.r[31] = (s64)save[0];
    M.r[29] = (s64)save[1];
    M.pc = (u32)save[2];
    return ret;
}

static u32 mips_pc(void) { return M.pc; }
static u32 mips_sp(void) { return (u32)M.r[29]; }
static u32 mips_ra(void) { return (u32)M.r[31]; }
static void mips_set_sp(u32 sp) { M.r[29] = (s32)sp; }

/* ---- exceptions: the CONTEXT of Windows NT 3.x for MIPS, 32-bit ---- */

#define CX_FLT 0x10
#define CX_INT 0x90
#define CX_LO 0x110
#define CX_HI 0x114
#define CX_FSR 0x118
#define CX_FIR 0x11c
#define CX_PSR 0x120
#define CX_FLAGS 0x124
#define CX_SIZE 0x130
#define CXI(c, n) ((c) + CX_INT + 4 * (n))
#define SP 29
#define RA 31

static void mips_capture(u32 c, u32 fir)
{
    memset(G + c, 0, CX_SIZE);
    for (int i = 0; i < 32; i++) {
        w32(CXI(c, i), (u32)M.r[i]);
        w32(c + CX_FLT + 4 * i, M.f[i]);
    }
    w32(c + CX_LO, (u32)M.lo);
    w32(c + CX_HI, (u32)M.hi);
    w32(c + CX_FSR, M.fcr31);
    w32(c + CX_FIR, fir);
    w32(c + CX_PSR, 0x2000ff11);
    w32(c + CX_FLAGS, 0x00010007);
}

static void mips_restore(u32 c)
{
    for (int i = 0; i < 32; i++) {
        M.r[i] = (s32)r32(CXI(c, i));
        M.f[i] = r32(c + CX_FLT + 4 * i);
    }
    M.r[0] = 0;
    M.lo = (s32)r32(c + CX_LO);
    M.hi = (s32)r32(c + CX_HI);
    M.fcr31 = r32(c + CX_FSR);
    set_round();
    M.pc = r32(c + CX_FIR);
}

/* the function table entry for pc: 5 words: begin, end, handler, handler data, end of prologue */
static u32 mips_lookup(u32 pc)
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
            if (pe < r32(fe) || pe >= r32(fe + 4))
                fe = pe; /* a second entry for the same function: go to the first */
            return fe;
        }
    }
    return 0;
}

/* Undo the prologue of the function that has `pc`, in the context at c.
   Returns the address where control left the caller. */
static u32 mips_vunwind(u32 pc, u32 fe, u32 c, int *infunc, u32 *virt, u32 *real, u32 ptrs)
{
    u32 dreg = 0, doff = 0, next, frame;
    int got_ra = 0, got_sp = 0;

    if (r32(pc) == 0x03e00008) { /* jr ra: only the delay slot is left */
        u32 i = r32(pc + 4), op = i >> 26, rs = (i >> 21) & 31, rt = (i >> 16) & 31, rd = (i >> 11) & 31;
        *infunc = 0;
        if (op == 9 && rt == SP && rs == SP)
            w32(CXI(c, SP), r32(CXI(c, SP)) + (u32)(s32)(s16)i);
        else if (op == 0 && (i & 63) == 0x21 && rd == SP && rs == SP)
            w32(CXI(c, SP), r32(CXI(c, SP)) + r32(CXI(c, rt)));
        *virt = *real = r32(CXI(c, SP));
        return r32(CXI(c, RA));
    }
    if (pc < r32(fe) || pc >= r32(fe + 16)) {
        *infunc = 1;
        pc = r32(fe + 16);
    } else {
        *infunc = 0;
    }
    frame = r32(CXI(c, SP));
    next = r32(CXI(c, RA)) - 4;
    while (pc > r32(fe)) {
        u32 i, op, rs, rt, rd, addr;
        s32 off;

        pc -= 4;
        i = r32(pc);
        op = i >> 26;
        off = (s16)i;
        rs = (i >> 21) & 31;
        rt = (i >> 16) & 31;
        rd = (i >> 11) & 31;
        addr = (u32)off + r32(CXI(c, rs));
        if (op == 43) { /* sw */
            if (rs == SP) {
                w32(CXI(c, rt), r32(addr));
                if (rt == RA) {
                    if (!got_ra) { next = r32(CXI(c, RA)) - 4; got_ra = 1; }
                    else next += 4;
                } else if (rt == SP) {
                    if (!got_sp) { frame = r32(CXI(c, SP)); got_sp = 1; }
                }
                if (ptrs)
                    w32(ptrs + 128 + 4 * rt, addr);
            }
        } else if (op == 57) { /* swc1 */
            if (rs == SP) {
                w32(c + CX_FLT + 4 * rt, r32(addr));
                if (ptrs)
                    w32(ptrs + 4 * rt, addr);
            }
        } else if (op == 61) { /* sdc1 */
            if (rs == SP) {
                w32(c + CX_FLT + 4 * rt, r32(addr));
                w32(c + CX_FLT + 4 * (rt + 1), r32(addr + 4));
                if (ptrs) {
                    w32(ptrs + 4 * rt, addr);
                    w32(ptrs + 4 * (rt + 1), addr + 4);
                }
            }
        } else if (op == 9) { /* addiu */
            if (rs == SP && rt == SP) {
                w32(CXI(c, SP), r32(CXI(c, SP)) - (u32)off);
                if (!got_sp) { frame = r32(CXI(c, SP)); got_sp = 1; }
            } else if (rt == dreg && rs == 0) {
                w32(CXI(c, SP), r32(CXI(c, SP)) + (u32)off);
                if (!got_sp) { frame = r32(CXI(c, SP)); got_sp = 1; }
            }
        } else if (op == 13) { /* ori */
            if (rs == dreg && rt == dreg) {
                doff = (u32)off & 0xffff;
            } else if (rt == dreg && rs == 0) {
                w32(CXI(c, SP), r32(CXI(c, SP)) + ((u32)off & 0xffff));
                if (!got_sp) { frame = r32(CXI(c, SP)); got_sp = 1; }
            }
        } else if (op == 0) {
            u32 fn = i & 63;
            if (fn == 0x21 || fn == 0x25) { /* addu, or: a move */
                if (rt == 0) {
                    w32(CXI(c, rs), r32(CXI(c, rd)));
                    if (rs == RA) {
                        if (!got_ra) { next = r32(CXI(c, RA)) - 4; got_ra = 1; }
                        else next += 4;
                    }
                } else if (rs == 0) {
                    w32(CXI(c, rt), r32(CXI(c, rd)));
                    if (rt == RA) {
                        if (!got_ra) { next = r32(CXI(c, RA)) - 4; got_ra = 1; }
                        else next += 4;
                    }
                }
            } else if (fn == 0x23) { /* subu sp, sp, reg */
                if (rd == SP && rs == SP)
                    dreg = rt;
            }
        } else if (op == 15) { /* lui */
            if (rt == dreg) {
                dreg = 0;
                w32(CXI(c, SP), r32(CXI(c, SP)) + doff + ((u32)off << 16));
                if (!got_sp) { frame = r32(CXI(c, SP)); got_sp = 1; }
            }
        }
    }
    w32(CXI(c, 0), 0);
    *virt = *real = frame;
    return next;
}

static u32 mips_handler(u32 fe) { return r32(fe + 8); }

static u32 mips_cget(u32 c, int what)
{
    switch (what) {
    case C_SP: return r32(CXI(c, SP));
    case C_NEXT: return r32(CXI(c, RA)) - 4;
    case C_FIR: return r32(c + CX_FIR);
    }
    return 0;
}

static void mips_cset(u32 c, int what, u32 v)
{
    if (what == C_FIR)
        w32(c + CX_FIR, v);
    else if (what == C_RETVAL)
        w32(CXI(c, 2), v);
}

/* the jump buffer: f20-f31 (12 words), s0-s7, s8, sp, type, fir */
#define JB_S0 0x30
#define JB_S8 0x50
#define JB_SP 0x54
#define JB_TYPE 0x58
#define JB_FIR 0x5c

/* an unwind for longjmp: the saved registers come from the jump buffer */
static void mips_unwind_fix(u32 c, u32 rec)
{
    u32 jb;

    if (!rec || r32(rec) != STATUS_LONGJUMP)
        return;
    jb = r32(rec + 20);
    for (int i = 0; i < 12; i++)
        w32(c + CX_FLT + 4 * (20 + i), r32(jb + 4 * (u32)i));
    for (int i = 0; i < 8; i++)
        w32(CXI(c, 16 + i), r32(jb + JB_S0 + 4 * (u32)i));
    w32(CXI(c, 30), r32(jb + JB_S8));
    w32(c + CX_FIR, r32(jb + JB_FIR));
    w32(CXI(c, RA), r32(jb + JB_FIR));
    w32(CXI(c, SP), r32(jb + JB_SP));
}

static u32 mips_nt_setjmp(u32 jb)
{
    for (int i = 0; i < 12; i++)
        w32(jb + 4 * (u32)i, M.f[20 + i]);
    for (int i = 0; i < 8; i++)
        w32(jb + JB_S0 + 4 * (u32)i, (u32)M.r[16 + i]);
    w32(jb + JB_S8, (u32)M.r[30]);
    w32(jb + JB_SP, (u32)M.r[29]);
    w32(jb + JB_TYPE, 0);
    w32(jb + JB_FIR, (u32)M.r[31]);
    return 0;
}

u32 guest_alloc(u32 n);

static void mips_nt_longjmp(u32 jb, u32 val)
{
    u32 rec;

    if (!val)
        val = 1;
    if (r32(jb + JB_TYPE) == 0) { /* a plain jump */
        for (int i = 0; i < 12; i++)
            M.f[20 + i] = r32(jb + 4 * (u32)i);
        for (int i = 0; i < 8; i++)
            M.r[16 + i] = (s32)r32(jb + JB_S0 + 4 * (u32)i);
        M.r[30] = (s32)r32(jb + JB_S8);
        M.r[29] = (s32)r32(jb + JB_SP);
        M.r[2] = (s32)val;
        M.pc = r32(jb + JB_FIR);
        guest_resume();
    }
    rec = guest_alloc(80); /* the type is the frame to unwind to */
    memset(G + rec, 0, 80);
    w32(rec, STATUS_LONGJUMP);
    w32(rec + 16, 1);
    w32(rec + 20, jb);
    seh_unwind(UW_NORMAL, r32(jb + JB_TYPE), r32(jb + JB_FIR), rec, val);
}

static void mips_state_save(void *p) { memcpy(p, &M, sizeof M); }

static void mips_state_load(const void *p, u32 teb)
{
    memcpy(&M, p, sizeof M);
    w32(0x7ffff4a8, teb);
    set_round();
}

const struct cpu cpu_mips = {
    "MIPS", 4096, 4000, 1,
    mips_init, mips_thunk, mips_thunk_ptr, mips_run, mips_call_begin, mips_call_end,
    mips_pc, mips_sp, mips_ra, mips_set_sp, mips_dump,
    CX_SIZE, 7, 0, 0,
    mips_capture, mips_restore, mips_lookup, mips_vunwind, mips_handler, mips_cget, mips_cset,
    mips_unwind_fix, mips_nt_setjmp, mips_nt_longjmp,
    sizeof M, mips_state_save, mips_state_load,
    0,
};
