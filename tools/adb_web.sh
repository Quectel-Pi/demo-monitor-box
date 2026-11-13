#!/usr/bin/env bash
# Open the Monitor Box Web UI through an ADB tunnel.
set -euo pipefail

ADB=("${ADB:-adb}")
SERIAL=""
LOCAL_PORT="${MB_ADB_WEB_PORT:-8080}"
REMOTE_PORT="${MB_ADB_WEB_REMOTE_PORT:-8080}"
# A VSCode Remote window runs in this VM but its browser runs on the client.
# Also publish the tunnel on the VM's LAN address for that case.
EXPOSE_BIND="${MB_ADB_WEB_BIND:-}"
OPEN=0

usage() {
    cat <<'EOF'
Usage: bash tools/adb_web.sh [--open] [--serial SERIAL] [--local-port PORT]
                             [--remote-port PORT] [--bind HOST_IP]

Create a reusable ADB forwarding rule for the Web UI and verify it.
Defaults: local 8080 -> device 8080, URL http://127.0.0.1:8080/
Use --bind when VS Code Remote's browser runs on another computer.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --open) OPEN=1; shift ;;
        --serial) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; SERIAL="$2"; shift 2 ;;
        --local-port) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; LOCAL_PORT="$2"; shift 2 ;;
        --remote-port) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; REMOTE_PORT="$2"; shift 2 ;;
        --bind) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; EXPOSE_BIND="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
done

case "$LOCAL_PORT:$REMOTE_PORT" in
    *[!0-9:]*|:*|*:) echo "[ERROR] Ports must be numbers." >&2; exit 2 ;;
esac
[ "$LOCAL_PORT" -ge 1 ] && [ "$LOCAL_PORT" -le 65535 ] || {
    echo "[ERROR] Invalid local port: $LOCAL_PORT" >&2; exit 2;
}
[ "$REMOTE_PORT" -ge 1 ] && [ "$REMOTE_PORT" -le 65535 ] || {
    echo "[ERROR] Invalid remote port: $REMOTE_PORT" >&2; exit 2;
}

if [ -n "$SERIAL" ]; then ADB+=(-s "$SERIAL"); fi
"${ADB[@]}" get-state >/dev/null 2>&1 || {
    echo "[ERROR] No ADB device connected. Run: adb devices" >&2
    exit 1
}

# Prefer the configured port, but move to the next free one if the host
# already uses it (for example by an older forwarding rule).
port_is_forwarded_by_us() {
    "${ADB[@]}" forward --list | awk -v lp="$1" -v rp="$2" '
        $2 == "tcp:" lp && $3 == "tcp:" rp { found = 1 }
        END { exit found ? 0 : 1 }'
}

port_is_listening() {
    ss -ltn 2>/dev/null | awk -v p=":$1" '$4 ~ p "$" { found = 1 } END { exit found ? 0 : 1 }'
}

if ! port_is_forwarded_by_us "$LOCAL_PORT" "$REMOTE_PORT"; then
    # A stale ADB rule on this host port is safe to replace for this device,
    # but a non-ADB listener must never be displaced.
    if port_is_listening "$LOCAL_PORT"; then
        owner="$(ss -ltnp 2>/dev/null | awk -v p=":$LOCAL_PORT" '$4 ~ p "$" {print; exit}')"
        if [[ "$owner" != *'"adb"'* ]]; then
            original="$LOCAL_PORT"
            for candidate in $(seq "$LOCAL_PORT" $((LOCAL_PORT + 19))); do
                if ! port_is_listening "$candidate"; then LOCAL_PORT="$candidate"; break; fi
            done
            [ "$LOCAL_PORT" != "$original" ] ||
                { echo "[ERROR] No free local port near $original." >&2; exit 1; }
            echo "[INFO] Host port $original is busy; using $LOCAL_PORT."
        fi
    fi
fi

"${ADB[@]}" forward "tcp:$LOCAL_PORT" "tcp:$REMOTE_PORT" >/dev/null
URL="http://127.0.0.1:$LOCAL_PORT/"
code=""
for _ in $(seq 1 20); do
    code="$(curl --noproxy '*' --silent --output /dev/null --write-out '%{http_code}' \
        --max-time 2 "${URL}status" 2>/dev/null || true)"
    [ "$code" = 200 ] && break
    sleep 0.25
done
if [ "$code" != 200 ]; then
    echo "[ERROR] ADB tunnel is up, but ${URL}status did not return 200 (got ${code:-no response})." >&2
    echo "        Check: adb shell 'systemctl status monitor-box --no-pager'" >&2
    exit 1
fi

echo "[OK] Monitor Box Web UI: $URL"
echo "     ADB forwarding: tcp:$LOCAL_PORT -> device tcp:$REMOTE_PORT"

if [ -n "$EXPOSE_BIND" ]; then
    command -v systemd-run >/dev/null && command -v socat >/dev/null || {
        echo "[ERROR] --bind requires systemd-run and socat." >&2; exit 1;
    }
    unit="monitor-box-web-${LOCAL_PORT}.service"
    if ! ss -ltn 2>/dev/null | awk -v addr="$EXPOSE_BIND:$LOCAL_PORT"         '$4 == addr { found = 1 } END { exit found ? 0 : 1 }'; then
        systemctl --user stop "$unit" >/dev/null 2>&1 || true
        systemctl --user reset-failed "$unit" >/dev/null 2>&1 || true
        systemd-run --user --unit="${unit%.service}" --collect             --property=Restart=always --property=RestartSec=1             socat "TCP-LISTEN:$LOCAL_PORT,bind=$EXPOSE_BIND,reuseaddr,fork"                   "TCP:127.0.0.1:$LOCAL_PORT" >/dev/null
        for _ in $(seq 1 20); do
            code="$(curl --noproxy '*' --silent --output /dev/null --write-out '%{http_code}'                 --max-time 2 "http://$EXPOSE_BIND:$LOCAL_PORT/status" 2>/dev/null || true)"
            [ "$code" = 200 ] && break
            sleep 0.25
        done
        [ "$code" = 200 ] || {
            echo "[ERROR] Could not expose the tunnel on $EXPOSE_BIND:$LOCAL_PORT." >&2
            exit 1
        }
    fi
    echo "[OK] VSCode-host URL: http://$EXPOSE_BIND:$LOCAL_PORT/"
fi
if [ "$OPEN" -eq 1 ]; then
    target="$URL"
    [ -n "$EXPOSE_BIND" ] && target="http://$EXPOSE_BIND:$LOCAL_PORT/"
    # Opening the URL from this non-graphical SSH shell is not reliable.
    # Print the address that works from the local VS Code browser instead.
    if [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && command -v xdg-open >/dev/null 2>&1; then
        xdg-open "$target" >/dev/null 2>&1 &
        echo "[OK] Opening in the host browser."
    else
        echo "[INFO] Open this URL in the browser on your local computer: $target"
    fi
fi
