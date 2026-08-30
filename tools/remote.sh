#!/usr/bin/env bash
#
# Drive the RoboFace server on a remote box over plain SSH.
#
# The server does not run on this Mac: its Application Layer / endpoint filters accept an inbound
# LAN connection and then tear the socket down before the first read (errno 57, ENOTCONN), so the
# board can reach nothing here. The server therefore lives on a Linux box on the same LAN and this
# script is the whole of its operations story -- deploy, start, stop, monitor.
#
# Deliberately no systemd: start/stop/status is a PID file and a log file, which is inspectable
# with the same commands whether or not the remote has a user session, and needs no sudo ever.
#
#   tools/remote.sh deploy [--dev]   sync the repo, build the venv, install dependencies
#   tools/remote.sh start            start the server (idempotent)
#   tools/remote.sh stop             stop it
#   tools/remote.sh restart          stop, then start
#   tools/remote.sh status           PID, listener, and reachability from *this* machine
#   tools/remote.sh health [--watch N]  healthcheck: port open + service answering; exit 0/1
#   tools/remote.sh logs [-f] [-n N] tail the server log
#   tools/remote.sh ping             a real WS handshake via tools/chat.py -- no LLM call
#   tools/remote.sh shell            interactive ssh into the deployment directory
#   tools/remote.sh help             this text
#
# Configuration, all overridable from the environment:
#
#   RF_REMOTE       user@host of the server box   (default ich@192.168.1.197, the box ich-picobox)
#   RF_REMOTE_DIR   deployment dir, relative to $HOME on the remote   (default roboface)
#   RF_REMOTE_PORT  the port the server binds     (default 8000)
#
# The address is literal rather than the mDNS name `ich-picobox.local`: resolving that costs about
# 1.2 s on every lookup, and the board cannot use it at all (the firmware links no resolver), so
# one form is used everywhere. If the box's DHCP lease changes, either update this line or export
# the new one:
#
#   export RF_REMOTE=ich@<new-address>

set -euo pipefail

RF_REMOTE="${RF_REMOTE:-ich@192.168.1.197}"
RF_REMOTE_DIR="${RF_REMOTE_DIR:-roboface}"
RF_REMOTE_PORT="${RF_REMOTE_PORT:-8000}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REMOTE_HOST="${RF_REMOTE#*@}"

# Non-interactive by default: a script that silently waits for a password prompt inside a loop is
# worse than one that fails saying so.
# accept-new trusts a host the first time it is seen but still refuses a *changed* key, so
# pointing at a fresh box works without a manual ssh, and a swapped key still stops the script.
SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -o StrictHostKeyChecking=accept-new)

