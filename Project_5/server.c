/*
 * server.c: University event registration server (TCP, POSIX threads)
 *
 * Build:  gcc -Wall -Wextra -pthread -o server server.c
 * Run:    ./server [port]          (default port 8080; Ctrl+C to stop)
 *
 * Contents: 1. TCP communication       5. Event capacity management
 *           2. Student authentication  6. Duplicate registrations
 *           3. Concurrency approach    7. Unexpected disconnections
 *           4. Shared resource protection   (+ shutdown, test hooks)
 *
 * 1. TCP COMMUNICATION
 *   Connection set-up (main):
 *     socket(AF_INET, SOCK_STREAM)  create a TCP socket
 *     setsockopt(SO_REUSEADDR)      allow a quick restart on the same port
 *     bind(INADDR_ANY, port)        attach it to the port on all interfaces
 *     listen(BACKLOG)               start accepting connection requests
 *     accept() in a loop            one new socket per client
 *   Up to MAX_CLIENTS (10) clients may be connected at once; an extra
 *   connection receives "ERR SERVER_FULL" and is closed.
 *
 *   Protocol: plain text, one message per line ending in "\n".
 *     Client commands:  LOGIN <student_id> | LIST | REGISTER <event_id> |
 *                       MYEVENTS | HELP | QUIT
 *     Server replies:   zero or more data lines, then exactly ONE status
 *                       line, so the client reads until "OK" or "ERR":
 *       data lines   INFO <text>
 *                    EVENT|<id>|<name>|<capacity>|<registered>|<available>
 *                    MYEVENT|<id>|<name>
 *       status line  OK <CODE> <text>   or   ERR <CODE> <text>
 *   On connection the server sends "OK WELCOME ...".
 *   Error codes: AUTH_REQUIRED, AUTH_FAILED, TOO_MANY_ATTEMPTS, IN_USE,
 *   ALREADY_LOGGED_IN, NO_SUCH_EVENT, DUPLICATE, EVENT_FULL, BAD_REQUEST,
 *   UNKNOWN_COMMAND, LINE_TOO_LONG, SERVER_FULL, TIMEOUT.
 *
 *   TCP is a byte stream, not a message stream: one recv() can return part
 *   of a line or several lines. read_line() buffers bytes until a newline
 *   arrives. send() may write only part of a buffer, so send_all() loops
 *   until everything is sent.
 *
 * 2. STUDENT AUTHENTICATION  (handle_login, client_thread)
 *   - students[] holds the 10 registered IDs (S1001-S1010) and names.
 *   - LOGIN <id> is checked against that list. Unknown IDs get
 *     "ERR AUTH_FAILED"; after MAX_LOGIN_ATTEMPTS (3) failures the server
 *     sends "ERR TOO_MANY_ATTEMPTS" and closes the connection.
 *   - Each session keeps its logged-in student (-1 = not logged in).
 *     client_thread() refuses LIST, REGISTER and MYEVENTS with
 *     "ERR AUTH_REQUIRED" until a login succeeds; only LOGIN, HELP and QUIT
 *     work before that.
 *   - A student can be logged in on only one connection at a time
 *     ("ERR IN_USE"). The check and the marking of this connection happen
 *     under clients_lock, so two simultaneous logins with the same ID
 *     cannot both succeed.
 *   - After a successful login the server sends the event list.
 *
 * 3. CONCURRENCY APPROACH
 *   Thread-per-client with POSIX threads. main() only accepts connections;
 *   for each one it reserves a slot in clients[] and starts client_thread()
 *   with pthread_create(), then pthread_detach() so the thread's resources
 *   are freed automatically when it ends. Every client therefore has its
 *   own thread blocking on its own socket, and sessions run independently:
 *   a slow or idle client never delays the others.
 *
 * 4. SHARED RESOURCE PROTECTION
 *   Data shared between threads and how it is protected:
 *     students[]  read-only after start-up -> no lock needed
 *     events[]    count and attendee list of each event -> that event's
 *                 own mutex, events[i].lock
 *     clients[]   connected-client list and active_clients ->
 *                 clients_lock (+ clients_cv, used at shutdown)
 *   Rules:
 *     - Shared fields are only read or written while holding their lock.
 *     - Locks are held only for memory operations, never while sending on
 *       a socket: data is copied under the lock and sent after unlocking,
 *       so a slow client cannot block other clients.
 *     - No thread ever holds clients_lock and an event lock together, so
 *       lock-ordering deadlocks cannot happen.
 *     - Separate locks per event let registrations for different events
 *       proceed in parallel.
 *
 * 5. EVENT CAPACITY MANAGEMENT  (handle_register)
 *   Each event stores its capacity and current count. Registration runs
 *   this sequence inside ONE critical section on the event's mutex:
 *       lock -> duplicate check -> count < capacity ? -> add student,
 *       count++ -> unlock
 *   The capacity check and the update cannot be separated, so the count
 *   can never pass the capacity.
 *
 *   The last place, two students at exactly the same time: event 102 has
 *   3/4 taken and threads A and B both handle "REGISTER 102". Both call
 *   pthread_mutex_lock(); the mutex admits one, say A. A sees 3 < 4, adds
 *   its student, sets the count to 4 and unlocks. Only then can B enter;
 *   it sees 4 >= 4 and replies EVENT_FULL. B can never act on the old
 *   value 3. Without the mutex both could read 3 and both register (5/4).
 *
 * 6. PREVENTION OF DUPLICATE REGISTRATIONS  (handle_register)
 *   Each event keeps the list of registered students (attendees[]). The
 *   first step of the critical section searches that list; if the student
 *   is already in it the reply is "ERR DUPLICATE" and nothing changes.
 *   Because the search happens under the same lock as the update, two
 *   requests from the same student cannot both pass the check. The
 *   duplicate check runs before the capacity check, so a student who is
 *   already registered for a full event is told "duplicate", not "full".
 *
 * 7. UNEXPECTED CLIENT DISCONNECTIONS  (read_line, send_all, client_thread)
 *     How the client disappears          What the server sees
 *     closes or crashes (even kill -9)   recv() returns 0
 *     connection reset                   recv() fails (ECONNRESET)
 *     goes silent, network lost          no data for IDLE_TIMEOUT_SEC
 *                                        (SO_RCVTIMEO) + TCP keep-alive
 *     gone when the server replies       send() fails; MSG_NOSIGNAL stops
 *                                        SIGPIPE killing the server;
 *                                        SO_SNDTIMEO stops endless blocking
 *   In every case client_thread() leaves its loop and runs one clean-up
 *   path: close the socket, remove the client from clients[] (freeing the
 *   slot and the student's login), log the reason, and end the thread.
 *   The server keeps running. A registration completed before the
 *   disconnect stays recorded exactly once, because each registration is
 *   finished inside its critical section before any reply is sent.
 *
 * SHUTDOWN (Ctrl+C / SIGTERM)
 *   The signal handler only sets a flag. main() notices it, stops
 *   accepting, shuts down every client socket (their threads see the
 *   connection close and exit), waits on clients_cv until no clients
 *   remain, prints the final registrations and destroys all mutexes.
 *
 * TEST HOOKS (off in the normal build; used for concurrency testing)
 *   -DREG_DELAY_US=n   pause inside the registration critical section
 *   -DUNSAFE_NO_LOCK   build without the event mutex (race demonstration)
 *   -DIDLE_TIMEOUT_SEC=n  shorter idle timeout
 */

