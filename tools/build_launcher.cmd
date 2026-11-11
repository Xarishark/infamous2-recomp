@echo off
rem Build inFamous2Launcher.exe (repo root) from launcher\inFamous2Launcher.cs with the C# compiler that
rem ships with Windows (.NET Framework 4.x). Nothing to install. Run from anywhere:
rem     tools\build_launcher.cmd
setlocal
set "ROOT=%~dp0.."
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (
    echo csc.exe not found: the .NET Framework 4.x is part of Windows 10 and 11 -- turn it on in "Windows features".
    exit /b 1
)
"%CSC%" /nologo /target:winexe /platform:anycpu /optimize+ /utf8output ^
    /out:"%ROOT%\inFamous2Launcher.exe" ^
    /reference:System.dll /reference:System.Core.dll /reference:System.Drawing.dll /reference:System.Windows.Forms.dll ^
    "%ROOT%\launcher\inFamous2Launcher.cs"
if errorlevel 1 (
    echo launcher build FAILED
    exit /b 1
)
echo built %ROOT%\inFamous2Launcher.exe
