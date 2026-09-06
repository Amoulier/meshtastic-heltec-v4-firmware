@ECHO OFF
SETLOCAL EnableDelayedExpansion
TITLE Meshtastic device-install

SET "SCRIPT_NAME=%~nx0"
SET "DEBUG=0"
SET "PYTHON="
SET "FILENAME="
SET "ESPTOOL_PORT="
SET "ESPTOOL_BAUD=115200"
SET "ESPTOOL_CMD="
SET "LOGCOUNTER=0"
SET "BPS_RESET=0"
@REM This Heltec-only fork uses default_16MB.csv. Never guess destructive offsets.
SET "EXPECTED_FACTORY_SIZE=0x650000"
SET "EXPECTED_NVS_OFFSET=0x9000"
SET "EXPECTED_NVS_SIZE=0x5000"
SET "EXPECTED_OTADATA_OFFSET=0xe000"
SET "EXPECTED_OTADATA_SIZE=0x2000"
SET "EXPECTED_APP0_OFFSET=0x10000"
SET "EXPECTED_APP0_SIZE=0x640000"
SET "EXPECTED_OTA_OFFSET=0x650000"
SET "EXPECTED_OTA_SIZE=0x640000"
SET "EXPECTED_OTA_SHA256=3e62c5451afda604bac372444f058fc689dd588a87772ad4be90c228e04c1995"
SET "EXPECTED_SPIFFS_OFFSET=0xc90000"
SET "EXPECTED_SPIFFS_SIZE=0x360000"
SET "EXPECTED_COREDUMP_OFFSET=0xFF0000"
SET "EXPECTED_COREDUMP_SIZE=0x10000"

GOTO getopts
:help
ECHO Flash image file to device, but first erasing and writing system information.
ECHO.
ECHO Usage: %SCRIPT_NAME% -f filename [-p PORT] [-P python] [--1200bps-reset]
ECHO.
ECHO Options:
ECHO     -f filename      A Heltec V4 Standard or Solar Router .factory.bin. (required)
ECHO                      Its metadata, OTA loader and LittleFS image must be beside it.
ECHO     -p PORT          Select COM1 through COM256. Required when flashing.
ECHO     -P python        Specify alternate python interpreter to use to invoke esptool. (default: python)
ECHO                      If supplied the script will use python.
ECHO                      If not supplied the script will try to find esptool in Path.
ECHO     --1200bps-reset  Attempt to place the device in correct mode. (1200bps Reset)
ECHO                      Some hardware requires this twice.
ECHO.
ECHO Example: %SCRIPT_NAME% -p COM17 --1200bps-reset
ECHO Example: %SCRIPT_NAME% -f firmware-heltec-v4-standard-VERSION.factory.bin -p COM11
EXIT /B 0

:version
ECHO Meshtastic Heltec V4 profile clean installer
EXIT /B 0

:getopts
IF "%~1"=="" GOTO endopts
IF /I "%~1"=="-?" GOTO help
IF /I "%~1"=="-h" GOTO help
IF /I "%~1"=="--help" GOTO help
IF /I "%~1"=="-v" GOTO version
IF /I "%~1"=="--version" GOTO version
IF /I "%~1"=="--debug" (
    SET "DEBUG=1"
    CALL :LOG_MESSAGE DEBUG "DEBUG mode: enabled."
    SHIFT
    GOTO getopts
)
IF /I "%~1"=="--1200bps-reset" (
    SET "BPS_RESET=1"
    SHIFT
    GOTO getopts
)
IF /I "%~1"=="-f" GOTO set-filename
IF "%~1"=="-p" GOTO set-port
IF /I "%~1"=="--port" GOTO set-port
IF "%~1"=="-P" GOTO set-python
CALL :LOG_MESSAGE ERROR "Unknown argument: %~1"
EXIT /B 1

:set-filename
SETLOCAL DisableDelayedExpansion
SET "OPTION_VALUE=%~2"
IF NOT DEFINED OPTION_VALUE (
    ENDLOCAL
    CALL :LOG_MESSAGE ERROR "-f requires a filename."
    EXIT /B 1
)
ENDLOCAL & SET "FILENAME=%OPTION_VALUE%"
SHIFT
SHIFT
GOTO getopts

