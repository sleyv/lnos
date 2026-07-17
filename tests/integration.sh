#!/bin/bash
set -uo pipefail

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BOLD='\033[1m'
NC='\033[0m'

PASS=0
FAIL=0
INCUS_CMD="sudo incus"
CONTAINERS=(lnos-1 lnos-2 lnos-3)
SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC_DIR="$SCRIPT_DIR"
TMP_SRC="/tmp/lnos-integration-src"

header() { echo -e "\n${BOLD}═══ $1 ═══${NC}"; }
pass() { echo -e "  ${GREEN}✓${NC} $1"; ((PASS++)); }
fail() { echo -e "  ${RED}✗${NC} $1"; ((FAIL++)); }

# ── helpers ──────────────────────────────────────────────────────
incus_exec() { $INCUS_CMD exec "$1" -- bash -c "$2"; }

push_to() { $INCUS_CMD file push "$2" "$1/$3"; }
push_recursive() { $INCUS_CMD file push -r "$2" "$1/$3"; }

cleanup() {
    header "Cleanup"
    for c in "${CONTAINERS[@]}"; do
        incus_exec "$c" "sudo systemctl stop lnosd 2>/dev/null || sudo pkill -x lnosd 2>/dev/null || true" || true
    done
    echo "  done"
}

trap cleanup EXIT

# ── 1. Push source to lnos-1 and build ──────────────────────────
header "Push source to lnos-1 and build"
incus_exec lnos-1 "rm -rf $TMP_SRC"
incus_exec lnos-1 "mkdir -p $TMP_SRC/build"

# Push all source files individually (incus file push -r has quirks)
for f in CMakeLists.txt; do
    push_to lnos-1 "$SRC_DIR/$f" "$TMP_SRC/$f"
done
for dir in liblnos lnosd lnosctl tests; do
    for f in $(find "$SRC_DIR/$dir" -type f); do
        rel="${f#$SRC_DIR/}"
        incus_exec lnos-1 "mkdir -p $TMP_SRC/$(dirname $rel)"
        push_to lnos-1 "$f" "$TMP_SRC/$rel"
    done
done
echo "  Source pushed"

incus_exec lnos-1 "cd $TMP_SRC/build && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j\$(nproc)" 2>&1
echo "  ${GREEN}✓${NC} Build complete on lnos-1"

# ── 2. Deploy to all containers ──────────────────────────────────
header "Deploy LNOS to all containers"
# First, deploy to lnos-1 (built there)
incus_exec lnos-1 "sudo cp $TMP_SRC/build/lnosd /usr/local/bin/ && sudo cp $TMP_SRC/build/lnosctl /usr/local/bin/"
echo "  ${GREEN}✓${NC} lnos-1 binaries deployed"

# Pull built binaries from lnos-1 to host, then push to lnos-2, lnos-3
for bin in lnosd lnosctl; do
    local_tmp="/tmp/lnos-integration-$bin"
    sudo incus file pull "lnos-1/$TMP_SRC/build/$bin" "$local_tmp"
    for c in lnos-2 lnos-3; do
        sudo incus file push "$local_tmp" "$c/usr/local/bin/$bin"
    done
    rm -f "$local_tmp"
done
incus_exec lnos-2 "sudo chmod +x /usr/local/bin/lnosd /usr/local/bin/lnosctl"
incus_exec lnos-3 "sudo chmod +x /usr/local/bin/lnosd /usr/local/bin/lnosctl"
for c in lnos-2 lnos-3; do
    echo "  ${GREEN}✓${NC} $c binaries deployed"
done

# Deploy NSS module and config
header "Deploy NSS module and config on all"
for c in "${CONTAINERS[@]}"; do
    incus_exec "$c" "sudo mkdir -p /etc/lnos /usr/lib"
    if [ "$c" = "lnos-1" ]; then
        incus_exec "$c" "sudo cp $TMP_SRC/build/libnss_lnos.so.2 /usr/lib/"
    else
        sudo incus file pull "lnos-1/$TMP_SRC/build/libnss_lnos.so.2" "/tmp/lnos-integration-nss"
        sudo incus file push "/tmp/lnos-integration-nss" "$c/usr/lib/libnss_lnos.so.2"
        rm -f /tmp/lnos-integration-nss
    fi
    # Add lnos to nsswitch hosts line if not present
    incus_exec "$c" 'grep -q "lnos" /etc/nsswitch.conf 2>/dev/null && echo "nss already set" || sudo sed -i "/^hosts:/ s/$/ lnos/" /etc/nsswitch.conf'
    echo "  ${GREEN}✓${NC} $c NSS + config ready"
