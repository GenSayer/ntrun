/*
 * seh.c: Windows exceptions for the guest program. Windows NT on the RISC CPUs has no
 * handler chain: to find the handler of a stack frame, it looks up the function that
 * has the frame's return address in the function table (.pdata) and undoes the
 * function's prologue to get to the frame of the caller. This file does the two
 * walks up the stack (find a handler; unwind to a target frame). The CPU files do
 * the part that depends on the CPU: the CONTEXT record and the prologue.
 */
#include <stdio.h>
#include <stdlib.h>
#include "ntrun.h"

#define EXC_NONCONTINUABLE 1
#define EXC_UNWINDING 2
#define EXC_EXIT_UNWIND 4
#define EXC_STACK_INVALID 8
#define EXC_NESTED_CALL 0x10
#define EXC_TARGET_UNWIND 0x20
#define EXC_COLLIDED_UNWIND 0x40

#define RECSIZE 80 /* EXCEPTION_RECORD: code, flags, record, address, number of parameters, 15 parameters */

void guest_exit(u32 code) __attribute__((noreturn));

/*
 * The host calls a handler through a return thunk. To a walk up the stack, that place is
 * a frame of its own (as the frames of NTDLL are on Windows): it has a function table
 * entry, and to unwind it gives the context that the handler was called for. So the
 * program's own walks get through it too (the C++ run-time for Alpha has one).
 */
#define FE_BOUNDARY (THUNK_BASE + (THUNK_MAX - 2) * THUNK_SLOT)

void seh_init(void)
{
    w32(FE_BOUNDARY, THUNK_BASE - 16);
    w32(FE_BOUNDARY + 4, THUNK_BASE + 16);
    w32(FE_BOUNDARY + 8, 0);
    w32(FE_BOUNDARY + 12, 0);
    w32(FE_BOUNDARY + 16, THUNK_BASE - 16);
}

/* RtlLookupFunctionEntry */
u32 seh_lookup(u32 pc)
{
    if (pc >= THUNK_BASE - 16 && pc < THUNK_BASE + 16)
        return FE_BOUNDARY;
    return cpu->lookup(pc);
}

/* RtlVirtualUnwind */
u32 seh_vunwind(u32 pc, u32 fe, u32 ctx, int *infunc, u32 *virt, u32 *real, u32 ptrs)
{
    if (fe == FE_BOUNDARY) {
        u32 sp = cpu->cget(ctx, C_SP), next, low, high;

        *infunc = 0;
        *virt = *real = sp;
        if (guest_cross(sp, ctx, &next))
            return next;
        stack_limits(&low, &high); /* the last frame: the walk ends here */
        *virt = *real = high;
        return pc;
    }
    return cpu->vunwind(pc, fe, ctx, infunc, virt, real, ptrs);
}

static void unhandled(u32 rec) __attribute__((noreturn));
static void unhandled(u32 rec)
{
    u32 code = r32(rec);

    fflush(stdout);
    fprintf(stderr, "ntrun: unhandled exception %08x in the program at address %08x", code, r32(rec + 12));
    if (code == STATUS_ACCESS_VIOLATION && r32(rec + 16) >= 2)
        fprintf(stderr, ": it could not %s address %08x", r32(rec + 20) ? "write to" : "read", r32(rec + 24));
    fprintf(stderr, "\n");
    if (getenv("NTRUN_DUMP"))
        cpu->dump();
    guest_exit(code);
}

