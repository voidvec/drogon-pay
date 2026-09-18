@echo off
setlocal

REM Recreates the dev database and applies the migration chain.
REM
REM Everything except the two lines below lives in scripts/migrate_db.py, the
REM same executor CI uses. This script used to carry its own list of
REM sql/NNN_*.sql files, as did three other consumers - one of which had
REM drifted to applying only 001 and 002 - plus a hardcoded PGPASSWORD=123456.
REM
REM Password resolution is done by migrate_db.py: it reads PGPASSWORD /
REM PAY_DB_PASSWORD from the environment, or from examples/pay-server/.env when
REM --env-file is passed. A password is never written on a command line here.
REM
REM   setup_database.bat                 reset the schema, replay the chain
REM   setup_database.bat --keep-data     apply pending migrations only

cd /d "%~dp0..\..\.."

if not defined PGDATABASE set "PGDATABASE=pay_test"
REM Only for the message below: a variable set inside a parenthesized block is
REM not visible to that block's own %expansions%, so default it up here.
if not defined PGHOST set "PGHOST=127.0.0.1"

set "EXTRA=--reset-schema --confirm-drop %PGDATABASE%"
if /i "%~1"=="--keep-data" set "EXTRA="
if defined EXTRA (
    REM migrate_db.py enforces this as the loopback-only dev reset it is: the
    REM repeated name here is not the safety check, the host rule is.
    echo Resetting the public schema of %PGDATABASE% on %PGHOST%, then applying the chain...
) else (
    echo Applying pending migrations into existing %PGDATABASE%...
)

python scripts\migrate_db.py --env-file "examples/pay-server/.env" ^
  --db "%PGDATABASE%" %EXTRA%
if %errorlevel% neq 0 (
    echo.
    echo Error: migrate_db.py failed - see its output above. No default password
    echo        is shipped any more; set PGPASSWORD or fill in
    echo        examples/pay-server/.env and retry.
    endlocal
    exit /b 1
)

echo Database setup complete.
endlocal
exit /b 0
