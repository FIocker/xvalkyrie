/* Test guest without CRT. XS_GUEST_MODE picks what it does. */
#include <windows.h>

__declspec(dllimport) int Absent(int value);
extern IMAGE_DOS_HEADER __ImageBase;

static const char ok[] = "guest-raw-ok\n";
static const char *const table[] = {ok}; /* forces a relocation */
static volatile LONG counter;

static DWORD WINAPI worker(void *argument) {
    (void)argument;
    counter = 5;
    return 0;
}

DWORD start(void) {
    static const wchar_t expected[] = L"xs_guest_raw.exe";
    char mode[16];
    wchar_t name[MAX_PATH];
    DWORD length, written, i;
    void *page;
    HANDLE thread;

    length = GetEnvironmentVariableA("XS_GUEST_MODE", mode, sizeof mode);
    if (length == 0 || length >= sizeof mode)
        mode[0] = 0;
    if (GetModuleHandleW(NULL) != (HMODULE)&__ImageBase)
        return 2;
    length = GetModuleFileNameW(NULL, name, MAX_PATH);
    if (length < 16 || length >= MAX_PATH)
        return 3;
    for (i = 0; i < 16; ++i)
        if (name[length - 16 + i] != expected[i])
            return 3;
    page = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!page)
        return 4;
    *(volatile int *)page = 7;
    VirtualFree(page, 0, MEM_RELEASE);
    thread = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    if (!thread)
        return 5;
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    if (counter != 5)
        return 6;
    if (mode[0] == 'm')
        return (DWORD)Absent(1);
    if (mode[0] == 'f')
        *(volatile int *)(ULONG_PTR)8 = 1;
    if (mode[0] == 'e')
        ExitProcess(9);
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), table[0], sizeof ok - 1, &written, NULL);
    return 42;
}