say()  { printf '\033[1m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[33mwarning:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# Run a script on the remote with the deployment paths already in scope. Every remote block below
# goes through this, so the directory and PID/log layout is defined in exactly one place.
remote_sh() {
    ssh "${SSH_OPTS[@]}" "$RF_REMOTE" \
        "RF_DIR='$RF_REMOTE_DIR' RF_PORT='$RF_REMOTE_PORT' bash -s" -- "$@"
}

require_ssh() {
    ssh "${SSH_OPTS[@]}" "$RF_REMOTE" true 2>/dev/null || die \
        "cannot ssh to $RF_REMOTE without a password.
  Install your key first:   ssh-copy-id $RF_REMOTE
  Or point elsewhere:       RF_REMOTE=user@host $0 $*"
}

# Is the port answering from this machine? This is the check that actually matters -- a listener
# the board cannot reach is not a running server, which is the entire lesson of this setup.
port_reachable() {
    nc -z -G 3 "$REMOTE_HOST" "$RF_REMOTE_PORT" >/dev/null 2>&1
}

cmd_deploy() {
    local with_dev=0
    [[ "${1:-}" == "--dev" ]] && with_dev=1

    require_ssh
    [[ -f "$REPO_ROOT/server/.env" ]] || warn "server/.env is absent locally -- the remote keeps whatever it already has"

    say "syncing the repository to $RF_REMOTE:~/$RF_REMOTE_DIR"
    # server/.env is excluded from the tree sync and pushed separately below: it holds the API keys,
    # and an --delete sync that happened to run without it would remove the remote's only copy.
    rsync -az --delete \
        --exclude '.git/' --exclude '.venv/' --exclude '__pycache__/' \
        --exclude '.pytest_cache/' --exclude '.mypy_cache/' --exclude '.ruff_cache/' \
        --exclude 'codegen/runs/' --exclude 'codegen/var/' \
        --exclude 'firmware/.pio/' --exclude 'node_modules/' --exclude '.DS_Store' \
        --exclude 'var/' --exclude 'server/.env' \
        -e "ssh ${SSH_OPTS[*]}" \
        "$REPO_ROOT/" "$RF_REMOTE:$RF_REMOTE_DIR/"

    if [[ -f "$REPO_ROOT/server/.env" ]]; then
        say "pushing server/.env (mode 600)"
        rsync -a -e "ssh ${SSH_OPTS[*]}" \
            "$REPO_ROOT/server/.env" "$RF_REMOTE:$RF_REMOTE_DIR/server/.env"
        remote_sh <<'EOF'
chmod 600 "$HOME/$RF_DIR/server/.env"
EOF
    fi

    say "ensuring the Python toolchain and dependencies"
    remote_sh "$with_dev" <<'EOF'
set -euo pipefail
with_dev="$1"
cd "$HOME/$RF_DIR"
export PATH="$HOME/.local/bin:$PATH"

# The server targets py311+ (pyproject sets target-version = "py311"; CI runs 3.13). Ubuntu 22.04
# ships 3.10, where asyncio.wait_for raises asyncio.TimeoutError -- NOT the builtin -- so the
# orchestrator's `except TimeoutError` would stop catching the first-token budget without a word.
# uv installs a standalone interpreter under $HOME, so this needs no root and touches no system
# package.
if ! command -v uv >/dev/null 2>&1; then
    echo "  installing uv"
    wget -qO /tmp/uv-install.sh https://astral.sh/uv/install.sh
    sh /tmp/uv-install.sh >/dev/null 2>&1
    rm -f /tmp/uv-install.sh
    export PATH="$HOME/.local/bin:$PATH"
fi
echo "  uv $(uv --version | awk '{print $2}')"

uv python install 3.13 >/dev/null 2>&1 || true
[[ -x .venv/bin/python ]] || uv venv --python 3.13 .venv >/dev/null
echo "  python $(.venv/bin/python -V | awk '{print $2}')"

reqs="requirements.txt"
[[ "$with_dev" == "1" ]] && reqs="requirements-dev.txt"
echo "  installing $reqs"
VIRTUAL_ENV="$PWD/.venv" uv pip install -q -r "$reqs"

mkdir -p var
EOF
    say "deployed"
}

cmd_start() {
    require_ssh
    remote_sh <<'EOF'
set -euo pipefail
cd "$HOME/$RF_DIR"
mkdir -p var

if [[ -f var/server.pid ]] && kill -0 "$(cat var/server.pid)" 2>/dev/null; then
    echo "  already running (pid $(cat var/server.pid))"
    exit 0
fi

[[ -f server/.env ]] || { echo "  server/.env is missing -- run 'deploy' first" >&2; exit 1; }
[[ -x .venv/bin/python ]] || { echo "  .venv is missing -- run 'deploy' first" >&2; exit 1; }

# The .env is deliberately NOT sourced. load_settings() reads server/.env itself, by a path
# derived from config.py's own location, so it is found from any directory -- and sourcing it in
# the shell loses data: WEATHER_URL's value contains an unquoted `&`, so the shell backgrounds at
# the first one and the assignment never reaches this process. Exit code 0, no message.
PYTHONPATH=server nohup .venv/bin/python -m roboface_server.app >> var/server.log 2>&1 &
echo $! > var/server.pid

# Wait for **our own** socket, not for any socket.
#
# The previous version waited for a listener on the port and reported success as soon as it saw
# one. When an older server was still holding the port -- a stray process with no PID file, which
# is the normal aftermath of a reboot or a manual start -- that check passed on its first
# iteration: the script said "started", the new process died on bind moments later, and the box
# went on serving the old code. That is the worst outcome available, because it is a confident
# success message over a stale deployment, and it is how a board ran a build four releases old
# while every deploy reported fine.
mine="$(cat var/server.pid)"
for _ in $(seq 1 40); do
    owner="$( (ss -ltnp 2>/dev/null || true) | grep ":$RF_PORT " | grep -o 'pid=[0-9]*' | head -1 | cut -d= -f2 )"
    if [[ -n "$owner" ]]; then
        if [[ "$owner" == "$mine" ]]; then
            echo "  started (pid $mine), listening on :$RF_PORT"
            exit 0
        fi
        echo "  :$RF_PORT is held by pid $owner, which is not the server just started" >&2
        pgrep -af 'roboface_server.app' | sed 's/^/    /' >&2
        kill "$mine" 2>/dev/null || true
        rm -f var/server.pid
        exit 1
    fi
    kill -0 "$mine" 2>/dev/null || {
        echo "  the process exited during startup; last lines:" >&2
        tail -15 var/server.log >&2
        rm -f var/server.pid
        exit 1
    }
    sleep 0.25
done
echo "  started (pid $mine) but nothing is listening on :$RF_PORT yet" >&2
tail -15 var/server.log >&2
exit 1
EOF
    port_reachable \
        && say "reachable from here at ws://$REMOTE_HOST:$RF_REMOTE_PORT/ws" \
        || warn "listening on the box, but not reachable from this machine (check the remote firewall)"
}

cmd_stop() {
    require_ssh
    remote_sh <<'EOF'
set -euo pipefail
cd "$HOME/$RF_DIR" 2>/dev/null || { echo "  nothing deployed"; exit 0; }

stopped=0
if [[ -f var/server.pid ]]; then
    pid="$(cat var/server.pid)"
    if kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null || true
        for _ in $(seq 1 40); do kill -0 "$pid" 2>/dev/null || break; sleep 0.25; done
        kill -0 "$pid" 2>/dev/null && { echo "  did not exit on TERM, sending KILL"; kill -9 "$pid" 2>/dev/null || true; }
        echo "  stopped (pid $pid)"
        stopped=1
    fi
    rm -f var/server.pid
fi

# **A PID file is a hint, not the authority.** What this script manages is "the RoboFace server on
# this box", and a server process with no PID file -- left by a reboot, a manual start, or a
# previous version of this script -- is still that. The earlier version only *noted* such a process
# and left it running, which meant `restart` could report success while the old code kept the port.
# On a single-purpose box that caution bought nothing and cost a great deal.
#
# Matched by executable rather than by command line alone: `pgrep -f roboface_server.app` also
# matches the shell running this very heredoc, whose command line contains the string.
for pid in $(pgrep -f 'roboface_server.app' 2>/dev/null || true); do
    [[ "$pid" == "$$" || "$pid" == "$PPID" ]] && continue
    readlink -f "/proc/$pid/exe" 2>/dev/null | grep -q python || continue
    kill "$pid" 2>/dev/null || true
    for _ in $(seq 1 40); do kill -0 "$pid" 2>/dev/null || break; sleep 0.25; done
    kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null || true
    echo "  stopped an unmanaged server (pid $pid)"
    stopped=1
done

[[ "$stopped" == "0" ]] && echo "  not running"
exit 0
EOF
}

cmd_status() {
    require_ssh
    say "remote"
    remote_sh <<'EOF'
cd "$HOME/$RF_DIR" 2>/dev/null || { echo "  nothing deployed at ~/$RF_DIR"; exit 0; }
if [[ -f var/server.pid ]] && kill -0 "$(cat var/server.pid)" 2>/dev/null; then
    pid="$(cat var/server.pid)"
    echo "  running   pid $pid, up $(ps -o etime= -p "$pid" | tr -d ' ')"
else
    echo "  stopped   (no live pid file)"
fi
listener="$( (ss -ltn 2>/dev/null || netstat -ltn 2>/dev/null) | grep ":$RF_PORT " | head -1 )"
echo "  listener  ${listener:-none on :$RF_PORT}"
echo "  sessions  $( (ss -tn state established "( sport = :$RF_PORT )" 2>/dev/null | tail -n +2 | wc -l) || echo '?' ) established"
(ss -tn state established "( sport = :$RF_PORT )" 2>/dev/null | tail -n +2 | awk '{print "            <- "$4}') || true
echo "  python    $([[ -x .venv/bin/python ]] && .venv/bin/python -V || echo 'no venv')"
echo "  log       $([[ -f var/server.log ]] && du -h var/server.log | cut -f1 || echo none)"
EOF
    say "from this machine"
    if port_reachable; then
        printf '  tcp       %s:%s reachable\n' "$REMOTE_HOST" "$RF_REMOTE_PORT"
    else
        printf '  tcp       %s:%s NOT reachable\n' "$REMOTE_HOST" "$RF_REMOTE_PORT"
    fi
}

cmd_logs() {
    require_ssh
    local follow=0 lines=80
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -f|--follow) follow=1; shift ;;
            -n) lines="$2"; shift 2 ;;
            *) die "unknown option for logs: $1" ;;
        esac
    done
    # -t so Ctrl-C reaches the remote tail instead of orphaning it.
    if [[ "$follow" == "1" ]]; then
        ssh -t "${SSH_OPTS[@]}" "$RF_REMOTE" "tail -n $lines -f '$RF_REMOTE_DIR/var/server.log'"
    else
        ssh "${SSH_OPTS[@]}" "$RF_REMOTE" "tail -n $lines '$RF_REMOTE_DIR/var/server.log'"
    fi
}