:set-port
SETLOCAL DisableDelayedExpansion
SET "OPTION_VALUE=%~2"
IF NOT DEFINED OPTION_VALUE (
    ENDLOCAL
    CALL :LOG_MESSAGE ERROR "%~1 requires a port."
    EXIT /B 1
)
ENDLOCAL & SET "ESPTOOL_PORT=%OPTION_VALUE%"
SHIFT
SHIFT
GOTO getopts

:set-python
SETLOCAL DisableDelayedExpansion
SET "OPTION_VALUE=%~2"
IF NOT DEFINED OPTION_VALUE (
    ENDLOCAL
    CALL :LOG_MESSAGE ERROR "-P requires a Python interpreter."
    EXIT /B 1
)
ENDLOCAL & SET "PYTHON=%OPTION_VALUE%"
SHIFT
SHIFT
GOTO getopts
:endopts

IF DEFINED PYTHON (
    powershell -NoProfile -NonInteractive -Command ^
        "$p = $env:PYTHON; if ($p.Contains([char]33) -or $p.Contains([char]94) " ^
        "-or $p.IndexOfAny([char[]]'*?') -ge 0) { exit 1 }; " ^
        "$commands = @(Get-Command -Name $p -CommandType Application -ErrorAction SilentlyContinue); " ^
        "if ($commands.Count -lt 1) { exit 1 }"
    IF !ERRORLEVEL! NEQ 0 (
        CALL :LOG_MESSAGE ERROR "-P must resolve to a literal Python executable; wildcards, carets and exclamation marks are forbidden."
        EXIT /B 1
    )
)

IF NOT DEFINED ESPTOOL_PORT (
    IF %BPS_RESET% EQU 0 (
        CALL :LOG_MESSAGE ERROR "An explicit -p COM port is required before erase or flash."
        EXIT /B 1
    )
) ELSE (
    powershell -NoProfile -NonInteractive -Command ^
        "$p = $env:ESPTOOL_PORT; if ($p -notmatch '^COM[1-9][0-9]{0,2}$' " ^
        "-or [int]$p.Substring(3) -gt 256) { exit 1 }"
    IF !ERRORLEVEL! NEQ 0 (
        CALL :LOG_MESSAGE ERROR "Port must be a dedicated serial port from COM1 through COM256."
        EXIT /B 1
    )
)

IF %BPS_RESET% EQU 1 GOTO skip-filename

CALL :LOG_MESSAGE DEBUG "Checking FILENAME parameter..."
IF "__!FILENAME!__"=="____" (
    CALL :LOG_MESSAGE DEBUG "Missing -f filename input."
    CALL :LOG_MESSAGE ERROR "A factory firmware filename is required."
    EXIT /B 1
) ELSE (
    CALL :LOG_MESSAGE DEBUG "Filename: !FILENAME!"
)

powershell -NoProfile -NonInteractive -Command ^
    "$p = $env:FILENAME; if ([string]::IsNullOrWhiteSpace($p) -or $p.Contains([char]33) -or $p.Contains([char]94) " ^
    "-or $p.IndexOfAny([char[]]'*?') -ge 0 -or -not (Test-Path -LiteralPath $p -PathType Leaf)) { exit 1 }"
IF !ERRORLEVEL! NEQ 0 (
    CALL :LOG_MESSAGE ERROR "Factory path must be one literal file; wildcards, carets and exclamation marks are forbidden."
    EXIT /B 1
)
CALL :LOG_MESSAGE DEBUG "Checking if !FILENAME! exists..."
IF NOT EXIST "!FILENAME!" (
    CALL :LOG_MESSAGE ERROR "File does not exist: !FILENAME!. Terminating."
    EXIT /B 1
)

FOR %%F IN ("!FILENAME!") DO (
    SET "FACTORY_PATH=%%~fF"
    SET "FIRMWARE_DIR=%%~dpF"
    SET "FACTORY_BASENAME=%%~nxF"
)
SET "PROGNAME=!FACTORY_BASENAME:~0,-12!"
SET "METAFILE=!FIRMWARE_DIR!!PROGNAME!.mt.json"
SET "OTA_BASENAME=mt-esp32s3-ota.bin"
SET "OTA_PATH=!FIRMWARE_DIR!!OTA_BASENAME!"
SET "SPIFFS_BASENAME=littlefs-!PROGNAME:firmware-=!.bin"
SET "SPIFFS_PATH=!FIRMWARE_DIR!!SPIFFS_BASENAME!"

