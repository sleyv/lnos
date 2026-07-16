#!/usr/bin/env bash
set -e

LNOS_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${LNOS_DIR}/build"

info()  { echo -e "\e[1;32m[INFO]\e[0m $*"; }
warn()  { echo -e "\e[1;33m[WARN]\e[0m $*"; }
err()   { echo -e "\e[1;31m[ERRO]\e[0m $*" >&2; }

info "=== LNOS setup ==="

# ----- dependencies -----
DEPS=""
for cmd in cmake g++ make; do
    command -v "$cmd" >/dev/null 2>&1 || DEPS="$DEPS $cmd"
done
if pkg-config --exists libsodium 2>/dev/null; then
    : 
elif ldconfig -p 2>/dev/null | grep -q libsodium; then
    :
else
    DEPS="$DEPS libsodium"
fi

if [ -n "$DEPS" ]; then
    info "Missing dependencies:$DEPS"
    if command -v apt-get >/dev/null 2>&1; then
        apt-get update -qq && apt-get install -y -qq cmake g++ make libsodium-dev
    elif command -v pacman >/dev/null 2>&1; then
        pacman -S --noconfirm cmake gcc make libsodium
    elif command -v dnf >/dev/null 2>&1; then
        dnf install -y cmake gcc-c++ make libsodium-devel
    else
        warn "Unknown package manager. Install deps manually: cmake, g++, libsodium"
    fi
fi

# ----- build -----
info "Building LNOS..."
cmake -S "$LNOS_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" -j"$(nproc)"

# ----- install NSS module -----
LIBDIR="/usr/lib"
if [ -d /usr/lib64 ] && [ ! -L /usr/lib64 ]; then
    LIBDIR="/usr/lib64"
fi
cp "$BUILD_DIR/libnss_lnos.so.2" "$LIBDIR/"
ldconfig
info "NSS module installed to $LIBDIR/libnss_lnos.so.2"

# ----- configure nsswitch.conf -----
NSSWITCH="/etc/nsswitch.conf"
if grep -q "^hosts:.*\<lnos\>" "$NSSWITCH" 2>/dev/null; then
    info "lnos already in nsswitch.conf"
else
    sed -i 's/^hosts:.*/& lnos/' "$NSSWITCH"
    info "Added lnos to $NSSWITCH (before dns)"
fi

# ----- LNOS config -----
info "Configuring LNOS..."
"$BUILD_DIR/lnosctl" init

read -r -p "Device name [default: $(hostname)]: " DEVICE
DEVICE="${DEVICE:-$(hostname)}"
read -r -p "Device type (e.g. pc, laptop, server, pi) [pc]: " DTYPE
DTYPE="${DTYPE:-pc}"
read -r -p "Owner [$(whoami)]: " OWNER
OWNER="${OWNER:-$(whoami)}"

NODE_NAME="${DEVICE}.${DTYPE}.${OWNER}"
"$BUILD_DIR/lnosctl" set name "$NODE_NAME"
info "Node name set to: $NODE_NAME"

info "Generating keys..."
"$BUILD_DIR/lnosctl" generatekeys

# ----- systemd service -----
UNIT="/etc/systemd/system/lnosd.service"
if [ ! -f "$UNIT" ]; then
    cat > "$UNIT" <<EOF
[Unit]
Description=LNOS overlay networking daemon
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=$BUILD_DIR/lnosd
Restart=on-failure
RestartSec=5
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF
    systemctl daemon-reload
    info "systemd unit created: $UNIT"
else
    info "systemd unit already exists: $UNIT"
fi

# ----- firewall -----
if command -v ufw >/dev/null 2>&1; then
    if ! ufw status | grep -q '239.255.42.99'; then
        ufw allow proto udp from 224.0.0.0/4 to 239.255.42.99 port 4545 comment 'LNOS multicast'
        info "ufw: allowed multicast 239.255.42.99:4545"
    fi
elif command -v firewall-cmd >/dev/null 2>&1; then
    firewall-cmd --add-rich-rule='rule family="ipv4" destination address="239.255.42.99" port port="4545" protocol="udp" accept' --permanent
    firewall-cmd --reload
    info "firewalld: allowed multicast 239.255.42.99:4545"
fi

# ----- seed owners.db -----
echo "$OWNER" > "$(sudo -u "$SUDO_USER" -H sh -c 'echo $HOME')/.config/lnos/owners.db" 2>/dev/null || true

# ----- done -----
echo ""
info "=== LNOS setup complete ==="
echo ""
echo "  Node name:     $NODE_NAME"
echo "  Config dir:    $(sudo -u "$SUDO_USER" -H sh -c 'echo $HOME')/.config/lnos"
echo "  Build dir:     $BUILD_DIR"
echo "  NSS module:    $LIBDIR/libnss_lnos.so.2"
echo "  Systemd unit:  $UNIT"
echo ""
echo "  Start daemon:  systemctl start lnosd"
echo "  Enable auto:   systemctl enable --now lnosd"
echo "  Check logs:    journalctl -u lnosd -f"
echo "  Resolve test:  getent hosts ${NODE_NAME}"
echo ""
echo "  (change firewall rules, multicast group, or port in the config files)"
