# Pay Plugin - ORM Models

This directory contains Drogon ORM models for the payment system.

## Current Models

All models have been generated and are actively used by the service layer:

- **PayOrder** - Order information (订单信息)
- **PayPayment** - Payment transaction details (支付交易详情)
- **PayRefund** - Refund transaction details (退款交易详情)
- **PayCallback** - Payment/refund callback records (回调记录)
- **PayLedger** - Transaction ledger entries (账本分录)
- **PayIdempotency** - Idempotency records (幂等性记录)

## Architecture Note

In the refactored service-based architecture, models are accessed through:

- **Services** (`services/`) - Business logic layer
- **Controllers** (`controllers/`) - HTTP request handling
- **Plugin** (`plugins/PayPlugin`) - Initialization and dependency injection

Models are **not** directly accessed by controllers. Controllers use services, and services use models.

## Model Relationships

```
PayOrder (1) ──┬──→ (1) PayPayment (payment)
               │
               └──→ (N) PayRefund (refunds)

PayPayment (1) ──→ (N) PayCallback (callbacks)
PayRefund (1) ────→ (N) PayCallback (callbacks)

PayOrder (1) ──→ (N) PayLedger (ledger entries)
```

## Regenerating Models

⚠️ **WARNING**: Models are currently in use by the service layer!

Only regenerate if:
- Database schema has changed
- You need to add new models
- Models are corrupted or missing

### Regenerate Models (Windows)

```batch
..\scripts\generate_models.bat
```

### Regenerate Models (Linux/Mac)

```bash
drogon_ctl create model models
```

### After Regeneration

If model interfaces change, you may need to update:

1. **Service Files** (`services/*.cc`)
   - Update model access patterns
   - Modify field references
   - Check method signatures

2. **Controller Files** (`controllers/*.cc`)
   - Update request parsing
   - Modify response formatting

3. **Plugin Files** (`plugins/PayPlugin.cc`)
   - Update model initialization
   - Check dependency injection

## Configuration

Model generation is configured in `model.json`:

```json
{
  "rdbms": "postgresql",
  "host": "127.0.0.1",
  "port": 5432,
  "dbname": "pay_test",
  "user": "test",
  "passwd": "123456",
  "tables": [
    "pay_order",
    "pay_payment",
    "pay_callback",
    "pay_refund",
    "pay_ledger",
    "pay_idempotency"
  ]
}
```

## Database Schema

See `sql/` directory for complete database schema:

- `sql/001_init_pay_tables.sql` - Initial table creation
- `sql/002_add_indexes.sql` - Performance indexes

## Model Usage Examples

### In Services

```cpp
#include "models/PayOrder.h"

// Query order
auto order = mapper.findBy(OrderNo, orderNo);

// Create order
PayOrder order;
order.setOrderNo(newOrderNo);
order.setUserId(userId);
order.setAmount(amount);          // decimal string in yuan, e.g. "9.99"
order.setStatus("CREATED");
mapper.insert(order);
```

### Important Notes

1. **Amount Storage**: All amounts are stored as `VARCHAR(32)` **decimal strings
   in yuan units** (the columns are not numeric, and there is no cents
   conversion)
   - `"9.99"`, `"100"`, `"0.5"` are valid; the controller-level check is
     `^\d+(\.\d{1,2})?$`
   - Never multiply by 100 and never store a cent integer

2. **Timestamps**: `created_at` and `updated_at` are maintained by PostgreSQL
   - `created_at` - Set automatically on insert (`DEFAULT CURRENT_TIMESTAMP`)
   - `updated_at` - Set by the `update_*_modtime` triggers created in
     `sql/001_init_pay_tables.sql`; do not assign it by hand

3. **Status Values**: Uppercase state-machine constants, defined in
   [TECH_SPECS.md](../../../../TECH_SPECS.md) "订单状态机"
   - Order: `CREATED`, `PAYING`, `PAID`, `REFUNDED`, `CLOSED`, `FAILED`
   - Payment: `INIT`, `PROCESSING`, `SUCCESS`, `FAIL`
   - Refund: `REFUND_INIT`, `REFUNDING`, `REFUND_SUCCESS`, `REFUND_FAIL`

## Troubleshooting

### Model Compilation Errors

If you get compilation errors after regenerating models:

1. Check if field names match database schema
2. Verify all required headers are included
3. Clean build: `rm -rf build && mkdir build && cd build && cmake ..`
4. Rebuild: `make` or `cmake --build .`

### Missing Models

If models are missing:

1. Check `model.json` has correct table list
2. Verify database tables exist
3. Run drogon_ctl with verbose flag: `drogon_ctl create model models --verbose`

### Outdated Models

If models seem outdated:

1. Check database schema in `sql/` directory
2. Compare model fields with table columns
3. Regenerate models if schema has changed
4. Update service layer code accordingly

## Further Information

- [Architecture Overview](../docs/architecture_overview.md)
- [Migration Guide](../docs/migration_guide.md)
- [Configuration Guide](../docs/configuration_guide.md)
