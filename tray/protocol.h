/* ARLinux shell tray protocol, shared by Wine Explorer and its client. */
#ifndef ARLINUX_TRAY_PROTOCOL_H
#define ARLINUX_TRAY_PROTOCOL_H
#define ARLINUX_TRAY_PROTOCOL 1
#define ARLINUX_TRAY_COPYDATA 0x41525431u
#define ARLINUX_TRAY_CLIENT L"ARLinux.Tray.Client.v1"
#define ARLINUX_TRAY_MAX_ICONS 128
#define ARLINUX_TRAY_ICON_SIZE 32
#define ARLINUX_TRAY_LIST 1
#define ARLINUX_TRAY_OPEN 2
#define ARLINUX_TRAY_MENU 3
struct arlinux_tray_request {
    DWORD version, operation, process, reserved;
    ULONGLONG token;
    LONG x, y;
};
struct arlinux_tray_reply { DWORD version, success, process, count; };
struct arlinux_tray_item {
    ULONGLONG token;
    WCHAR title[128];
    BYTE pixels[ARLINUX_TRAY_ICON_SIZE * ARLINUX_TRAY_ICON_SIZE * 4]; /* RGBA */
};
#endif