IF NOT EXIST "!METAFILE!" (
    CALL :LOG_MESSAGE ERROR "Required metadata file is missing: !METAFILE!"
    EXIT /B 1
)
IF NOT EXIST "!OTA_PATH!" (
    CALL :LOG_MESSAGE ERROR "Required OTA loader is missing: !OTA_PATH!"
    EXIT /B 1
)
IF NOT EXIST "!SPIFFS_PATH!" (
    CALL :LOG_MESSAGE ERROR "Required LittleFS image is missing: !SPIFFS_PATH!"
    EXIT /B 1
)
FOR %%F IN ("!METAFILE!") DO IF %%~zF LEQ 0 (
    CALL :LOG_MESSAGE ERROR "Metadata file is empty: !METAFILE!."
    EXIT /B 1
)

@REM A factory install erases flash. Validate every destructive input, its
@REM partition, manifest row, byte count, and MD5 before reaching erase_flash.
powershell -NoProfile -NonInteractive -Command ^
    "$ErrorActionPreference = 'Stop'; " ^
    "$m = Get-Content -LiteralPath $env:METAFILE -Raw | ConvertFrom-Json; " ^
    "if ([string]$m.platformioTarget -cnotin @('heltec-v4-standard','heltec-v4-solar-router') " ^
    "-or [string]$m.mcu -cne 'esp32s3' -or [string]$m.hwModelSlug -cne 'HELTEC_V4' " ^
    "-or [string]$m.partitionScheme -cne '16MB' " ^
    "-or [string]$m.repo -cne 'Amoulier/meshtastic-heltec-v4-firmware') " ^
    "{ throw 'metadata target, MCU, or hardware model is invalid' }; " ^
    "$version = [string]$m.version; if ($m.version -isnot [string] -or $version -cnotmatch '^[0-9A-Za-z][0-9A-Za-z._-]*$') " ^
    "{ throw 'metadata version is invalid' }; " ^
    "$expectedName = 'firmware-' + [string]$m.platformioTarget + '-' + $version + '.factory.bin'; " ^
    "if ($env:FACTORY_BASENAME -cne $expectedName) " ^
    "{ throw 'factory filename does not match the manifest target' }; " ^
    "$parts = @($m.part); if ($parts.Count -ne 6) { throw 'partition map must contain exactly six rows' }; " ^
    "if (@($parts.name | Sort-Object -Unique).Count -ne $parts.Count " ^
    "-or @($parts.offset | Sort-Object -Unique).Count -ne $parts.Count) { throw 'partition map is duplicated' }; " ^
    "function Confirm-Part($name,$type,$subtype,$offset,$size) { " ^
    "$rows = @($parts | Where-Object { [string]$_.name -ceq $name -or [string]$_.subtype -ceq $subtype }); " ^
    "if ($rows.Count -ne 1) { throw ('partition is absent or duplicated: ' + $name) }; $p = $rows[0]; " ^
    "if ([string]$p.name -cne $name -or [string]$p.type -cne $type " ^
    "-or [string]$p.subtype -cne $subtype -or [string]$p.offset -cne $offset " ^
    "-or [string]$p.size -cne $size -or -not ($p.PSObject.Properties.Name -contains 'flags') " ^
    "-or [string]$p.flags -cne '') { throw ('partition definition is invalid: ' + $name) } }; " ^
    "Confirm-Part 'nvs' 'data' 'nvs' $env:EXPECTED_NVS_OFFSET $env:EXPECTED_NVS_SIZE; " ^
    "Confirm-Part 'otadata' 'data' 'ota' $env:EXPECTED_OTADATA_OFFSET $env:EXPECTED_OTADATA_SIZE; " ^
    "Confirm-Part 'app0' 'app' 'ota_0' $env:EXPECTED_APP0_OFFSET $env:EXPECTED_APP0_SIZE; " ^
    "Confirm-Part 'app1' 'app' 'ota_1' $env:EXPECTED_OTA_OFFSET $env:EXPECTED_OTA_SIZE; " ^
    "Confirm-Part 'spiffs' 'data' 'spiffs' $env:EXPECTED_SPIFFS_OFFSET $env:EXPECTED_SPIFFS_SIZE; " ^
    "Confirm-Part 'coredump' 'data' 'coredump' $env:EXPECTED_COREDUMP_OFFSET $env:EXPECTED_COREDUMP_SIZE; " ^
    "$files = @($m.files); if ($files.Count -eq 0) { throw 'manifest file table is missing' }; " ^
    "if (@($files.name | Sort-Object -Unique).Count -ne $files.Count) { throw 'manifest filenames are duplicated' }; " ^
    "function Confirm-File($path,$name,$part,$maximumHex) { " ^
    "$rows = @($files | Where-Object { [string]$_.name -ceq $name }); " ^
    "if ($rows.Count -ne 1) { throw ('manifest file is absent or duplicated: ' + $name) }; $e = $rows[0]; " ^
    "if ($part -ceq '') { if ($e.PSObject.Properties.Name -contains 'part_name') " ^
    "{ throw ('unexpected partition assignment: ' + $name) } } " ^
    "elseif ([string]$e.part_name -cne $part) { throw ('invalid partition assignment: ' + $name) }; " ^
    "$md5 = [string]$e.md5; $bytesText = [string]$e.bytes; " ^
    "if ($e.md5 -isnot [string] -or $e.bytes -is [string] " ^
    "-or $md5 -cnotmatch '^[0-9a-f]{32}$' -or $bytesText -cnotmatch '^[1-9][0-9]*$') " ^
    "{ throw ('invalid hash or byte count: ' + $name) }; " ^
    "$expectedBytes = [int64]::Parse($bytesText, [Globalization.CultureInfo]::InvariantCulture); " ^
    "$maximum = [Convert]::ToInt64(($maximumHex -replace '^0x',''), 16); " ^
    "$item = Get-Item -LiteralPath $path; if ($item.PSIsContainer -or $item.Name -cne $name " ^
    "-or $item.Length -ne $expectedBytes -or $item.Length -gt $maximum) " ^
    "{ throw ('file size is invalid or exceeds its partition: ' + $name) }; " ^
    "$actualMd5 = (Get-FileHash -LiteralPath $path -Algorithm MD5).Hash.ToLowerInvariant(); " ^
    "if ($actualMd5 -cne $md5) { throw ('file MD5 does not match metadata: ' + $name) } }; " ^
    "Confirm-File $env:FACTORY_PATH $env:FACTORY_BASENAME '' $env:EXPECTED_FACTORY_SIZE; " ^
    "Confirm-File $env:OTA_PATH $env:OTA_BASENAME 'app1' $env:EXPECTED_OTA_SIZE; " ^
    "Confirm-File $env:SPIFFS_PATH $env:SPIFFS_BASENAME 'spiffs' $env:EXPECTED_SPIFFS_SIZE; " ^
    "$otaSha256 = (Get-FileHash -LiteralPath $env:OTA_PATH -Algorithm SHA256).Hash.ToLowerInvariant(); " ^
    "if ($otaSha256 -cne $env:EXPECTED_OTA_SHA256) { throw 'OTA loader SHA-256 is not the pinned release image' }"