/* Look for a handler, from the frame at pc up. fir: where the program goes on if a handler says so. */
static void dispatch(u32 code, u32 flags, u32 nparams, const u32 *params, u32 fir, u32 pc) __attribute__((noreturn));
static void dispatch(u32 code, u32 flags, u32 nparams, const u32 *params, u32 fir, u32 pc)
{
    const u32 csz = cpu->ctx_size, mask = cpu->frame_mask;
    u32 sp = cpu->sp(), low, high;
    u32 base = (sp - (2 * csz + RECSIZE + 16 + 128)) & ~15u;
    u32 rec = base + 32, ctx = rec + RECSIZE, ctx1 = ctx + csz, dc = ctx1 + csz;
    u32 pc0 = pc, next, eflags, nested = 0, virt = 0, real = 0;
    int infunc = 0;

    stack_limits(&low, &high);
    if (sp < low + 4096 || sp > high) { /* no stack left to work on */
        fflush(stdout);
        fprintf(stderr, "ntrun: exception %08x at address %08x, and the stack pointer %08x is bad\n", code, fir, sp);
        guest_exit(code);
    }
    memset(G + rec, 0, RECSIZE);
    w32(rec, code);
    w32(rec + 4, flags & EXC_NONCONTINUABLE);
    w32(rec + 12, fir);
    w32(rec + 16, nparams);
    for (u32 k = 0; k < nparams; k++)
        w32(rec + 20 + 4 * k, params[k]);
    cpu->capture(ctx, fir);
    memcpy(G + ctx1, G + ctx, csz);
    cpu->set_sp(base); /* the handlers run below these records */
    eflags = flags & EXC_NONCONTINUABLE;
    do {
        u32 fe = seh_lookup(pc), h;

        if (!fe && !cpu->unwind_null) {
            next = cpu->cget(ctx1, C_NEXT);
            if (next == pc)
                break;
            pc = next;
            continue;
        }
        next = seh_vunwind(pc, fe, ctx1, &infunc, &virt, &real, 0);
        if (!fe || fe == FE_BOUNDARY) {
            if (next == pc)
                break;
            pc = next;
            continue;
        }
        if (virt < low || virt > high || (virt & mask)) {
            eflags |= EXC_STACK_INVALID;
            break;
        }
        h = cpu->handler(fe);
        if (h && infunc) {
            u32 a[4] = { rec, virt, ctx, dc }, disp;
            w32(dc, pc);
            w32(dc + 4, fe);
            w32(dc + 8, virt);
            w32(dc + 12, ctx);
            w32(rec + 4, eflags);
            disp = guest_call2(h, 4, a, ctx, pc0);
            eflags |= r32(rec + 4) & EXC_NONCONTINUABLE;
            if (nested == virt) {
                eflags &= ~(u32)EXC_NESTED_CALL;
                nested = 0;
            }
            switch (disp) {
            case 0: /* continue the program */
                if (eflags & EXC_NONCONTINUABLE)
                    die("a handler continued exception %08x, which cannot continue", code);
                cpu->restore(ctx);
                guest_resume();
            case 1: /* continue the search */
                break;
            case 2:
                eflags |= EXC_NESTED_CALL;
                if (r32(dc + 8) > nested)
                    nested = r32(dc + 8);
                break;
            default:
                die("exception handler: bad disposition %u", disp);
            }
        }
        pc = next;
    } while (cpu->cget(ctx1, C_SP) < high);
    w32(rec + 4, eflags);
    unhandled(rec);
}

/*
 * x86. Here Windows has a list of handlers: the first word of the TEB (FS:[0]) points to the
 * newest record { next record, handler }, and each function with a handler puts its record
 * on the list. The handlers are called from the newest one up; a handler that takes the
 * exception calls RtlUnwind and then goes to its own place (it does not return).
 */
#define CHAIN_END 0xffffffffu

