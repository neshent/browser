@echo off
set MSYSTEM=MINGW64
set PATH=C:\msys64\mingw64\bin;C:\msys64\usr\bin;%PATH%
set GDK_BACKEND=win32
set GTK_DATA_PREFIX=C:\msys64\mingw64
set GTK_PATH=C:\msys64\mingw64
set XDG_DATA_DIRS=C:\msys64\mingw64\share
set GDK_PIXBUF_MODULE_FILE=C:\msys64\mingw64\lib\gdk-pixbuf-2.0\2.10.0\loaders.cache

cd /d "%~dp0"
start "" "%~dp0nishant-browser.exe" %*
