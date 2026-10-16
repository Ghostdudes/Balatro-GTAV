@echo off
setlocal
set HERE=%~dp0
set CSC=C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe

if not exist "%CSC%" (
    set CSC=C:\Windows\Microsoft.NET\Framework\v4.0.30319\csc.exe
)

if not exist "%CSC%" (
    echo Error: Could not find csc.exe compiler on this machine.
    exit /b 1
)

if not exist "%HERE%..\..\releases" mkdir "%HERE%..\..\releases"

echo Compiling Installer.exe...
"%CSC%" /nologo /target:winexe /platform:anycpu /optimize+ ^
  /r:System.dll,System.Windows.Forms.dll,System.Drawing.dll ^
  /out:"%HERE%..\..\releases\Installer.exe" "%HERE%Installer.cs"

if %ERRORLEVEL% equ 0 (
    echo Successfully built: %HERE%..\..\releases\Installer.exe
) else (
    echo Build failed with error %ERRORLEVEL%
)