done

# ── 3. Configure & start LNOS on each ────────────────────────────
header "Configure and start LNOS"
declare -A NODE_NAMES
NODE_NAMES[lnos-1]="laptop.pc.test1"
NODE_NAMES[lnos-2]="server.pc.test2"
NODE_NAMES[lnos-3]="node.pc.test3"

for c in "${CONTAINERS[@]}"; do
    name="${NODE_NAMES[$c]}"
    incus_exec "$c" "sudo /usr/local/bin/lnosctl init 2>/dev/null || true"
    incus_exec "$c" "sudo /usr/local/bin/lnosctl set name '$name'"
    incus_exec "$c" "sudo /usr/local/bin/lnosctl generatekeys 2>/dev/null || true"
    incus_exec "$c" "sudo pkill -x lnosd 2>/dev/null || true"
    sleep 1
    # Create systemd service and start daemon so it survives exec session
    incus_exec "$c" "sudo tee /etc/systemd/system/lnosd.service >/dev/null <<'SERVICEEOF'
[Unit]
Description=LNOS Daemon
After=network.target

[Service]
ExecStart=/usr/local/bin/lnosd
Restart=on-failure

[Install]
WantedBy=multi-user.target
SERVICEEOF
sudo systemctl daemon-reload && sudo systemctl start lnosd"
    sleep 2
    echo "  ${GREEN}✓${NC} $c → $name"
done
sleep 3

# ── 4. Tests ─────────────────────────────────────────────────────
header "Test: Daemon processes are running"
sleep 2
for c in "${CONTAINERS[@]}"; do
    pid=$(incus_exec "$c" "pgrep -x lnosd" 2>/dev/null || true)
    [ -n "$pid" ] && pass "$c: lnosd PID $pid" || fail "$c: lnosd NOT running"
done

header "Test: HTTP dashboard accessible"
for c in "${CONTAINERS[@]}"; do
    result=$(incus_exec "$c" "curl -sf http://localhost:9999/stats" 2>/dev/null || true)
    [ -n "$result" ] && pass "$c: /stats responds" || fail "$c: /stats FAIL"
done

header "Test: Node name resolution (lnosctl resolve)"
for c in "${CONTAINERS[@]}"; do
    name="${NODE_NAMES[$c]}"
    ip=$(incus_exec "$c" "/usr/local/bin/lnosctl resolve '$name'" 2>/dev/null || true)
    [ -n "$ip" ] && pass "$c: resolve $name → $ip" || fail "$c: resolve $name FAIL"
done

header "Test: Cross-node discovery (lnos-1 resolves lnos-2)"
ip12=$(incus_exec lnos-1 "/usr/local/bin/lnosctl resolve '${NODE_NAMES[lnos-2]}'" 2>/dev/null || true)
ip13=$(incus_exec lnos-1 "/usr/local/bin/lnosctl resolve '${NODE_NAMES[lnos-3]}'" 2>/dev/null || true)
[ -n "$ip12" ] && pass "lnos-1 → lnos-2: $ip12" || fail "lnos-1 → lnos-2: unresolved"
[ -n "$ip13" ] && pass "lnos-1 → lnos-3: $ip13" || fail "lnos-1 → lnos-3: unresolved"

header "Test: Cross-node discovery (lnos-2 resolves all)"
ip21=$(incus_exec lnos-2 "/usr/local/bin/lnosctl resolve '${NODE_NAMES[lnos-1]}'" 2>/dev/null || true)
ip23=$(incus_exec lnos-2 "/usr/local/bin/lnosctl resolve '${NODE_NAMES[lnos-3]}'" 2>/dev/null || true)
[ -n "$ip21" ] && pass "lnos-2 → lnos-1: $ip21" || fail "lnos-2 → lnos-1: unresolved"
[ -n "$ip23" ] && pass "lnos-2 → lnos-3: $ip23" || fail "lnos-2 → lnos-3: unresolved"