#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_PORT        8080
#define MAX_CLIENTS         10      /* simultaneous connections (>= 5) */
#define BACKLOG             16      /* pending connections for listen() */
#define MAX_LINE            256     /* longest accepted command line */
#define MAX_LOGIN_ATTEMPTS  3       /* failed logins before disconnect */
#define MAX_CAPACITY        16      /* upper bound for any event capacity */

/* Unexpected disconnections: a client that sends nothing for this many
 * seconds is disconnected (also catches peers that vanished without
 * closing the connection, e.g. a lost network). Sends that cannot complete
 * within SEND_TIMEOUT_SEC (client not reading) also end the session. */
#ifndef IDLE_TIMEOUT_SEC
#define IDLE_TIMEOUT_SEC    300
#endif
#define SEND_TIMEOUT_SEC    10

/* Test hooks for the Part C concurrency tests (OFF by default)
 * REG_DELAY_US    pause inside the registration critical section, between
 *                 the capacity check and the update, to widen the race
 *                 window so simultaneous requests really overlap.
 * UNSAFE_NO_LOCK  build WITHOUT the event lock, to show the race that the
 *                 lock prevents. Never used in the real server build. */
#ifndef REG_DELAY_US
#define REG_DELAY_US 0
#endif
#ifdef UNSAFE_NO_LOCK
#define EVENT_LOCK(ev)   ((void)0)
#define EVENT_UNLOCK(ev) ((void)0)
#else
#define EVENT_LOCK(ev)   pthread_mutex_lock(&(ev)->lock)
#define EVENT_UNLOCK(ev) pthread_mutex_unlock(&(ev)->lock)
#endif

