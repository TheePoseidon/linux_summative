# Project 5: Developing a Concurrent TCP-Based University Event Registration System

A TCP client-server system in C. Students log in with their student ID, view events and register. The server handles each client in its own POSIX thread and protects every event with a mutex, so an event's capacity is never exceeded.

## Files

| File | Purpose |
|---|---|
| `server.c` | Multithreaded TCP server: authentication, events, registration, logging, shutdown |
| `client.c` | Interactive TCP client |
| `demo.sh` | Runs the Part D demonstration with one server and several clients |
| `demo_output.txt` | Output of `bash demo.sh` |

## Environment, Build and Run

GCC 15.2.0, glibc 2.43, Ubuntu 26.04 LTS (x86-64).

```
gcc -Wall -Wextra -pthread -o server server.c
gcc -Wall -Wextra -o client client.c
./server 9600                 # default port 8080, Ctrl+C stops it
./client 127.0.0.1 9600       # one terminal per student
bash demo.sh > demo_output.txt
```

## Design

- **TCP communication:**
  - The server sets up with `socket`, `bind`, `listen`, then calls `accept` in a loop.
  - Messages are text lines: `LOGIN <id>`, `LIST`, `REGISTER <event_id>`, `MYEVENTS`, `QUIT`.
  - Each reply ends with one `OK <CODE>` or `ERR <CODE>` line.
  - `read_line()` buffers the byte stream, and `send_all()` loops until every byte is sent.
- **Authentication:**
  - The server has 10 registered IDs (S1001 to S1010). Unknown IDs get `ERR AUTH_FAILED`, and after 3 failures the client is disconnected.
  - `LIST`, `REGISTER` and `MYEVENTS` return `ERR AUTH_REQUIRED` until the student logs in.
  - An ID can only be logged in on one connection at a time.
- **Concurrency:**
  - There is one thread per client (`pthread_create` and `pthread_detach`), with up to 10 clients connected at once.
  - The `clients[]` list is protected by `clients_lock`.
- **Shared resources:**
  - Each event has its own `pthread_mutex_t`.
  - Locks are never held while sending on a socket.
  - Event locks and `clients_lock` are never held together, so there is no deadlock.
- **Capacity and duplicates:**
  - `handle_register` does three steps under the event's mutex: check for a duplicate, check `registered < capacity`, then add the student and increment the count.
  - Two students racing for the last place can't both succeed, because the second one only enters after the count has been updated.
- **Disconnections:**
  - The server detects a client that has gone through `recv()` returning 0, a connection reset, the idle timeout (`SO_RCVTIMEO`) or keep-alive.
  - `MSG_NOSIGNAL` stops the server being killed by SIGPIPE.
  - The thread then closes the socket, removes the client from the list and exits, while the server keeps running.
- **Extras:** server log with timestamps, `SERVER_FULL` and `TIMEOUT` replies, and a graceful Ctrl+C shutdown that prints the final registrations.

## Demonstration (from `demo_output.txt`)

| # | Scenario | Evidence |
|---|---|---|
| 1 | Successful authentication | `[SUCCESS] Authentication successful. Welcome, Amina Uwase (S1001)` |
| 2 | Unsuccessful authentication | `[FAILED] Authentication failed: Unknown student ID 'S9999' (2 attempt(s) left)` |
| 3 | Event list | Table of 101 AI Workshop (5), 102 Cybersecurity Seminar (4), 103 Research Conference (6) |
| 4 | Successful registration | `[SUCCESS] Registration confirmed: S1001 registered for 101 AI Workshop (place 1 of 5, 4 left)` |
| 5 | Event already full | `[REJECTED] Event full - no places left: 102 Cybersecurity Seminar is full (4/4)` |
| 6 | Duplicate registration | `[REJECTED] Duplicate registration: S1001 is already registered for 101 AI Workshop` |
| 7 | 5+ simultaneous clients | `=> 6 TCP connections established to the server` |
| 8 | Capacity not exceeded | 6 clients register for 102 at the same moment: 4 SUCCESS, 2 REJECTED |
| 9 | Client disconnection | Ctrl+C: `Client #10 disconnected (client sent QUIT)`; `kill -9`: `Client #11 disconnected (client closed the connection)` |

Six simultaneous registrations for event 102 (capacity 4), from the server log:
```
[19:55:03] S1007 REGISTERED for 102 Cybersecurity Seminar (1/4)
[19:55:03] S1005 REGISTERED for 102 Cybersecurity Seminar (2/4)
[19:55:03] S1006 REGISTERED for 102 Cybersecurity Seminar (3/4)
[19:55:03] S1003 REGISTERED for 102 Cybersecurity Seminar (4/4)
[19:55:03] S1004 rejected: 102 Cybersecurity Seminar is FULL (4/4)
[19:55:03] S1002 rejected: 102 Cybersecurity Seminar is FULL (4/4)
```

Graceful session close and server shutdown:
```
Registration session closed. Goodbye, S1001.
[19:55:09] Shutdown requested - closing listening socket
[19:55:09] Final registrations:
[19:55:09]   101 AI Workshop            1/5: S1001
[19:55:09]   102 Cybersecurity Seminar  4/4: S1007 S1005 S1006 S1003
[19:55:09]   103 Research Conference    1/6: S1010
[19:55:09] Server stopped
```

After the crashed client (S1010, `kill -9`), the same student logged in again straight away and registered for event 103, which shows the server kept running.
