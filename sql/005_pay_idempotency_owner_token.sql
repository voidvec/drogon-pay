-- Add an ownership token to idempotency reservations (audit F2 root cause).
-- A pay_idempotency row written by the WeChat callback chains is a
-- two-phase reservation: INSERT with response_snapshot = NULL marks the
-- delivery that holds the key, and the snapshot is finalized only after the
-- business transaction commits. Until now nothing recorded WHO held the
-- reservation, so the finalize UPDATE (key-only) could stamp a snapshot onto
-- a row that a later retry had already deleted and re-reserved, and the
-- original delivery had no way to learn its ownership was lost.
--
-- owner_token carries a per-delivery random token written at reservation
-- time. The finalize UPDATE is guarded by it: zero matched rows means the
-- reservation was taken away while this delivery ran, and the transaction
-- must roll back rather than ACK. Rows without a token (created by the
-- create/refund request paths, which keep their own single-phase protocol,
-- or by pre-migration code) stay NULL and are unaffected.
--
-- Idempotent: ALTER TABLE ... ADD COLUMN IF NOT EXISTS (PG 9.6+).

ALTER TABLE IF EXISTS pay_idempotency
    ADD COLUMN IF NOT EXISTS owner_token VARCHAR(64);