/* Registered students (read-only) */
typedef struct {
    const char *id;
    const char *name;
} Student;

static const Student students[] = {
    {"S1001", "Amina Uwase"},     {"S1002", "Brian Otieno"},
    {"S1003", "Chloe Mukamana"},  {"S1004", "David Kamau"},
    {"S1005", "Esther Ingabire"}, {"S1006", "Felix Mugisha"},
    {"S1007", "Grace Wanjiru"},   {"S1008", "Hassan Abdi"},
    {"S1009", "Irene Ishimwe"},   {"S1010", "Joseph Kiprop"},
};
#define NUM_STUDENTS ((int)(sizeof students / sizeof students[0]))

/* Events (shared, one mutex per event) */
typedef struct {
    int             id;
    const char     *name;
    int             capacity;
    int             registered;                 /* current participants */
    int             attendees[MAX_CAPACITY];    /* indexes into students[] */
    pthread_mutex_t lock;
} Event;

static Event events[] = {
    {.id = 101, .name = "AI Workshop",           .capacity = 5},
    {.id = 102, .name = "Cybersecurity Seminar", .capacity = 4},
    {.id = 103, .name = "Research Conference",   .capacity = 6},
};
#define NUM_EVENTS ((int)(sizeof events / sizeof events[0]))

/* Connected clients (shared, protected by clients_lock) */
typedef struct {
    int  in_use;
    int  fd;
    int  number;                    /* connection number, for logging */
    int  student;                   /* index in students[], -1 = not logged in */
    char addr[32];                  /* "ip:port" */
} ClientSlot;

static ClientSlot      clients[MAX_CLIENTS];
static int             active_clients;
static int             next_number = 1;
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  clients_cv   = PTHREAD_COND_INITIALIZER;

static volatile sig_atomic_t stop_requested;    /* set by SIGINT/SIGTERM */

/*
 * Logging: one complete line per fputs call, so lines from different
 * threads never interleave.
 */
static void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_msg(const char *fmt, ...)
{
    char line[1024];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int len = (int)strftime(line, sizeof line, "[%H:%M:%S] ", &tm);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + len, sizeof line - (size_t)len, fmt, ap);
    va_end(ap);
    strncat(line, "\n", sizeof line - strlen(line) - 1);
    fputs(line, stdout);
}

/* Print the list of connected clients. CALLER MUST HOLD clients_lock. */
static void log_client_list(void)
{
    char buf[1024];
    int len = snprintf(buf, sizeof buf, "Connected clients (%d/%d):",
                       active_clients, MAX_CLIENTS);
    for (int i = 0; i < MAX_CLIENTS && len < (int)sizeof buf; i++) {
        if (!clients[i].in_use)
            continue;
        len += snprintf(buf + len, sizeof buf - (size_t)len,
                        " [#%d %s %s]", clients[i].number, clients[i].addr,
                        clients[i].student >= 0
                            ? students[clients[i].student].id
                            : "not logged in");
    }
    if (active_clients == 0)
        snprintf(buf + len, sizeof buf - (size_t)len, " none");
    log_msg("%s", buf);
}

/* Socket helpers */

/* Send the whole buffer. send() may write only part of it, so loop.
 * MSG_NOSIGNAL: if the client has gone, return an error instead of
 * killing the server with SIGPIPE. */
