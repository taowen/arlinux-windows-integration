// Product shell bridge; independent of accessibility and application windows.
#include <windows.h>
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
int main() {
    SetProcessDPIAware();
    WNDCLASSW cls{}; cls.lpfnWndProc = receive; cls.lpszClassName = ARLINUX_TRAY_CLIENT;
    RegisterClassW(&cls);
    HWND client = CreateWindowW(cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream command(line); std::string operation, extra;
        arlinux_tray_request request{}; request.version = ARLINUX_TRAY_PROTOCOL;
        command >> operation;
        request.operation = operation == "list" ? ARLINUX_TRAY_LIST : operation == "open" ? ARLINUX_TRAY_OPEN : operation == "menu" ? ARLINUX_TRAY_MENU : 0;
        bool valid = request.operation && (request.operation == ARLINUX_TRAY_LIST ||
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
            std::cout << "{\"error\":\"Wine tray bridge unavailable\"}" << std::endl; continue;
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
    }
    DestroyWindow(client);
}
