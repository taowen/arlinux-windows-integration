// Product shell bridge; independent of accessibility and application windows.
#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include "protocol.h"
static std::vector<BYTE> reply;
static HWND server;
static LRESULT CALLBACK receive(HWND window, UINT message, WPARAM sender, LPARAM data) {
    if (message != WM_COPYDATA) return DefWindowProcW(window, message, sender, data);
    const auto *copy = reinterpret_cast<const COPYDATASTRUCT *>(data);
    if (reinterpret_cast<HWND>(sender) != server || copy->dwData != ARLINUX_TRAY_COPYDATA ||
        copy->cbData < sizeof(arlinux_tray_reply) || copy->cbData > sizeof(arlinux_tray_reply) +
        ARLINUX_TRAY_MAX_ICONS * sizeof(arlinux_tray_item)) return FALSE;
    const auto *bytes = static_cast<const BYTE *>(copy->lpData);
    reply.assign(bytes, bytes + copy->cbData);
    return TRUE;
}
static void quoted(const WCHAR *value) {
    int count = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    std::string text(count > 0 ? count : 1, '\0');
    if (count > 0) WideCharToMultiByte(CP_UTF8, 0, value, -1, text.data(), count, nullptr, nullptr);
    const char *hex = "0123456789abcdef";
    std::cout << '"';
    for (unsigned char c : text) {
        if (!c) break;
        if (c == '"' || c == '\\') std::cout << '\\' << c;
        else if (c < 32) std::cout << "\\u00" << hex[c >> 4] << hex[c & 15];
        else std::cout << c;
    }
    std::cout << '"';
}
static std::wstring quote(const std::wstring &value) {
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
                if (std::wstring(argv[1]) == L"--close-window") success = PostMessageW(request.window, WM_CLOSE, 0, 0);
                else {
                    const bool maximize = std::wstring(argv[1]) == L"--maximize";
                    const bool zoom = maximize && !IsZoomed(request.window);
                    ShowWindow(request.window, zoom ? SW_MAXIMIZE : SW_RESTORE);
                    BOOL activated = GetForegroundWindow() == request.window || SetForegroundWindow(request.window);
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
int main(int argc, char **argv) {
    if (argc > 1 && (strcmp(argv[1], "--run") == 0 || strcmp(argv[1], "--stop") == 0 || strcmp(argv[1], "--focus") == 0 ||
                     strcmp(argv[1], "--maximize") == 0 || strcmp(argv[1], "--close-window") == 0)) {
        int count; wchar_t **wide = CommandLineToArgvW(GetCommandLineW(), &count);
        if (!wide) return 2;
        int result = task(count, wide); LocalFree(wide); return result;
    }
    const bool show = argc == 2 && strcmp(argv[1], "--window") == 0;
    if (argc > 1 && !show) return 2;
    SetProcessDPIAware();
    WNDCLASSW cls{}; cls.lpfnWndProc = receive; cls.lpszClassName = ARLINUX_TRAY_CLIENT;
    RegisterClassW(&cls);
    HWND client = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    std::string line;
    while (show ? (line = "window", true) : static_cast<bool>(std::getline(std::cin, line))) {
        std::istringstream command(line); std::string operation, extra;
        arlinux_tray_request request{}; request.version = ARLINUX_TRAY_PROTOCOL;
        command >> operation;
        request.operation = operation == "list" ? ARLINUX_TRAY_LIST : operation == "open" ? ARLINUX_TRAY_OPEN : operation == "menu" ? ARLINUX_TRAY_MENU : operation == "window" ? ARLINUX_TRAY_WINDOW : 0;
        bool valid = request.operation && (request.operation == ARLINUX_TRAY_LIST || request.operation == ARLINUX_TRAY_WINDOW ||
            bool(command >> request.process >> request.token >> request.x >> request.y));
        if (!valid || (command >> extra)) { std::cout << "{\"error\":\"Invalid tray request\"}" << std::endl; continue; }
        server = FindWindowW(L"Shell_TrayWnd", nullptr);
        if (!server && request.operation == ARLINUX_TRAY_LIST) { std::cout << "{\"process\":0,\"icons\":[]}" << std::endl; continue; }
        reply.clear();
        COPYDATASTRUCT copy{ARLINUX_TRAY_COPYDATA, sizeof(request), &request};
        DWORD_PTR result = 0;
        if (!server || !SendMessageTimeoutW(server, WM_COPYDATA, reinterpret_cast<WPARAM>(client),
                reinterpret_cast<LPARAM>(&copy), SMTO_ABORTIFHUNG, 3000, &result) ||
            !result || reply.size() < sizeof(arlinux_tray_reply)) {
            std::cout << "{\"error\":\"Wine tray bridge unavailable\"}" << std::endl; if (show) return 1; continue;
        }
        arlinux_tray_reply header; std::memcpy(&header, reply.data(), sizeof(header));
        if (header.version != ARLINUX_TRAY_PROTOCOL || header.count > ARLINUX_TRAY_MAX_ICONS ||
            reply.size() != sizeof(header) + header.count * sizeof(arlinux_tray_item)) {
            std::cout << "{\"error\":\"Invalid tray reply\"}" << std::endl; continue;
        }
        if (request.operation != ARLINUX_TRAY_LIST) std::cout << (header.success ? "true" : "false");
        else {
            std::cout << "{\"process\":" << header.process << ",\"icons\":[";
            const char *hex = "0123456789abcdef";
            for (DWORD i = 0; i < header.count; ++i) {
                arlinux_tray_item item; std::memcpy(&item, reply.data() + sizeof(header) + i * sizeof(item), sizeof(item));
                item.title[127] = 0;
                if (i) std::cout << ',';
                std::cout << "{\"token\":" << item.token << ",\"name\":"; quoted(item.title);
                std::cout << ",\"pixels\":\"";
                for (auto c : item.pixels) std::cout << hex[c >> 4] << hex[c & 15];
                std::cout << "\"}";
            }
            std::cout << "]}";
        }
        std::cout << std::endl;
        if (show) return header.success ? 0 : 1;
    }
    DestroyWindow(client);
}