IF !ERRORLEVEL! NEQ 0 (
    CALL :LOG_MESSAGE ERROR "Factory bundle or metadata validation failed; refusing to erase flash."
    EXIT /B 1
)
CALL :LOG_MESSAGE INFO "Validated complete !FACTORY_BASENAME! bundle before erase."

:skip-filename

CALL :LOG_MESSAGE DEBUG "Determine the correct esptool command to use..."
IF NOT "__%PYTHON%__"=="____" (
    SET "ESPTOOL_CMD="!PYTHON!" -m esptool"
    CALL :LOG_MESSAGE DEBUG "Python interpreter supplied."
) ELSE (
    CALL :LOG_MESSAGE DEBUG "Python interpreter NOT supplied. Looking for esptool..."
    WHERE esptool >nul 2>&1
    IF !ERRORLEVEL! EQU 0 (
        @REM WHERE exits with code 0 if esptool is found.
        SET "ESPTOOL_CMD=esptool"
    ) ELSE (
        SET "ESPTOOL_CMD=python -m esptool"
        CALL :RESET_ERROR
    )
)

CALL :LOG_MESSAGE DEBUG "Checking esptool command !ESPTOOL_CMD!..."
@REM %VAR% not !VAR!: cmd will not split a delayed-expanded command token that
@REM carries a path, so the "python -m esptool" form never starts.
%ESPTOOL_CMD% version >nul 2>&1
SET "ESPTOOL_EXIT=!ERRORLEVEL!"
IF NOT "!ESPTOOL_EXIT!"=="0" (
    CALL :LOG_MESSAGE ERROR "esptool availability probe failed: !ESPTOOL_CMD!"
    EXIT /B 1
)
%ESPTOOL_CMD% version 2>&1 | powershell -NoProfile -NonInteractive -Command ^
    "$text = [Console]::In.ReadToEnd(); " ^
    "$match = [regex]::Match($text, '(?:^|[^0-9])([0-9]+\.[0-9]+(?:\.[0-9]+)?)(?:$|[^0-9])'); " ^
    "if (-not $match.Success -or [version]$match.Groups[1].Value -lt [version]'4.5.1') { exit 1 }"
