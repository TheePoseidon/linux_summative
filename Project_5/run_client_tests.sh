#!/usr/bin/env bash
# run_client_tests.sh - build server and client, then run client scenarios.
# Usage: bash run_client_tests.sh
# Each scenario feeds keyboard input to ./client through a here-document;
# the client echoes piped input so the transcripts show what was typed.

set -u
cd "$(dirname "$0")"
PORT=9300

section() {
    echo
    echo "======================================================================"
    echo "$1"
    echo "======================================================================"
}

section "BUILD"
echo "\$ gcc -Wall -Wextra -pthread -o server server.c"
gcc -Wall -Wextra -pthread -o server server.c || exit 1
echo "\$ gcc -Wall -Wextra -o client client.c"
gcc -Wall -Wextra -o client client.c || exit 1

./server "$PORT" > server_log.txt 2>&1 &
SERVER_PID=$!
sleep 0.5

section "SCENARIO 1 - Failed then successful login, registration results, exit"
echo "Input: S9999, S1001, 1, 2/101, 2/101 (duplicate), 2/999, 2/abc, 3, 4"
./client 127.0.0.1 "$PORT" << 'EOF'
S9999
S1001
1
2
101
2
101
2
999
2
abc
3
4
EOF

section "SCENARIO 2 - Six students register for event 102 (capacity 4) at once"
for id in S1002 S1003 S1004 S1005 S1006 S1007; do
    printf '%s\n2\n102\n4\n' "$id" | ./client 127.0.0.1 "$PORT" \
        > "result_$id.txt" 2>&1 &
done
wait $(jobs -p | grep -v "^$SERVER_PID$") 2> /dev/null
for id in S1002 S1003 S1004 S1005 S1006 S1007; do
    printf '%s: %s\n' "$id" "$(grep -E 'SUCCESS\] Reg|REJECTED' "result_$id.txt")"
    rm -f "result_$id.txt"
done

section "SCENARIO 3 - Registering for a full event (102 is now 4/4)"
./client 127.0.0.1 "$PORT" << 'EOF'
S1008
2
102
4
EOF

section "SCENARIO 4 - Three failed logins: server disconnects the client"
./client 127.0.0.1 "$PORT" << 'EOF'
X1
X2
X3
EOF
echo "[client exit status: $?]"

section "SCENARIO 5 - Student leaves before logging in"
./client 127.0.0.1 "$PORT" << 'EOF'
q
EOF

section "SCENARIO 6 - Input ends (Ctrl+D) during the menu: graceful close"
printf 'S1009\n' | ./client 127.0.0.1 "$PORT"

section "SCENARIO 7 - Server not running"
./client 127.0.0.1 9399
echo "[client exit status: $?]"

kill -INT "$SERVER_PID"
wait "$SERVER_PID" 2> /dev/null

section "SERVER LOG"
cat server_log.txt
rm -f server_log.txt