static int send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Format one protocol line and send it with a trailing newline. */
static int send_line(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int send_line(int fd, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int len = vsnprintf(line, sizeof line - 1, fmt, ap);
    va_end(ap);
    if (len < 0)
        return -1;
    if (len > (int)sizeof line - 2)
        len = (int)sizeof line - 2;
    line[len++] = '\n';
    return send_all(fd, line, (size_t)len);
}

/* TCP is a byte stream: one recv() may return half a line or several
 * lines. LineReader buffers incoming bytes and hands out complete lines. */
typedef struct {
    int    fd;
    char   buf[1024];
    size_t len;
    int    discarding;      /* skipping the rest of an over-long line */
} LineReader;

/* Returns 1 = line read, 0 = connection closed, -1 = error (including a
 *         connection reset), -2 = line too long (it is discarded),
 *         -3 = idle timeout (no data for IDLE_TIMEOUT_SEC). */
static int read_line(LineReader *r, char *out, size_t out_size)
{
    for (;;) {
        char *nl = memchr(r->buf, '\n', r->len);
        if (nl != NULL) {
            size_t n = (size_t)(nl - r->buf);
            int too_long = r->discarding || n >= out_size;
            if (!too_long) {
                memcpy(out, r->buf, n);
                out[n] = '\0';
                if (n > 0 && out[n - 1] == '\r')    /* accept CRLF too */
                    out[n - 1] = '\0';
            }
            memmove(r->buf, nl + 1, r->len - n - 1);  /* drop the line */
            r->len -= n + 1;
            r->discarding = 0;
            if (too_long)
                return -2;
            return 1;
        }
        if (r->len == sizeof r->buf) {      /* full buffer, no newline */
            r->len = 0;
            r->discarding = 1;
        }
        ssize_t got = recv(r->fd, r->buf + r->len, sizeof r->buf - r->len, 0);
        if (got == 0)
            return 0;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return -3;                  /* SO_RCVTIMEO expired */
            return -1;                      /* e.g. ECONNRESET */
        }
        r->len += (size_t)got;
    }
}

/* Lookups (read-only data, or immutable event fields: no lock needed) */
static int find_student(const char *id)
{
    for (int i = 0; i < NUM_STUDENTS; i++)
        if (strcmp(students[i].id, id) == 0)
            return i;
    return -1;
}

static Event *find_event(int id)
{
    for (int i = 0; i < NUM_EVENTS; i++)
        if (events[i].id == id)      /* id and name never change */
            return &events[i];
    return NULL;
}

/*
 * Request handlers. Each sends its data lines and ONE status line.
 * Return -1 if the connection should be closed.
 */

/* Send one EVENT line per event. Each event's counter is copied while
 * holding that event's lock; the socket write happens after unlocking. */
static int send_event_list(int fd)
{
    for (int i = 0; i < NUM_EVENTS; i++) {
        pthread_mutex_lock(&events[i].lock);
        int registered = events[i].registered;
        pthread_mutex_unlock(&events[i].lock);

        if (send_line(fd, "EVENT|%d|%s|%d|%d|%d", events[i].id,
                      events[i].name, events[i].capacity, registered,
                      events[i].capacity - registered) < 0)
            return -1;
    }
    return 0;
}

/* LOGIN <student_id> */
static int handle_login(int fd, int slot, int *student, int *failed,
                        const char *arg)
{
    if (*student >= 0)
        return send_line(fd, "ERR ALREADY_LOGGED_IN Already logged in as %s",
                         students[*student].id);
    if (arg == NULL)
        return send_line(fd, "ERR BAD_REQUEST Usage: LOGIN <student_id>");

    int idx = find_student(arg);
    if (idx < 0) {
        /* Unknown ID: reject. After MAX_LOGIN_ATTEMPTS, disconnect. */
        (*failed)++;
        log_msg("Client #%d: login REJECTED for unknown ID '%s' "
                "(attempt %d/%d)", clients[slot].number, arg, *failed,
                MAX_LOGIN_ATTEMPTS);
        if (*failed >= MAX_LOGIN_ATTEMPTS) {
            send_line(fd, "ERR TOO_MANY_ATTEMPTS Too many failed logins - "
                          "disconnecting");
            return -1;
        }
        return send_line(fd, "ERR AUTH_FAILED Unknown student ID '%s' "
                             "(%d attempt(s) left)", arg,
                         MAX_LOGIN_ATTEMPTS - *failed);
    }

    /* Check that this student is not logged in on another connection and
     * mark this connection as theirs, in ONE critical section, so two
     * simultaneous logins with the same ID cannot both succeed. */
    pthread_mutex_lock(&clients_lock);
    int in_use = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].in_use && clients[i].student == idx)
            in_use = 1;
    if (!in_use) {
        clients[slot].student = idx;
        log_msg("Client #%d: authenticated as %s (%s)",
                clients[slot].number, students[idx].id, students[idx].name);
        log_client_list();
    }
    pthread_mutex_unlock(&clients_lock);

    if (in_use)
        return send_line(fd, "ERR IN_USE %s is already logged in on another "
                             "connection", students[idx].id);

    *student = idx;
    /* Successful login: greet, then send the list of events. */
    if (send_line(fd, "INFO Welcome, %s (%s)", students[idx].name,
                  students[idx].id) < 0 || send_event_list(fd) < 0)
        return -1;
    return send_line(fd, "OK LOGIN %s %d events available",
                     students[idx].id, NUM_EVENTS);
}

