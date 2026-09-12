#!/usr/bin/env bash
# Real etcd + production TransServer/RetinaServer lifecycle + one INSERT/read.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
MVN=${MVN:-mvn}
ETCD_BIN=${ETCD_BIN:-etcd}
: "${PIXELS_HOME:?PIXELS_HOME must contain the matching Retina native runtime}"

if [[ $(javac -version 2>&1) != 'javac 1.8'* ]]; then
    echo "Normal Pixels daemon verification requires JDK 8" >&2
    exit 2
fi
command -v "$ETCD_BIN" >/dev/null

WORK=${INGEST_DAEMON_WORK_DIR:-$(mktemp -d "${TMPDIR:-/tmp}/pixels-ingest-daemon-XXXXXXXX")}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
CLIENT_PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("", 0)); print(s.getsockname()[1]); s.close()')
PEER_PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("", 0)); print(s.getsockname()[1]); s.close()')
RETINA_PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("", 0)); print(s.getsockname()[1]); s.close()')
TRANSACTION_PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("", 0)); print(s.getsockname()[1]); s.close()')
ETCD_PID=''

cleanup() {
    local result=$?
    trap - EXIT
    if [[ -n "$ETCD_PID" ]]; then
        kill "$ETCD_PID" 2>/dev/null || true
        wait "$ETCD_PID" 2>/dev/null || true
    fi
    printf 'Normal daemon verification exit=%s evidence=%s\n' "$result" "$WORK"
    exit "$result"
}
trap cleanup EXIT

if [[ ${INGEST_DAEMON_SKIP_BUILD:-0} != 1 ]]; then
    env -u LD_PRELOAD "$MVN" -B -ntp -f "$ROOT/pom.xml" -pl pixels-daemon -am \
        -DskipTests -Dpixels.retina.test.allocator="$PIXELS_HOME/lib/libjemalloc.so" \
        install > "$WORK/build.log" 2>&1
fi
env -u LD_PRELOAD "$MVN" -B -ntp -f "$ROOT/pixels-daemon/pom.xml" \
    org.apache.maven.plugins:maven-dependency-plugin:2.10:build-classpath \
    -Dmdep.outputFile="$WORK/dependencies.cp" > "$WORK/dependencies.log" 2>&1

"$ETCD_BIN" --name lifecycle \
    --data-dir "$WORK/etcd-data" \
    --listen-client-urls "http://127.0.0.1:$CLIENT_PORT" \
    --advertise-client-urls "http://127.0.0.1:$CLIENT_PORT" \
    --listen-peer-urls "http://127.0.0.1:$PEER_PORT" \
    --initial-advertise-peer-urls "http://127.0.0.1:$PEER_PORT" \
    --initial-cluster "lifecycle=http://127.0.0.1:$PEER_PORT" \
    --logger zap --log-level error > "$WORK/etcd.log" 2>&1 &
ETCD_PID=$!
for _ in $(seq 1 100); do
    if curl --silent --fail "http://127.0.0.1:$CLIENT_PORT/health" | grep -q '"health":"true"'; then
        break
    fi
    kill -0 "$ETCD_PID" 2>/dev/null || {
        echo "etcd exited before readiness" >&2
        exit 1
    }
    sleep .1
done
curl --silent --fail "http://127.0.0.1:$CLIENT_PORT/health" | grep -q '"health":"true"'

CP="$ROOT/pixels-daemon/target/test-classes:$ROOT/pixels-daemon/target/classes:$(cat "$WORK/dependencies.cp")"
export LD_LIBRARY_PATH="$PIXELS_HOME/lib:${LD_LIBRARY_PATH:-}"
ALLOCATOR=()
if [[ -f "$PIXELS_HOME/lib/libjemalloc.so.2" ]]; then
    ALLOCATOR=("LD_PRELOAD=$PIXELS_HOME/lib/libjemalloc.so.2${LD_PRELOAD:+:$LD_PRELOAD}")
elif [[ -f "$PIXELS_HOME/lib/libjemalloc.so" ]]; then
    ALLOCATOR=("LD_PRELOAD=$PIXELS_HOME/lib/libjemalloc.so${LD_PRELOAD:+:$LD_PRELOAD}")
fi
run_phase() {
    local phase=$1
    env "${ALLOCATOR[@]}" \
        PIXELS_CONFIG="$ROOT/pixels-common/src/main/resources/pixels.properties" \
        java -Xmx1g -cp "$CP" \
        io.pixelsdb.pixels.daemon.transaction.ingest.NormalIngestDaemonMain \
        "$WORK/state" "$CLIENT_PORT" "$RETINA_PORT" "$TRANSACTION_PORT" "$phase"
}

run_phase write 2>&1 | tee "$WORK/daemon.log"
run_phase recover 2>&1 | tee -a "$WORK/daemon.log"
grep -q '^PIXELS_NORMAL_INGEST_DAEMON_PHASE1_PASS rows=65 ' "$WORK/daemon.log"
grep -q '^PIXELS_NORMAL_INGEST_DAEMON_PASS rows=65 .* checkpointRestart=2$' "$WORK/daemon.log"
