#!/usr/bin/env bash
set -e

LNOS_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${LNOS_DIR}/build"

info()  { echo -e "\e[1;32m[INFO]\e[0m $*"; }
warn()  { echo -e "\e[1;33m[WARN]\e[0m $*"; }
err()   { echo -e "\e[1;31m[ERRO]\e[0m $*" >&2; }

is_root() { [ "$(id -u)" -eq 0 ]; }

info "=== LNOS setup ==="

# ----- deps -----
PM=""
APT_DEPS="cmake g++ make libsodium-dev"
PAC_DEPS="cmake gcc make libsodium"
DNF_DEPS="cmake gcc-c++ make libsodium-devel"
ZYP_DEPS="cmake gcc-c++ make libsodium-devel"
EMG_DEPS="dev-util/cmake sys-devel/gcc dev-libs/libsodium"
APK_DEPS="cmake g++ make libsodium-dev"

if command -v apt-get >/dev/null 2>&1; then      PM="apt-get"
elif command -v pacman >/dev/null 2>&1; then    PM="pacman"
elif command -v dnf >/dev/null 2>&1; then       PM="dnf"
elif command -v zypper >/dev/null 2>&1; then    PM="zypper"
elif command -v emerge >/dev/null 2>&1; then    PM="emerge"
elif command -v apk >/dev/null 2>&1; then       PM="apk"
fi

MISSING=""
for cmd in cmake g++ make; do
    command -v "$cmd" >/dev/null 2>&1 || MISSING="$MISSING $cmd"
done
if ! ldconfig -p 2>/dev/null | grep -q libsodium && ! pkg-config --exists libsodium 2>/dev/null; then
    MISSING="$MISSING libsodium"
fi

if [ -n "$MISSING" ]; then
    info "Installing dependencies:$MISSING"
    case "$PM" in
        apt-get) [ "$(id -u)" -eq 0 ] && apt-get update -qq && apt-get install -y -qq $APT_DEPS || warn "run as root: apt-get install $APT_DEPS" ;;
        pacman)  pacman -S --noconfirm $PAC_DEPS ;;
        dnf)     dnf install -y $DNF_DEPS ;;
        zypper)  zypper install -y $ZYP_DEPS ;;
        emerge)  emerge --ask n $EMG_DEPS ;;
        apk)     apk add $APK_DEPS ;;
        *)       warn "Unknown package manager. Install: cmake, g++, libsodium-dev" ;;
    esac
fi

# ----- build -----
info "Building LNOS..."
cmake -S "$LNOS_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$BUILD_DIR" -j"$(nproc)"

# ----- determine lib dir -----
for d in /usr/lib64 /usr/lib /lib64 /lib; do
    if [ -f "$d/libnss_files.so.2" ]; then
        LIBDIR="$d"
        break
    fi
done
[ -n "$LIBDIR" ] || LIBDIR="/usr/lib"

# ----- install binaries -----
install -m 755 "$BUILD_DIR/lnosd" /usr/local/bin/lnosd
install -m 755 "$BUILD_DIR/lnosctl" /usr/local/bin/lnosctl
info "Binaries → /usr/local/bin/lnosd, /usr/local/bin/lnosctl"

# ----- install NSS module -----
cp "$BUILD_DIR/libnss_lnos.so.2" "$LIBDIR/"
ldconfig
info "NSS module → $LIBDIR/libnss_lnos.so.2"

# ----- nsswitch.conf -----
if grep -q "^hosts:.*\blnos\b" /etc/nsswitch.conf 2>/dev/null; then
    info "lnos already in /etc/nsswitch.conf"
else
    sed -i 's/^hosts:.*/& lnos/' /etc/nsswitch.conf
    info "Added 'lnos' to /etc/nsswitch.conf (before dns)"
fi

# ----- generate keys first -----
info "Generating keys..."
"$BUILD_DIR/lnosctl" generatekeys
CFG_DIR="$("$BUILD_DIR/lnosctl" init 2>&1 | grep -oP '/\S+lnos')"
[ -n "$CFG_DIR" ] && CFG_DIR="${CFG_DIR:-$HOME/.config/lnos}"

# ----- node name selection -----
DEVICE_NAMES=(laptop server node box desktop thinkpad raspi cubie macbook)
DEVICE_TYPES=(pc server laptop pi node hub)
OWNER_NAMES=(coldfox redcat bluejay greywolf darkowl silverfox goldfish wildbear moose coyote raven hawk eagle puma lynx bear wolf fox deer)

HOSTNAME_BASED="$(hostname).pc.$(whoami)"

pick_random_name() {
    local d="${DEVICE_NAMES[$((RANDOM % ${#DEVICE_NAMES[@]}))]}"
    local t="${DEVICE_TYPES[$((RANDOM % ${#DEVICE_TYPES[@]}))]}"
    local o="${OWNER_NAMES[$((RANDOM % ${#OWNER_NAMES[@]}))]}"
    echo "${d}.${t}.${o}"
}