/* REGISTER <event_id> */
static int handle_register(int fd, int student, const char *arg)
{
    char *end;
    long id;

    if (arg == NULL)
        return send_line(fd, "ERR BAD_REQUEST Usage: REGISTER <event_id>");
    errno = 0;
    id = strtol(arg, &end, 10);
    if (errno != 0 || *end != '\0' || end == arg)
        return send_line(fd, "ERR BAD_REQUEST Event ID must be a number");

    Event *ev = find_event((int)id);
    if (ev == NULL)
        return send_line(fd, "ERR NO_SUCH_EVENT Event %ld does not exist", id);

    /* Critical section: duplicate check, capacity check and update
     * happen atomically under the event's lock. While one thread is here,
     * any other thread registering for THIS event waits at the lock, so the
     * last place can only be given to one student. */
    enum { REGISTERED, DUPLICATE, FULL } result;
    int registered, capacity = ev->capacity;

    EVENT_LOCK(ev);
    result = REGISTERED;
    for (int i = 0; i < ev->registered && i < MAX_CAPACITY; i++) /* 1. */
        if (ev->attendees[i] == student)
            result = DUPLICATE;
    if (result == REGISTERED && ev->registered >= ev->capacity)
        result = FULL;                                   /* 2. full? */
    if (result == REGISTERED) {
        if (REG_DELAY_US > 0) {                          /* test hook only */
            struct timespec d = {0, REG_DELAY_US * 1000L};
            nanosleep(&d, NULL);
        }
        if (ev->registered < MAX_CAPACITY)               /* array bound */
            ev->attendees[ev->registered] = student;
        ev->registered++;                                /* 3. register */
    }
    registered = ev->registered;
    EVENT_UNLOCK(ev);
    /* End of critical section; reply is sent without the lock. */

    switch (result) {
    case REGISTERED:
        log_msg("%s REGISTERED for %d %s (%d/%d)", students[student].id,
                ev->id, ev->name, registered, capacity);
        return send_line(fd, "OK REGISTERED %s registered for %d %s "
                             "(place %d of %d, %d left)",
                         students[student].id, ev->id, ev->name, registered,
                         capacity, capacity - registered);
    case DUPLICATE:
        log_msg("%s DUPLICATE registration for %d rejected",
                students[student].id, ev->id);
        return send_line(fd, "ERR DUPLICATE %s is already registered for "
                             "%d %s", students[student].id, ev->id, ev->name);
    default:
        log_msg("%s rejected: %d %s is FULL (%d/%d)", students[student].id,
                ev->id, ev->name, registered, capacity);
        return send_line(fd, "ERR EVENT_FULL %d %s is full (%d/%d)",
                         ev->id, ev->name, registered, capacity);
    }
}

/* MYEVENTS */
static int handle_myevents(int fd, int student)
{
    int count = 0;
    for (int i = 0; i < NUM_EVENTS; i++) {
        int found = 0;
        pthread_mutex_lock(&events[i].lock);
        for (int j = 0; j < events[i].registered && j < MAX_CAPACITY; j++)
            if (events[i].attendees[j] == student)
                found = 1;
        pthread_mutex_unlock(&events[i].lock);

        if (found) {
            count++;
            if (send_line(fd, "MYEVENT|%d|%s", events[i].id,
                          events[i].name) < 0)
                return -1;
        }
    }
    return send_line(fd, "OK MYEVENTS %d registration(s)", count);
}

static int handle_help(int fd)
{
    static const char *lines[] = {
        "INFO LOGIN <student_id>   log in",
        "INFO LIST                 list events",
        "INFO REGISTER <event_id>  register for an event",
        "INFO MYEVENTS             show your registrations",
        "INFO QUIT                 disconnect",
    };
    for (size_t i = 0; i < sizeof lines / sizeof lines[0]; i++)
        if (send_line(fd, "%s", lines[i]) < 0)
            return -1;
    return send_line(fd, "OK HELP");
}

