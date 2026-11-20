/* tls.c: static thread-local storage (__declspec(thread)): the loader must give the main
   thread its copy of the .tls data before the program starts */
#include <stdio.h>
#include <windows.h>

__declspec(thread) int counter = 41;
__declspec(thread) char name[16] = "thread data";
__declspec(thread) int zeroed[4];

int main(void)
{
    DWORD slot;

    counter++;
    printf("static: %d \"%s\" %d\n", counter, name, zeroed[0] + zeroed[3]);

    slot = TlsAlloc();
    printf("TlsAlloc %s\n", slot != 0xffffffff ? "ok" : "failed");
    TlsSetValue(slot, (LPVOID)1234);
    printf("TlsGetValue %d\n", (int)TlsGetValue(slot));
    printf("TlsFree %d\n", TlsFree(slot));
    return 0;
}
