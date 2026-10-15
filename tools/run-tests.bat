@echo off
rem Runs the tests of one build: tools\run-tests.bat [Release^|Debug] [Win32^|x64]
rem (build first: premake5 vs2026, then msbuild build\FusionFixUtils.slnx with the same configuration and platform)
setlocal
set "CFG=%~1"
if "%CFG%"=="" set "CFG=Release"
set "PLAT=%~2"
if "%PLAT%"=="" set "PLAT=Win32"
set "ROOT=%~dp0.."
set "BIN=%ROOT%\bin\%PLAT%\%CFG%"
set FAILED=0

if not exist "%BIN%\FusionFixUtilsTests.exe" (
    echo %BIN%\FusionFixUtilsTests.exe is missing: build %CFG% %PLAT% first
    exit /b 1
)

pushd "%BIN%"
echo == FusionFixUtilsTests %CFG% %PLAT%
"%BIN%\FusionFixUtilsTests.exe" || set FAILED=1
echo == AsiSettingsTests %CFG% %PLAT%
"%BIN%\AsiSettingsTests.exe" || set FAILED=1
echo == asi-settings-validate on the test fixture
"%BIN%\asi-settings-validate.exe" "%ROOT%\asi_settings\tests\fixtures\NFSUnderground.WidescreenFix.json" "%ROOT%\asi_settings\tests\fixtures\NFSUnderground.WidescreenFix.en.json" || set FAILED=1
echo == asi-settings-validate on the validator's cases (good-*: no errors, bad-*: errors)
set "CASES=%ROOT%\asi_settings\tests\validator"
for %%f in ("%CASES%\good-*.json") do (
    "%BIN%\asi-settings-validate.exe" "%%f" "%CASES%\text.en.json" >nul || (echo FAILED: %%~nxf has errors& set FAILED=1)
)
for %%f in ("%CASES%\bad-*.json") do (
    "%BIN%\asi-settings-validate.exe" "%%f" "%CASES%\text.en.json" >nul && (echo FAILED: %%~nxf passed& set FAILED=1)
)
"%BIN%\asi-settings-validate.exe" "%CASES%\good-1.0.json" "%CASES%\text.en.json" "%CASES%\text.en_US.json" >nul && (echo FAILED: the language tag en_US passed& set FAILED=1)
popd

if "%FAILED%"=="1" (
    echo FAILED: %CFG% %PLAT%
    exit /b 1
)
echo All tests passed: %CFG% %PLAT%
exit /b 0
