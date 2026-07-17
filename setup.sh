#!/usr/bin/env bash

LNOS_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${LNOS_DIR}/build"

uninstall() {
    echo ""; header "Uninstall LNOS"
    local s=""; [ "$(id -u)" -ne 0 ] && s="sudo"
    $s systemctl stop lnosd >/dev/null 2>&1 || true
    $s systemctl disable lnosd >/dev/null 2>&1 || true
    $s rm -f /etc/systemd/system/lnosd.service >/dev/null 2>&1 || true
    $s systemctl daemon-reload >/dev/null 2>&1 || true
    $s rc-service lnosd stop >/dev/null 2>&1 || true
    $s rc-update del lnosd >/dev/null 2>&1 || true
    $s rm -f /etc/init.d/lnosd >/dev/null 2>&1 || true
    $s rm -f /usr/local/bin/lnosd /usr/local/bin/lnosctl
    for d in /usr/lib64 /usr/lib /lib64 /lib; do
        [ -f "$d/libnss_lnos.so.2" ] && $s rm -f "$d/libnss_lnos.so" "$d/libnss_lnos.so.2"
    done
    $s ldconfig >/dev/null 2>&1 || true
    $s sed -i 's/ lnos//g; s/  / /g' /etc/nsswitch.conf >/dev/null 2>&1 || true
    if command -v nft >/dev/null 2>&1; then
        $s nft delete rule inet filter input ip daddr 239.255.42.99 udp dport 4545 >/dev/null 2>&1 || true
    fi
    $s ufw delete allow proto udp to 239.255.42.99 port 4545 >/dev/null 2>&1 || true
    $s firewall-cmd --permanent --remove-rich-rule='rule family="ipv4" destination address="239.255.42.99" port port="4545" protocol="udp" accept' >/dev/null 2>&1 || true
    $s firewall-cmd --reload >/dev/null 2>&1 || true
    $s iptables -D INPUT -d 239.255.42.99 -p udp --dport 4545 -j ACCEPT >/dev/null 2>&1 || true
    read -r -p "Remove config and keys? [y/N]: " ans; [[ "$ans" =~ ^[yY] ]] && $s rm -rf /etc/lnos
    read -r -p "Remove build directory? [y/N]: " ans; [[ "$ans" =~ ^[yY] ]] && rm -rf "$BUILD_DIR"
    info "Uninstall complete"
    exit 0
}

[ "$1" = "--uninstall" ] || [ "$1" = "-u" ] && uninstall

BOLD='\e[1m'
DIM='\e[2m'
GREEN='\e[1;32m'
CYAN='\e[1;36m'
YELLOW='\e[1;33m'
RED='\e[1;31m'
BLUE='\e[38;5;32m'
NC='\e[0m'
UL='\e[4m'

info()  { echo -e " ${GREEN}◆${NC} $*"; }
warn()  { echo -e " ${YELLOW}⚠${NC} $*"; }
header(){ echo -e "\n ${BOLD}${CYAN}── $* ──${NC}\n"; }

INTERACTIVE=false
[ -t 0 ] || [ -t 1 ] || [ -t 2 ] && INTERACTIVE=true

prompt() {
    local v="$1" msg="$2" def="$3"
    if $INTERACTIVE; then
        if [ -t 0 ]; then
            read -r -p "$msg" "$v" || true
        else
            read -r -p "$msg" "$v" < /dev/tty 2>/dev/null || true
        fi
    fi
    [ -z "$(eval echo \$$v)" ] && eval "$v=\$def"
}

echo -e ""
echo -e " ${GREEN}Local Network Overlay System${NC}"
echo -e " ${CYAN}encrypted peer discovery & name resolution${NC}"
echo -e ""
echo -e " ${BLUE} ██▓     ███▄    █  ▒█████    ██████ ${NC}"
echo -e " ${BLUE}▓██▒     ██ ▀█   █ ▒██▒  ██▒▒██    ▒ ${NC}"
echo -e " ${BLUE}▒██░    ▓██  ▀█ ██▒▒██░  ██▒░ ▓██▄   ${NC}"
echo -e " ${BLUE}▒██░    ▓██▒  ▐▌██▒▒██   ██░  ▒   ██▒${NC}"
echo -e " ${BLUE}░██████▒▒██░   ▓██░░ ████▓▒░▒██████▒▒${NC}"
echo -e " ${BLUE}░ ▒░▓  ░░ ▒░   ▒ ▒ ░ ▒░▒░▒░ ▒ ▒▓▒ ▒ ░${NC}"
echo -e " ${BLUE}░ ░ ▒  ░░ ░░   ░ ▒░  ░ ▒ ▒░ ░ ░▒  ░ ░${NC}"
echo -e " ${BLUE}  ░ ░      ░   ░ ░ ░ ░ ░ ▒  ░  ░  ░  ${NC}"
echo -e " ${BLUE}    ░  ░         ░     ░ ░        ░    ${NC}"
echo -e ""

