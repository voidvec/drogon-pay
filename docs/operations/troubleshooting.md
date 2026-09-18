# Pay Plugin Troubleshooting

## Common Issues

### Build Issues

#### Issue: CMake configuration fails

**Symptoms:**
```
CMake Error: Could not find Drogon
```

**Solution:**
```bash
# Ensure Conan is properly configured
conan install .. --build=missing
```

#### Issue: Linker errors

**Symptoms:**
```
undefined reference to `drogon::...`
```

**Solution:**
- Ensure Drogon is installed: `conan install drogon/[version]`
- Check CMakeLists.txt includes all required libraries

### Runtime Issues

#### Issue: Server starts but returns 500 errors

**Symptoms:**
```
Error: db client not ready
```

**Solution:**
1. Check PostgreSQL is running: `sudo systemctl status postgresql`
2. Verify database configuration in config.json
3. Check database logs: `sudo journalctl -u postgres`

#### Issue: Redis connection refused

**Symptoms:**
```
Redis client not available, idempotency will be database-only
```

**Solution:**
1. Start Redis: `sudo systemctl start redis-server`
2. Verify Redis is listening: `redis-cli ping`
3. Check Redis configuration in config.json

### WeChat Pay Issues

#### Issue: Signature verification fails

**Symptoms:**
```
Callback rejected with signature verification error
```

**Solution:**
1. Verify certificate files exist and are readable
2. Check certificate serial_no matches configuration
3. Verify API v3 key is correct
4. Check system time is accurate (signature verification is time-sensitive)

#### Issue: Certificate refresh fails

**Symptoms:**
```
Failed to refresh WeChat certificate
```

**Solution:**
1. Check certificate file paths in config.json
2. Verify file permissions
3. Ensure network connectivity to WeChat servers

### Performance Issues

#### Issue: Slow payment creation

**Symptoms:**
- Payment creation takes >5 seconds
- Database queries are slow

**Solution:**
1. Check database indexes: `\di idx_pay_order_user_id` should exist
2. Enable Redis caching for idempotency
3. Check database connection pool settings
4. Monitor database query performance

#### Issue: High memory usage

**Symptoms:**
- Server uses >1GB memory
- Memory grows over time

**Solution:**
1. Check for memory leaks (use valgrind)
2. Review callback lifecycle management
3. Check for unbounded cache growth
4. Monitor Redis memory usage

### Idempotency Issues

#### Issue: Duplicate payments created

**Symptoms:**
- Same payment processed multiple times
- Idempotency key conflicts

**Solution:**
1. Ensure controllers extract or generate idempotency keys
2. Check IdempotencyService is properly integrated
3. Verify Redis is working for fast path
4. Check database idempotency records

### Testing Issues

#### Issue: Integration tests fail

**Symptoms:**
- Tests fail with "connection refused"
- Mock objects not working

**Solution:**
1. Ensure test database exists
2. Check mock expectations match actual service calls
3. Verify test configuration is correct
4. Re-run a single failing case with the Drogon test runner:
   `PayBackendTests -r <TestName>` (or `ctest --test-dir build/<preset> -C Release --output-on-failure`)

## Debug Mode

### Enable Debug Logging

Edit `config.json`（日志配置位于 `app.log` 下）:
```json
{
  "app": {
    "log": {
      "log_level": "DEBUG"
    }
  }
}
```

### Enable Drogon Debug Mode

Set environment variable:
```bash
export DROGON_CTRL_LOG_LEVEL=debug
./PayServer
```

## Getting Help

If issues persist:

1. Check logs: `tail -f /var/log/payserver.log`
2. Review architecture: [architecture_overview.md](../architecture/architecture_overview.md)
3. Review configuration: [configuration_guide.md](../development/configuration_guide.md)
4. Search existing issues: Check project issue tracker
5. Create support ticket with:
   - Log files
   - Configuration files (redacted)
   - System information
   - Steps to reproduce