IF !ERRORLEVEL! NEQ 0 (
    CALL :LOG_MESSAGE ERROR "esptool 4.5.1 or newer is required."
    EXIT /B 1
)

@REM esptool v5 renamed subcommands to dashes; older versions only take underscores.
@REM Probe here: the --debug and --port rewrites below leave ESPTOOL_CMD unusable.
SET "ESPTOOL_WRITE_FLASH=write_flash"
SET "ESPTOOL_READ_FLASH_STATUS=read_flash_status"
SET "ESPTOOL_CHIP_ID=chip_id"
SET "ESPTOOL_FLASH_ID=flash_id"
SET "ESPTOOL_NO_RESET=no_reset"
%ESPTOOL_CMD% 2>&1 | findstr /C:"write-flash" >nul
IF !ERRORLEVEL! EQU 0 (
    SET "ESPTOOL_WRITE_FLASH=write-flash"
    SET "ESPTOOL_READ_FLASH_STATUS=read-flash-status"
    SET "ESPTOOL_CHIP_ID=chip-id"
    SET "ESPTOOL_FLASH_ID=flash-id"
    SET "ESPTOOL_NO_RESET=no-reset"
)
CALL :RESET_ERROR
CALL :LOG_MESSAGE DEBUG "Using esptool write command: !ESPTOOL_WRITE_FLASH!"

IF %DEBUG% EQU 1 (
    CALL :LOG_MESSAGE DEBUG "Skipping ESPTOOL_CMD steps."
    SET "ESPTOOL_CMD=REM !ESPTOOL_CMD!"
)

CALL :LOG_MESSAGE DEBUG "Using esptool command: !ESPTOOL_CMD!"
IF "__!ESPTOOL_PORT!__" == "____" (
    CALL :LOG_MESSAGE WARN "Using esptool port: UNSET."
) ELSE (
    SET "ESPTOOL_CMD=!ESPTOOL_CMD! --port !ESPTOOL_PORT!"
    CALL :LOG_MESSAGE INFO "Using esptool port: !ESPTOOL_PORT!."
)
CALL :LOG_MESSAGE INFO "Using esptool baud: !ESPTOOL_BAUD!."

IF %BPS_RESET% EQU 1 (
    @REM Attempt to change mode via 1200bps Reset.
    CALL :RUN_ESPTOOL 1200 --chip esp32s3 --after !ESPTOOL_NO_RESET! !ESPTOOL_READ_FLASH_STATUS! || EXIT /B 1
    EXIT /B 0
)

@REM Flashing operations.
CALL :LOG_MESSAGE INFO "Verifying an ESP32-S3 is present on !ESPTOOL_PORT!..."
CALL :RUN_ESPTOOL !ESPTOOL_BAUD! --chip esp32s3 !ESPTOOL_CHIP_ID! || EXIT /B 1
CALL :VERIFY_FLASH_SIZE || EXIT /B 1
CALL :LOG_MESSAGE INFO "Erasing once and writing factory, OTA loader, and LittleFS as one transaction..."
CALL :RUN_COMPLETE_INSTALL || EXIT /B 1

CALL :LOG_MESSAGE INFO "Script complete!."
EXIT /B 0

:eof
ENDLOCAL
EXIT /B %ERRORLEVEL%


