#!/usr/bin/env bash
# demo.sh - Part D demonstration with the real ./server and ./client.
#
# Runs one server and several client instances (up to 6 at the same time)
# and prints, for every step, what the student saw (client output) and
# what the server recorded (server log lines written during that step).
#
# Usage: bash demo.sh            (about 15 seconds)

set -u
cd "$(dirname "$0")"
PORT=9600
LOG=demo_server.log

# ---------------------------------------------------------------- helpers
banner() {
    echo
    echo "##########################################################################"
    echo "# $1"
    echo "##########################################################################"
}
mark() { wc -l < "$LOG"; }                      # current log length
server_log_since() {                            # log lines after a mark
    echo "--- server log ---"
    tail -n +"$(( $1 + 1 ))" "$LOG"
}
now() { date +%s.%N; }
sleep_until() {                                 # sleep to an absolute time
    local t
    t=$(awk -v a="$1" -v b="$(now)" 'BEGIN { d = a - b; print (d > 0 ? d : 0) }')
    sleep "$t"
}
client() { ./client 127.0.0.1 "$PORT"; }

# ------------------------------------------------------------------ build
banner "BUILD AND START THE SERVER"
echo "\$ gcc -Wall -Wextra -pthread -o server server.c"
gcc -Wall -Wextra -pthread -o server server.c || exit 1
echo "\$ gcc -Wall -Wextra -o client client.c"
gcc -Wall -Wextra -o client client.c || exit 1
echo "\$ ./server $PORT"
./server "$PORT" > "$LOG" 2>&1 &
SERVER_PID=$!
sleep 0.5
cat "$LOG"

# --------------------------------------------------------------- step 1
banner "STEP 1 - Unsuccessful authentication            (requirement 2)"
echo "A client enters two unknown student IDs, then quits."
echo
m=$(mark)
printf 'S9999\nABC123\nq\n' | client
sleep 0.2
server_log_since "$m"

# --------------------------------------------------------------- step 2
banner "STEP 2 - Successful authentication, event list, registration,
#          duplicate registration        (requirements 1, 3, 4 and 6)"
echo "S1001 logs in (the event list is shown automatically), registers for"
echo "event 101, then tries to register for event 101 again."
echo
m=$(mark)
printf 'S1001\n2\n101\n2\n101\n3\n4\n' | client
sleep 0.2
server_log_since "$m"

# --------------------------------------------------------------- step 3
banner "STEP 3 - Six simultaneous clients register for event 102 at the
#          same moment (capacity 4)          (requirements 7 and 8)"
echo "Six client instances log in and stay connected. At the same instant"
echo "all six send REGISTER 102. Event 102 has only 4 places."
echo
m=$(mark)
GO=$(awk -v t="$(now)" 'BEGIN { printf "%.3f", t + 2 }')   # common start
CROWD="S1002 S1003 S1004 S1005 S1006 S1007"
for id in $CROWD; do
    (
        printf '%s\n' "$id"             # log in now
        sleep_until "$GO"               # wait for the common start time
        printf '2\n102\n'               # register for 102
        sleep 1
        printf '4\n'                    # exit
    ) | client > "demo_$id.txt" 2>&1 &
done

sleep_until "$(awk -v g="$GO" 'BEGIN { print g - 0.7 }')"
echo "While all six are connected:"
echo
echo "\$ ss -tn state established '( sport = :$PORT )'"
if command -v ss > /dev/null; then
    ss -tn state established "( sport = :$PORT )"
    echo "=> $(ss -tn state established "( sport = :$PORT )" | tail -n +2 | wc -l) TCP connections established to the server"
else
    echo "(ss not installed)"
fi
echo
grep "Connected clients (6/10)" "$LOG" | tail -1 | cut -c1-160
echo "..."

wait $(jobs -p | grep -v "^$SERVER_PID$") 2> /dev/null
echo
echo "Registration result shown to each student:"
for id in $CROWD; do
    printf '  %s  %s\n' "$id" \
        "$(grep -E 'SUCCESS\] Registration|REJECTED' "demo_$id.txt" | sed 's/^ *//')"
done
echo
echo "Full transcript of one of the six clients ($(echo $CROWD | cut -d' ' -f6)):"
sed 's/^/  | /' "demo_S1007.txt"
rm -f demo_S100*.txt
echo
server_log_since "$m" | grep -v "Connected clients"
echo "(connected-client list lines omitted here; see the full log at the end)"

# --------------------------------------------------------------- step 4
banner "STEP 4 - Registering for an event that is already full (requirement 5)"
echo "S1008 sees event 102 marked FULL and tries to register anyway."
echo
m=$(mark)
printf 'S1008\n2\n102\n4\n' | client
sleep 0.2
server_log_since "$m"

# --------------------------------------------------------------- step 5
banner "STEP 5 - Client disconnections                    (requirement 9)"
echo "5a. S1009 presses Ctrl+C in the middle of the menu."
echo
m=$(mark)
# ./client is called directly (not via the helper function) so that $! is
# the client's own PID and the signal reaches the client process.
( printf 'S1009\n'; sleep 2 ) | ./client 127.0.0.1 "$PORT" > demo_ctrlc.txt 2>&1 &
CTRLC_PID=$!
sleep 1
kill -INT "$CTRLC_PID"
wait "$CTRLC_PID" 2> /dev/null
tail -4 demo_ctrlc.txt
rm -f demo_ctrlc.txt
sleep 0.2
server_log_since "$m"

echo
echo "5b. S1010's client process crashes (killed with SIGKILL) while logged"
echo "    in - it cannot send QUIT or close the connection itself."
echo
m=$(mark)
( printf 'S1010\n'; sleep 2 ) | ./client 127.0.0.1 "$PORT" > /dev/null 2>&1 &
CRASH_PID=$!
sleep 1
echo "\$ kill -9 $CRASH_PID      (client process)"
kill -9 "$CRASH_PID"
wait "$CRASH_PID" 2> /dev/null
sleep 0.3
server_log_since "$m"

echo
echo "5c. The server keeps running: S1010 logs in again straight away and"
echo "    registers for event 103."
echo
m=$(mark)
printf 'S1010\n2\n103\n4\n' | client
sleep 0.2
server_log_since "$m"

# ------------------------------------------------------------ final state
banner "FINAL STATE - stop the server (Ctrl+C) and show its records"
m=$(mark)
kill -INT "$SERVER_PID"
wait "$SERVER_PID" 2> /dev/null
{ kill $(jobs -p); wait; } 2> /dev/null      # leftover input feeders
server_log_since "$m"

banner "COMPLETE SERVER LOG"
cat "$LOG"
rm -f "$LOG"