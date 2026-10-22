# ARLinux Windows integration

Native ARM64 Windows accessibility observer and Job Object task owner for Bionic Wine. The two independent build stages share this repository and its cache. The task executable retains its existing `windows-tray.exe` artifact name, but no longer implements a tray protocol: Explorer owns the ordinary native tray window and its mouse callbacks. Linux and Android clients belong to their host products.

Build procedures: https://github.com/taowen/wine/blob/arlinux/arlinux/BUILD.md

For startup hangs, the observer's `windows-all` inventory includes hidden HWNDs,
their owning thread and native styles without invoking MSAA providers. Ordinary
`windows` still lists visible windows only. `post-null HWND` explicitly posts one
`WM_NULL` to an observed window for queue-wake isolation. It does not activate or
show the window and is never used automatically by the launcher. A successful
diagnostic wake is not a compatibility fix or normal user-journey acceptance.