cmd_ping() {
    port_reachable || die "$REMOTE_HOST:$RF_REMOTE_PORT is not reachable from here -- try '$0 status'"
    local python="$REPO_ROOT/.venv/bin/python"
    [[ -x "$python" ]] || python="python3"
    say "WS handshake to ws://$REMOTE_HOST:$RF_REMOTE_PORT/ws"
    printf '/ping\n/stats\n/quit\n' | "$python" "$REPO_ROOT/tools/chat.py" \
        --url "ws://$REMOTE_HOST:$RF_REMOTE_PORT/ws" --device-id remote-sh
}

cmd_shell() {
    exec ssh -t "$RF_REMOTE" "cd '$RF_REMOTE_DIR' 2>/dev/null; exec \$SHELL -l"
}

# The healthcheck proper: two questions, in the order they can fail, each answered from *this*
# machine rather than from the box. A listener the board cannot reach is not a running service --
# which is the whole reason the server does not live on the Mac.
#
#   1. is the port open?          (TCP reaches something)
#   2. does the service answer?   (a real hello/ping over the protocol)
#
# Exits 0 only when both pass, so it composes: `tools/remote.sh health && deploy_something`.
cmd_health() {
    local interval=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --watch) interval="${2:-10}"; shift 2 ;;
            *) die "unknown option for health: $1" ;;
        esac
    done

    local python="$REPO_ROOT/.venv/bin/python"
    [[ -x "$python" ]] || python="python3"

    while :; do
        local stamp tcp_ok=0 ws_ok=0
        stamp="$(date '+%H:%M:%S')"

        port_reachable && tcp_ok=1

        if [[ "$tcp_ok" == "1" ]]; then
            # A real handshake through protocol.py, so a wire-contract change breaks this loudly
            # rather than letting the healthcheck pass against a server that no longer speaks it.
            #
            # Collected into a variable rather than piped straight into grep: `grep -q` exits on
            # the first match, chat.py then dies of SIGPIPE, and under `set -o pipefail` that made
            # a *successful* ping report as a failure.
            local reply
            reply="$(printf '/ping\n/quit\n' | "$python" "$REPO_ROOT/tools/chat.py" \
                --url "ws://$REMOTE_HOST:$RF_REMOTE_PORT/ws" --device-id healthcheck 2>&1 || true)"
            case "$reply" in *pong*) ws_ok=1 ;; esac
        fi

        if [[ "$tcp_ok" == "1" && "$ws_ok" == "1" ]]; then
            printf '\033[32mOK\033[0m       %s  %s:%s  port open, service answering\n' \
                "$stamp" "$REMOTE_HOST" "$RF_REMOTE_PORT"
            [[ "$interval" == "0" ]] && return 0
        elif [[ "$tcp_ok" == "1" ]]; then
            printf '\033[33mDEGRADED\033[0m %s  %s:%s  port open, but the service did not answer\n' \
                "$stamp" "$REMOTE_HOST" "$RF_REMOTE_PORT"
            [[ "$interval" == "0" ]] && return 1
        else
            printf '\033[31mDOWN\033[0m     %s  %s:%s  port not reachable\n' \
                "$stamp" "$REMOTE_HOST" "$RF_REMOTE_PORT"
            [[ "$interval" == "0" ]] && return 1
        fi
        sleep "$interval"
    done
}

usage() {
    # Print the header comment block and stop at the first line that is not a comment, so the
    # help text cannot drift out of sync with the script the way a hard-coded line range does.
    awk 'NR>2 { if ($0 !~ /^#/) exit; sub(/^# ?/, ""); print }' "${BASH_SOURCE[0]}"
    exit "${1:-0}"
}

case "${1:-}" in
    deploy)  shift; cmd_deploy "$@" ;;
    start)   shift; cmd_start ;;
    stop)    shift; cmd_stop ;;
    restart) shift; cmd_stop; cmd_start ;;
    status)  shift; cmd_status ;;
    health)  shift; cmd_health "$@" ;;
    logs)    shift; cmd_logs "$@" ;;
    ping)    shift; cmd_ping ;;
    shell)   shift; cmd_shell ;;
    -h|--help|help|"") usage 0 ;;
    *) printf 'unknown command: %s\n\n' "$1" >&2; usage 1 ;;
esac
