#!/usr/bin/env bash
# Simulates one workflow run on a single machine and measures the relay:
#   job "build", step "compile"  -> put a large file
#   job "build", step "package"  -> get it back (same job)
#   job "test"                   -> get it (another job), twice at the same time
#   cleanup step                 -> release the run
# Reports throughput, the relay's peak memory, and checks every byte.
#
# Usage: bench/bench.sh [BUILD_DIR] [SIZE_MIB]
set -euo pipefail

BUILD_DIR="${1:-build}"
SIZE_MIB="${2:-1024}"
WORK="$(mktemp -d)"
trap 'pkill -INT -P "${SERVER_PID:-0}" 2>/dev/null || true; rm -rf "$WORK"' EXIT

PORT=17700
LIMIT_MIB=$((SIZE_MIB + 256))

echo "== payload: ${SIZE_MIB} MiB of random data"
head -c "$((SIZE_MIB * 1024 * 1024))" /dev/urandom > "$WORK/in.bin"
SHA_IN="$(sha256sum "$WORK/in.bin" | cut -d' ' -f1)"

/usr/bin/time -v "$BUILD_DIR/jrelay-server" --dev-auth --bind "tcp://127.0.0.1:$PORT" \
  --memory-limit "${LIMIT_MIB}M" 2> "$WORK/server.log" &
SERVER_PID=$!
sleep 0.5

export JRELAY_ENDPOINT="tcp://127.0.0.1:$PORT"
export JRELAY_TOKEN="dev:424242"
J="$BUILD_DIR/jrelay"

echo "== job build / step compile: put"
GITHUB_JOB=build GITHUB_ACTION=compile "$J" put --name app.bin --file "$WORK/in.bin"

echo "== job build / step package: get (same job)"
GITHUB_JOB=build "$J" get --step compile --name app.bin --out "$WORK/same-job.bin"

echo "== job test: two concurrent gets (other job)"
GITHUB_JOB=test "$J" get --job build --step compile --name app.bin --out "$WORK/other-1.bin" &
P1=$!
GITHUB_JOB=test2 "$J" get --job build --step compile --name app.bin --out "$WORK/other-2.bin" &
P2=$!
wait "$P1" "$P2"

for f in same-job other-1 other-2; do
  [[ "$(sha256sum "$WORK/$f.bin" | cut -d' ' -f1)" == "$SHA_IN" ]] || { echo "MISMATCH in $f"; exit 1; }
done
echo "== all copies identical to the input (sha256)"

echo "== does not fit: a second ${SIZE_MIB} MiB message must be rejected (D1)"
set +e
GITHUB_JOB=build GITHUB_ACTION=again "$J" put --file "$WORK/in.bin"
RC=$?
set -e
[[ $RC -eq 5 ]] || { echo "expected exit code 5 (no memory), got $RC"; exit 1; }

echo "== cleanup step: release the run"
GITHUB_JOB=cleanup "$J" release

echo "== a late \"Re-run failed jobs\" gets a clear message"
set +e
GITHUB_JOB=test "$J" get --job build --step compile --name app.bin --out /dev/null
RC=$?
set -e
[[ $RC -eq 4 ]] || { echo "expected exit code 4 (run finished), got $RC"; exit 1; }

# SERVER_PID is GNU time; signal the relay itself (its child) so time can report.
pkill -INT -P "$SERVER_PID" jrelay-server
wait "$SERVER_PID" || true
echo "== relay peak memory (max RSS): $(grep 'Maximum resident' "$WORK/server.log" | awk '{print $NF}') KiB"
