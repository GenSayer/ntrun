/* app.c: uses the static library, the DLL, a string resource and the message table */
#include <stdio.h>
#include <windows.h>
#include "msg.h"

int util_add(int a, int b);
__declspec(dllimport) int dll_mul(int a, int b);

int main(void)
{
    char buf[128];
    DWORD args[2];

    printf("lib %d dll %d\n", util_add(2, 3), dll_mul(6, 7));
    buf[0] = 0;
    LoadString(GetModuleHandle(NULL), 100, buf, sizeof buf);
    printf("string: %s\n", buf);
    args[0] = (DWORD)"ntrun";
    buf[0] = 0;
    FormatMessage(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_ARGUMENT_ARRAY, NULL, MSG_HELLO, 0, buf, sizeof buf,
        (va_list *)args);
    printf("message: %s", buf);
    args[0] = 3;
    args[1] = 4;
    FormatMessage(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_ARGUMENT_ARRAY, NULL, MSG_COUNT, 0, buf, sizeof buf,
        (va_list *)args);
    printf("message: %s", buf);
    return 0;
}
