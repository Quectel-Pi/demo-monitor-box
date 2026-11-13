#!/usr/bin/env bash
# Update an existing /opt/monitor_box systemd installation from the SDK host.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ADB=("${ADB:-adb}")
BUILD=1

usage() {
    echo "Usage: bash $0 [--serial SERIAL] [--no-build]"
    echo "Build and update mb-capture over ADB; preserve settings, models and events."
    echo "Requires root adb shell and an installed monitor-box.service."
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --serial)
            [ "$#" -ge 2 ] || { usage >&2; exit 2; }
            ADB+=(-s "$2")
            shift 2
            ;;
        --no-build) BUILD=0; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

"${ADB[@]}" get-state >/dev/null
[ "$("${ADB[@]}" shell id -u | tr -d '\r')" = 0 ] || {
    echo "[ERROR] Root adb shell required; enable device root access first." >&2
    exit 1
}
[ "$("${ADB[@]}" shell uname -m | tr -d '\r')" = aarch64 ] || {
    echo "[ERROR] Expected an aarch64 device." >&2
    exit 1
}

if [ "$BUILD" -eq 1 ]; then
    echo "==> Cross-building mb-capture"
    # The generated Web UI header is embedded into mb-capture. Regenerate it
    # before building so a JavaScript/CSS edit is not deployed as stale bytes.
    python3 "$ROOT/tools/embed_web_ui.py"
    cmake -S "$ROOT" -B "$ROOT/build" \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/../../tools/cmake/aarch64-m2-debian-toolchain.cmake"
    cmake --build "$ROOT/build" --target mb-capture -j2
fi

BIN="$ROOT/build/mb-capture"
MIGRATOR="$ROOT/tools/migrate_config.py"
DEFAULT_CONFIG="$ROOT/config/monitor_box.json"
[ -x "$BIN" ] || { echo "[ERROR] Missing executable: $BIN" >&2; exit 1; }
[ -r "$MIGRATOR" ] || { echo "[ERROR] Missing migrator: $MIGRATOR" >&2; exit 1; }
[ -r "$DEFAULT_CONFIG" ] || { echo "[ERROR] Missing default configuration: $DEFAULT_CONFIG" >&2; exit 1; }
file "$BIN" | grep -q 'ELF 64-bit.*ARM aarch64' || {
    echo "[ERROR] Not an aarch64 ELF executable: $BIN" >&2
    exit 1
}
HASH="$(sha256sum "$BIN")"
HASH="${HASH%% *}"
STAGE="$("${ADB[@]}" shell 'mktemp -d /tmp/mb-backend-update.XXXXXXXX' | tr -d '\r')"
[[ "$STAGE" =~ ^/tmp/mb-backend-update\.[A-Za-z0-9]+$ ]] || {
    echo "[ERROR] Invalid device staging path: $STAGE" >&2
    exit 1
}
cleanup() {
    "${ADB[@]}" shell "rm -f '$STAGE/mb-capture' '$STAGE/migrate_config.py' '$STAGE/monitor_box.default.json' '$STAGE/status.json'; rmdir '$STAGE'" ||
        echo "[WARN] Could not clean device staging directory: $STAGE" >&2
}
trap cleanup EXIT

echo "==> Transferring backend, config migrator and default schema"
"${ADB[@]}" push "$BIN" "$STAGE/mb-capture"
"${ADB[@]}" push "$MIGRATOR" "$STAGE/migrate_config.py"
"${ADB[@]}" push "$DEFAULT_CONFIG" "$STAGE/monitor_box.default.json"
"${ADB[@]}" shell bash -s -- "$STAGE" "$HASH" <<'REMOTE'
set -euo pipefail
stage="$1"
expected="$2"
target=/opt/monitor_box/bin/mb-capture
config=/opt/monitor_box/etc/monitor_box/monitor_box.json
service=monitor-box.service

fail() { echo "[ERROR] $*" >&2; exit 1; }
[ "$(systemctl show "$service" -p LoadState --value)" = loaded ] ||
    fail "Service is not installed; run install.sh and setup_autostart.sh first."
[ -x "$target" ] && [ -r "$config" ] || fail "Backend/configuration not installed."
command -v python3 >/dev/null
command -v curl >/dev/null
exec_start="$(systemctl show "$service" -p ExecStart --value)"
[[ "$exec_start" == *"path=$target ;"* && "$exec_start" == *"--config $config ;"* ]] ||
    fail "Nonstandard service ExecStart; refusing to replace the wrong backend: $exec_start"
actual="$(sha256sum "$stage/mb-capture")"
[ "${actual%% *}" = "$expected" ] || fail "Transfer checksum mismatch."

health_url="$(python3 - "$config" <<'PY'
import json
import sys
with open(sys.argv[1]) as f:
    config = json.load(f)
web = config.get("web", {})
if web.get("enabled", True):
    host = web.get("listen", "0.0.0.0")
    host = "127.0.0.1" if host == "0.0.0.0" else host
    if ":" in host:
        host = "[::1]" if host == "::" else "[" + host + "]"
    print("http://%s:%d/status" % (host, web.get("port", 8080)))
PY
)"
was_active=0
if systemctl is-active --quiet "$service"; then was_active=1; fi
install -m 0755 "$target" "$target.previous"
install -m 0755 "$stage/mb-capture" "$target.new"

rollback() {
    trap - ERR
    echo "[ERROR] Update failed; restoring previous backend." >&2
    systemctl stop "$service" || echo "[ERROR] Could not stop failed service." >&2
    if mv -f "$target.previous" "$target"; then
        if [ "$was_active" -eq 1 ]; then
            systemctl start "$service" || echo "[ERROR] Could not restart previous backend." >&2
        fi
    else
        echo "[ERROR] Restore failed; backup: $target.previous" >&2
    fi
    rm -f "$target.new"
    systemctl status "$service" --no-pager -l || :
    exit 1
}
trap rollback ERR
echo "==> Stopping service (active recordings will be finalized)"
systemctl stop "$service"
mv -f "$target.new" "$target"
echo "==> Migrating persistent configuration if needed"
python3 "$stage/migrate_config.py" "$config" "$stage/monitor_box.default.json"
systemctl start "$service"

echo "==> Checking running executable and backend health"
ready=0
for ((i = 0; i < 30; ++i)); do
    pid="$(systemctl show "$service" -p MainPID --value)"
    if systemctl is-active --quiet "$service" && [ "$pid" -gt 0 ] &&
       [ -r "/proc/$pid/exe" ]; then
        running_hash="$(sha256sum "/proc/$pid/exe")"
        if [ "${running_hash%% *}" = "$expected" ]; then
            if [ -z "$health_url" ] ||
               curl --noproxy '*' --fail --silent --show-error --max-time 2 \
                   "$health_url" >"$stage/status.json"; then
                ready=1
                break
            fi
        fi
    fi
    sleep 1
done
rm -f "$stage/status.json"
[ "$ready" -eq 1 ]
trap - ERR
echo "[OK] Backend updated and running: PID=$pid SHA256=$expected"
echo "     Backup: $target.previous"
echo "     Settings were preserved; a legacy config was backed up before migration."
REMOTE

echo "[OK] Device backend update complete."
