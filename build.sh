#!/bin/sh
# Cross-compile TouchMT with MinGW-w64 (Debian/Ubuntu: apt install gcc-mingw-w64)
set -e
CFLAGS="-O2 -Wall -Wextra -mwindows -static -static-libgcc"
LIBS="-luser32 -lgdi32"
x86_64-w64-mingw32-gcc $CFLAGS -o touchmt-x64.exe touchmt.c $LIBS \
  -Wl,--major-subsystem-version,5,--minor-subsystem-version,2,--major-os-version,5,--minor-os-version,2
i686-w64-mingw32-gcc $CFLAGS -o touchmt-x86.exe touchmt.c $LIBS \
  -Wl,--major-subsystem-version,5,--minor-subsystem-version,1,--major-os-version,5,--minor-os-version,1
x86_64-w64-mingw32-strip touchmt-x64.exe
i686-w64-mingw32-strip touchmt-x86.exe
echo "Built touchmt-x64.exe and touchmt-x86.exe"