static void dispatch_chain(u32 code, u32 flags, u32 nparams, const u32 *params, u32 fir) __attribute__((noreturn));
static void dispatch_chain(u32 code, u32 flags, u32 nparams, const u32 *params, u32 fir)
{
    const u32 csz = cpu->ctx_size, teb = guest_teb();
    u32 sp = cpu->sp(), low, high;
    u32 base = (sp - (csz + RECSIZE + 16 + 64)) & ~3u;
    u32 rec = base + 32, ctx = rec + RECSIZE, dc = ctx + csz;
    u32 eflags = flags & EXC_NONCONTINUABLE, nested = 0, reg;

    stack_limits(&low, &high);
    if (sp < low + 4096 || sp > high) {
        fflush(stdout);
        fprintf(stderr, "ntrun: exception %08x at address %08x, and the stack pointer %08x is bad\n", code, fir, sp);
        guest_exit(code);
    }
    memset(G + rec, 0, RECSIZE);
    w32(rec, code);
    w32(rec + 12, fir);
    w32(rec + 16, nparams);
    for (u32 k = 0; k < nparams; k++)
        w32(rec + 20 + 4 * k, params[k]);
    cpu->capture(ctx, fir);
    cpu->set_sp(base); /* the handlers run below these records */
    for (reg = r32(teb); reg != CHAIN_END; reg = r32(reg)) {
        u32 a[4] = { rec, reg, ctx, dc }, disp;

        if (reg < low || reg + 8 > high || (reg & 3)) {
            eflags |= EXC_STACK_INVALID;
            break;
        }
        w32(rec + 4, eflags);
        w32(dc, 0);
        disp = guest_call2(r32(reg + 4), 4, a, 0, 0);
        eflags |= r32(rec + 4) & EXC_NONCONTINUABLE;
        if (nested == reg) {
            eflags &= ~(u32)EXC_NESTED_CALL;
            nested = 0;
        }
        switch (disp) {
        case 0: /* continue the program */
            if (eflags & EXC_NONCONTINUABLE)
                die("a handler continued exception %08x, which cannot continue", code);
            cpu->restore(ctx);
            guest_resume();
        case 1: /* continue the search */
            break;
        case 2: /* an exception in a handler: the frame of that handler */
            eflags |= EXC_NESTED_CALL;
            if (r32(dc) > nested)
                nested = r32(dc);
            break;
        default:
            die("exception handler: bad disposition %u", disp);
        }
    }
    w32(rec + 4, eflags);
    unhandled(rec);
}

/* RtlUnwind on x86: call the handlers from the newest one up to the target record with the
   "unwind" flag and take them off the list. Then it returns: the caller goes to its place. */
void seh_unwind_chain(u32 target, u32 target_ip, u32 rec, u32 retval)
{
    const u32 csz = cpu->ctx_size, teb = guest_teb();
    u32 sp = cpu->sp(), low, high;
    u32 base = (sp - (csz + RECSIZE + 16 + 64)) & ~3u;
    u32 rec1 = base + 32, ctx = rec1 + RECSIZE, dc = ctx + csz, reg;

    (void)target_ip;
    stack_limits(&low, &high);
    if (!rec) {
        rec = rec1;
        memset(G + rec, 0, RECSIZE);
        w32(rec, STATUS_UNWIND);
        w32(rec + 12, cpu->pc());
    }
    w32(rec + 4, r32(rec + 4) | EXC_UNWINDING | (target ? 0 : EXC_EXIT_UNWIND));
    cpu->capture(ctx, cpu->pc());
    cpu->cset(ctx, C_RETVAL, retval);
    cpu->set_sp(base);
    for (reg = r32(teb); reg != CHAIN_END && reg != target;) {
        u32 a[4] = { rec, reg, ctx, dc }, disp, next;

        if (target && target < reg)
            die("unwind: the target record %08x is not on the list (record %08x)", target, reg);
        if (reg < low || reg + 8 > high || (reg & 3))
            die("unwind: bad handler record %08x", reg);
        w32(dc, 0);
        disp = guest_call2(r32(reg + 4), 4, a, 0, 0);
        if (disp == 3) { /* it ran into another unwind: go on from the record that one was at */
            reg = r32(dc);
        } else if (disp != 1) {
            die("unwind handler: bad disposition %u", disp);
        }
        next = r32(reg);
        w32(teb, next); /* the record is off the list */
        reg = next;
    }
    cpu->set_sp(sp);
    if (reg == CHAIN_END && target && target != CHAIN_END)
        die("unwind: the target record %08x is not on the list", target);
    if (!target) /* an unwind of all the records with no place to go to: the end of the program */
        unhandled(rec);
}

