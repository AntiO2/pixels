# Transactional ingestion: stream-staging foundation

Related: pixelsdb/pixels-trino#180.

## Implemented boundary

`pixels-common.ingest` defines immutable mutation stream IDs, opaque batches,
content-bound SHA-256 digests, exact stream seals, and a transport interface.
`LocalMutationJournal` persists multiple writers and transactions in one local
journal. No business primary key is required; equal-valued rows remain distinct
batches/rows. `DELETE_ROWS` is reserved as a transport kind, not implemented SQL DELETE.

The journal does not call MainIndex, SinglePointIndex, RowIdAllocator, or Retina
visibility. Those remain the authoritative storage primitives for committed
installation. Staging must not allocate replacement identities or expose rows.

## Local storage contract

Append acknowledges acceptance. A stream seal verifies sequence, schema/format,
counts, and digest, then forces the WAL, atomically replaces a checksummed
`durable.offset`, and forces the directory. Recovery validates the entire
acknowledged prefix. Only an unacknowledged suffix is truncated. A lost/corrupt
marker or acknowledged record is an error, not a fresh deployment.

Use an already-created directory on a local filesystem supporting atomic rename,
file force, and directory force. A local file lock permits one owner; it is not
distributed fencing. Local-volume loss is outside this guarantee. No power-loss
or storage-hardware durability certification is implied by the tests.

The byte and record limits bound disk admission and in-memory descriptors.
Rotation, checkpoints, and reclamation are not implemented. A full journal fails
admission; the caller must not treat this journal as an unlimited production WAL.
`close()` does not make unsealed appends acknowledged.

`discardAbortedTransaction` records a previously verified authoritative ABORT.
It is not a transaction decision API. Its persistent tombstone rejects old and
new streams for that transaction. Payload cleanup is deferred; no shared row or
index compensation occurs.

## Verification

```sh
bash tools/verify-ingest-contract.sh
```

Requires JDK 9+ and validates the Java-8-compatible subset without Maven, native
Retina, or running services. JUnit 4 wrappers in `pixels-retina` run the same 17
contract cases under the normal module test runner. Explicitly enable tests in
this repository's Maven configuration.

## Required integration before SQL enablement

Participant registration and authorization; owner epochs; the RPC/payload
adapter; schema validation and constraint/row intents; durable transaction
manifest and Prepare/Commit/Abort decisions; replay-safe installation using the
existing indexes; publication/ReadViews; checkpoint and log reclamation; and
primary-key-optional file finalization remain separate implementation work.

Do not register the new SQL write path until those correctness boundaries are
implemented. The staging journal is not a committed database and has no public
query API.
