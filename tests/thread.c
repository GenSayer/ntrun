/* thread.c: threads, a critical section, events, a mutex, a semaphore, waits, data for each thread */
#include <stdio.h>
#include <windows.h>
#include <process.h>

static CRITICAL_SECTION cs;
static int counter;
static HANDLE ev, mtx, sem;
__declspec(thread) int tlsvar = 7;

static DWORD WINAPI worker(LPVOID arg)
{
    int i;

    for (i = 0; i < 1000; i++) {
        EnterCriticalSection(&cs);
        counter++;
        LeaveCriticalSection(&cs);
        if (i % 100 == 0)
            Sleep(0);
    }
    return 100 + (int)arg;
}

static DWORD WINAPI tlsworker(LPVOID arg)
{
    tlsvar += (int)arg; /* this thread's own copy; it starts at 7 */
    WaitForSingleObject(ev, INFINITE);
    return tlsvar;
}

static DWORD WINAPI mtxworker(LPVOID arg)
{
    WaitForSingleObject(mtx, INFINITE); /* the first thread has it: this waits */
    counter += 10;
    ReleaseMutex(mtx);
    ReleaseSemaphore(sem, 1, NULL);
    return 0;
}

static void __cdecl crtworker(void *arg)
{
    counter += (int)arg;
    SetEvent(ev);
}

int main(void)
{
    HANDLE h[4];
    DWORD id, code, r;
    int i;

    InitializeCriticalSection(&cs);
    for (i = 0; i < 4; i++)
        h[i] = CreateThread(NULL, 0, worker, (LPVOID)i, 0, &id);
    r = WaitForMultipleObjects(4, h, TRUE, INFINITE);
    printf("1 wait for all %s, counter %d\n", r < 4 ? "ok" : "failed", counter);
    for (i = 0; i < 4; i++) {
        GetExitCodeThread(h[i], &code);
        printf("2 thread %d exit code %d\n", i, (int)code);
        CloseHandle(h[i]);
    }

    ev = CreateEvent(NULL, TRUE, FALSE, NULL);
    h[0] = CreateThread(NULL, 0, tlsworker, (LPVOID)1, 0, &id);
    h[1] = CreateThread(NULL, 0, tlsworker, (LPVOID)2, 0, &id);
    r = WaitForSingleObject(h[0], 50);
    GetExitCodeThread(h[0], &code);
    printf("3 wait with a time limit %d, exit code %d\n", (int)r, (int)code);
    SetEvent(ev);
    r = WaitForMultipleObjects(2, h, TRUE, INFINITE);
    GetExitCodeThread(h[0], &code);
    printf("4 thread data %d", (int)code);
    GetExitCodeThread(h[1], &code);
    printf(" %d, first thread %d\n", (int)code, tlsvar);

    counter = 0;
    h[2] = CreateThread(NULL, 0, worker, (LPVOID)9, CREATE_SUSPENDED, &id);
    Sleep(30);
    printf("5 suspended: counter %d\n", counter);
    r = ResumeThread(h[2]);
    WaitForSingleObject(h[2], INFINITE);
    GetExitCodeThread(h[2], &code);
    printf("6 resumed (%d): counter %d, exit code %d\n", (int)r, counter, (int)code);

    counter = 0;
    mtx = CreateMutex(NULL, TRUE, NULL);
    sem = CreateSemaphore(NULL, 0, 1, NULL);
    h[3] = CreateThread(NULL, 0, mtxworker, NULL, 0, &id);
    Sleep(30);
    printf("7 the mutex keeps the thread out: counter %d\n", counter);
    ReleaseMutex(mtx);
    r = WaitForSingleObject(sem, INFINITE);
    printf("8 semaphore %d, counter %d\n", (int)r, counter);
    r = WaitForSingleObject(sem, 0);
    printf("9 semaphore again %d\n", (int)r);

    ResetEvent(ev);
    _beginthread(crtworker, 0, (void *)5);
    WaitForSingleObject(ev, INFINITE);
    printf("10 _beginthread: counter %d\n", counter);
    printf("done\n");
    return 0;
}
