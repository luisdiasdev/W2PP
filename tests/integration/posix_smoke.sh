#!/usr/bin/env bash
# POSIX smoke test for the DBSrv/TMSrv host.
#
# 1. Builds a temporary runtime tree (no repository-relative data required).
# 2. Starts DBSrv and waits for its game port.
# 3. Starts TMSrv with W2PP_FORCE_CONSOLE=1, connects to the 8281 listener,
#    sends the init handshake, then sends "quit" through the console pipe.
# 4. Starts TMSrv a second time and shuts it down with SIGINT.
#
# Every wait is timeout-bounded and the temporary tree is removed on exit.

set -u

EXE_DIR=""
SOURCE_DIR=""

while [ $# -gt 0 ]; do
	case "$1" in
		--exe-dir) EXE_DIR="$2"; shift 2 ;;
		--source-dir) SOURCE_DIR="$2"; shift 2 ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

if [ -z "$EXE_DIR" ] || [ -z "$SOURCE_DIR" ]; then
	echo "usage: $0 --exe-dir <dir> --source-dir <repo>" >&2
	exit 2
fi

for required in dbsrv tmsrv; do
	if [ ! -x "$EXE_DIR/$required" ]; then
		echo "missing executable: $EXE_DIR/$required" >&2
		exit 2
	fi
done

ROOT="$(mktemp -d "${TMPDIR:-/tmp}/w2pp-smoke.XXXXXX")" || exit 2
RUN_DIR="$ROOT/work/run"
COMMON_DIR="$ROOT/Common"
CONSOLE_FIFO="$ROOT/console"
DBSRV_PID=""
TMSRV_PID=""
CONSOLE_FD=""

cleanup() {
	trap - EXIT INT TERM
	if [ -n "$CONSOLE_FD" ]; then
		eval "exec $CONSOLE_FD>&-" 2>/dev/null || true
	fi
	for pid in "$TMSRV_PID" "$DBSRV_PID"; do
		if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
			kill -TERM "$pid" 2>/dev/null || true
			tries=0
			while kill -0 "$pid" 2>/dev/null && [ "$tries" -lt 50 ]; do
				sleep 0.1
				tries=$((tries + 1))
			done
			kill -KILL "$pid" 2>/dev/null || true
			wait "$pid" 2>/dev/null || true
		fi
	done
	rm -rf "$ROOT"
}
trap cleanup EXIT INT TERM

fail() {
	echo "FAIL: $*" >&2
	if [ -f "$ROOT/dbsrv.log" ]; then
		echo "--- dbsrv.log ---" >&2
		tail -n 40 "$ROOT/dbsrv.log" >&2 || true
	fi
	if [ -f "$ROOT/tmsrv.log" ]; then
		echo "--- tmsrv.log ---" >&2
		tail -n 40 "$ROOT/tmsrv.log" >&2 || true
	fi
	exit 1
}

pass() {
	echo "ok - $*"
}

mkdir -p "$RUN_DIR/BaseMob" "$RUN_DIR/Log" \
	"$RUN_DIR/Import/Item" "$RUN_DIR/Import/User" "$RUN_DIR/Import/Donate" \
	"$COMMON_DIR/Settings"

cp "$EXE_DIR/dbsrv" "$EXE_DIR/tmsrv" "$RUN_DIR/"

for name in TK FM BM HT; do
	head -c 816 /dev/zero > "$RUN_DIR/BaseMob/$name"
done

head -c 1048576 /dev/zero > "$RUN_DIR/AttributeMap.dat"
head -c 16 /dev/zero > "$RUN_DIR/heightmap.dat"

printf 'Sapphire 8\nLastCapsule 1\n' > "$RUN_DIR/Config.txt"
printf '0 0 127.0.0.1\n0 1 127.0.0.1\n' > "$RUN_DIR/serverlist.txt"
printf '0 127 0 0 1\n' > "$RUN_DIR/Admin.txt"
printf '127.0.0.1\n' > "$RUN_DIR/localip.txt"

for name in LevelItem.txt NPCGener.txt MobName.txt Language.txt; do
	: > "$RUN_DIR/$name"
done

for name in SancRate.txt QuestsRate.txt CompRate.txt MobMerc.txt CastleQuest.txt; do
	: > "$COMMON_DIR/Settings/$name"
done

cp "$SOURCE_DIR/Code/ItemEffect.h" "$COMMON_DIR/ItemEffect.h" || fail "copy ItemEffect.h"

for name in itemlist.csv extraitem.csv InitItem.csv SkillData.csv Guilds.txt; do
	: > "$COMMON_DIR/$name"
done

