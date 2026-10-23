@echo off
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul 2>&1
if errorlevel 1 ( echo [X] vcvars32 failed & exit /b 1 )
set INCLUDE=H:\Seven Kingdoms 2 HD\_work\sdk\microsoft.windows.sdk.cpp\c\Include\10.0.28000.0\ucrt;H:\Seven Kingdoms 2 HD\_work\sdk\microsoft.windows.sdk.cpp\c\Include\10.0.28000.0\shared;H:\Seven Kingdoms 2 HD\_work\sdk\microsoft.windows.sdk.cpp\c\Include\10.0.28000.0\um;H:\Seven Kingdoms 2 HD\_work\sdk\microsoft.windows.sdk.cpp\c\Include\10.0.28000.0\winrt;%INCLUDE%
set LIB=H:\Seven Kingdoms 2 HD\_work\sdk\microsoft.windows.sdk.cpp.x86\c\ucrt\x86;H:\Seven Kingdoms 2 HD\_work\sdk\microsoft.windows.sdk.cpp.x86\c\um\x86;%LIB%
cd /d "%~dp0"
if exist obj rmdir /s /q obj
mkdir obj
set CFLAGS=/nologo /c /EHsc /std:c++17 /W3 /utf-8 /D_CRT_SECURE_NO_WARNINGS /O2 /Oi
echo [1/2] compiling...
cl %CFLAGS% /Foobj\ pch.cpp core.cpp hooks.cpp cjk.cpp diag.cpp ai.cpp dllmain.cpp
if errorlevel 1 ( echo [X] COMPILE FAILED & exit /b 1 )
echo [2/2] linking...
link /nologo /DLL /OUT:sk2_zh_new.asi /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /MACHINE:X86 /RELEASE obj\pch.obj obj\core.obj obj\hooks.obj obj\cjk.obj obj\diag.obj obj\ai.obj obj\dllmain.obj user32.lib gdi32.lib
if errorlevel 1 ( echo [X] LINK FAILED & exit /b 1 )
echo [OK] built sk2_zh_new.asi