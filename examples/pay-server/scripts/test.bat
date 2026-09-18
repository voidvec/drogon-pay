@echo off
setlocal enabledelayedexpansion
REM Test script for Pay Plugin Backend using CTest
REM
REM Flow is label-based on purpose. A parenthesized block expands %~1 and
REM %ERRORLEVEL% when the block is *parsed*, so `shift` inside a block cannot
REM read the next argument and `if %errorlevel% neq 0` after a command inside a
REM block always sees 0. Both bugs lived here until this rewrite.

REM Capture the script directory before any shift: shift moves %0 as well,
REM so %~dp0 becomes unreliable after the argument parsing loop below.
set SCRIPT_DIR=%~dp0
set SCRIPT_NAME=%~nx0

echo ========================================
echo Pay Plugin Backend Test Runner (CTest)
echo ========================================

REM Parse command line arguments
set LIST_TESTS=0
set TEST_PATTERN=
set VERBOSE=1
set OUTPUT=0
set BUILD_TYPE=Release

:parse_args
if "%~1"=="" goto end_parse
if /i "%~1"=="-l" goto opt_list
if /i "%~1"=="-r" goto opt_run
if /i "%~1"=="-v" goto opt_verbose
if /i "%~1"=="-o" goto opt_output
if /i "%~1"=="-debug" goto opt_debug
if /i "%~1"=="-release" goto opt_release
set UNKNOWN_OPT=%~1
goto usage_fail

:opt_list
set LIST_TESTS=1
shift
goto parse_args

:opt_run
shift
if "%~1"=="" goto usage_fail
set TEST_PATTERN=%~1
shift
goto parse_args

:opt_verbose
set VERBOSE=1
shift
goto parse_args

:opt_output
set OUTPUT=1
shift
goto parse_args

:opt_debug
set BUILD_TYPE=Debug
shift
goto parse_args

:opt_release
set BUILD_TYPE=Release
shift
goto parse_args

:usage_fail
if defined UNKNOWN_OPT echo Unknown option: %UNKNOWN_OPT%
echo Usage: %SCRIPT_NAME% [-l] [-r ExactTestName] [-v] [-o] [-debug^|-release]
echo   -l                 List available tests
echo   -r ^<name^>          Run one test by its exact name (see -l)
echo   -v                 Verbose output (already the default)
echo   -o                 Also save test_results.log in the build directory
echo   -debug / -release  Select the build configuration to test
endlocal
exit /b 1
:end_parse

REM Change to the preset build directory at the repository root (created by build.bat)
set PRESET=windows-msvc
if /i "%BUILD_TYPE%"=="Debug" set PRESET=windows-msvc-debug

set BUILD_ROOT=%SCRIPT_DIR%..\..\..\build\%PRESET%
if not exist "%BUILD_ROOT%" (
    echo Error: build\%PRESET% directory not found
    echo Please build the project first using build.bat
    endlocal
    exit /b 1
)
cd /d "%BUILD_ROOT%"
set BUILD_DIR_ABS=%CD%
echo Build directory: %CD%
echo.

REM Determine test directory (CTest may be configured at the root or under tests\)
set TEST_DIR=.
if not exist "CTestTestfile.cmake" set TEST_DIR=tests
if not exist "%TEST_DIR%\CTestTestfile.cmake" (
    echo Error: CTest configuration not found
    echo Expected locations:
    echo   - %CD%\CTestTestfile.cmake
    echo   - %CD%\tests\CTestTestfile.cmake
    echo.
    echo Please build the project first using build.bat
    endlocal
    exit /b 1
)

echo Test directory: %CD%\%TEST_DIR%
echo.

REM No DB_* / PAY_* environment here on purpose: tests/main.cc loads the .env
REM that build.bat copied next to the binary, so credentials do not belong in
REM this script. (A dead DB_PASS=123456 block used to sit here, read by nothing.)

