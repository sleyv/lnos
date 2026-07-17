#!/usr/bin/env bash
set -e

LNOS_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${LNOS_DIR}/build"

# Try to make stdin interactive (curl | bash compat)
exec < /dev/tty 2>/dev/null || true

# ----- colors -----
BOLD='\e[1m'
DIM='\e[2m'
GREEN='\e[1;32m'
CYAN='\e[1;36m'
YELLOW='\e[1;33m'
RED='\e[1;31m'
MAGENTA='\e[1;35m'
NC='\e[0m'
UL='\e[4m'

info()  { echo -e " ${GREEN}◆${NC} $*"; }
warn()  { echo -e " ${YELLOW}⚠${NC} $*"; }
err()   { echo -e " ${RED}✗${NC} $*" >&2; }
header(){ echo -e "\n ${BOLD}${CYAN}── $* ──${NC}\n"; }
box()   { local lines=()
          while IFS= read -r line; do lines+=("$line"); done
          echo -e "${DIM}┌─────────────────────────────────────────────────────────────┐${NC}"
          for line in "${lines[@]}"; do
            local plain; plain=$(echo -e "$line" | sed 's/\x1b\[[0-9;]*m//g')
            local len=${#plain}
            local pad=$((59 - len)); [ "$pad" -lt 0 ] && pad=0
            printf "${DIM}│${NC} "
            echo -en "$(echo -e "$line")"
            printf "%${pad}s ${DIM}│${NC}\n" ""
          done
          echo -e "${DIM}└─────────────────────────────────────────────────────────────┘${NC}"; }

# ----- interactive check -----
INTERACTIVE=false
if [ -t 0 ] || [ -t 1 ] || [ -t 2 ]; then
    INTERACTIVE=true
    if [ ! -t 0 ]; then
        exec < /dev/tty 2>/dev/null || INTERACTIVE=false
    fi
fi

prompt() {
    local v="$1" msg="$2" def="$3"
    if $INTERACTIVE; then
        local r=false
        read -r -p "$msg" "$v" < /dev/tty 2>/dev/null && r=true
        $r || read -r -p "$msg" "$v" 2>/dev/null || true
    fi
    [ -z "$(eval echo \$$v)" ] && eval "$v=\$def"
}

# ----- banner -----
echo -e ""
echo -e " ${MAGENTA} _      _   _    ____    ____ ${NC}"
echo -e " ${MAGENTA}| |    | \\ | |  / __ \\  / ___|${NC}"
echo -e " ${MAGENTA}| |    |  \\| | | |  | | \\___ \\ ${NC}"
echo -e " ${MAGENTA}| |___ | |\\  | | |__| |  ___) |${NC}"
echo -e " ${MAGENTA}|_____||_| \\_|  \\____/  |____/ ${NC}"
echo -e " ${GREEN}Local Network Overlay System${NC}"
echo -e " ${CYAN}encrypted peer discovery & name resolution${NC}"
echo -e ""

# ----- root check -----
if [ "$(id -u)" -eq 0 ]; then
    if [ -z "$SUDO_USER" ]; then
        if $INTERACTIVE; then
            echo -e " ${YELLOW}⚠${NC} Running as ${BOLD}root${NC} (not via sudo)."
            echo -e "   Configs will be owned by root."
            prompt ans "  Continue as root? [y/N]: " "n"
            case "$ans" in [yY]|[yY][eE][sS]) ;; *) echo "  Exiting."; exit 1 ;; esac
        else
            echo -e " ${YELLOW}⚠${NC} Running as root (non-interactive) — continuing"
        fi
        AS_USER=""
        SUDO=""
    else
        AS_USER="sudo -u ${SUDO_USER}"
        SUDO=""
    fi
else
    AS_USER=""
    SUDO="sudo"
fi

# ----- whoami for hostname-based name -----
if [ -n "$SUDO_USER" ]; then
    WHOAMI="$SUDO_USER"
else
    WHOAMI="$(whoami)"
fi

