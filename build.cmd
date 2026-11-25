@echo off
rem Builds Release binaries into build\ using the newest Visual Studio found by vswhere.
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cmake -G Ninja -S "%~dp0." -B "%~dp0build" -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%~dp0build"
