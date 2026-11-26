"""
test_server.py - automated tests for the event registration server (Part A).

Starts ./server on a test port, runs each scenario with real TCP clients,
stops the server with SIGINT and prints the server's own log.

Usage (after building the server):
    gcc -Wall -Wextra -pthread -o server server.c
    python3 test_server.py [server_binary]
"""

import signal
import socket
import subprocess
import sys
import threading
import time

SERVER = sys.argv[1] if len(sys.argv) > 1 else "./server"
HOST, PORT = "127.0.0.1", 9200
results = {"pass": 0, "fail": 0}


class Client:
    """Minimal protocol client: send a line, read until the status line."""

    def __init__(self, name="C"):
        self.name = name
        self.sock = socket.create_connection((HOST, PORT), timeout=5)
        self.file = self.sock.makefile("r", encoding="utf-8", newline="\n")

    def read_reply(self):
        lines = []
        while True:
            line = self.file.readline()
            if line == "":
                lines.append("<connection closed>")
                return lines
            line = line.rstrip("\n")
            lines.append(line)
            if line.startswith("OK ") or line.startswith("ERR "):
                return lines

    def send(self, command):
        self.sock.sendall((command + "\n").encode())
        return self.read_reply()

    def close(self):
        self.sock.close()


def heading(title):
    print("\n" + "=" * 72 + "\n" + title + "\n" + "=" * 72)


def check(description, condition):
    results["pass" if condition else "fail"] += 1
    print(f"  [{'PASS' if condition else 'FAIL'}] {description}")


def talk(client, command, show=True):
    """Send a command, print the exchange, return the final status line."""
    reply = client.send(command)
    if show:
        print(f"{client.name} > {command}")
        for line in reply:
            print(f"{client.name} < {line}")
    return reply[-1]


# ---------------------------------------------------------------------------
def test_session():
    heading("TEST 1 - Single session: authentication, listing, registration")
    c = Client("C1")
    print("C1 < " + c.read_reply()[-1])
    check("LIST before login is refused",
          talk(c, "LIST").startswith("ERR AUTH_REQUIRED"))
    check("REGISTER before login is refused",
          talk(c, "REGISTER 101").startswith("ERR AUTH_REQUIRED"))
    check("Unknown student ID is rejected",
          talk(c, "LOGIN S9999").startswith("ERR AUTH_FAILED"))
    check("Registered student logs in and receives the event list",
          talk(c, "LOGIN S1001").startswith("OK LOGIN"))
    check("Registration accepted",
          talk(c, "REGISTER 103").startswith("OK REGISTERED"))
    check("Duplicate registration rejected",
          talk(c, "REGISTER 103").startswith("ERR DUPLICATE"))
    check("Unknown event rejected",
          talk(c, "REGISTER 999").startswith("ERR NO_SUCH_EVENT"))
    check("Non-numeric event ID rejected",
          talk(c, "REGISTER abc").startswith("ERR BAD_REQUEST"))
    check("MYEVENTS shows the registration",
          talk(c, "MYEVENTS").startswith("OK MYEVENTS 1"))
    check("QUIT ends the session", talk(c, "QUIT").startswith("OK BYE"))
    c.close()


def test_auth_limits():
    heading("TEST 2 - Authentication limits")
    a = Client("A")
    a.read_reply()
    talk(a, "LOGIN S1002")
    b = Client("B")
    b.read_reply()
    check("Same student cannot log in twice at the same time",
          talk(b, "LOGIN S1002").startswith("ERR IN_USE"))
    a.close()
    b.close()

    c = Client("C")
    c.read_reply()
    talk(c, "LOGIN BAD1")
    talk(c, "LOGIN BAD2")
    last = talk(c, "LOGIN BAD3")
    closed = c.file.readline() == ""
    check("Third failed login disconnects the client",
          last.startswith("ERR TOO_MANY_ATTEMPTS") and closed)
    c.close()


def test_concurrent_registration():
    heading("TEST 3 - 10 simultaneous clients race for event 102 (capacity 4)")
    ids = [f"S{1001 + i}" for i in range(10)]
    clients = []
    for i, sid in enumerate(ids):
        c = Client(f"T{i + 1}")
        c.read_reply()
        status = c.send(f"LOGIN {sid}")[-1]
        assert status.startswith("OK LOGIN"), status
        clients.append(c)
    print(f"  {len(clients)} clients connected and logged in at the same time")

    barrier = threading.Barrier(len(clients))
    replies = [None] * len(clients)

    def worker(i):
        barrier.wait()                       # release all at the same moment
        replies[i] = clients[i].send("REGISTER 102")[-1]

    threads = [threading.Thread(target=worker, args=(i,))
               for i in range(len(clients))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    for c, r in zip(clients, replies):
        print(f"  {c.name}: {r}")
    accepted = sum(r.startswith("OK REGISTERED") for r in replies)
    full = sum(r.startswith("ERR EVENT_FULL") for r in replies)
    event = [l for l in clients[0].send("LIST") if l.startswith("EVENT|102")]
    print(f"  Server state: {event[0]}")
    check("Exactly 4 registrations accepted", accepted == 4)
    check("The other 6 rejected as EVENT_FULL", full == 6)
    check("Server reports 4/4 registered, 0 available",
          event[0] == "EVENT|102|Cybersecurity Seminar|4|4|0")

    # The server is now at its 10-client limit
    extra = Client("X")
    check("11th connection refused with SERVER_FULL",
          extra.read_reply()[-1].startswith("ERR SERVER_FULL"))
    extra.close()
    for c in clients:
        c.send("QUIT")
        c.close()


def test_parallel_events():
    heading("TEST 4 - Simultaneous registrations for different events")
    plan = [("S1003", 101), ("S1004", 101), ("S1005", 103), ("S1006", 103),
            ("S1007", 101), ("S1008", 103)]
    clients = []
    for sid, _ in plan:
        c = Client(sid)
        c.read_reply()
        c.send(f"LOGIN {sid}")
        clients.append(c)
    barrier = threading.Barrier(len(plan))
    replies = [None] * len(plan)

    def worker(i):
        barrier.wait()
        replies[i] = clients[i].send(f"REGISTER {plan[i][1]}")[-1]

    threads = [threading.Thread(target=worker, args=(i,))
               for i in range(len(plan))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    for r in replies:
        print(f"  {r}")
    check("All 6 registrations accepted",
          all(r.startswith("OK REGISTERED") for r in replies))
    listing = clients[0].send("LIST")
    for line in listing:
        print(f"  {line}")
    check("Counters correct: 101 = 3/5, 103 = 4/6 (incl. S1001 from TEST 1)",
          "EVENT|101|AI Workshop|5|3|2" in listing
          and "EVENT|103|Research Conference|6|4|2" in listing)
    for c in clients:
        c.send("QUIT")
        c.close()


# ---------------------------------------------------------------------------
server = subprocess.Popen([SERVER, str(PORT)], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True)
time.sleep(0.5)
try:
    test_session()
    test_auth_limits()
    test_concurrent_registration()
    test_parallel_events()
finally:
    time.sleep(0.3)
    server.send_signal(signal.SIGINT)
    log, _ = server.communicate(timeout=10)

heading("SERVER LOG")
print(log, end="")
heading("RESULT")
print(f"{results['pass']} passed, {results['fail']} failed; "
      f"server exit status {server.returncode}")
sys.exit(1 if results["fail"] or server.returncode else 0)