header "Dependencies"

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
    info "Installing:$MISSING"
    case "$PM" in
        apt-get) [ "$(id -u)" -eq 0 ] && apt-get update -qq && apt-get install -y -qq $APT_DEPS || warn "run as root: apt-get install $APT_DEPS" ;;
        pacman)  pacman -S --noconfirm $PAC_DEPS ;;
        dnf)     dnf install -y $DNF_DEPS ;;
        zypper)  zypper install -y $ZYP_DEPS ;;
        emerge)  emerge --ask n $EMG_DEPS ;;
        apk)     apk add $APK_DEPS ;;
        *)       warn "Unknown package manager. Install: cmake, g++, libsodium-dev" ;;
    esac
else
    info "All dependencies satisfied"
fi

header "Build"

info "Building LNOS..."
cmake -S "$LNOS_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build "$BUILD_DIR" -j"$(nproc)"
info "Build complete"

# ----- determine lib dir -----
for d in /usr/lib64 /usr/lib /lib64 /lib; do
    if [ -f "$d/libnss_files.so.2" ]; then
        LIBDIR="$d"
        break
    fi
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

# ----- nsswitch.conf (handle multiple entries gracefully) -----
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
    $SUDO "$BUILD_DIR/lnosctl" generatekeys
fi
$SUDO "$BUILD_DIR/lnosctl" init
info "  Keys stored in ${CFG_DIR}"

# ----- running daemon check -----
DAEMON_ALREADY_RUNNING=false
DAEMON_PID=""
DAEMON_RESTART_AFTER=false
if pgrep -x lnosd >/dev/null 2>&1; then
    DAEMON_ALREADY_RUNNING=true
    if $INTERACTIVE; then
        echo -e ""
        warn "lnosd is already running (PID $(pgrep -x lnosd))"
        echo -e ""
        echo -e "  ${BOLD}What do you want to do?${NC}"
        echo -e "    ${CYAN}1${NC}) Keep running — collision check via it, apply name on restart"
        echo -e "    ${CYAN}2${NC}) Restart daemon after setup — apply new config now"
        echo -e "    ${CYAN}3${NC}) Stop daemon — setup manages it from scratch"
        prompt DAEMON_CHOICE "  Choice [1]: " "1"
        case "$DAEMON_CHOICE" in
            2) DAEMON_RESTART_AFTER=true; info "Will restart lnosd after setup" ;;
            3) info "Stopping lnosd..."
               $SUDO systemctl stop lnosd 2>/dev/null || pkill lnosd 2>/dev/null || true
               sleep 1 ;;
            *) info "Using running daemon" ;;
        esac
    else
        info "lnosd already running (PID $(pgrep -x lnosd)) — keeping it"
    fi
fi

# ----- node name selection -----
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
    local name="$1"
    local result
    result=$("$BUILD_DIR/lnosctl" resolve "$name" 2>/dev/null || true)
    case "$result" in
        *[0-9]*) return 1 ;;
        *)        return 0 ;;
    esac
}

start_temp_daemon() {
    info "Starting temporary daemon for collision check..."
    # Cache sudo credentials in the foreground so background sudo doesn't block or get stopped
    $SUDO true
    $SUDO "$BUILD_DIR/lnosd" > /dev/null 2>&1 &
    DAEMON_PID=$!
    SOCKET_PATH="${CFG_DIR}/lnosd.sock"
    for i in 1 2 3 4 5 6 7 8 9 10; do
        [ -S "$SOCKET_PATH" ] && break
        sleep 1
    done
    if [ ! -S "$SOCKET_PATH" ]; then
        warn "Daemon didn't start in time, skipping collision check"
        return 1
    fi
    return 0
}

stop_temp_daemon() {
    # Cleanly kill the background lnosd child process by its build path
    $SUDO pkill -f "$BUILD_DIR/lnosd" 2>/dev/null || true
    if [ -n "$DAEMON_PID" ]; then
        $SUDO kill "$DAEMON_PID" 2>/dev/null
        for i in 1 2 3 4 5; do
            $SUDO kill -0 "$DAEMON_PID" 2>/dev/null || break
            sleep 0.2
        done
        unset DAEMON_PID
    fi
}

