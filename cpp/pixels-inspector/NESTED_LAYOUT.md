# Portable nested-column contract

Pixels declares `ARRAY`, `MAP`, and `STRUCT` in `pixels.proto`, but the current
Java and C++ `ColumnWriter`/`ColumnReader` factories do not emit or consume
them. The portable inspector therefore owns the following V1 columnar contract
instead of treating enum presence as format evidence.

## Schema graph

Each `Footer.types` entry has a corresponding row-group chunk/index/encoding.
Scalar entries have no subtypes. `ARRAY` has one subtype, `MAP` has two
subtypes (key then value), and `STRUCT` has zero or more named subtypes. The
graph is an acyclic, single-parent forest with a maximum depth of 32.

## STRUCT

A STRUCT chunk contains no value payload. Its pixel positions are zero and its
optional null bitmaps use the normal chunk null layout. Each child chunk has
the same logical row count as the STRUCT. A null STRUCT suppresses all child
values at that row. Values are rendered as name-keyed JSON objects.

## ARRAY and MAP

Each non-null physical parent value is two unsigned 64-bit integers in the
chunk byte order: the inclusive flattened-child start and exclusive end.
`pixelPositions` point to the first pair in each parent pixel; normal
null-padding rules determine physical parent positions.

Ranges must be ordered, non-overlapping, and within the logical child row
count. An ARRAY's one child chunk contains the flattened elements. A MAP's key
and value chunks contain the same flattened count. Maps render as ordered
`[[key,value], ...]` arrays so duplicate keys are preserved.

Child chunks use the existing scalar, variable-width, dictionary, vector, or
nested contracts recursively. Their logical row count is the sum of explicit
per-pixel `numberOfValues` statistics. A requested parent page may address only
the contiguous child interval covered by that page and is bounded by the
portable page and aggregate-element limits.

This contract is exercised by deterministic Core-owned fixtures and is not
presented as compatibility with a writer that does not yet exist.
