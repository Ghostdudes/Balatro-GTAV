@echo off
setlocal enabledelayedexpansion
title Balatro in GTA V - Setup Installer
color 0F

echo ===============================================================================
echo                   BALATRO IN GRAND THEFT AUTO V - INSTALLER
echo ===============================================================================
echo.
echo  This installer will copy the required mod files into your Grand Theft Auto V
echo  and Balatro directories.
echo.

set HERE=%~dp0
set MOD_FILES=%HERE%mod_files

if not exist "%MOD_FILES%" (
    if exist "%HERE%bin" set "MOD_FILES=%HERE%bin"
    if exist "%HERE%..\bin" set "MOD_FILES=%HERE%..\bin"
)

if not exist "%MOD_FILES%" (
    echo [ERROR] Missing 'bin' or 'mod_files' folder. Please keep install.bat inside the repository or unpacked release folder.
    pause
    exit /b 1
)

:: Auto-detect GTA V directory
set GTA_DIR=
if exist "D:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced\GTA5_Enhanced.exe" (
    set "GTA_DIR=D:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced"
) else if exist "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto V Enhanced\GTA5_Enhanced.exe" (
    set "GTA_DIR=C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto V Enhanced"
) else if exist "E:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced\GTA5_Enhanced.exe" (
    set "GTA_DIR=E:\SteamLibrary\steamapps\common\Grand Theft Auto V Enhanced"
) else if exist "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto V\GTA5.exe" (
    set "GTA_DIR=C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto V"
) else if exist "C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto V Balatro test\GTA5.exe" (
    set "GTA_DIR=C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto V Balatro test"
) else if exist "D:\SteamLibrary\steamapps\common\Grand Theft Auto V\GTA5.exe" (
    set "GTA_DIR=D:\SteamLibrary\steamapps\common\Grand Theft Auto V"
) else if exist "E:\SteamLibrary\steamapps\common\Grand Theft Auto V\GTA5.exe" (
    set "GTA_DIR=E:\SteamLibrary\steamapps\common\Grand Theft Auto V"
)

:: Auto-detect Balatro executable
set BALATRO_EXE=
if exist "C:\Program Files (x86)\Steam\steamapps\common\Balatro\Balatro.exe" (
    set "BALATRO_EXE=C:\Program Files (x86)\Steam\steamapps\common\Balatro\Balatro.exe"
) else if exist "D:\SteamLibrary\steamapps\common\Balatro\Balatro.exe" (
    set "BALATRO_EXE=D:\SteamLibrary\steamapps\common\Balatro\Balatro.exe"
) else if exist "E:\SteamLibrary\steamapps\common\Balatro\Balatro.exe" (
    set "BALATRO_EXE=E:\SteamLibrary\steamapps\common\Balatro\Balatro.exe"
)

:PROMPT_GTA
echo -------------------------------------------------------------------------------
if defined GTA_DIR (
    echo Detected GTA V folder:
    echo   "%GTA_DIR%"
    echo.
    set /p "USER_GTA=Press ENTER to accept this path, or paste your GTA V path: "
    if defined USER_GTA set "GTA_DIR=!USER_GTA!"
) else (
    set /p "GTA_DIR=Enter or paste your GTA V installation folder: "
)

:: Strip surrounding quotes if user entered them
set "GTA_DIR=%GTA_DIR:"=%"

set HAS_GTA_EXE=0
if exist "%GTA_DIR%\GTA5.exe" set HAS_GTA_EXE=1
if exist "%GTA_DIR%\GTA5_Enhanced.exe" set HAS_GTA_EXE=1

if "!HAS_GTA_EXE!"=="0" (
    echo.
    echo [WARNING] Could not find GTA5.exe or GTA5_Enhanced.exe inside "%GTA_DIR%".
    echo Please make sure you selected the main game directory.
    echo.
    goto PROMPT_GTA
)

echo [OK] Valid GTA V directory confirmed.
if not exist "%GTA_DIR%\dxgi.dll" if not exist "%GTA_DIR%\d3d11.dll" if not exist "%GTA_DIR%\ReShade64.asi" if not exist "%GTA_DIR%\ReShade64.dll" (
    echo [WARNING] ReShade is not detected in your GTA V folder.
    echo           ReShade with full add-on support is required for video display!
)
echo.

:PROMPT_BALATRO
echo -------------------------------------------------------------------------------
if defined BALATRO_EXE (
    echo Detected Balatro executable:
    echo   "%BALATRO_EXE%"
    echo.
    set /p "USER_BALATRO=Press ENTER to accept this path, or paste your Balatro.exe path: "
    if defined USER_BALATRO set "BALATRO_EXE=!USER_BALATRO!"
) else (
    set /p "BALATRO_EXE=Enter or paste the full path to Balatro.exe: "
)