wait_for_port() {
	local port="$1"
	local timeout_s="$2"
	local deadline=$((SECONDS + timeout_s))

	while [ "$SECONDS" -lt "$deadline" ]; do
		if (exec 9<>"/dev/tcp/127.0.0.1/$port") 2>/dev/null; then
			exec 9>&-
			return 0
		fi
		sleep 0.1
	done

	return 1
}

wait_for_exit() {
	local pid="$1"
	local timeout_s="$2"
	local deadline=$((SECONDS + timeout_s))

	while [ "$SECONDS" -lt "$deadline" ]; do
		if ! kill -0 "$pid" 2>/dev/null; then
			wait "$pid"
			return $?
		fi
		sleep 0.1
	done

	return 1
}

send_init_handshake() {
	exec 9<>"/dev/tcp/127.0.0.1/8281" 2>/dev/null || return 1
	printf '\x11\xf3\x11\x1f' >&9 || { exec 9>&-; return 1; }
	# A successful open already proves the listener accepted; keep the socket
	# briefly so the server observes the init bytes.
	sleep 0.3
	exec 9>&-
	return 0
}

send_oversized_frame() {
	exec 8<>"/dev/tcp/127.0.0.1/8281" 2>/dev/null || return 1
	# Init handshake followed by a header declaring a 0xFFFF-byte frame, which
	# is outside the supported size window.
	printf '\x11\xf3\x11\x1f\xff\xff\x00\x00\x00\x00\x00\x00\x00\x00\x00\x00' >&8 ||
		{ exec 8>&-; return 1; }
	sleep 0.3
	exec 8>&-
	return 0
}

# --- DBSrv -----------------------------------------------------------------

"$RUN_DIR/dbsrv" > "$ROOT/dbsrv.log" 2>&1 &
DBSRV_PID=$!

wait_for_port 7514 15 || fail "DBSrv did not listen on 7514 (pid $DBSRV_PID)"

if ! kill -0 "$DBSRV_PID" 2>/dev/null; then
	fail "DBSrv exited during startup"
fi

pass "DBSrv listens on 7514"

# --- TMSrv: console quit ---------------------------------------------------

mkfifo "$CONSOLE_FIFO" || fail "mkfifo"

# Open the FIFO for reading in the child first (it blocks there), then open the
# single write end in the parent. Opening read-write in the parent would leave a
# writer behind and the console thread would never see EOF after "quit".
W2PP_FORCE_CONSOLE=1 "$RUN_DIR/tmsrv" <"$CONSOLE_FIFO" > "$ROOT/tmsrv.log" 2>&1 &
TMSRV_PID=$!

exec 3>"$CONSOLE_FIFO"
CONSOLE_FD=3

wait_for_port 8281 20 || fail "TMSrv did not listen on 8281 (pid $TMSRV_PID)"
send_init_handshake || fail "init handshake on 8281 failed"
pass "TMSrv accepts the init handshake on 8281"

send_oversized_frame || fail "could not send the oversized frame probe"
sleep 0.3
grep -q "frame size out of range" "$ROOT/tmsrv.log" ||
	fail "oversized frame was not rejected with a logged close"
pass "TMSrv rejects an oversized frame with a logged close"

printf 'quit\n' >&3
exec 3>&-
CONSOLE_FD=""

wait_for_exit "$TMSRV_PID" 20
TMSRV_STATUS=$?

if kill -0 "$TMSRV_PID" 2>/dev/null; then
	kill -TERM "$TMSRV_PID" 2>/dev/null || true
	fail "TMSrv did not exit after quit"
fi

TMSRV_PID=""

[ "$TMSRV_STATUS" -eq 0 ] || fail "TMSrv quit exit status $TMSRV_STATUS"
pass "TMSrv exits cleanly after quit"

# --- TMSrv: SIGINT ---------------------------------------------------------

"$RUN_DIR/tmsrv" > "$ROOT/tmsrv-sigint.log" 2>&1 < /dev/null &
TMSRV_PID=$!

wait_for_port 8281 20 || fail "second TMSrv did not listen on 8281"
send_init_handshake || fail "second init handshake on 8281 failed"

kill -INT "$TMSRV_PID" || fail "failed to send SIGINT"

wait_for_exit "$TMSRV_PID" 20
TMSRV_STATUS=$?

if kill -0 "$TMSRV_PID" 2>/dev/null; then
	kill -TERM "$TMSRV_PID" 2>/dev/null || true
	fail "TMSrv did not exit after SIGINT"
fi

TMSRV_PID=""

[ "$TMSRV_STATUS" -eq 0 ] || fail "TMSrv SIGINT exit status $TMSRV_STATUS"
pass "TMSrv exits cleanly on SIGINT"

exit 0