SUDO="sudo"
WHOAMI="$(whoami)"
if [ "$(id -u)" -eq 0 ]; then
    if [ -z "$SUDO_USER" ]; then
        if $INTERACTIVE; then
            echo -e " ${YELLOW}⚠${NC} Running as ${BOLD}root${NC} (not via sudo)."
            echo -e "   Configs will be owned by root."
            prompt ans "  Continue as root? [y/N]: " "n"
            case "$ans" in [yY]|[yY][eE][sS]) ;; *) echo "  Exiting."; exit 1 ;; esac
        fi
        SUDO=""
    else
        WHOAMI="$SUDO_USER"
        SUDO=""
    fi
fi

header "Dependencies"

PM=""
for pair in "apt-get:apt-get" "pacman:pacman" "dnf:dnf" "zypper:zypper" "emerge:emerge" "apk:apk"; do
    cmd="${pair#*:}"; [ -z "$PM" ] && command -v "${pair%%:*}" >/dev/null 2>&1 && PM="$cmd"
done

MISSING=""
for cmd in cmake g++ make; do
    command -v "$cmd" >/dev/null 2>&1 || MISSING="$MISSING $cmd"
done
! ldconfig -p 2>/dev/null | grep -q libsodium && ! pkg-config --exists libsodium 2>/dev/null && MISSING="$MISSING libsodium"

if [ -n "$MISSING" ]; then
    info "Installing:$MISSING"
    case "$PM" in
        apt-get) $SUDO apt-get update -qq && $SUDO apt-get install -y -qq cmake g++ make libsodium-dev ;;
        pacman)  pacman -S --noconfirm cmake gcc make libsodium ;;
        dnf)     dnf install -y cmake gcc-c++ make libsodium-devel ;;
        zypper)  zypper install -y cmake gcc-c++ make libsodium-devel ;;
        emerge)  emerge --ask n dev-util/cmake sys-devel/gcc dev-libs/libsodium ;;
        apk)     apk add cmake g++ make libsodium-dev ;;
        *)       warn "Install manually: cmake, g++, libsodium-dev" ;;
    esac
else
    info "All dependencies satisfied"
fi

if [ -f /usr/local/bin/lnosd ]; then
    if $INTERACTIVE; then
        echo -e ""
        warn "LNOS already installed (found /usr/local/bin/lnosd)"
        echo -e "    ${CYAN}1${NC}) Reinstall — clean build & full reinstall"
        echo -e "    ${CYAN}2${NC}) Update — rebuild & reinstall, keep config"
        echo -e "    ${CYAN}3${NC}) Uninstall — remove everything"
        echo -e "    ${CYAN}4${NC}) Cancel"
        prompt REINSTALL "    Choice [1]: " "1"
        case "$REINSTALL" in
            2) info "Updating LNOS..." ;;
            3) uninstall ;;
            4) echo "  Exiting."; exit 0 ;;
            *) rm -rf "$BUILD_DIR"; info "Reinstalling LNOS..." ;;
        esac
    else
        info "LNOS already installed — rebuilding"
    fi
fi

header "Build"

info "Building LNOS..."
cmake -S "$LNOS_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$BUILD_DIR" -j"$(nproc)"
info "Build complete"

for d in /usr/lib64 /usr/lib /lib64 /lib; do
    [ -f "$d/libnss_files.so.2" ] && { LIBDIR="$d"; break; }
done
[ -n "$LIBDIR" ] || LIBDIR="/usr/lib"

header "Install"

info "Installing binaries..."
$SUDO install -m 755 "$BUILD_DIR/lnosd" /usr/local/bin/lnosd
$SUDO install -m 755 "$BUILD_DIR/lnosctl" /usr/local/bin/lnosctl
info "  lnosd   → /usr/local/bin/lnosd"
info "  lnosctl → /usr/local/bin/lnosctl"