set "BALATRO_EXE=%BALATRO_EXE:"=%"

if not exist "%BALATRO_EXE%" (
    echo.
    echo [WARNING] Could not find "%BALATRO_EXE%".
    echo Please make sure you entered the full path to Balatro.exe.
    echo.
    goto PROMPT_BALATRO
)

echo [OK] Valid Balatro executable confirmed.
echo.

echo ===============================================================================
echo  Installing files...
echo ===============================================================================

:: 1. Copy GTA V files
echo [*] Copying BalatroPassthrough.asi to GTA V...
copy /Y "%MOD_FILES%\gta\BalatroPassthrough.asi" "%GTA_DIR%\" >nul || goto FAIL

echo [*] Configuring BalatroPassthrough.ini...
powershell -Command "$content = Get-Content '%MOD_FILES%\gta\BalatroPassthrough.ini' -Raw; $content = $content -replace 'ExecutablePath=.*', ('ExecutablePath=' + [regex]::Escape('%BALATRO_EXE%').Replace('\','/')); Set-Content -Path '%GTA_DIR%\BalatroPassthrough.ini' -Value $content" >nul 2>&1
if errorlevel 1 (
    copy /Y "%MOD_FILES%\gta\BalatroPassthrough.ini" "%GTA_DIR%\" >nul
)

echo [*] Copying ReShade shader (BalatroPhone.fx)...
if not exist "%GTA_DIR%\reshade-shaders\Shaders" (
    mkdir "%GTA_DIR%\reshade-shaders\Shaders" >nul 2>&1
)
copy /Y "%MOD_FILES%\gta\reshade-shaders\Shaders\BalatroPhone.fx" "%GTA_DIR%\reshade-shaders\Shaders\" >nul || goto FAIL

if exist "%GTA_DIR%\ReShadePreset.ini" (
    findstr /C:"BalatroPhone@BalatroPhone.fx" "%GTA_DIR%\ReShadePreset.ini" >nul 2>&1
    if errorlevel 1 (
        powershell -Command "$p = '%GTA_DIR%\ReShadePreset.ini'; $t = Get-Content $p -Raw; if ($t -match 'Techniques=') { $t = $t -replace 'Techniques=', 'Techniques=BalatroPhone@BalatroPhone.fx,' }; if ($t -match 'TechniqueSorting=') { $t = $t -replace 'TechniqueSorting=', 'TechniqueSorting=BalatroPhone@BalatroPhone.fx,' }; Set-Content -Path $p -Value $t.Trim()" >nul 2>&1
    )
) else (
    (echo Techniques=BalatroPhone@BalatroPhone.fx & echo TechniqueSorting=BalatroPhone@BalatroPhone.fx) > "%GTA_DIR%\ReShadePreset.ini"
)

:: 2. Copy Balatro mod files to %APPDATA%
set "BALATRO_MOD_DIR=%APPDATA%\Balatro\Mods\BalatroPassthrough"
echo [*] Creating Balatro mod folder: %BALATRO_MOD_DIR%...
if not exist "%BALATRO_MOD_DIR%" (
    mkdir "%BALATRO_MOD_DIR%" >nul 2>&1
)

echo [*] Copying Balatro bridge scripts...
copy /Y "%MOD_FILES%\balatro\BalatroPassthrough\balatro_bridge.lua" "%BALATRO_MOD_DIR%\" >nul || goto FAIL
copy /Y "%MOD_FILES%\balatro\BalatroPassthrough\lovely.toml" "%BALATRO_MOD_DIR%\" >nul || goto FAIL

echo.
echo ===============================================================================
echo                      INSTALLATION COMPLETED SUCCESSFULLY!
echo ===============================================================================
echo.
echo  HOW TO PLAY:
echo    1. Launch Grand Theft Auto V.
echo    2. Press [F9] in-game to pull up the phone (Balatro launches automatically).
echo    3. Press [F10] to cycle display modes:
echo         - Mode 0: In-engine phone (HUD in 3rd person / in-hand in 1st person)
echo         - Mode 1: Large centered phone
echo         - Mode 2: Fullscreen mode
echo    4. Use your mouse to click and drag cards.
echo    5. Press [F9] again to close the phone at any time.
echo.
echo ===============================================================================
pause
exit /b 0

:FAIL
echo.
echo [ERROR] Installation failed while copying files.
echo Please ensure Grand Theft Auto V and Balatro are closed and try again.
pause
exit /b 1
