@echo off
call "D:\Visual Studio Community\VC\Auxiliary\Build\vcvars32.bat" >nul
cd /d "%~dp0"
cl /nologo /O2 /W3 /LD /MT fastboot.c /Fe:DIFastBoot.asi /link user32.lib
