#!/usr/bin/env bash
# Runs a server, an optional network simulator and a bot swarm, then prints the results.
#
#   scripts/stress_test.sh [--preset NAME] [--bots N] [--full M] [--duration SEC] [--latency MS]
#                          [--jitter MS] [--loss PERCENT] [--rollback TICKS] [--port N] [--record FILE]
#
# Works in Git Bash on Windows and on Linux/macOS. Exit code is cb_bot's (0 = everyone playing,
# no desyncs) or 3 if the recorded replay does not verify.

set -uo pipefail

preset="clang-release"
bots=32
full=4
duration=30
latency=0
jitter=0
loss=0
rollback=8
port=17950
record=""

while [[ $# -gt 0 ]]; do
	case "$1" in
		--preset) preset="$2" ;;
		--bots) bots="$2" ;;
		--full) full="$2" ;;
		--duration) duration="$2" ;;
		--latency) latency="$2" ;;
		--jitter) jitter="$2" ;;
		--loss) loss="$2" ;;
		--rollback) rollback="$2" ;;
		--port) port="$2" ;;
		--record) record="$2" ;;
		*) echo "unknown option $1"; exit 1 ;;
	esac
	shift 2
done

if [[ -n "${LOCALAPPDATA:-}" ]]; then
	bin="$(cygpath -u "$LOCALAPPDATA" 2>/dev/null || echo "$LOCALAPPDATA")/cinderbox-build/$preset/bin"
	ext=".exe"
else
	bin="$HOME/.cache/cinderbox-build/$preset/bin"
	ext=""
fi
work="$(mktemp -d)"
server_log="$work/server.log"

server_args=(--port "$port")
[[ -n "$record" ]] && server_args+=(--record "$record")
"$bin/cb_server$ext" "${server_args[@]}" > "$server_log" 2>&1 &
server_pid=$!

bot_port=$port
netsim_pid=""
if [[ "$latency" != "0" || "$jitter" != "0" || "$loss" != "0" ]]; then
	bot_port=$((port + 1))
	"$bin/cb_netsim$ext" --listen "$bot_port" --target "127.0.0.1:$port" --latency "$latency" --jitter "$jitter" \
		--loss "$loss" > "$work/netsim.log" 2>&1 &
	netsim_pid=$!
fi

cleanup() {
	kill "$server_pid" 2>/dev/null
	[[ -n "$netsim_pid" ]] && kill "$netsim_pid" 2>/dev/null
	wait 2>/dev/null
}
trap cleanup EXIT

sleep 1
echo "== $bots bots ($full full), ${latency}+${jitter} ms each way, ${loss}% loss, rollback window $rollback, ${duration}s"
"$bin/cb_bot$ext" --port "$bot_port" --count "$bots" --full "$full" --duration "$duration" --report 10 \
	--rollback "$rollback" --stagger 20
code=$?

# The server prints a status line every 5 s; the last full one covers the end of the run.
grep "tick" "$server_log" | tail -1 | sed "s/^/   /"
grep -E "reported a desync|reconnected|rejecting" "$server_log" | sed "s/^/   /"

cleanup
trap - EXIT

if [[ -n "$record" ]]; then
	"$bin/cb_replay$ext" verify "$record" | tail -1 | sed 's/^/   replay: /'
	[[ ${PIPESTATUS[0]} -eq 0 ]] || code=3
fi
rm -rf "$work"
exit $code
