# drogon-pay Documentation

Documentation index for the `drogon-pay` plugin library and its example host.

## 📂 Directory Structure

```
docs/
├── architecture/    # Architecture design documents
├── development/     # Integration & development guides
├── api/             # API usage and configuration
├── testing/         # Testing guides
├── deployment/      # Deployment & monitoring setup
├── operations/      # Operations manuals & checklists
├── review/          # Dated code audits of a subsystem
└── history/         # Historical records (plans, reports, superseded docs)
```

## 📖 Quick Links

### For Host Application Developers (consuming the drogon-pay library)

1. **[Plugin Integration Guide](development/plugin_integration.md)** — 5-step
   setup, configuration keys, route table, custom channel SPI, old→new config
   mapping
2. [Architecture Overview](architecture/architecture_overview.md)
3. Example host: [examples/pay-server/README.md](../examples/pay-server/README.md)
   · Admin console: [examples/pay-admin/README.md](../examples/pay-admin/README.md)

### For Contributors

1. [Environment Setup](development/environment_setup.md)
2. [Configuration Guide](development/configuration_guide.md)
3. [Logging Standards](development/logging_standards.md)
4. [Testing Guide](testing/testing_guide.md)

## 📝 Document Index

### Architecture

- [architecture_overview.md](architecture/architecture_overview.md) — plugin
  library architecture: layers, channel SPI, data flow

### Development

- [plugin_integration.md](development/plugin_integration.md) — **host
  integration guide** (5-step setup, custom channel SPI, config mapping)
- [environment_setup.md](development/environment_setup.md) — development
  environment setup
- [configuration_guide.md](development/configuration_guide.md) — application
  configuration guide
- [logging_standards.md](development/logging_standards.md) — logging
  conventions and levels
- [alipay_sandbox_setup.md](development/alipay_sandbox_setup.md) — Alipay
  sandbox setup
- [alipay_sandbox_quickstart.md](development/alipay_sandbox_quickstart.md) —
  Alipay sandbox quick start

### API

- [pay-api-examples.md](api/pay-api-examples.md) — payment API usage examples
- [api_configuration_guide.md](api/api_configuration_guide.md) — API
  configuration guide
- [api_key_configuration.md](api/api_key_configuration.md) — API key
  configuration

### Testing

- [testing_guide.md](testing/testing_guide.md) — test framework usage (ctest /
  DROGON_TEST)
- [e2e_testing_guide.md](testing/e2e_testing_guide.md) — end-to-end testing
  guide

### Deployment

- [deployment_guide.md](deployment/deployment_guide.md) — build & deployment
  instructions
- [monitoring_setup.md](deployment/monitoring_setup.md) — Prometheus / Grafana
  monitoring setup

### Operations

- [operations_manual.md](operations/operations_manual.md) — operations manual
- [troubleshooting.md](operations/troubleshooting.md) — troubleshooting guide
- [security_checklist.md](operations/security_checklist.md) — security
  checklist
- [health_check_implementation.md](operations/health_check_implementation.md)
  — health check implementation

### Review

- [2026-09-20-wechat-pay-api-audit.md](review/2026-09-20-wechat-pay-api-audit.md)
  — code-level audit of the WeChat Pay V3 flow (inbound notifications,
  outbound transactions/refunds/certificates) against the official API, with
  the fix batches and the deliberately deferred items

## 🗄️ Historical Documents

Everything under [history/](history/) is a historical record kept for
reference. Paths and commands inside these documents reflect the repository
layout **at the time of writing** (e.g. the pre-refactoring
`PayBackend/`/`PayFrontend/` monolith) and are not updated.

- [history/refactoring/](history/refactoring/) — 2026-04 service-layer
  refactoring design & implementation records
- [history/superpowers/](history/superpowers/) — plans / specs / reports from
  earlier development iterations
- [history/review/](history/review/) — deep code review report and its
  remediation plan
- [history/reports/](history/reports/) — dated status, performance, security
  audit, and release reports
- [history/migration_guide.md](history/migration_guide.md) — superseded
  service-layer migration guide (replaced by
  [plugin_integration.md](development/plugin_integration.md))
- [history/archive/](history/archive/) — legacy documentation and backups

## 🤝 Contributing

When adding new documentation:

1. Place it in the appropriate category directory
2. Use clear, descriptive filenames
3. Update this README's index
4. Follow the existing naming conventions

## 📜 Document Lifecycle

- **Active documents** — kept in category directories (architecture/,
  development/, ...) and updated as the code evolves
- **Status/dated reports** — go to `history/reports/` with date prefixes
- **Superseded guides & project records** — move to `history/` (never edited
  afterwards)
