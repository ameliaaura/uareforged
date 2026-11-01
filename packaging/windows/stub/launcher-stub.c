/* UA Reforged - "UA Reforged.exe" (Windows).
 *
 * A small starter: it runs the launcher script launcher\UA-Reforged.ps1 (next to this
 * file) with the Windows PowerShell that ships with Windows 10 and 11, with no console
 * window, passes its own arguments on (for example -env Ghor_env), waits for it and
 * returns its exit code. The script does the rest (game folder, Reforge pack, start).
 * Only one copy runs at a time.
 *
 * Build: x86_64-w64-mingw32-gcc -municode -mwindows -O2 -s launcher-stub.c stub.res.o -o "UA Reforged.exe"
 */
#include <windows.h>
#include <wchar.h>

#define MAXCMD 32768

static void fail(const wchar_t *msg)
{
    MessageBoxW(NULL, msg, L"UA Reforged", MB_OK | MB_ICONERROR);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR args, int show)
{
    (void)inst; (void)prev; (void)show;

    HANDLE once = CreateMutexW(NULL, TRUE, L"Local\\UAReforgedLauncher");
    if (once && GetLastError() == ERROR_ALREADY_EXISTS)
        return 0; /* already running */

    static wchar_t dir[MAX_PATH * 4], ps[MAX_PATH * 2], script[MAX_PATH * 4], cmd[MAXCMD];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH * 4);
    if (n == 0 || n >= MAX_PATH * 4)
    {
        fail(L"Could not find the UA Reforged folder.");
        return 1;
    }
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash)
        *slash = 0;

    UINT s = GetSystemDirectoryW(ps, MAX_PATH);
    if (s == 0 || s >= MAX_PATH)
    {
        fail(L"Could not find the Windows system folder.");
        return 1;
    }
    wcscat(ps, L"\\WindowsPowerShell\\v1.0\\powershell.exe");
    if (GetFileAttributesW(ps) == INVALID_FILE_ATTRIBUTES)
    {
        fail(L"UA Reforged needs Windows PowerShell, which is part of Windows 10 and 11, "
             L"but it was not found on this PC.");
        return 1;
    }

    _snwprintf(script, MAX_PATH * 4, L"%ls\\launcher\\UA-Reforged.ps1", dir);
    script[MAX_PATH * 4 - 1] = 0;
    if (GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES)
    {
        fail(L"UA Reforged is incomplete (launcher\\UA-Reforged.ps1 is missing). Please install it again.");
        return 1;
    }

    int len = _snwprintf(cmd, MAXCMD,
                         L"\"%ls\" -NoLogo -NoProfile -NonInteractive -Sta -ExecutionPolicy Bypass "
                         L"-WindowStyle Hidden -File \"%ls\"%ls%ls",
                         ps, script, (args && args[0]) ? L" " : L"", args ? args : L"");
    if (len < 0 || len >= MAXCMD)
    {
        fail(L"The command line is too long.");
        return 1;
    }

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(ps, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, dir, &si, &pi))
    {
        fail(L"Could not start Windows PowerShell for UA Reforged.");
        return 1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    if (once)
        CloseHandle(once);
    return (int)rc;
}
