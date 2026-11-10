#!/usr/bin/env bash
# Runs one command of the Stellaris Job Relay action. All inputs arrive as
# environment variables (IN_*), never interpolated into the script, so values
# with quotes, $ or newlines are safe.
set -euo pipefail

export JRELAY_ENDPOINT="$IN_ENDPOINT"
export JRELAY_TOKEN="${IN_TOKEN:-dev:${GITHUB_RUN_ID}:${GITHUB_RUN_ATTEMPT}}"
if [ -n "$IN_SERVER_KEY" ]; then export JRELAY_SERVER_PUBLIC_KEY="$IN_SERVER_KEY"; fi

pid_file="$RUNNER_TEMP/jrelay-server.pid"
log_file="$RUNNER_TEMP/jrelay-server.log"

set_output() {  # name value (multi-line safe)
  local delim
  delim="JRELAY_$(openssl rand -hex 8)"
  { echo "$1<<$delim"; printf '%s\n' "$2"; echo "$delim"; } >> "$GITHUB_OUTPUT"
}

mask_lines() {  # registers every non-empty line of $1 as a secret
  while IFS= read -r line; do
    [ -n "$line" ] && echo "::add-mask::$line"
  done <<< "$1"
}

need_step() {
  if [ -z "$IN_STEP" ]; then
    echo "::error::input 'step' is required for '$IN_COMMAND' (for put: use the id of the calling step)"
    exit 2
  fi
}

# Runs jrelay, keeps its status line, and extracts the digest from it.
run_jrelay() {
  local status rc=0
  status="$(mktemp)"
  "$@" 2>"$status" || rc=$?
  cat "$status" >&2
  if [ "$rc" -ne 0 ]; then rm -f "$status"; exit "$rc"; fi
  local digest
  digest=$(grep -oE 'blake3 [0-9a-f]{64}' "$status" | cut -d' ' -f2 || true)
  [ -n "$digest" ] && echo "digest=$digest" >> "$GITHUB_OUTPUT"
  rm -f "$status"
}

case "$IN_COMMAND" in
  start)
    if [ -f "$pid_file" ] && kill -0 "$(cat "$pid_file")" 2>/dev/null; then
      echo "jrelay: relay already running in this job"
      exit 0
    fi
    echo "::warning::Stellaris Job Relay prototype: dev mode without token verification. Only for 127.0.0.1 or a closed network."
    nohup jrelay-server --dev-auth --bind "$IN_ENDPOINT" --memory-limit "$IN_MEMORY" > "$log_file" 2>&1 &
    echo $! > "$pid_file"
    for _ in $(seq 1 50); do
      grep -q "listening on" "$log_file" 2>/dev/null && break
      sleep 0.1
    done
    cat "$log_file"
    grep -q "listening on" "$log_file"
    ;;

  put)
    need_step
    if [ -n "$IN_VALUE" ] && [ -n "$IN_FILE" ]; then
      echo "::error::give either 'value' or 'file', not both"; exit 2
    fi
    if [ -n "$IN_VALUE" ]; then
      [ "$IN_MASK" = "true" ] && mask_lines "$IN_VALUE"
      run_jrelay jrelay put --step "$IN_STEP" --name "$IN_NAME" < <(printf '%s' "$IN_VALUE")
      echo "size=$(printf '%s' "$IN_VALUE" | wc -c)" >> "$GITHUB_OUTPUT"
    elif [ -n "$IN_FILE" ]; then
      run_jrelay jrelay put --step "$IN_STEP" --name "$IN_NAME" --file "$IN_FILE"
      echo "size=$(stat -c %s "$IN_FILE")" >> "$GITHUB_OUTPUT"
    else
      echo "::error::put needs 'value' or 'file'"; exit 2
    fi
    ;;

  get)
    need_step
    args=(get --step "$IN_STEP" --name "$IN_NAME")
    [ -n "$IN_JOB" ] && args+=(--job "$IN_JOB")
    if [ -n "$IN_OUT" ]; then
      run_jrelay jrelay "${args[@]}" --out "$IN_OUT"
      echo "size=$(stat -c %s "$IN_OUT")" >> "$GITHUB_OUTPUT"
    else
      tmp="$(mktemp)"
      run_jrelay jrelay "${args[@]}" --out "$tmp"
      size=$(stat -c %s "$tmp")
      value="$(cat "$tmp")"
      rm -f "$tmp"
      [ "$IN_MASK" = "true" ] && mask_lines "$value"
      if [ "$size" -gt 1000000 ]; then
        echo "::warning::message is $size bytes; GitHub outputs are limited. Use 'out' to get it as a file."
      fi
      set_output value "$value"
      echo "size=$size" >> "$GITHUB_OUTPUT"
    fi
    ;;

  release)
    jrelay release || true
    if [ -f "$pid_file" ]; then
      kill -INT "$(cat "$pid_file")" 2>/dev/null || true
      sleep 0.5
      rm -f "$pid_file"
      cat "$log_file" 2>/dev/null || true
    fi
    ;;

  *)
    echo "::error::unknown command '$IN_COMMAND' (expected start, put, get or release)"
    exit 2
    ;;
esac