NEED_COLLISION_CHECK=true
$DAEMON_ALREADY_RUNNING && NEED_COLLISION_CHECK=false

RANDOM_PREVIEW="$(pick_random_name)"

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
        2) NODE_NAME="${RANDOM_FALLBACK:-$(pick_random_name)}" ;;
        3) prompt NODE_NAME "  Enter name (device.type.owner): " ""; [ -z "$NODE_NAME" ] && NODE_NAME="$HOSTNAME_BASED" ;;
        *) NODE_NAME="$HOSTNAME_BASED" ;;
    esac

    if [ -n "$NODE_NAME" ]; then
        if $NEED_COLLISION_CHECK; then
            start_temp_daemon && CHECK="ok" || CHECK="skip"
            if [ "$CHECK" = "ok" ] && ! check_name_free "$NODE_NAME"; then
                warn "Name '${NODE_NAME}' is ${BOLD}taken${NC}!"
                if $INTERACTIVE; then
                    echo ""
                    echo -e "    ${CYAN}1${NC}) Try hostname-based"
                    echo -e "    ${CYAN}2${NC}) Try another random"
                    echo -e "    ${CYAN}3${NC}) Manual input"
                    prompt NAME_CHOICE "    Choice [2]: " "2"
                    NODE_NAME=""
                    RANDOM_FALLBACK="$(pick_random_name)"
                else
                    info "Using a random suffix"
                    NODE_NAME="${NODE_NAME}.$(tr -dc a-z0-9 < /dev/urandom 2>/dev/null | head -c4 || echo "x4g7")"
                fi
                stop_temp_daemon
            else
                stop_temp_daemon
            fi
        elif $DAEMON_ALREADY_RUNNING; then
            if ! check_name_free "$NODE_NAME"; then
                warn "Name '${NODE_NAME}' is ${BOLD}taken${NC}!"
                if $INTERACTIVE; then
                    echo ""
                    echo -e "    ${CYAN}1${NC}) Try hostname-based"
                    echo -e "    ${CYAN}2${NC}) Try another random"
                    echo -e "    ${CYAN}3${NC}) Manual input"
                    prompt NAME_CHOICE "    Choice [2]: " "2"
                    NODE_NAME=""
                    RANDOM_FALLBACK="$(pick_random_name)"
                else
                    info "Using a random suffix"
                    NODE_NAME="${NODE_NAME}.$(tr -dc a-z0-9 < /dev/urandom 2>/dev/null | head -c4 || echo "x4g7")"
                fi
            fi
        fi
    fi
done

$SUDO "$BUILD_DIR/lnosctl" set name "$NODE_NAME"
info "Node name set to ${GREEN}${NODE_NAME}${NC}"

# ----- systemd / OpenRC -----
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
        $SUDO systemctl daemon-reload
        info "systemd unit created"
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
        $SUDO chmod +x "$INITD"
        info "OpenRC init script created"
    else
        info "OpenRC init script exists"
    fi
fi

# ----- firewall -----
FWR=""
if command -v ufw >/dev/null 2>&1; then
    if $SUDO ufw allow proto udp to 239.255.42.99 port 4545 comment 'LNOS' 2>/dev/null; then
        FWR="ufw"
        $SUDO ufw reload 2>/dev/null || true
    fi
elif command -v firewall-cmd >/dev/null 2>&1; then
    if $SUDO firewall-cmd --permanent --add-rich-rule='rule family="ipv4" destination address="239.255.42.99" port port="4545" protocol="udp" accept' >/dev/null 2>&1; then
        $SUDO firewall-cmd --reload >/dev/null 2>&1 && FWR="firewalld"
    fi
elif command -v iptables >/dev/null 2>&1; then
    if ! $SUDO iptables -C INPUT -d 239.255.42.99 -p udp --dport 4545 -j ACCEPT 2>/dev/null; then
        $SUDO iptables -A INPUT -d 239.255.42.99 -p udp --dport 4545 -j ACCEPT && FWR="iptables"
    else
        FWR="iptables (already set)"
    fi