/* Client thread: one per connected client */
static void *client_thread(void *arg)
{
    int slot = (int)(intptr_t)arg;
    /* fd and number were written by main before this thread was created
     * and are not changed while the client is connected. */
    int fd = clients[slot].fd;
    int number = clients[slot].number;
    int student = -1;           /* logged-in student, -1 = not logged in */
    int failed = 0;             /* failed login attempts */
    LineReader reader = {.fd = fd};
    char line[MAX_LINE];

    /* Detect clients that disappear without closing the connection:
     *  - SO_RCVTIMEO: recv() gives up after IDLE_TIMEOUT_SEC of silence
     *  - SO_SNDTIMEO: send() gives up if the client stops reading
     *  - SO_KEEPALIVE: the kernel probes long-idle connections */
    struct timeval rcv = {IDLE_TIMEOUT_SEC, 0}, snd = {SEND_TIMEOUT_SEC, 0};
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof rcv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof on);
    const char *reason = "client closed the connection";

    send_line(fd, "OK WELCOME University Event Registration Server - "
                  "log in with: LOGIN <student_id>");

    for (;;) {
        int rc = read_line(&reader, line, sizeof line);
        if (rc == 0)
            break;                                  /* orderly close */
        if (rc == -1) {
            reason = "connection lost unexpectedly";/* reset, network */
            break;
        }
        if (rc == -3) {
            reason = "idle timeout";
            send_line(fd, "ERR TIMEOUT No activity for %d seconds - "
                          "disconnecting", IDLE_TIMEOUT_SEC);
            break;
        }
        if (rc == -2) {
            if (send_line(fd, "ERR LINE_TOO_LONG Maximum %d characters",
                          MAX_LINE - 1) < 0)
                break;
            continue;
        }

        /* Split "COMMAND argument" (extra spaces are ignored). */
        char *save = NULL;
        char *cmd = strtok_r(line, " \t", &save);
        char *argument = strtok_r(NULL, " \t", &save);
        if (cmd == NULL)
            continue;                               /* blank line */

        int result;
        if (strcasecmp(cmd, "QUIT") == 0) {
            send_line(fd, "OK BYE Goodbye");
            reason = "client sent QUIT";
            break;
        } else if (strcasecmp(cmd, "HELP") == 0) {
            result = handle_help(fd);
        } else if (strcasecmp(cmd, "LOGIN") == 0) {
            result = handle_login(fd, slot, &student, &failed, argument);
        } else if (student < 0) {
            /* Every other command requires authentication. */
            result = send_line(fd, "ERR AUTH_REQUIRED Log in first: "
                                   "LOGIN <student_id>");
        } else if (strcasecmp(cmd, "LIST") == 0) {
            result = send_event_list(fd) < 0 ? -1
                   : send_line(fd, "OK EVENTS %d events", NUM_EVENTS);
        } else if (strcasecmp(cmd, "REGISTER") == 0) {
            result = handle_register(fd, student, argument);
        } else if (strcasecmp(cmd, "MYEVENTS") == 0) {
            result = handle_myevents(fd, student);
        } else {
            result = send_line(fd, "ERR UNKNOWN_COMMAND '%s' (try HELP)", cmd);
        }
        if (result < 0) {
            reason = failed >= MAX_LOGIN_ATTEMPTS
                         ? "too many failed logins"
                         : "could not send reply (client gone)";
            break;
        }
    }

    /* Remove this client from the connected list and close its socket.
     * Closing inside the lock means main never sees a closed descriptor
     * in the list. Signalling clients_cv lets a shutting-down main know
     * that one more client has left. */
    pthread_mutex_lock(&clients_lock);
    close(fd);
    clients[slot].in_use = 0;
    clients[slot].student = -1;
    active_clients--;
    log_msg("Client #%d disconnected (%s)", number, reason);
    log_client_list();
    pthread_cond_signal(&clients_cv);
    pthread_mutex_unlock(&clients_lock);
    return NULL;
}

/* main: listening socket and accept loop */
static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;         /* only async-signal-safe work here */
}

