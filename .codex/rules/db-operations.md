---
description: DB access must be the async + Mapper + Criteria combo; raw SQL only in the listed exemptions
globs:
  - "examples/pay-server/**"
---

Every database operation in this codebase MUST be the **async callback + Mapper
+ Criteria** combo. The three parts are jointly mandatory, not pick-one.

## The combo (all three, every query)

1. **Async callback** — prefer `Mapper::findOne` / `execSqlAsync` with a moved
   `std::function<...> &&callback` (last param). Synchronous
   `Mapper::findBy`-with-future is RESTRICTED, only when a sync result is truly
   needed. `CoroMapper` is FORBIDDEN.
2. **Mapper API** — SELECT via `Mapper::findBy` / `findOne`, INSERT via
   `Mapper::insert`, UPDATE via `Mapper::update`. No hand-rolled SQL for CRUD.
3. **Criteria** — build WHERE conditions with `Criteria`, never by concatenating
   strings into a query. For multi-row membership, use `Criteria::In(...)`. For
   compound conditions, chain `&&` / `||` on Criteria objects.

JOIN-in-a-single-query is forbidden — split into multiple queries (or
`Criteria::In`). Capture `auto sharedCb = shared_from_this()` in the callback to
avoid use-after-free.

## `findOne` asserts uniqueness

`Mapper::findOne` reports **both** "no row" and "more than one row" through its
*error* callback, so querying a non-unique column with it silently turns into a
failure path the moment a second row becomes legal — which is what happened to
the callback audit lookups before the QR booking made multi-attempt orders
reachable. Use `findOne` only where a unique index guarantees one row; for "the
newest of several" chain `orderBy(col, DESC).limit(1).findBy(...)` and handle
the empty vector.

## Guarding `Mapper` construction

`Mapper<Model>(dbClient)` can throw `std::exception` on construction itself — a
dropped connection, or a client left in a bad state by a previous operation. An
uncaught throw inside an async callback escapes into the Drogon event loop and
takes the process down.

**Requirement 1: the block that constructs a `Mapper<...>` MUST sit inside
`try { ... } catch (const std::exception &e) { ... } catch (...) { ... }`.**
This is per construction site, not per function: a `Mapper` built inside a
`findOne` / `findBy` callback needs its own guard, because the enclosing
`try` cannot catch anything thrown on a later event-loop turn.

**Requirement 2: the `catch` block MUST report the failure through the same
callback the success path uses.** `LOG_ERROR` followed by `return` leaves the
caller waiting for a response that never arrives. Use the shape the call site
already has — `reportMapperFailure()` in
`libs/drogon-pay/src/services/CallbackService.cc` (a channel callback answering
the channel's own FAIL body), `sharedCb->call(...)` where the service wrapped
the callback in `pay::utils::OnceCallback` (`CheckResult{status=Error}` for
`StatusCallback`, `false` for `UpdateCallback`), `(*sharedCb)(...)` for a plain
captured `std::function`, and `{}` / an empty result for the value-shaped ones.

## The raw-SQL exemptions (and only these)

Raw SQL is allowed ONLY for:
- **DDL** (schema setup, migrations),
- **`UPDATE ... RETURNING`** (when you need the updated row back in one step),
- **documented batch operations** (state the justification in a comment),
- **`INSERT ... ON CONFLICT`** (upsert/reserve patterns the Mapper cannot express;
  justify in a comment),
- **connectivity probes** (`SELECT 1` health checks that touch no table),
- **explicit transaction `COMMIT`** (when durability MUST be confirmed before an
  external side effect — e.g. channel ACK / outbound call; an implicit
  destructor-time commit would race with the network call. Justify in a comment).

Anything else as raw SQL is a violation. A PreToolUse hook also guards
credential placeholders in these files, so failures show up before runtime.

The `project-conventions` skill holds the full statement; this file is the
path-scoped reminder that loads when you edit `examples/pay-server/**`.