$SUDO cp "$BUILD_DIR/libnss_lnos.so.2" "$LIBDIR/"
$SUDO ldconfig
info "  NSS     → ${LIBDIR}/libnss_lnos.so.2"

NSSWITCH_LNOS=$(grep -c '^hosts:.*\<lnos\>' /etc/nsswitch.conf 2>/dev/null || true)
if [ "$NSSWITCH_LNOS" -gt 1 ]; then
    $SUDO sed -i 's/\blnos\s*//g' /etc/nsswitch.conf
    $SUDO sed -i 's/^hosts:.*/& lnos/' /etc/nsswitch.conf
    info "  nsswitch.conf: cleaned duplicate lnos entries"
elif [ "$NSSWITCH_LNOS" -eq 0 ]; then
    $SUDO sed -i 's/^hosts:.*/& lnos/' /etc/nsswitch.conf
    info "  nsswitch.conf: added lnos"
else
    info "  nsswitch.conf: lnos already present"
fi

$SUDO mkdir -p /etc/lnos
CFG_DIR="/etc/lnos"

header "Keys & Configuration"

if [ -f "${CFG_DIR}/public.key" ]; then
    info "Keys already exist, keeping them"
else
    info "Generating Ed25519 keys..."
    $SUDO "$BUILD_DIR/lnosctl" generatekeys 2>/dev/null || true
fi
$SUDO "$BUILD_DIR/lnosctl" init 2>/dev/null || true
info "  Keys stored in ${CFG_DIR}"

DAEMON_ALREADY_RUNNING=false
if pgrep -x lnosd >/dev/null 2>&1; then
    info "Stopping running lnosd (PID $(pgrep -x lnosd))..."
    $SUDO systemctl stop lnosd 2>/dev/null || $SUDO pkill -x lnosd 2>/dev/null || true
    sleep 1
fi

DEVICE_NAMES=(laptop server node box desktop thinkpad raspi cubie macbook)
DEVICE_TYPES=(pc server laptop pi node hub)
OWNER_NAMES=(coldfox redcat bluejay greywolf darkowl silverfox goldfish wildbear moose coyote raven hawk eagle puma lynx bear wolf fox deer)
HOSTNAME_BASED="$(hostname).pc.${WHOAMI}"

pick_random_name() {
    local d="${DEVICE_NAMES[$((RANDOM % ${#DEVICE_NAMES[@]}))]}"
    local t="${DEVICE_TYPES[$((RANDOM % ${#DEVICE_TYPES[@]}))]}"
    local o="${OWNER_NAMES[$((RANDOM % ${#OWNER_NAMES[@]}))]}"
    echo "${d}.${t}.${o}"
}

check_name_free() {
    local result; result=$("$BUILD_DIR/lnosctl" resolve "$1" 2>/dev/null)
    case "$result" in *[0-9]*) return 1 ;; *) return 0 ;; esac
}

RANDOM_FALLBACK="$(pick_random_name)"
RANDOM_PREVIEW="$RANDOM_FALLBACK"

if $INTERACTIVE; then
    echo -e ""
    echo -e "  ${BOLD}Choose node name format:${NC}"
    echo -e "    ${CYAN}1${NC}) Hostname-based — ${GREEN}${HOSTNAME_BASED}${NC}"
    echo -e "    ${CYAN}2${NC}) Random        — ${GREEN}${RANDOM_PREVIEW}${NC}"
    echo -e "    ${CYAN}3${NC}) Manual input"
    prompt NAME_CHOICE "  Choice [1]: " "1"
else
    NAME_CHOICE="1"
    info "Using hostname-based name (non-interactive)"
fi

NODE_NAME=""
while [ -z "$NODE_NAME" ]; do
    case "$NAME_CHOICE" in
        1) NODE_NAME="$HOSTNAME_BASED" ;;
        2) NODE_NAME="$RANDOM_FALLBACK" ;;
        3) prompt NODE_NAME "  Enter name (device.type.owner): " ""; [ -z "$NODE_NAME" ] && NODE_NAME="$HOSTNAME_BASED" ;;
        *) NODE_NAME="$HOSTNAME_BASED" ;;
    esac

    if [ -z "$NODE_NAME" ]; then continue; fi

    # collision check только против уже запущенного ВНЕШНЕГО daemon
    if $DAEMON_ALREADY_RUNNING; then
        if ! check_name_free "$NODE_NAME"; then
            warn "Name '${NODE_NAME}' is ${BOLD}taken${NC}!"
            if $INTERACTIVE; then
                echo ""; echo -e "    ${CYAN}1${NC}) Try hostname-based"; echo -e "    ${CYAN}2${NC}) Try another random"; echo -e "    ${CYAN}3${NC}) Manual input"
                prompt NAME_CHOICE "    Choice [2]: " "2"; NODE_NAME=""
                RANDOM_FALLBACK="$(pick_random_name)"
            else
                info "Using a random suffix"
                NODE_NAME="${NODE_NAME}.$(tr -dc a-z0-9 < /dev/urandom 2>/dev/null | head -c4 || echo "x4g7")"
            fi
        fi
    fi
