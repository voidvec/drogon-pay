# Health Check Endpoint Implementation

## Overview
Health check endpoints are implemented in the example host (`examples/pay-server`)
to monitor service status and connectivity to dependencies. Two routes are
exposed: `/healthz` (liveness) and `/readyz` (readiness).

## Implementation Details

### Files
1. **examples/pay-server/controllers/HealthCheckController.h**
   - Header file for the health check controller
   - Defines the HealthCheckController class
   - Registers the `/healthz` and `/readyz` routes for GET and OPTIONS methods

2. **examples/pay-server/controllers/HealthCheckController.cc**
   - Implementation of the health check logic
   - `/healthz` checks the event loop / running state
   - `/readyz` checks database (`SELECT 1`) and Redis (`PING`) connectivity
   - Returns JSON response with service status

### API Specification

**Endpoint:** `GET /healthz` — liveness (process is alive)

**Response Format (Alive - HTTP 200):**
```json
{
  "status": "alive"
}
```

**Response Format (Dead - HTTP 503):**
```json
{
  "status": "dead",
  "reason": "event_loop_null"
}
```

**Endpoint:** `GET /readyz` — readiness (dependencies reachable)

**Response Format (Ready - HTTP 200):**
```json
{
  "status": "ready"
}
```

**Response Format (Not ready - HTTP 503):**
```json
{
  "status": "not_ready",
  "failed": ["db"]
}
```

`failed` is an array that may contain `"db"`, `"redis"`, and `"timeout"`
(the readiness probe has a 1-second deadline). Readiness also applies a
consecutive-failure threshold before flipping to "not_ready".

**Endpoint:** `GET /health` — retired, answers 404

The route was an alias of `/readyz` whose own `Sunset` header named `2026-08-28`.
The window closed with every caller already migrated, so the alias is gone rather
than still answering "successfully": a 404 is what tells an un-migrated probe its
endpoint stopped existing. `HealthProbe_RetiredCompatEndpoint_Answers404` pins both
the status and the absence of registration.

### Health Check Logic

1. **Liveness (`/healthz`):**
   - Verifies the Drogon event loop is non-null and the app is running
   - HTTP 200 `"alive"` / HTTP 503 `"dead"`

2. **Readiness (`/readyz`):**
   - Database: runs `SELECT 1`; failure recorded as `"db"`
   - Redis: runs `PING`; only probed when a Redis client is configured
     (when `redis_client` is omitted from config, Redis is not checked)
   - A 1-second deadline records `"timeout"` if probes do not complete
   - HTTP 200 `"ready"` when no failures, otherwise HTTP 503 `"not_ready"`
     with the `failed` list

### Building the Project

The health check controller is part of the example host build (see the
example host CMakeLists):

```bash
examples\pay-server\scripts\build.bat
```

Build output will show:
```
HealthCheckController.cc
PayServer.vcxproj -> D:\...\build\windows-msvc\examples\pay-server\Release\PayServer.exe
```

### Testing the Endpoint

**Prerequisites:**
1. PostgreSQL database running on localhost:5432
2. Redis server running on localhost:6379 (optional)
3. PayServer executable built

**Start the server:**
```bash
cd examples/pay-server
build/windows-msvc/examples/pay-server/Release/PayServer.exe
```

**Test liveness:**
```bash
curl http://localhost:5566/healthz
```

**Expected response (alive):**
```json
{
  "status": "alive"
}
```

**Test readiness:**
```bash
curl http://localhost:5566/readyz
```

**Expected response (ready, with DB reachable):**
```json
{
  "status": "ready"
}
```

**Test without database (not ready):**
```json
{
  "status": "not_ready",
  "failed": ["db"]
}
```

### Integration with Existing Code

The health check controller follows the same pattern as other example-host
controllers:
- Uses Drogon's HttpController framework
- Automatic route registration via METHOD_LIST_BEGIN/END macros
- Handles OPTIONS requests for CORS
- Returns JSON responses using Json::Value

### Notes

- `/readyz` probes real connectivity: `SELECT 1` for the database and `PING`
  for Redis
- Redis is treated as an optional service — when no `redis_client` is
  configured it is not probed at all
- The endpoints automatically handle OPTIONS preflight requests for CORS
- `/health` is retired: not registered at all, so it answers 404 instead of
  proxying `/readyz`

### Build Status

✅ Build completed successfully
✅ No compilation warnings
✅ Executable created at: build/windows-msvc/examples/pay-server/Release/PayServer.exe
