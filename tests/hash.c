/* hash.c: the MD5 and SHA-1 sums of ADVAPI32 (CryptCreateHash and so on). The compilers of 2005
   make a sum of each source file with them. hash.out has the sums as "md5" and "shasum" of the
   host give them. The program takes the functions by name: the libraries of Visual C++ 4.x for
   the RISC CPUs do not have them. */
#include <windows.h>
#include <stdio.h>
#include <string.h>

typedef BOOL (WINAPI *ACQUIRE)(DWORD *, const char *, const char *, DWORD, DWORD);
typedef BOOL (WINAPI *CREATE)(DWORD, DWORD, DWORD, DWORD, DWORD *);
typedef BOOL (WINAPI *DATA)(DWORD, const BYTE *, DWORD, DWORD);
typedef BOOL (WINAPI *PARAM)(DWORD, DWORD, BYTE *, DWORD *, DWORD);
typedef BOOL (WINAPI *DESTROY)(DWORD);

static ACQUIRE acquire;
static CREATE create;
static DATA data;
static PARAM param;
static DESTROY destroy;
static DWORD prov;

/* the sum of N bytes at P, given to the system PIECE bytes at a time */
static void sum(const char *name, DWORD alg, const BYTE *p, DWORD n, DWORD piece)
{
    DWORD h = 0, size = 0, len = sizeof size, i;
    BYTE out[32];

    if (!create(prov, alg, 0, 0, &h)) {
        printf("%s: no hash, error %08lx\n", name, GetLastError());
        return;
    }
    for (i = 0; i < n; i += piece)
        data(h, p + i, n - i < piece ? n - i : piece, 0);
    param(h, 4, (BYTE *)&size, &len, 0);
    len = sizeof out;
    if (!param(h, 2, out, &len, 0))
        printf("%s: no sum, error %08lx\n", name, GetLastError());
    printf("%s %lu %lu ", name, size, len);
    for (i = 0; i < len; i++)
        printf("%02x", out[i]);
    printf("\n");
    destroy(h);
}

int main(void)
{
    static BYTE big[100000];
    static const char *text[] = { "", "abc", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" };
    HMODULE m = LoadLibrary("advapi32.dll");
    DWORD i, need = 4, h = 0;
    BYTE out[32];

    acquire = (ACQUIRE)GetProcAddress(m, "CryptAcquireContextA");
    create = (CREATE)GetProcAddress(m, "CryptCreateHash");
    data = (DATA)GetProcAddress(m, "CryptHashData");
    param = (PARAM)GetProcAddress(m, "CryptGetHashParam");
    destroy = (DESTROY)GetProcAddress(m, "CryptDestroyHash");
    if (!acquire || !create || !data || !param || !destroy) {
        printf("the system has no hash functions\n");
        return 1;
    }
    if (!acquire(&prov, NULL, NULL, 1, 0xf0000000)) {
        printf("no provider, error %08lx\n", GetLastError());
        return 1;
    }
    for (i = 0; i < sizeof big; i++)
        big[i] = (BYTE)(i * 7 + i / 251);
    for (i = 0; i < 3; i++) {
        sum("md5 ", 0x8003, (const BYTE *)text[i], (DWORD)strlen(text[i]), 64);
        sum("sha1", 0x8004, (const BYTE *)text[i], (DWORD)strlen(text[i]), 64);
    }
    sum("md5 ", 0x8003, big, sizeof big, 1000);
    sum("sha1", 0x8004, big, sizeof big, 1000);
    sum("md5 ", 0x8003, big, 12345, 7);
    sum("sha1", 0x8004, big, 12345, 7);
    /* a buffer that is too small: error 234 and the size that it needs */
    create(prov, 0x8003, 0, 0, &h);
    data(h, (const BYTE *)"abc", 3, 0);
    printf("small: %d", param(h, 2, out, &need, 0));
    printf(" %lu %lu\n", GetLastError(), need);
    destroy(h);
    /* an algorithm that is not there */
    printf("other: %d\n", create(prov, 0x1234, 0, 0, &h));
    return 0;
}