done

$SUDO "$BUILD_DIR/lnosctl" set name "$NODE_NAME" 2>/dev/null || warn "Failed to set node name (try: sudo lnosctl set name $NODE_NAME)"

if command -v systemctl >/dev/null 2>&1; then
    UNIT="/etc/systemd/system/lnosd.service"
    if [ ! -f "$UNIT" ]; then
        $SUDO tee "$UNIT" > /dev/null <<EOF
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
        $SUDO systemctl daemon-reload; info "systemd unit created"
    else
        info "systemd unit exists"
    fi
elif command -v rc-update >/dev/null 2>&1; then
    INITD="/etc/init.d/lnosd"
    if [ ! -f "$INITD" ]; then
        $SUDO tee "$INITD" > /dev/null <<EOF
#!/sbin/openrc-run
command="/usr/local/bin/lnosd"
command_background=true
pidfile="/run/lnosd.pid"
EOF
        $SUDO chmod +x "$INITD"; info "OpenRC init script created"
    else
        info "OpenRC init script exists"
    fi
fi

FWR=""
if command -v ufw >/dev/null 2>&1; then
    $SUDO ufw allow proto udp to 239.255.42.99 port 4545 comment 'LNOS' 2>/dev/null && FWR="ufw" && $SUDO ufw reload 2>/dev/null || true
elif command -v firewall-cmd >/dev/null 2>&1; then
    $SUDO firewall-cmd --permanent --add-rich-rule='rule family="ipv4" destination address="239.255.42.99" port port="4545" protocol="udp" accept' >/dev/null 2>&1 && $SUDO firewall-cmd --reload >/dev/null 2>&1 && FWR="firewalld"
elif command -v nft >/dev/null 2>&1; then
    if ! $SUDO nft list ruleset 2>/dev/null | grep -q "239.255.42.99"; then
        $SUDO nft add rule inet filter input ip daddr 239.255.42.99 udp dport 4545 accept 2>/dev/null && FWR="nftables"
    else
        FWR="nftables (already set)"
    fi
elif command -v iptables >/dev/null 2>&1; then
    if ! $SUDO iptables -C INPUT -d 239.255.42.99 -p udp --dport 4545 -j ACCEPT 2>/dev/null; then
        $SUDO iptables -A INPUT -d 239.255.42.99 -p udp --dport 4545 -j ACCEPT && FWR="iptables"
    else
        FWR="iptables (already set)"
    fi
fi
[ -n "$FWR" ] && info "Firewall: opened multicast (239.255.42.99:4545) via ${FWR}" || warn "No firewall tool detected; ensure multicast (239.255.42.99:4545) is not blocked"

echo "${NODE_NAME##*.}" | $SUDO tee "${CFG_DIR}/owners.db" > /dev/null || true

if [ "${DAEMON_RESTART_AFTER}" = "true" ]; then
    info "Restarting lnosd with new config..."
    RESTARTED=false
    command -v systemctl >/dev/null 2>&1 && $SUDO systemctl restart lnosd 2>/dev/null && RESTARTED=true
    if ! $RESTARTED; then
        pkill lnosd 2>/dev/null || true; sleep 1
        nohup /usr/local/bin/lnosd > /dev/null 2>&1 &
    fi
    sleep 2
    pgrep -x lnosd >/dev/null 2>&1 && info "lnosd restarted successfully" || warn "lnosd failed to restart — check logs: journalctl -u lnosd"
fi

