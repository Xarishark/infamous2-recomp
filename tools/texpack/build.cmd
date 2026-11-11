@echo off
rem Build texpack.exe (texture pack authoring) into build\texpack.exe.
rem Needs LLVM clang-cl and the Visual Studio C++ build tools; links only Windows libraries (WIC).
setlocal
set ROOT=%~dp0..\..
set TK=%ROOT%\external\ps3recomp
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VS=%%i
if not defined VS ( echo Visual Studio C++ tools not found & exit /b 1 )
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist "%ROOT%\build\texpack-obj" mkdir "%ROOT%\build\texpack-obj"
"%ProgramFiles%\LLVM\bin\clang-cl.exe" /nologo /O2 /W3 -Wno-unused-function -D_CRT_SECURE_NO_WARNINGS ^
  -I "%TK%\include" -I "%TK%\libs\video" ^
  "%~dp0texpack.c" "%TK%\libs\video\rsx_texture_layout.c" ^
  /Fe:"%ROOT%\build\texpack.exe" /Fo:"%ROOT%\build\texpack-obj\\" || exit /b 1
echo built %ROOT%\build\texpack.exe
