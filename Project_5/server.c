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
#define MAX_CLIENTS         10
#define BACKLOG             16
#define MAX_LINE            256
#define MAX_LOGIN_ATTEMPTS  3
#define MAX_CAPACITY        16

#ifndef IDLE_TIMEOUT_SEC
#define IDLE_TIMEOUT_SEC    300
#endif
#define SEND_TIMEOUT_SEC    10

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

typedef struct {
    int             id;
    const char     *name;
    int             capacity;
    int             registered;
    int             attendees[MAX_CAPACITY];
    pthread_mutex_t lock;
} Event;

static Event events[] = {
    {.id = 101, .name = "AI Workshop",           .capacity = 5},
    {.id = 102, .name = "Cybersecurity Seminar", .capacity = 4},
    {.id = 103, .name = "Research Conference",   .capacity = 6},
};
#define NUM_EVENTS ((int)(sizeof events / sizeof events[0]))

typedef struct {
    int  in_use;
    int  fd;
    int  number;
    int  student;
    char addr[32];
} ClientSlot;

static ClientSlot      clients[MAX_CLIENTS];
static int             active_clients;
static int             next_number = 1;
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  clients_cv   = PTHREAD_COND_INITIALIZER;

static volatile sig_atomic_t stop_requested;

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

typedef struct {
    int    fd;
    char   buf[1024];
    size_t len;
    int    discarding;
} LineReader;

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
                if (n > 0 && out[n - 1] == '\r')
                    out[n - 1] = '\0';
            }
            memmove(r->buf, nl + 1, r->len - n - 1);
            r->len -= n + 1;
            r->discarding = 0;
            if (too_long)
                return -2;
            return 1;
        }
        if (r->len == sizeof r->buf) {
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
                return -3;
            return -1;
        }
        r->len += (size_t)got;
    }
}

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
        if (events[i].id == id)
            return &events[i];
    return NULL;
}

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
    if (send_line(fd, "INFO Welcome, %s (%s)", students[idx].name,
                  students[idx].id) < 0 || send_event_list(fd) < 0)
        return -1;
    return send_line(fd, "OK LOGIN %s %d events available",
                     students[idx].id, NUM_EVENTS);
}

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

    enum { REGISTERED, DUPLICATE, FULL } result;
    int registered, capacity = ev->capacity;

    EVENT_LOCK(ev);
    result = REGISTERED;
    for (int i = 0; i < ev->registered && i < MAX_CAPACITY; i++)
        if (ev->attendees[i] == student)
            result = DUPLICATE;
    if (result == REGISTERED && ev->registered >= ev->capacity)
        result = FULL;
    if (result == REGISTERED) {
        if (REG_DELAY_US > 0) {
            struct timespec d = {0, REG_DELAY_US * 1000L};
            nanosleep(&d, NULL);
        }
        if (ev->registered < MAX_CAPACITY)
            ev->attendees[ev->registered] = student;
        ev->registered++;
    }
    registered = ev->registered;
    EVENT_UNLOCK(ev);

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

static void *client_thread(void *arg)
{
    int slot = (int)(intptr_t)arg;
    int fd = clients[slot].fd;
    int number = clients[slot].number;
    int student = -1;
    int failed = 0;
    LineReader reader = {.fd = fd};
    char line[MAX_LINE];

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
            break;
        if (rc == -1) {
            reason = "connection lost unexpectedly";
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

        char *save = NULL;
        char *cmd = strtok_r(line, " \t", &save);
        char *argument = strtok_r(NULL, " \t", &save);
        if (cmd == NULL)
            continue;

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

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
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

    struct sigaction sa = {0};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    for (int i = 0; i < NUM_EVENTS; i++)
        pthread_mutex_init(&events[i].lock, NULL);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    if (listen(listen_fd, BACKLOG) < 0) {
        perror("listen");
        close(listen_fd);
        return EXIT_FAILURE;
    }

    log_msg("Server listening on port %d (max %d clients)", port, MAX_CLIENTS);
    for (int i = 0; i < NUM_EVENTS; i++)
        log_msg("Event %d %-22s capacity %d", events[i].id, events[i].name,
                events[i].capacity);

    while (!stop_requested) {
        struct pollfd pfd = {.fd = listen_fd, .events = POLLIN};
        int ready = poll(&pfd, 1, 500);
        if (ready <= 0)
            continue;

        struct sockaddr_in peer;
        socklen_t peer_len = sizeof peer;
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (fd < 0)
            continue;

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);

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
        pthread_detach(tid);
    }

    log_msg("Shutdown requested - closing listening socket");
    close(listen_fd);

    pthread_mutex_lock(&clients_lock);
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].in_use)
            shutdown(clients[i].fd, SHUT_RDWR);
    while (active_clients > 0)
        pthread_cond_wait(&clients_cv, &clients_lock);
    pthread_mutex_unlock(&clients_lock);

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