REM Locate the test executable. MSVC is a multi-config generator, so the binary
REM nests under tests\<Config>\; single-config generators keep it in tests\.
REM -l and -r run this binary directly, because tests/CMakeLists.txt registers
REM exactly ONE ctest case (the whole suite); `ctest -R Foo` therefore matches
REM nothing and exits 0, which reads as a pass. The binary's own -r takes an
REM exact DROGON_TEST name and exits 1 when no such case exists.
set TEST_EXE_DIR=
if exist "tests\%BUILD_TYPE%\PayBackendTests.exe" set TEST_EXE_DIR=tests\%BUILD_TYPE%
if not defined TEST_EXE_DIR if exist "tests\PayBackendTests.exe" set TEST_EXE_DIR=tests

if %LIST_TESTS% neq 0 goto list_tests
if not "%TEST_PATTERN%"=="" goto run_one
goto run_all

:list_tests
if not defined TEST_EXE_DIR (
    echo Error: PayBackendTests.exe not found under %CD%\tests
    echo Build the tests first using build.bat
    endlocal
    exit /b 1
)
echo ========================================
echo Available Tests
echo ========================================
pushd "%TEST_EXE_DIR%"
PayBackendTests.exe -l
set RC=!ERRORLEVEL!
popd
endlocal
exit /b %RC%

:run_one
if not defined TEST_EXE_DIR (
    echo Error: PayBackendTests.exe not found under %CD%\tests
    echo Build the tests first using build.bat
    endlocal
    exit /b 1
)
echo ========================================
echo Running Test: %TEST_PATTERN%
echo ========================================
echo.
pushd "%TEST_EXE_DIR%"
if "%OUTPUT%"=="1" (
    PayBackendTests.exe -r "%TEST_PATTERN%" > "%BUILD_DIR_ABS%\test_results.log" 2>&1
) else (
    PayBackendTests.exe -r "%TEST_PATTERN%"
)
set RC=!ERRORLEVEL!
popd
if "%OUTPUT%"=="1" (
    type test_results.log
    echo Results saved to: %CD%\test_results.log
)
if "%RC%" neq "0" goto report_fail
echo.
echo Test %TEST_PATTERN% passed.
endlocal
exit /b 0

:run_all
echo ========================================
echo Running All Tests
echo ========================================
echo.

REM An all-tests run has to prove there is something to run: ctest exits 0 when
REM no case is registered, so a build that skipped the test target would
REM otherwise print "ALL TESTS PASSED" over an empty suite.
set TEST_COUNT=
for /f "tokens=3" %%N in ('ctest -C %BUILD_TYPE% -N 2^>^&1 ^| findstr /b /c:"Total Tests"') do set TEST_COUNT=%%N
if not defined TEST_COUNT set TEST_COUNT=0
if not "%TEST_COUNT%"=="0" goto count_ok
echo Error: ctest lists no tests under %BUILD_DIR_ABS%
echo An empty suite exits 0, so refuse to call it a pass - build the
echo PayBackendTests target first (build.bat).
endlocal
exit /b 1
:count_ok
echo Registered ctest cases: %TEST_COUNT%
echo.

set CTEST_CMD=ctest -C %BUILD_TYPE% --output-on-failure
if "%VERBOSE%"=="1" set CTEST_CMD=%CTEST_CMD% -V

if "%OUTPUT%"=="1" (
    %CTEST_CMD% > test_results.log 2>&1
) else (
    %CTEST_CMD%
)
set RC=!ERRORLEVEL!
if "%OUTPUT%"=="1" (
    type test_results.log
    echo Results saved to: %CD%\test_results.log
)
if "%RC%" neq "0" goto report_fail
echo.
echo ========================================
echo ALL TESTS PASSED
echo ========================================
endlocal
exit /b 0

:report_fail
echo.
echo ========================================
echo TESTS FAILED
echo ========================================
echo If this came from -r, the name may not exist: run
echo   %SCRIPT_NAME% -l
echo for the exact case names.
endlocal
exit /b 1
