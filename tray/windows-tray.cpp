// Windows launch ownership and task control through named Job Objects.
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <string>
#include <vector>
static std::wstring quote(const std::wstring &value) {
    // Some Windows programs parse switches directly from the raw command line.
    if (!value.empty() && value.find_first_of(L" \t\r\n\v\f\"") == std::wstring::npos) return value;
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : value) {
        if (c == L'\\') { ++slashes; continue; }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') result += L'\\';
        result += c;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
struct focus_request { HANDLE job; HWND window; LONGLONG area; DWORD query_error; bool visible; };
static BOOL CALLBACK find_owned_window(HWND window, LPARAM param) {
    auto *request = reinterpret_cast<focus_request *>(param);
    DWORD pid; GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, pid);
    BOOL member = FALSE;
    if (process) {
        if (!IsProcessInJob(process, request->job, &member)) request->query_error = GetLastError();
        CloseHandle(process);
    } else request->query_error = GetLastError();
    if (!member || GetWindowTextLengthW(window) == 0 || GetWindow(window, GW_OWNER) ||
        (GetWindowLongPtrW(window, GWL_EXSTYLE) & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE))) return TRUE;
    RECT rect{}; GetWindowRect(window, &rect);
    LONGLONG area = static_cast<LONGLONG>(rect.right - rect.left) * (rect.bottom - rect.top);
    bool visible = IsWindowVisible(window) != FALSE;
    if (!request->window || (visible && !request->visible) ||
        (visible == request->visible && area > request->area)) {
        request->window = window; request->area = area; request->visible = visible;
    }
    return TRUE;
}
static HWND activation_window(HWND window) {
    // A disabled owner must yield focus to its active modal popup. Restoring
    // the owner itself can put it above the dialog while leaving it disabled.
    while (!IsWindowEnabled(window)) {
        HWND popup = GetLastActivePopup(window);
        if (!popup || popup == window || !IsWindowVisible(popup)) break;
        window = popup;
    }
    return window;
}
static int task(int argc, wchar_t **argv) {
    if (argc < 3 || wcslen(argv[2]) != 36 || wcsspn(argv[2], L"0123456789abcdef-") != 36) return 2;
    std::wstring name = L"Local\\ARLinux.Task." + std::wstring(argv[2]);
    if (std::wstring(argv[1]) != L"--run") {
        HANDLE job = OpenJobObjectW(JOB_OBJECT_QUERY | JOB_OBJECT_TERMINATE, FALSE, name.c_str());
        if (!job) return 3;
        BOOL success = FALSE;
        if (std::wstring(argv[1]) == L"--stop") success = TerminateJobObject(job, 1);
        else {
            focus_request request{job, nullptr, 0, 0, false};
            EnumWindows(find_owned_window, reinterpret_cast<LPARAM>(&request));
            if (request.window) {
                if (std::wstring(argv[1]) == L"--close-window" && IsWindowEnabled(request.window))
                    success = PostMessageW(request.window, WM_CLOSE, 0, 0);
                else {
                    const bool maximize = std::wstring(argv[1]) == L"--maximize";
                    const bool zoom = maximize && !IsZoomed(request.window);
                    if (maximize) ShowWindow(request.window, zoom ? SW_MAXIMIZE : SW_RESTORE);
                    else if (IsIconic(request.window)) ShowWindow(request.window, SW_RESTORE);
                    else if (!IsWindowVisible(request.window)) ShowWindow(request.window, SW_SHOW);
                    HWND target = activation_window(request.window);
                    BOOL activated = GetForegroundWindow() == target || SetForegroundWindow(target);
                    if (!activated) std::fprintf(stderr, "Task foreground activation was denied\n");
                    // Foreground policy is independent of a completed size change.
                    // ShowWindow's return value is prior visibility, not success.
                    success = maximize ? (IsZoomed(request.window) != FALSE) == zoom : activated;
                    std::fprintf(stderr, "Task window control hwnd=%p style=%08lx exstyle=%08lx resized=%d activated=%d\n",
                                 static_cast<void *>(request.window), GetWindowLongW(request.window, GWL_STYLE),
                                 GetWindowLongW(request.window, GWL_EXSTYLE), maximize && success, activated);
                }
            } else std::fprintf(stderr, "Task has no normal owned window (process query error %lu)\n", request.query_error);
        }
        CloseHandle(job);
        return success ? 0 : 4;
    }
    if (argc < 4) return 2;
    HANDLE job = CreateJobObjectW(nullptr, name.c_str());
    if (!job || GetLastError() == ERROR_ALREADY_EXISTS) { if (job) CloseHandle(job); return 5; }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        CloseHandle(job); return 6;
    }
    std::wstring command;
    for (int i = 3; i < argc; ++i) { if (!command.empty()) command += L' '; command += quote(argv[i]); }
    std::vector<wchar_t> buffer(command.begin(), command.end()); buffer.push_back(0);
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED,
                        nullptr, nullptr, &startup, &process)) { CloseHandle(job); return 7; }
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        TerminateProcess(process.hProcess, 1); WaitForSingleObject(process.hProcess, INFINITE);
        CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(job); return 8;
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        TerminateJobObject(job, 1); CloseHandle(process.hThread); CloseHandle(process.hProcess); CloseHandle(job); return 9;
    }
    CloseHandle(process.hThread);
    DWORD status = 1;
    for (;;) {
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)) {
            TerminateJobObject(job, 1); break;
        }
        if (!accounting.ActiveProcesses) { GetExitCodeProcess(process.hProcess, &status); break; }
        Sleep(100);
    }
    CloseHandle(process.hProcess); CloseHandle(job);
    return static_cast<int>(status);
}
int main() {
    int count;
    wchar_t **arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return 2;
    const std::wstring operation = count > 1 ? arguments[1] : L"";
    if (operation == L"--prepare-desktop" && count == 2) {
        // Initializing User32's message-window host starts Wine's shared
        // Explorer before any application is assigned to a Job Object.
        HWND window = CreateWindowW(L"STATIC", L"", 0, 0, 0, 0, 0,
                                    HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        const bool ready = window && FindWindowW(L"Shell_TrayWnd", nullptr);
        if (window) DestroyWindow(window);
        LocalFree(arguments);
        return ready ? 0 : 10;
    }
    const bool valid = operation == L"--run" || operation == L"--stop" ||
                       operation == L"--focus" || operation == L"--maximize" ||
                       operation == L"--close-window";
    const int result = valid ? task(count, arguments) : 2;
    LocalFree(arguments);
    return result;
}
