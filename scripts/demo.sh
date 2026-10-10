#!/bin/sh
# demo.sh - Pi side: start / stop / monitor the whole demo (run as root).
#
#   /tmp/demo.sh start          taskmgr (enforcing) + abs + airbag + decoder + nav + ui
#   /tmp/demo.sh start --off    same, taskmgr watch-only (no demotion)
#   /tmp/demo.sh panel          LED panel in key mode (1 = decoder, 2 = ui, s = stress, q = quit)
#   /tmp/demo.sh dash           dashboard on core 3
#   /tmp/demo.sh status         what is running + guardian log
#   /tmp/demo.sh killtm         kill test: SIGKILL taskmgr, guardian restarts it
#   /tmp/demo.sh stop           stop everything, print the actors' timing results
#   /tmp/demo.sh install        (no root) create /tmp/demo-* shortcuts for run.ps1
#
# Everything except "install" must run as root (su).
#
# Guardian: taskmgr runs inside a loop that restarts it 1 s after any exit
# (crash, kill test). Every start/exit is timestamped in /tmp/run1/guard.log,
# which gives the recovery time. All programs ignore SIGHUP, so closing the
# ssh window does not kill them. "stop" sets a flag first, so the guardian
# does not restart taskmgr during shutdown.

BIN=/tmp
RUN=/tmp/run1
ALL="panel ui nav decoder airbag abs taskmgr"

say()  { echo "demo: $*"; }
die()  { echo "demo: ERROR: $*"; exit 1; }
need_root() { [ "$(id -u)" = "0" ] || die "run as root: su, then /tmp/demo.sh $1"; }

# pids of every process with this name (empty if none)
pids_of() { pidin -p "$1" -F "%a" 2>/dev/null | tail -n +2 | tr -d ' ' | sort -u; }

kill_all_named() {           # $1 = signal, $2 = name
    for pid in $(pids_of "$2"); do kill -"$1" "$pid" 2>/dev/null; done
}

guard() {                    # runs in the background, restarts taskmgr
    trap '' HUP
    n=0
    while [ ! -f "$RUN/stop" ]; do
        n=$((n + 1))
        echo "$(date +%T) guard: start taskmgr #$n $TMARGS" >> "$RUN/guard.log"
        "$BIN/taskmgr" $TMARGS >> "$RUN/taskmgr.out" 2>&1
        rc=$?
        [ -f "$RUN/stop" ] && break
        echo "$(date +%T) guard: taskmgr EXITED (code $rc), restart in 1 s" >> "$RUN/guard.log"
        sleep 1
    done
    echo "$(date +%T) guard: stopped" >> "$RUN/guard.log"
}

stop_all() {
    mkdir -p "$RUN"
    touch "$RUN/stop"                                  # guardian must not restart
    if [ -f "$RUN/guard.pid" ]; then
        kill "$(cat "$RUN/guard.pid")" 2>/dev/null
        rm -f "$RUN/guard.pid"
    fi
    for p in $ALL; do kill_all_named TERM "$p"; done   # TERM: abs/nav print summaries
    sleep 1
    for p in $ALL; do kill_all_named KILL "$p"; done   # anything still alive
}

status() {
    for p in taskmgr abs airbag decoder nav ui panel; do
        pids=$(pids_of "$p" | tr '\n' ' ')
        printf "  %-8s %s\n" "$p" "${pids:-NOT RUNNING}"
    done
    [ -f "$RUN/guard.log" ] && { echo "  guardian log (last 3):"; tail -3 "$RUN/guard.log" | sed 's/^/    /'; }
}

case "$1" in
start)
    need_root start
    TMARGS=""
    [ "$2" = "--off" ] && TMARGS="--off"
    for p in taskmgr abs airbag decoder nav ui panel; do
        [ -x "$BIN/$p" ] || die "$BIN/$p missing or not executable (deploy first)"
    done
    say "stopping old copies"
    stop_all
    mkdir -p "$RUN"
    rm -f "$RUN/stop"
    cd "$BIN" || die "no $BIN"
    trap '' HUP                                        # children inherit: survive ssh close
    echo "=== run $(date) taskmgr $TMARGS" >> "$RUN/taskmgr.out"
    export TMARGS
    guard &
    echo $! > "$RUN/guard.pid"
    sleep 1
    ./abs 0         >> "$RUN/abs.out"     2>&1 &
    ./airbag 0      >> "$RUN/airbag.out"  2>&1 &
    ./decoder light >> "$RUN/decoder.out" 2>&1 &
    ./nav           >> "$RUN/nav.out"     2>&1 &
    ./ui            >> "$RUN/ui.out"      2>&1 &
    sleep 1
    say "started (taskmgr ${TMARGS:-enforcing}):"
    status
    ;;
stop)
    need_root stop
    stop_all
    say "stopped. Results:"
    for p in abs airbag decoder nav ui; do
        echo "  $(grep 'cycles' "$RUN/$p.out" 2>/dev/null | tail -1)"
    done
    ;;
status)
    status
    ;;
panel)
    need_root panel
    i=0
    dpid=$(pids_of decoder | head -1)
    upid=$(pids_of ui | head -1)
    while { [ -z "$dpid" ] || [ -z "$upid" ]; } && [ $i -lt 60 ]; do   # wait up to 60 s for "start"
        [ $i -eq 0 ] && say "waiting for decoder and ui to start..."
        sleep 1; i=$((i + 1))
        dpid=$(pids_of decoder | head -1)
        upid=$(pids_of ui | head -1)
    done
    [ -n "$dpid$upid" ] || die "neither decoder nor ui is running (run: /tmp/demo.sh start)"
    [ -n "$dpid" ] || say "decoder not running: only ui can be stressed"
    [ -n "$upid" ] || say "ui not running: only decoder can be stressed"
    cd "$BIN" || die "no $BIN"
    exec ./panel "${dpid:-0}" "${upid:-0}" key
    ;;
dash)
    need_root dash
    sleep 2
    on -C3 -p10 python3 "$BIN/coremap.py" --tm-log "$RUN/taskmgr.out" \
        || python3 "$BIN/coremap.py" --tm-log "$RUN/taskmgr.out"
    ;;
killtm)
    need_root killtm
    say "SIGKILL taskmgr at $(date +%T); guardian restarts it in ~1 s"
    kill_all_named KILL taskmgr
    sleep 3
    tail -3 "$RUN/guard.log"
    ;;
install)
    for m in start startoff stop status panel dash killtm; do
        arg=$m; extra=""
        [ "$m" = "startoff" ] && { arg=start; extra=" --off"; }
        printf '#!/bin/sh\nexec /tmp/demo.sh %s%s\n' "$arg" "$extra" > "/tmp/demo-$m"
        chmod +x "/tmp/demo-$m"
    done
    chmod +x /tmp/demo.sh
    say "shortcuts /tmp/demo-{start,startoff,stop,status,panel,dash,killtm} ready"
    ;;
*)
    echo "usage: $0 start [--off] | stop | status | panel | dash | killtm | install"
    exit 1
    ;;
esac
