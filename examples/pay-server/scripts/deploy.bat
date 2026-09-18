@echo off
REM Pay Server Deployment Script for Windows
REM Usage: deploy.bat [environment]
REM   environment: development | staging | production (default: development)

setlocal enabledelayedexpansion

REM Configuration
set SCRIPT_DIR=%~dp0
set PROJECT_ROOT=%SCRIPT_DIR%..
set REPO_ROOT=%SCRIPT_DIR%..\..\..
set BUILD_DIR=%PROJECT_ROOT%\build
set DEPLOY_ENV=%1
if "%DEPLOY_ENV%"=="" set DEPLOY_ENV=development

REM Colors (limited support in Windows)
set INFO=[INFO]
set WARN=[WARN]
set ERROR=[ERROR]

REM Logging functions
:log_info
echo %INFO% %~1
goto :eof

:log_warn
echo %WARN% %~1
goto :eof

:log_error
echo %ERROR% %~1
goto :eof

REM Load environment variables
:load_env
set ENV_FILE=%PROJECT_ROOT%\.env.%DEPLOY_ENV%

if not exist "%ENV_FILE%" (
    call :log_warn Environment file not found: %ENV_FILE%
    call :log_info Using default .env.example
    set ENV_FILE=%PROJECT_ROOT%\.env.example
)

if exist "%ENV_FILE%" (
    call :log_info Loading environment from: %ENV_FILE%
    for /f "tokens=*" %%a in ('type "%ENV_FILE%" ^| findstr /v "^#" ^| findstr /v "^$"') do set %%a
)
goto :eof

REM Check prerequisites
:check_prerequisites
call :log_info Checking prerequisites...

where cmake >nul 2>&1
if errorlevel 1 (
    call :log_error CMake not found in PATH
    exit /b 1
)

call :log_info All prerequisites met
goto :eof

REM Build application
:build_app
call :log_info Building application for %DEPLOY_ENV% environment...

REM Reuse the Conan + CMake preset flow from build.bat (bare cmake would
REM miss the conan_toolchain.cmake that provides Drogon and friends)
call "%SCRIPT_DIR%build.bat" -release
if errorlevel 1 (
    call :log_error Build failed
    exit /b 1
)

call :log_info Build complete
goto :eof

REM Setup database
:setup_database
call :log_info Setting up database...

REM Check if PostgreSQL is running
pg_isready -h %DB_HOST% -p %DB_PORT% -U %DB_USER%
if errorlevel 1 (
    call :log_error Cannot connect to PostgreSQL at %DB_HOST%:%DB_PORT%
    exit /b 1
)

REM Provisioning: the app role has no CREATEDB, so creating the database is a
REM superuser step and stays here rather than moving into the executor.
psql -h %DB_HOST% -p %DB_PORT% -U %DB_USER% -lqt ^| findstr %DB_NAME% >nul
if errorlevel 1 (
    call :log_info Creating database: %DB_NAME%
    psql -h %DB_HOST% -p %DB_PORT% -U %DB_USER% -c "CREATE DATABASE %DB_NAME%;"
) else (
    call :log_warn Database %DB_NAME% already exists
)

REM Apply migrations with the one executor. This used to be a
REM "for %%f in (%PROJECT_ROOT%\sql\*.sql)" loop, which was dead twice over:
REM sql/ moved to the repository root after the plugin refactor, so the glob
REM matched nothing and the step quietly succeeded without applying anything.
call :log_info Running database migrations...
python "%REPO_ROOT%\scripts\migrate_db.py" ^
    --host %DB_HOST% --port %DB_PORT% --user %DB_USER% --db %DB_NAME%
if errorlevel 1 (
    call :log_error migrate_db.py failed - see its output above
    exit /b 1
)

call :log_info Database setup complete
goto :eof

REM Check Redis connection
:check_redis
call :log_info Checking Redis connection...

redis-cli -h %REDIS_HOST% -p %REDIS_PORT% ping | findstr "PONG" >nul
if errorlevel 1 (
    call :log_error Cannot connect to Redis at %REDIS_HOST%:%REDIS_PORT%
    exit /b 1
)

call :log_info Redis connection successful
goto :eof

REM Setup certificates
:setup_certificates
call :log_info Setting up certificates...

set CERT_DIR=%PROJECT_ROOT%\certs

if not exist "%CERT_DIR%" (
    call :log_info Creating certificates directory: %CERT_DIR%
    mkdir "%CERT_DIR%"
)

REM Check if WeChat certificates exist
if not exist "%CERT_DIR%\apiclient_cert.pem" if not exist "%CERT_DIR%\apiclient_key.pem" (
    call :log_warn WeChat Pay certificates not found in %CERT_DIR%
    call :log_info Please place your certificates in the certs directory
)

goto :eof

REM Health check
:health_check
call :log_info Performing health check...

set MAX_ATTEMPTS=30
set ATTEMPT=1

:health_check_loop
if %ATTEMPT% gtr %MAX_ATTEMPTS% (
    call :log_error Health check failed after %MAX_ATTEMPTS% attempts
    exit /b 1
)

curl -f -s http://%SERVER_HOST%:%SERVER_PORT%/api/health >nul 2>&1
if errorlevel 1 (
    call :log_info Waiting for server to start... (%ATTEMPT%/%MAX_ATTEMPTS%)
    timeout /t 2 /nobreak >nul
    set /a ATTEMPT+=1
    goto health_check_loop
)

call :log_info Health check passed
goto :eof

REM Deploy
:deploy
call :log_info Starting deployment for %DEPLOY_ENV% environment...

call :load_env
call :check_prerequisites
call :build_app

if "%DEPLOY_ENV%"=="development" (
    call :setup_database
    call :check_redis
    call :setup_certificates
) else if "%DEPLOY_ENV%"=="staging" (
    call :setup_database
    call :check_redis
    call :setup_certificates
) else if "%DEPLOY_ENV%"=="production" (
    call :setup_database
    call :check_redis
    call :setup_certificates
) else (
    call :log_error Unknown environment: %DEPLOY_ENV%
    exit /b 1
)

call :log_info Deployment complete!

call :log_info To start the server manually:
call :log_info   cd %BUILD_DIR%\windows-msvc\Release
call :log_info   PayServer.exe

goto :eof

REM Rollback
:rollback
call :log_warn Rolling back deployment...
call :log_info Please manually restore previous version
goto :eof

REM Main
if "%2"=="" (
    call :deploy
) else if "%2"=="deploy" (
    call :deploy
) else if "%2"=="rollback" (
    call :rollback
) else (
    echo Usage: %0 [environment] [command]
    echo   environment: development ^| staging ^| production
    echo   command: deploy ^| rollback
    exit /b 1
)

endlocal
