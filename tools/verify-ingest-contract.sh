#!/usr/bin/env bash
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf -- "$build"' EXIT
common="$repo/pixels-common/src/main/java/io/pixelsdb/pixels/common/ingest"
retina="$repo/pixels-retina/src/main/java/io/pixelsdb/pixels/retina/ingest"
tests="$repo/pixels-retina/src/test/java/io/pixelsdb/pixels/retina/ingest"
# JDK 9+; verifies the new Java-8-compatible subset, not the Maven/native reactor.
javac --release 8 -Xlint:all -Xlint:-options -Werror -d "$build" \
    "$common"/*.java "$retina/LocalMutationJournal.java" \
    "$tests/LocalMutationJournalContract.java"
java -cp "$build" io.pixelsdb.pixels.retina.ingest.LocalMutationJournalContract