check_name_free() {
    local name="$1"
    local result
    result=$("$BUILD_DIR/lnosctl" resolve "$name" 2>/dev/null)
    # If resolve returns an IP (contains a digit), name is taken
    case "$result" in
        *[0-9]*) return 1 ;;  # taken
        *)        return 0 ;;  # free
    esac
}

# Start daemon temporarily for name collision check
start_daemon_if_needed() {
    if ! pgrep -x lnosd >/dev/null 2>&1; then
        "$BUILD_DIR/lnosd" &
        DAEMON_PID=$!
        sleep 2
    fi
}

stop_daemon_if_started() {
    if [ -n "$DAEMON_PID" ]; then
        kill "$DAEMON_PID" 2>/dev/null
        wait "$DAEMON_PID" 2>/dev/null
        unset DAEMON_PID
    fi
}

info "Choose node name:"
echo "  1) Hostname-based — $HOSTNAME_BASED"
echo "  2) Random — $(pick_random_name)"
echo "  3) Manual input"
read -r -p "Choice [1]: " NAME_CHOICE
NAME_CHOICE="${NAME_CHOICE:-1}"

NODE_NAME=""
while [ -z "$NODE_NAME" ]; do
    case "$NAME_CHOICE" in
        1) NODE_NAME="$HOSTNAME_BASED" ;;
        2) NODE_NAME="$(pick_random_name)" ;;
        3) read -r -p "Enter node name (device.type.owner): " NODE_NAME ;;
        *) read -r -p "Invalid choice. Enter name manually: " NODE_NAME ;;
    esac

    if [ -n "$NODE_NAME" ]; then
        start_daemon_if_needed
        if ! check_name_free "$NODE_NAME"; then
            echo ""
            warn "Name '$NODE_NAME' is already taken on the network!"
            echo "Choose another:"
            echo "  1) Try another hostname-based"
            echo "  2) Try another random"
            echo "  3) Manual input"
            read -r -p "Choice [2]: " NAME_CHOICE
            NAME_CHOICE="${NAME_CHOICE:-2}"
            NODE_NAME=""
        fi
    fi
done

stop_daemon_if_started

"$BUILD_DIR/lnosctl" set name "$NODE_NAME"
info "Node name → $NODE_NAME"

# ----- systemd service -----
if command -v systemctl >/dev/null 2>&1; then
    UNIT="/etc/systemd/system/lnosd.service"
    if [ ! -f "$UNIT" ]; then
        cat > "$UNIT" <<EOF
[Unit]
Description=LNOS overlay networking daemon
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/bin/lnosd
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF
        systemctl daemon-reload
        info "systemd unit → $UNIT"
    else
        info "systemd unit already exists"
    fi
elif command -v rc-update >/dev/null 2>&1; then
    INITD="/etc/init.d/lnosd"
    cat > "$INITD" <<EOF
#!/sbin/openrc-run
command="/usr/local/bin/lnosd"
command_background=true
pidfile="/run/lnosd.pid"
EOF
    chmod +x "$INITD"
    info "OpenRC init script → $INITD"
fi

# ----- firewall -----
info "Firewall: opening multicast group 239.255.42.99:4545"
info "(all nodes must use the same group:port to discover each other)"
if command -v ufw >/dev/null 2>&1; then
    ufw allow proto udp to 239.255.42.99 port 4545 comment 'LNOS' 2>/dev/null && info "  ufw: done"
elif command -v firewall-cmd >/dev/null 2>&1; then
    firewall-cmd --permanent --add-rich-rule='rule family="ipv4" destination address="239.255.42.99" port port="4545" protocol="udp" accept' >/dev/null && firewall-cmd --reload >/dev/null && info "  firewalld: done"
elif command -v iptables >/dev/null 2>&1; then
    iptables -C INPUT -d 239.255.42.99 -p udp --dport 4545 -j ACCEPT 2>/dev/null || {
        iptables -A INPUT -d 239.255.42.99 -p udp --dport 4545 -j ACCEPT
        info "  iptables: done"
    }
else
    warn "  no firewall tool found; ensure multicast is not blocked"
fi

# ----- seed owners.db -----
echo "$OWNER" > "${CFG_DIR}/owners.db" 2>/dev/null || true
chmod 644 "${CFG_DIR}/owners.db" 2>/dev/null || true

# ----- done -----
echo ""
info "=== LNOS setup complete ==="
echo ""
echo "  Node name:     $NODE_NAME"
echo "  Config dir:    $CFG_DIR"
echo "  Binaries:      /usr/local/bin/lnosd, /usr/local/bin/lnosctl"
echo "  NSS module:    $LIBDIR/libnss_lnos.so.2"
echo ""
echo "  Start:    systemctl start lnosd"
echo "  Autostart: systemctl enable --now lnosd"
echo "  Manual:   sudo lnosd"
echo "  CLI:      lnosctl stats"
echo "  Logs:     journalctl -u lnosd -f"
echo "  Resolve:  getent hosts ${NODE_NAME}"
echo "  Uninstall: ./uninstall.sh"
echo ""
echo "  All nodes in the same network must use the same multicast group and port."
echo "  Default is 239.255.42.99:4545 — change only if it conflicts:"
echo "    lnosctl set mcast_group <ip>"
echo "    lnosctl set port <num>"