fi
if [ -n "$FWR" ]; then
    info "Firewall: opened multicast (239.255.42.99:4545) via ${FWR}"
else
    warn "No firewall tool detected; ensure multicast (239.255.42.99:4545) is not blocked"
fi

# ----- seed owners.db -----
NODE_OWNER="${NODE_NAME##*.}"
echo "$NODE_OWNER" | $SUDO tee "${CFG_DIR}/owners.db" >/dev/null
$SUDO chmod 644 "${CFG_DIR}/owners.db"
$SUDO chown root:root "${CFG_DIR}/owners.db"

# ----- restart if needed -----
if [ "${DAEMON_RESTART_AFTER}" = "true" ]; then
    info "Restarting lnosd with new config..."
    if command -v systemctl >/dev/null 2>&1; then
        $SUDO systemctl restart lnosd 2>/dev/null && RESTARTED=true
    fi
    if [ ! "${RESTARTED}" = "true" ]; then
        pkill lnosd 2>/dev/null || true
        sleep 1
        nohup /usr/local/bin/lnosd > /dev/null 2>&1 &
    fi
    sleep 2
    if pgrep -x lnosd >/dev/null 2>&1; then
        info "lnosd restarted successfully"
    else
        warn "lnosd failed to restart — check logs: journalctl -u lnosd"
    fi
fi

# ----- detect IP -----
MY_IP=""
for iface in $(ip -4 addr show scope global up | grep -oP 'inet \K[\d.]+'); do
    case "$iface" in
        127.*|172.17.*|172.18.*) ;;
        *) MY_IP="$iface"; break ;;
    esac
done
[ -z "$MY_IP" ] && MY_IP="<detect on target machine>"

# ----- done -----
echo -e ""
header "Setup complete"

W=59
pline() { local t; t=$(echo -e "$*" | sed 's/\x1b\[[0-9;]*m//g'); local pad=$((W - ${#t})); [ "$pad" -lt 0 ] && pad=0; echo -e "${DIM}│${NC} $(echo -e "$*")$(printf '%*s' "$pad" '') ${DIM}│${NC}"; }

echo -e "${DIM}┌─────────────────────────────────────────────────────────────┐${NC}"
pline " ${GREEN}Your node${NC}"
pline "   Name:  ${BOLD}${NODE_NAME}${NC}"
pline "   IP:    ${MY_IP}"
pline "   Port:  4545    Group: 239.255.42.99"
pline ""
pline " ${GREEN}Dashboard${NC}"
pline "   ${UL}http://localhost:9999${NC}   — web UI with peer list"
pline "   ${UL}http://${MY_IP}:9999${NC}    — from other machines on LAN"
pline ""
pline " ${GREEN}Commands${NC}"
pline "   systemctl ${BOLD}start${NC} lnosd           start the daemon"
pline "   systemctl ${BOLD}enable --now${NC} lnosd    enable on boot + start"
pline "   lnosctl ${BOLD}stats${NC}                   show peer count & metrics"
pline "   getent hosts ${BOLD}${NODE_NAME}${NC}        resolve your own name"
pline "   journalctl ${BOLD}-u lnosd -f${NC}          follow daemon logs"
pline ""
pline " ${GREEN}On other machines${NC}"
pline "   Install LNOS the same way — all nodes discover"
pline "   each other automatically on the same multicast"
pline "   group (239.255.42.99:4545)."
echo -e "${DIM}└─────────────────────────────────────────────────────────────┘${NC}"

echo -e ""
echo -e " ${DIM}Config:${NC}      ${BOLD}${CFG_DIR}/${NC}"
echo -e " ${DIM}Binaries:${NC}    ${BOLD}/usr/local/bin/{lnosd,lnosctl}${NC}"
echo -e " ${DIM}NSS module:${NC}  ${BOLD}${LIBDIR}/libnss_lnos.so.2${NC}"
echo -e " ${DIM}nsswitch:${NC}    ${BOLD}/etc/nsswitch.conf${NC} (hosts: ... lnos)"
echo -e " ${DIM}Uninstall:${NC}   ${BOLD}./uninstall.sh${NC}"
echo -e ""
