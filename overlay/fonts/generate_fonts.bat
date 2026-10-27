@echo off
setlocal enabledelayedexpansion

where binary_to_compressed_c >nul 2>&1 || (
    echo the program "binary_to_compressed_c" was not found in current location or in %%PATH%%
    goto FAILURE
)

set "output=fonts.hpp"

if exist "%output%" (
    del /q "%output%"
)

echo #pragma once> "%output%"

set count=0
for %%f in (*.ttf *.otf) do (
    rem Set "symbol" to file name without extension
    set "symbol=%%~nf"
    rem Replace all dashes with underscores
    set "symbol=!symbol:-=_!"
    rem Replace all spaces with underscores
    set "symbol=!symbol: =_!"
    echo !symbol!| findstr /r /c:"^[A-Za-z_][A-Za-z0-9_]*$" >nul
    if errorlevel 1 (
        echo Error parsing "%%f": names can only contains characters from A-Z, 0-9, underscores dashes and spaces and cannot start with a digit>&2
        goto FAILURE
    ) else (
        binary_to_compressed_c "%%f" !symbol! >> "%output%" || (
            echo Command "binary_to_compressed_c "%%f" !symbol!" failed>&2
            goto FAILURE
        )
        echo "%%f" done
        echo Symbols:
        echo     !symbol!_compressed_size
        echo     !symbol!_compressed_data
        set /a count+=1
    )
)

if count equ 0 (
    echo No .ttf or .otf font files found!>&2
    goto FAILURE
) else (
    powershell -Command "(Get-Content "%output%" -Raw).Replace('static const','inline constexpr') | Set-Content -NoNewline "%output%""
    echo Done !count! font/s.
    pause
    exit
)

:FAILURE
if exist "%output%" (
    del /q "%output%"
)
pause
exit /b 1
