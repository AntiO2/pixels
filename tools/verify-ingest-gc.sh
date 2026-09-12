#!/usr/bin/env bash
set -euo pipefail
repo=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
build=$(mktemp -d)
trap 'rm -rf -- "$build"' EXIT
common="$repo/pixels-common/src/main/java/io/pixelsdb/pixels/common/ingest"
retina="$repo/pixels-retina/src/main/java/io/pixelsdb/pixels/retina/ingest"
tests="$repo/pixels-retina/src/test/java/io/pixelsdb/pixels/retina/ingest"
javac_args=(-source 8 -target 8)
if [[ $(javac -version 2>&1) != 'javac 1.8'* ]]; then
    javac_args=(--release 8)
fi
javac "${javac_args[@]}" -Xlint:all -Xlint:-options -Werror -d "$build" \
    "$common"/*.java "$retina/LocalMutationJournal.java" \
    "$tests/LocalMutationJournalContract.java" "$tests/LocalMutationJournalGcContract.java"
java -cp "$build" io.pixelsdb.pixels.retina.ingest.LocalMutationJournalContract
java -cp "$build" io.pixelsdb.pixels.retina.ingest.LocalMutationJournalGcContract
