/* jmpex.c: longjmp that unwinds (setjmpex.h): the __finally blocks run */
#include <stdio.h>
#include <setjmpex.h>

static jmp_buf jb;
static int fin;

static void f(int n)
{
    __try {
        if (n == 0)
            longjmp(jb, 3);
        f(n - 1);
    } __finally {
        fin++;
    }
}

int main(void)
{
    int r = setjmp(jb);
    if (r == 0)
        f(4);
    printf("setjmpex %d finally %d\n", r, fin);
    return 0;
}
