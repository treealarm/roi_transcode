#!/usr/bin/env bash
# Stop everything this repository starts. One place, so the editor's stop button and a
# terminal do the same thing.
#
# Kills by listening port, never by process name. `pkill -f serve.py` looks obvious and is
# a trap: pkill matches whole command lines, so the shell that ran the command matches its
# own argument and kills itself. That cost a debugging cycle here on 2026-08-31.
#
#   ./integration/scripts/stop-services.sh [port ...]
set -uo pipefail

PORTS=("${@:-8765}")
stopped=0

for port in "${PORTS[@]}"; do
    # ss prints one line per listening socket; take the pids out of users:(("x",pid=N,fd=M)).
    pids=$(ss -ltnp 2>/dev/null | awk -v p=":${port}\$" '$4 ~ p' \
        | grep -oE 'pid=[0-9]+' | cut -d= -f2 | sort -u)
    if [[ -z "$pids" ]]; then
        echo "port ${port}: nothing listening"
        continue
    fi
    for pid in $pids; do
        name=$(ps -o comm= -p "$pid" 2>/dev/null || echo '?')
        if kill "$pid" 2>/dev/null; then
            echo "port ${port}: stopped ${name} (pid ${pid})"
            stopped=$((stopped + 1))
        else
            echo "port ${port}: could not stop pid ${pid}" >&2
        fi
    done
done

# Give them a moment, then say plainly if anything survived rather than reporting success.
sleep 1
for port in "${PORTS[@]}"; do
    if ss -ltn 2>/dev/null | awk -v p=":${port}\$" '$4 ~ p' | grep -q .; then
        echo "port ${port}: still listening after SIGTERM" >&2
        exit 1
    fi
done

echo "stopped ${stopped} service(s)"