int main(int argc, char *argv[])
{
    int port = DEFAULT_PORT;
    if (argc > 1) {
        char *end;
        long p = strtol(argv[1], &end, 10);
        if (*end != '\0' || p < 1 || p > 65535) {
            fprintf(stderr, "Usage: %s [port 1-65535]\n", argv[0]);
            return EXIT_FAILURE;
        }
        port = (int)p;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Signals: Ctrl+C / SIGTERM request a clean shutdown. SA_RESTART is
     * not set, so a blocking call returns EINTR and the flag is seen. */
    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);       /* writes to closed sockets -> EPIPE */

    for (int i = 0; i < NUM_EVENTS; i++)
        pthread_mutex_init(&events[i].lock, NULL);

    /* 1. Create the TCP socket (IPv4, stream). */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }
    /* Allow an immediate restart on the same port after the server stops. */
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    /* 2. Bind it to the port on all network interfaces. */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    /* 3. Listen for incoming connections. */
    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    log_msg("Server listening on port %d (max %d clients)", port, MAX_CLIENTS);
    for (int i = 0; i < NUM_EVENTS; i++)
        log_msg("Event %d %-22s capacity %d", events[i].id, events[i].name,
                events[i].capacity);

    /* 4. Accept loop. poll() waits up to 500 ms for a connection, so the
     *    stop flag is checked regularly even when nobody connects. */
    while (!stop_requested) {
        struct pollfd pfd = {.fd = listen_fd, .events = POLLIN};
        int ready = poll(&pfd, 1, 500);
        if (ready <= 0)
            continue;                       /* timeout or EINTR */

        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0)
            continue;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);

        /* Reserve a slot in the connected-clients list. */
        pthread_mutex_lock(&clients_lock);
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (!clients[i].in_use) {
                slot = i;
                break;
            }
        if (slot < 0) {
            pthread_mutex_unlock(&clients_lock);
            log_msg("Connection from %s:%d refused - server full", ip,
                    ntohs(peer.sin_port));
            send_line(fd, "ERR SERVER_FULL Maximum %d clients - try later",
                      MAX_CLIENTS);
            close(fd);
            continue;
        }
        clients[slot].in_use = 1;
        clients[slot].fd = fd;
        clients[slot].student = -1;
        clients[slot].number = next_number++;
        snprintf(clients[slot].addr, sizeof clients[slot].addr, "%s:%d",
                 ip, ntohs(peer.sin_port));
        active_clients++;
        log_msg("Client #%d connected from %s", clients[slot].number,
                clients[slot].addr);
        log_client_list();
        pthread_mutex_unlock(&clients_lock);

        /* 5. One thread per client. SIGINT/SIGTERM are blocked in the new
         *    thread (it inherits this mask), so signals go to main only. */
        sigset_t block, old;
        sigemptyset(&block);
        sigaddset(&block, SIGINT);
        sigaddset(&block, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &block, &old);

        pthread_t tid;
        int rc = pthread_create(&tid, NULL, client_thread,
                                (void *)(intptr_t)slot);
        pthread_sigmask(SIG_SETMASK, &old, NULL);
        if (rc != 0) {
            log_msg("pthread_create failed: %s", strerror(rc));
            pthread_mutex_lock(&clients_lock);
            close(fd);
            clients[slot].in_use = 0;
            active_clients--;
            pthread_mutex_unlock(&clients_lock);
            continue;
        }
        pthread_detach(tid);    /* resources freed automatically on exit */
    }

    /* Graceful shutdown */
    log_msg("Shutdown requested - closing listening socket");
    close(listen_fd);

    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].in_use)
            shutdown(clients[i].fd, SHUT_RDWR);   /* wakes blocked recv() */
    while (active_clients > 0)
        pthread_cond_wait(&clients_cv, &clients_lock);
    pthread_mutex_unlock(&clients_lock);

    /* All client threads have finished: events can be read directly. */
    log_msg("Final registrations:");
    for (int i = 0; i < NUM_EVENTS; i++) {
        char list[256] = "";
        for (int j = 0; j < events[i].registered && j < MAX_CAPACITY; j++) {
            strncat(list, " ", sizeof list - strlen(list) - 1);
            strncat(list, students[events[i].attendees[j]].id,
                    sizeof list - strlen(list) - 1);
        }
        log_msg("  %d %-22s %d/%d:%s", events[i].id, events[i].name,
                events[i].registered, events[i].capacity,
                events[i].registered ? list : " (none)");
        pthread_mutex_destroy(&events[i].lock);
    }
    pthread_mutex_destroy(&clients_lock);
    pthread_cond_destroy(&clients_cv);
    log_msg("Server stopped");
    return EXIT_SUCCESS;
}