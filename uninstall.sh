#!/usr/bin/env bash
set -e

LNOS_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${LNOS_DIR}/build"

info()  { echo -e "\e[1;32m[INFO]\e[0m $*"; }
warn()  { echo -e "\e[1;33m[WARN]\e[0m $*"; }
err()   { echo -e "\e[1;31m[ERRO]\e[0m $*" >&2; }

is_root() { [ "$(id -u)" -eq 0 ]; }

info "=== LNOS uninstall ==="
if ! is_root; then
    err "This script must be run as root"
    exit 1
fi

# ----- stop daemon -----
if command -v systemctl >/dev/null 2>&1; then
    systemctl stop lnosd 2>/dev/null || true
    systemctl disable lnosd 2>/dev/null || true
    rm -f /etc/systemd/system/lnosd.service
    systemctl daemon-reload 2>/dev/null || true
    info "systemd service removed"
elif [ -f /etc/init.d/lnosd ]; then
    rc-service lnosd stop 2>/dev/null || true
    rc-update del lnosd 2>/dev/null || true
    rm -f /etc/init.d/lnosd
    info "OpenRC service removed"
fi

# ----- remove binaries -----
rm -f /usr/local/bin/lnosd /usr/local/bin/lnosctl
info "Removed /usr/local/bin/lnosd, /usr/local/bin/lnosctl"

# ----- remove NSS module -----
for d in /usr/lib64 /usr/lib /lib64 /lib; do
    if [ -f "$d/libnss_lnos.so.2" ]; then
        rm -f "$d/libnss_lnos.so" "$d/libnss_lnos.so.2"
        info "Removed $d/libnss_lnos.so.2"
    fi
done
ldconfig 2>/dev/null || true

# ----- revert nsswitch.conf -----
if grep -q '\blnos\b' /etc/nsswitch.conf 2>/dev/null; then
    sed -i 's/ lnos//g; s/  / /g' /etc/nsswitch.conf
    info "Removed lnos from /etc/nsswitch.conf"
fi

# ----- firewall cleanup -----
if command -v ufw >/dev/null 2>&1; then
    ufw delete allow proto udp to 239.255.42.99 port 4545 2>/dev/null && info "ufw rule removed"
elif command -v firewall-cmd >/dev/null 2>&1; then
    firewall-cmd --permanent --remove-rich-rule='rule family="ipv4" destination address="239.255.42.99" port port="4545" protocol="udp" accept' >/dev/null 2>&1 && firewall-cmd --reload >/dev/null 2>&1 && info "firewalld rule removed"
elif command -v iptables >/dev/null 2>&1; then
    iptables -D INPUT -d 239.255.42.99 -p udp --dport 4545 -j ACCEPT 2>/dev/null && info "iptables rule removed"
fi

# ----- remove config -----
read -r -p "Remove LNOS config and keys? [y/N]: " RMCFG
if [ "$RMCFG" = "y" ] || [ "$RMCFG" = "Y" ]; then
    for d in /root/.config/lnos /etc/lnos; do
        [ -d "$d" ] && rm -rf "$d" && info "Removed $d"
    done
    # also remove from home dirs
    for h in /home/*; do
        [ -d "$h/.config/lnos" ] && rm -rf "$h/.config/lnos" && info "Removed $h/.config/lnos"
    done
fi

# ----- remove build -----
if [ -d "$BUILD_DIR" ]; then
    rm -rf "$BUILD_DIR"
    info "Removed build dir: $BUILD_DIR"
fi

read -r -p "Remove LNOS source directory (this script will self-destruct)? [y/N]: " RMSRC
if [ "$RMSRC" = "y" ] || [ "$RMSRC" = "Y" ]; then
    cd /tmp && rm -rf "$LNOS_DIR"
    info "LNOS source removed"
fi

info "=== LNOS uninstall complete ==="