:RUN_ESPTOOL
@REM Subroutine used to run ESPTOOL_CMD with arguments.
@REM Also handles %ERRORLEVEL%.
@REM CALL :RUN_ESPTOOL [Baud] [up to five esptool arguments]
@REM.
@REM Example:: CALL :RUN_ESPTOOL 115200 --chip esp32s3 write_flash 0x00 "firmwarefile.bin"
IF %DEBUG% EQU 1 CALL :LOG_MESSAGE DEBUG "About to run esptool with validated arguments."
CALL :RESET_ERROR
%ESPTOOL_CMD% --baud %~1 %~2 %~3 %4 %5 %6
SET "RUN_EXIT=!ERRORLEVEL!"
IF NOT "!RUN_EXIT!"=="0" (
    CALL :LOG_MESSAGE ERROR "esptool command failed with exit code !RUN_EXIT!."
    EXIT /B !RUN_EXIT!
)
GOTO :eof

:RUN_COMPLETE_INSTALL
IF %DEBUG% EQU 1 GOTO :eof
CALL :RESET_ERROR
%ESPTOOL_CMD% --baud !ESPTOOL_BAUD! --chip esp32s3 !ESPTOOL_WRITE_FLASH! --erase-all 0x00 "!FACTORY_PATH!" !EXPECTED_OTA_OFFSET! "!OTA_PATH!" !EXPECTED_SPIFFS_OFFSET! "!SPIFFS_PATH!"
SET "RUN_EXIT=!ERRORLEVEL!"
IF NOT "!RUN_EXIT!"=="0" (
    CALL :LOG_MESSAGE ERROR "Complete factory installation failed with exit code !RUN_EXIT!."
    EXIT /B !RUN_EXIT!
)
GOTO :eof

:VERIFY_FLASH_SIZE
IF %DEBUG% EQU 1 GOTO :eof
CALL :RESET_ERROR
%ESPTOOL_CMD% --baud !ESPTOOL_BAUD! --chip esp32s3 !ESPTOOL_FLASH_ID! 2>&1 | "%SystemRoot%\System32\findstr.exe" /I /R /C:"flash size: 16 *MB" >nul
SET "FLASH_SIZE_EXIT=!ERRORLEVEL!"
IF NOT "!FLASH_SIZE_EXIT!"=="0" (
    CALL :LOG_MESSAGE ERROR "Selected device is not reporting the required 16MB flash; refusing to erase."
    EXIT /B 1
)
GOTO :eof

:LOG_MESSAGE
@REM Subroutine used to print log messages in four different levels.
@REM DEBUG messages only get printed if [-d] flag is passed to script.
@REM CALL :LOG_MESSAGE [ERROR|INFO|WARN|DEBUG] "Message"
@REM.
@REM Example:: CALL :LOG_MESSAGE INFO "Message."
SET /A LOGCOUNTER=LOGCOUNTER+1
SET "LOG_LEVEL=%~1"
SET "LOG_TEXT=%~2"
CALL :GET_TIMESTAMP
IF "!LOG_LEVEL!" == "ERROR" ECHO [91m!LOG_LEVEL! [0m[37m^| !TIMESTAMP! !LOGCOUNTER! [0m[91m!LOG_TEXT![0m
IF "!LOG_LEVEL!" == "INFO" ECHO [32m!LOG_LEVEL!  [0m[37m^| !TIMESTAMP! !LOGCOUNTER! [0m[32m!LOG_TEXT![0m
IF "!LOG_LEVEL!" == "WARN" ECHO [33m!LOG_LEVEL!  [0m[37m^| !TIMESTAMP! !LOGCOUNTER! [0m[33m!LOG_TEXT![0m
IF "!LOG_LEVEL!" == "DEBUG" IF %DEBUG% EQU 1 ECHO [34m!LOG_LEVEL! [0m[37m^| !TIMESTAMP! !LOGCOUNTER! [0m[34m!LOG_TEXT![0m
GOTO :eof

:GET_TIMESTAMP
@REM Subroutine used to set !TIMESTAMP! to HH:MM:ss.
@REM CALL :GET_TIMESTAMP
@REM.
@REM Updates: !TIMESTAMP!
FOR /F "tokens=1,2,3 delims=:,." %%a IN ("%TIME%") DO (
    SET "HH=%%a"
    SET "MM=%%b"
    SET "ss=%%c"
)
SET "TIMESTAMP=!HH!:!MM!:!ss!"
GOTO :eof

:RESET_ERROR
@REM Subroutine to reset %ERRORLEVEL% to 0.
@REM CALL :RESET_ERROR
@REM.
@REM Updates: %ERRORLEVEL%
EXIT /B 0
GOTO :eof