DAEMON_JUST_STARTED=false
if ! $DAEMON_ALREADY_RUNNING && [ "${DAEMON_RESTART_AFTER}" != "true" ]; then
    info "Starting lnosd..."
    if command -v systemctl >/dev/null 2>&1 && [ -f /etc/systemd/system/lnosd.service ]; then
        $SUDO systemctl start lnosd 2>/dev/null || $SUDO nohup /usr/local/bin/lnosd > /dev/null 2>&1 &
    else
        $SUDO nohup /usr/local/bin/lnosd > /dev/null 2>&1 &
    fi
    sleep 2
    if pgrep -x lnosd >/dev/null 2>&1; then
        echo -e " ${GREEN}✓${NC} lnosd is running"; DAEMON_JUST_STARTED=true
    else
        warn "lnosd didn't start — run: sudo systemctl start lnosd"
    fi
fi

if $DAEMON_JUST_STARTED || $DAEMON_ALREADY_RUNNING; then
    for i in $(seq 1 10); do
        curl -sf http://localhost:9999 >/dev/null 2>&1 && break
        sleep 1
    done
    if curl -sf http://localhost:9999 >/dev/null 2>&1; then
        if command -v xdg-open >/dev/null 2>&1; then
            nohup xdg-open "http://localhost:9999" > /dev/null 2>&1 &
        elif command -v sensible-browser >/dev/null 2>&1; then
            nohup sensible-browser "http://localhost:9999" > /dev/null 2>&1 &
        fi
    else
        warn "Dashboard not reachable at http://localhost:9999 — daemon HTTP server may have failed to start"
    fi
fi

MY_IP=""
for iface in $(ip -4 addr show scope global up | grep -oP 'inet \K[\d.]+'); do
    case "$iface" in 127.*|172.17.*|172.18.*) ;; *) MY_IP="$iface"; break ;; esac
done
[ -z "$MY_IP" ] && MY_IP="<detect on target machine>"

echo -e ""
header "Setup complete"

W=65
pline() { local t; t=$(echo -e "$*" | sed 's/\x1b\[[0-9;]*m//g'); local pad=$((W - ${#t} - 2)); [ "$pad" -lt 0 ] && pad=0; echo -e " ${DIM}│${NC} $(echo -e "$*")$(printf '%*s' "$pad" '') ${DIM}│${NC}"; }
H=; for ((i=0; i<W; i++)); do H+="─"; done
echo -e " ${DIM}┌${H}┐${NC}"
pline " ${GREEN}Your node${NC}"
pline "   Name:  ${BOLD}${NODE_NAME}${NC}"
pline "   IP:    ${MY_IP}"
pline "   Port:  4545    Group: 239.255.42.99"
pline ""
pline " ${GREEN}Dashboard${NC}"
pline "   ${UL}http://localhost:9999${NC}     — local"
pline "   ${UL}http://${MY_IP}:9999${NC}      — LAN"
pline "   ${UL}http://${NODE_NAME}:9999${NC}  — via overlay"
pline ""
pline " ${GREEN}Status${NC}"
if pgrep -x lnosd >/dev/null 2>&1; then
    pline "   ${GREEN}●${NC} lnosd is ${GREEN}running${NC}  (PID $(pgrep -x lnosd | head -1))"
else
    pline "   ${YELLOW}○${NC} lnosd is ${YELLOW}stopped${NC}  — run: sudo systemctl start lnosd"
fi
pline ""
pline " ${GREEN}Commands${NC}"
pline "   systemctl ${BOLD}start${NC} lnosd"
pline "   lnosctl ${BOLD}stats${NC}                   show peer count & metrics"
pline "   getent hosts ${BOLD}${NODE_NAME}${NC}        resolve your own name"
pline "   journalctl ${BOLD}-u lnosd -f${NC}          follow daemon logs"
pline ""
pline " ${GREEN}On other machines${NC}"
pline "   Install LNOS the same way — all nodes discover"
pline "   each other automatically on the same multicast"
pline "   group (239.255.42.99:4545)."
echo -e " ${DIM}└${H}┘${NC}"

echo -e ""
echo -e " ${DIM}Config:${NC}      ${BOLD}${CFG_DIR}/${NC}"
echo -e " ${DIM}Binaries:${NC}    ${BOLD}/usr/local/bin/{lnosd,lnosctl}${NC}"
echo -e " ${DIM}NSS module:${NC}  ${BOLD}${LIBDIR}/libnss_lnos.so.2${NC}"
echo -e " ${DIM}nsswitch:${NC}    ${BOLD}/etc/nsswitch.conf${NC} (hosts: ... lnos)"
echo -e " ${DIM}Uninstall:${NC}   ${BOLD}./setup.sh --uninstall${NC}"
echo -e ""
