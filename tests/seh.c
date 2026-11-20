/* seh.c: structured exception handling, setjmp/longjmp and CPU traps */
#include <stdio.h>
#include <setjmp.h>
#include <windows.h>

static jmp_buf jb;
static int fin_count;

static int depth(int n)
{
    volatile int x[16];
    x[0] = n;
    if (n == 0)
        longjmp(jb, 7);
    return depth(n - 1) + x[0];
}

static int filter(unsigned long code, unsigned long *seen)
{
    *seen = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

/* an access violation: what it did (0 read, 1 write) and the address */
static unsigned long av_kind, av_addr;

static int av_filter(EXCEPTION_POINTERS *p)
{
    if (p->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        return EXCEPTION_CONTINUE_SEARCH;
    av_kind = p->ExceptionRecord->ExceptionInformation[0];
    av_addr = p->ExceptionRecord->ExceptionInformation[1];
    return EXCEPTION_EXECUTE_HANDLER;
}

static void raise_in(int n)
{
    volatile int pad[8];
    pad[0] = n;
    if (n == 0)
        RaiseException(0xE0001234, 0, 0, NULL);
    else
        raise_in(n - 1);
    pad[1] = pad[0];
}

static void with_finally(int n)
{
    __try {
        raise_in(n);
    } __finally {
        fin_count++;
    }
}

static int leave_early(void)
{
    __try {
        return 1;
    } __finally {
        fin_count += 10;
    }
    return 0;
}

int main(void)
{
    unsigned long seen = 0, args[2];
    int r;
    int *volatile np = 0;
    volatile int zero = 0, v = 0;

    r = setjmp(jb);
    if (r == 0)
        depth(5);
    printf("1 longjmp %d\n", r);

    __try {
        raise_in(3);
        printf("2 not reached\n");
    } __except (filter(GetExceptionCode(), &seen)) {
        printf("2 except %08lx\n", seen);
    }

    __try {
        with_finally(2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("3 finally %d\n", fin_count);
    }

    __try {
        v = *np;
        printf("4 not reached %d\n", v);
    } __except (av_filter(GetExceptionInformation())) {
        printf("4 access violation: kind %lu, address %08lx\n", av_kind, av_addr);
    }

    np = (int *)0x10;
    __try {
        *np = 5;
        printf("5 not reached\n");
    } __except (av_filter(GetExceptionInformation())) {
        printf("5 write fault: kind %lu, address %08lx\n", av_kind, av_addr);
    }
    np = 0;

    __try {
        RaiseException(0xE0000001, 0, 0, NULL);
        printf("6 continued\n");
    } __except (EXCEPTION_CONTINUE_EXECUTION) {
        printf("6 wrong\n");
    }

    __try {
        __try {
            RaiseException(0xE0000002, 0, 0, NULL);
        } __finally {
            printf("7 inner finally, abnormal %d\n", AbnormalTermination());
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("7 outer except\n");
    }

    args[0] = 11;
    args[1] = 22;
    __try {
        RaiseException(0xE0000003, EXCEPTION_NONCONTINUABLE, 2, args);
    } __except (seen = (GetExceptionInformation())->ExceptionRecord->ExceptionInformation[1], EXCEPTION_EXECUTE_HANDLER) {
        printf("8 parameter %lu\n", seen);
    }

    r = leave_early();
    printf("9 return %d finally %d\n", r, fin_count);

    __try {
        __try {
            v = *np;
        } __except (EXCEPTION_CONTINUE_SEARCH) {
            printf("10 wrong\n");
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("10 outer got it\n");
    }

    __try {
        v = 10 / zero;
        printf("11 no trap %d\n", v != 0);
    } __except (filter(GetExceptionCode(), &seen)) {
        printf("11 except %08lx\n", seen);
    }

    printf("done\n");
    return 0;
}
