@echo off
REM ===========================================================================
REM  Build this plugin on Windows.
REM
REM  Most people writing Conan-Api plugins are on Windows, and the .sh next to
REM  this file is a bash script that does them no good. This one builds the same
REM  source with whichever compiler you already have.
REM
REM  It tries, in order:
REM     1. cl.exe   — Visual Studio. Open "x64 Native Tools Command Prompt"
REM                   (Start menu, under Visual Studio) and run this from there.
REM     2. g++      — MinGW-w64 for Windows, if it's on your PATH.
REM
REM  Neither one? Install Visual Studio Community (free) with the "Desktop
REM  development with C++" workload. That's the shortest path.
REM
REM  You can also just make a Visual Studio project by hand: New Project ->
REM  Dynamic-Link Library (DLL), platform x64, add the .cpp, point
REM  "Additional Include Directories" at the SDK's include folder, and set
REM  Runtime Library to /MT. This script only saves you those clicks.
REM ===========================================================================
setlocal enabledelayedexpansion

REM ── which .cpp are we building ─────────────────────────────────────────────
REM
REM The folder holds exactly one plugin source, named after the folder. Finding
REM it rather than hard-coding a name keeps this file identical in every
REM example, so there's one thing to fix if it ever needs fixing.
set "HERE=%~dp0"
for %%F in ("%HERE%*.cpp") do set "SRC=%%~nxF"
if not defined SRC (
    echo   x no .cpp found in this folder.
    exit /b 1
)
set "NAME=%SRC:~0,-4%"

REM ── where the headers are ─────────────────────────────────────────────────
REM
REM Searched for, never assumed: this same file sits in the development tree and
REM in the SDK people download, and those have different shapes.
REM
REM CONAN_SDK_INCLUDE comes first and outranks every guess below. This plugin
REM lives in a repository of its own, so it can be cloned with the SDK unpacked
REM anywhere on the disk — and then there is no path to walk up to. Whoever
REM downloaded the two separately sets one variable instead of editing a script.
set "INC="
if defined CONAN_SDK_INCLUDE (
    if exist "%CONAN_SDK_INCLUDE%\Conan\ConanPluginApi.h" set "INC=%CONAN_SDK_INCLUDE%"
)
if not defined INC (
    for %%D in ("%HERE%include" "%HERE%..\include" "%HERE%..\..\include" ^
                "%HERE%..\..\api\include" "%HERE%..\..\..\include") do (
        if not defined INC if exist "%%~fD\Conan\ConanPluginApi.h" set "INC=%%~fD"
    )
)
if not defined INC (
    echo   x could not find Conan\ConanPluginApi.h
    echo.
    echo     Searched inside this folder and up to three levels above it, in
    echo     include\ and api\include\.
    echo.
    echo     If you downloaded the SDK separately, point at it:
    echo         set CONAN_SDK_INCLUDE=C:\path\to\Conan-Api-SDK\include
    echo         compilar.bat
    exit /b 1
)

echo == building %NAME%.dll ==
echo    source:  %SRC%
echo    headers: %INC%
echo.

REM ── 1. Visual Studio ──────────────────────────────────────────────────────
REM
REM /MT and not /MD: without it your DLL needs the Visual C++ redistributable
REM installed on whatever machine runs the server. That machine is often a
REM rented box or a container under Wine, where it isn't — and the symptom is
REM LoadLibrary failing with a generic code that explains nothing.
where cl.exe >nul 2>&1
if %errorlevel%==0 (
    echo    compiler: cl.exe ^(Visual Studio^)
    cl.exe /nologo /LD /MT /O2 /EHsc /std:c++17 ^
        /I "%INC%" "%HERE%%SRC%" ^
        /link /OUT:"%HERE%%NAME%.dll"
    if errorlevel 1 goto :failed
    goto :done
)

REM ── 2. MinGW-w64 ──────────────────────────────────────────────────────────
REM
REM -static-libgcc -static-libstdc++ is MinGW's equivalent of /MT, and it's
REM there for the same reason.
REM
REM -Wl,--no-insert-timestamp makes the build REPRODUCIBLE: without it MinGW
REM stamps the build time into the PE header, so two builds of the same source
REM come out with different hashes and "check the hash" stops meaning anything.
where g++.exe >nul 2>&1
if %errorlevel%==0 (
    echo    compiler: g++ ^(MinGW-w64^)
    g++ -std=c++17 -O2 -shared ^
        -I "%INC%" -o "%HERE%%NAME%.dll" "%HERE%%SRC%" ^
        -static-libgcc -static-libstdc++ ^
        -Wl,--exclude-all-symbols ^
        -Wl,--no-insert-timestamp
    if errorlevel 1 goto :failed
    goto :done
)

echo   x no C++ compiler found.
echo.
echo     For Visual Studio: open the "x64 Native Tools Command Prompt for VS"
echo     from the Start menu and run this script from there. Running it from an
echo     ordinary cmd window won't find cl.exe.
echo.
echo     For MinGW-w64: make sure g++.exe is on your PATH.
exit /b 1

:failed
echo.
echo   x the build FAILED. The compiler's message is above.
exit /b 1

:done
REM Clean up what the compiler leaves behind, so the folder holds the source
REM and the DLL and nothing else.
del /q "%HERE%*.obj" "%HERE%*.exp" "%HERE%*.lib" 2>nul
echo.
echo   OK  %HERE%%NAME%.dll
echo.
echo   Copy the WHOLE folder into your server's
echo      ConanSandbox\Binaries\Win64\Conan-Api\Plugins\
echo   and restart. The folder has a HYPHEN: Conan-Api, never ConanApi.
exit /b 0