/* RaiseException, called by the program */
void seh_raise(u32 code, u32 flags, u32 nargs, u32 args)
{
    u32 p[15], ra = cpu->ra();

    if (nargs > 15)
        nargs = 15;
    if (!args)
        nargs = 0;
    for (u32 k = 0; k < nargs; k++)
        p[k] = r32(args + 4 * k);
    if (cpu->chain)
        dispatch_chain(code, flags, nargs, p, ra);
    dispatch(code, flags, nargs, p, ra, ra - 4);
}

/* a trap of the CPU at pc */
void seh_fault(u32 code, u32 pc, u32 nparams, u32 p0, u32 p1)
{
    u32 p[2] = { p0, p1 };
    if (cpu->chain)
        dispatch_chain(code, 0, nparams, p, pc);
    dispatch(code, 0, nparams, p, pc, pc);
}

/*
 * Unwind: call the handlers of the frames from the caller up to the target frame, then
 * go on at target_ip in the target frame.
 * UW_NORMAL is RtlUnwind. Alpha has two more: UW_RFP (RtlUnwindRfp: the target is a real
 * frame pointer) and UW_RETURN (RtlUnwindReturn: return from the target frame).
 */
void seh_unwind(int mode, u32 target, u32 target_ip, u32 rec, u32 retval)
{
    const u32 csz = cpu->ctx_size, mask = cpu->frame_mask;
    u32 sp = cpu->sp(), ra = cpu->ra(), low, high;
    u32 base = (sp - (3 * csz + RECSIZE + 16 + 128)) & ~15u;
    u32 rec1 = base + 32, c1 = rec1 + RECSIZE, c2 = c1 + csz, cp = c2 + csz, dc = cp + csz;
    u32 pc = ra - 4, next = 0, eflags, virt = sp, real = sp, fir = mode == UW_RETURN ? 0 : target_ip;
    int infunc = 0, more;

    stack_limits(&low, &high);
    cpu->capture(c1, ra);
    cpu->set_sp(base);
    cpu->cset(c1, C_FIR, fir);
    if (!rec) {
        rec = rec1;
        memset(G + rec, 0, RECSIZE);
        w32(rec, STATUS_UNWIND);
        w32(rec + 12, pc);
    }
    eflags = EXC_UNWINDING;
    if (!target && mode != UW_RETURN)
        w32(rec + 4, r32(rec + 4) | EXC_EXIT_UNWIND);
    do {
        u32 fe = seh_lookup(pc), h;

        if (fe == FE_BOUNDARY) {
            next = seh_vunwind(pc, fe, c1, &infunc, &virt, &real, 0);
            if (next == pc)
                die("unwind: the target frame %08x is not on the stack", target);
            pc = next;
        } else if (!fe) {
            next = cpu->unwind_null ? cpu->vunwind(pc, 0, c1, &infunc, &virt, &real, 0) : cpu->cget(c1, C_NEXT);
            if (next == pc)
                die("unwind: no function table entry for %08x", pc);
            pc = next;
        } else if (mode == UW_NORMAL) {
            /* a look at the frame: the context changes only when the frame goes */
            memcpy(G + cp, G + c1, csz);
            next = cpu->vunwind(pc, fe, cp, &infunc, &virt, &real, 0);
            if (virt < low || virt > high || (target && target < virt) || (virt & mask))
                die("unwind: bad stack (frame %08x, target %08x, pc %08x)", virt, target, pc);
            h = cpu->handler(fe);
            if (h && infunc) {
                w32(dc, pc);
                w32(dc + 4, fe);
                w32(dc + 8, virt);
                w32(dc + 12, c1);
                do {
                    u32 a[4] = { rec, virt, c1, dc }, disp;
                    if (target == virt)
                        eflags |= EXC_TARGET_UNWIND;
                    w32(rec + 4, eflags);
                    cpu->cset(c1, C_RETVAL, retval);
                    disp = guest_call2(h, 4, a, c1, pc);
                    eflags &= ~(u32)(EXC_COLLIDED_UNWIND | EXC_TARGET_UNWIND);
                    if (disp == 1) {
                        if (virt != target)
                            next = cpu->vunwind(pc, fe, c1, &infunc, &virt, &real, 0);
                    } else if (disp == 3) {
                        pc = r32(dc);
                        fe = r32(dc + 4);
                        c1 = r32(dc + 12);
                        cpu->cset(c1, C_FIR, fir);
                        eflags |= EXC_COLLIDED_UNWIND;
                        virt = r32(dc + 8);
                        h = cpu->handler(fe);
                    } else {
                        die("unwind handler: bad disposition %u", disp);
                    }
                } while (eflags & EXC_COLLIDED_UNWIND);
            } else if (virt != target) {
                next = cpu->vunwind(pc, fe, c1, &infunc, &virt, &real, 0);
            }
            pc = next;
        } else {
            /* Alpha, two contexts: c1 is the caller's, c2 the one of this frame */
            memcpy(G + c2, G + c1, csz);
            next = cpu->vunwind(pc, fe, c1, &infunc, &virt, &real, 0);
            if (mode == UW_RFP ? (real < low || virt > high || real > virt || (target && target < real) || (virt & mask) || (real & mask))
                               : (virt < low || virt > high || target < virt || (virt & mask)))
                die("unwind: bad stack (frame %08x/%08x, target %08x, pc %08x)", virt, real, target, pc);
            h = cpu->handler(fe);
            if (h && infunc) {
                w32(dc, pc);
                w32(dc + 4, fe);
                w32(dc + 8, virt);
                w32(dc + 12, c2);
                do {
                    u32 a[4] = { rec, virt, c2, dc }, disp;
                    if (target == (mode == UW_RFP ? real : virt))
                        eflags |= EXC_TARGET_UNWIND;
                    w32(rec + 4, eflags);
                    cpu->cset(c2, C_RETVAL, retval);
                    disp = guest_call2(h, 4, a, c2, pc);
                    eflags &= ~(u32)(EXC_COLLIDED_UNWIND | EXC_TARGET_UNWIND);
                    if (disp == 3) {
                        pc = r32(dc);
                        fe = r32(dc + 4);
                        memmove(G + c1, G + r32(dc + 12), csz);
                        cpu->cset(c1, C_FIR, fir);
                        memcpy(G + c2, G + c1, csz);
                        eflags |= EXC_COLLIDED_UNWIND;
                        next = cpu->vunwind(pc, fe, c1, &infunc, &virt, &real, 0);
                        h = cpu->handler(fe);
                    } else if (disp != 1) {
                        die("unwind handler: bad disposition %u", disp);
                    }
                } while (eflags & EXC_COLLIDED_UNWIND);
            }
            pc = next;
        }
        more = mode == UW_RFP ? (real < high && real != target) : (virt < high && virt != target);
    } while (more);

    if (mode == UW_NORMAL && virt == target) {
        cpu->cset(c1, C_RETVAL, retval);
        if (cpu->unwind_fix)
            cpu->unwind_fix(c1, rec);
        cpu->restore(c1);
        guest_resume();
    }
    if (mode == UW_RFP && real == target) {
        cpu->cset(c2, C_RETVAL, retval);
        cpu->restore(c2);
        guest_resume();
    }
    if (mode == UW_RETURN && cpu->cget(c1, C_SP) == target) {
        cpu->cset(c1, C_RETVAL, retval);
        cpu->cset(c1, C_FIR, next + 4);
        cpu->restore(c1);
        guest_resume();
    }
    unhandled(rec);
}
