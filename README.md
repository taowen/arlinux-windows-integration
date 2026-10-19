# ARLinux Windows integration

Native ARM64 Windows accessibility observer and Job Object task owner for Bionic Wine. The two independent build stages share this repository and its cache. The task executable retains its existing `windows-tray.exe` artifact name, but no longer implements a tray protocol: Explorer owns the ordinary native tray window and its mouse callbacks. Linux and Android clients belong to their host products.

Build procedures: https://github.com/taowen/wine/blob/arlinux/arlinux/BUILD.md
