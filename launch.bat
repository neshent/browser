@echo off
set PATH=C:\msys64\mingw64\bin;%PATH%
set GTK_DATA_PREFIX=C:\msys64\mingw64
set GTK_EXE_PREFIX=C:\msys64\mingw64
set GTK_PATH=C:\msys64\mingw64
set XDG_DATA_DIRS=C:\msys64\mingw64\share
set GDK_PIXBUF_MODULEDIR=C:\msys64\mingw64\lib\gdk-pixbuf-2.0\2.10.0\loaders
set GDK_PIXBUF_MODULE_FILE=C:\msys64\mingw64\lib\gdk-pixbuf-2.0\2.10.0\loaders.cache
start "" "%~dp0nishant-browser.exe" %*