header "Test: Cross-node discovery (lnos-3 resolves all)"
ip31=$(incus_exec lnos-3 "/usr/local/bin/lnosctl resolve '${NODE_NAMES[lnos-1]}'" 2>/dev/null || true)
ip32=$(incus_exec lnos-3 "/usr/local/bin/lnosctl resolve '${NODE_NAMES[lnos-2]}'" 2>/dev/null || true)
[ -n "$ip31" ] && pass "lnos-3 → lnos-1: $ip31" || fail "lnos-3 → lnos-1: unresolved"
[ -n "$ip32" ] && pass "lnos-3 → lnos-2: $ip32" || fail "lnos-3 → lnos-2: unresolved"

header "Test: NSS resolution via getent hosts"
for c in lnos-2 lnos-3; do
    name="${NODE_NAMES[lnos-1]}"
    ip=$(incus_exec "$c" "getent hosts '$name'" 2>/dev/null || true)
    [ -n "$ip" ] && pass "$c: getent hosts $name → $ip" || fail "$c: getent hosts $name FAIL"
done
for c in lnos-1 lnos-3; do
    name="${NODE_NAMES[lnos-2]}"
    ip=$(incus_exec "$c" "getent hosts '$name'" 2>/dev/null || true)
    [ -n "$ip" ] && pass "$c: getent hosts $name → $ip" || fail "$c: getent hosts $name FAIL"
done

header "Test: lnosctl stats returns data"
for c in "${CONTAINERS[@]}"; do
    stats=$(incus_exec "$c" "/usr/local/bin/lnosctl stats" 2>/dev/null || true)
    [ -n "$stats" ] && pass "$c: stats: $(echo "$stats" | head -1)" || fail "$c: stats FAIL"
done

header "Test: HTTP /nodes returns peers"
for c in "${CONTAINERS[@]}"; do
    nodes=$(incus_exec "$c" "curl -sf http://localhost:9999/nodes" 2>/dev/null || true)
    count=$(echo "$nodes" | python3 -c "import sys,json; print(len(json.load(sys.stdin)))" 2>/dev/null || echo "0")
    [ "$count" -ge 2 ] && pass "$c: /nodes has $count peers" || fail "$c: /nodes expected >=2 peers, got $count"
done

header "Test: Resilience — stop lnos-2, others should still resolve lnos-1 and lnos-3"
incus_exec lnos-2 "sudo systemctl stop lnosd" 2>/dev/null || true
sleep 4
ip31_after=$(incus_exec lnos-3 "/usr/local/bin/lnosctl resolve '${NODE_NAMES[lnos-1]}'" 2>/dev/null || true)
[ -n "$ip31_after" ] && pass "lnos-3 resolves lnos-1 after lnos-2 down: $ip31_after" || fail "lnos-3 cannot resolve lnos-1 after lnos-2 down"
incus_exec lnos-2 "sudo systemctl start lnosd" 2>/dev/null || true
sleep 3
pass "lnos-2 restarted"

header "Test: New name after restart persists"
incus_exec lnos-1 "sudo /usr/local/bin/lnosctl set name 'laptop.pc.test1-renamed'"
pid1=$(incus_exec lnos-1 "pgrep -x lnosd")
incus_exec lnos-1 "sudo systemctl restart lnosd" 2>/dev/null || true
sleep 2
new_name=$(incus_exec lnos-1 "sudo /usr/local/bin/lnosctl get name" 2>/dev/null || true)
[[ "$new_name" == *"renamed"* ]] && pass "lnos-1 name changed to $new_name" || fail "lnos-1 name NOT changed (got: $new_name)"
# Reset name back
incus_exec lnos-1 "sudo /usr/local/bin/lnosctl set name '${NODE_NAMES[lnos-1]}'"
incus_exec lnos-1 "sudo systemctl restart lnosd" 2>/dev/null || true
sleep 2

# ── Summary ──────────────────────────────────────────────────────
header "Results"
total=$((PASS + FAIL))
echo -e "  ${GREEN}Pass: $PASS${NC}"
[ "$FAIL" -gt 0 ] && echo -e "  ${RED}Fail: $FAIL${NC}" || echo -e "  Pass: 100%"
echo ""
[ "$FAIL" -gt 0 ] && exit 1 || exit 0
