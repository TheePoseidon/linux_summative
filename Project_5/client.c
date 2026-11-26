#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <netdb.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT "8080"
#define MAX_LINE     512
#define MAX_REPLY    32

typedef struct {
    int    fd;
    char   buf[2048];
    size_t len;
} Connection;

typedef struct {
    char lines[MAX_REPLY][MAX_LINE];
    int  count;
    int  ok;
    char code[32];
    const char *text;
} Reply;

static volatile sig_atomic_t interrupted;
static int echo_input;

static void on_sigint(int sig)
{
    (void)sig;
    interrupted = 1;
}

static int connect_to_server(const char *host, const char *port)
{
    struct addrinfo hints = {0}, *res, *ai;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host, port, &hints, &res);
    if (rc != 0) {
        fprintf(stderr, "Cannot resolve %s: %s\n", host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    for (ai = res; ai != NULL; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

static int send_command(Connection *c, const char *command, const char *arg)
{
    char line[MAX_LINE];
    int len = arg ? snprintf(line, sizeof line, "%s %s", command, arg)
                  : snprintf(line, sizeof line, "%s", command);
    if (len < 0 || len >= (int)sizeof line - 1)
        return -1;
    line[len++] = '\n';

    const char *p = line;
    while (len > 0) {
        ssize_t n = send(c->fd, p, (size_t)len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (int)n;
    }
    return 0;
}

static int read_line(Connection *c, char *out, size_t size)
{
    for (;;) {
        char *nl = memchr(c->buf, '\n', c->len);
        if (nl != NULL) {
            size_t n = (size_t)(nl - c->buf);
            size_t copy = n < size - 1 ? n : size - 1;
            memcpy(out, c->buf, copy);
            out[copy] = '\0';
            if (copy > 0 && out[copy - 1] == '\r')
                out[copy - 1] = '\0';
            memmove(c->buf, nl + 1, c->len - n - 1);
            c->len -= n + 1;
            return 1;
        }
        if (c->len == sizeof c->buf)
            c->len = 0;
        ssize_t got = recv(c->fd, c->buf + c->len, sizeof c->buf - c->len, 0);
        if (got == 0)
            return 0;
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        c->len += (size_t)got;
    }
}

static int read_reply(Connection *c, Reply *r)
{
    r->count = 0;
    for (;;) {
        char line[MAX_LINE];
        if (read_line(c, line, sizeof line) != 1)
            return -1;
        if (r->count < MAX_REPLY)
            snprintf(r->lines[r->count++], MAX_LINE, "%s", line);

        int ok = strncmp(line, "OK ", 3) == 0;
        if (ok || strncmp(line, "ERR ", 4) == 0) {
            char *status = r->lines[r->count - 1];
            char *code = status + (ok ? 3 : 4);
            char *space = strchr(code, ' ');
            size_t code_len = space ? (size_t)(space - code) : strlen(code);
            if (code_len >= sizeof r->code)
                code_len = sizeof r->code - 1;
            memcpy(r->code, code, code_len);
            r->code[code_len] = '\0';
            r->text = space ? space + 1 : "";
            r->ok = ok;
            return 0;
        }
    }
}

static int request(Connection *c, Reply *r, const char *command,
                   const char *arg)
{
    if (send_command(c, command, arg) < 0)
        return -1;
    return read_reply(c, r);
}

static int prompt(const char *text, char *out, size_t size)
{
    printf("%s", text);
    fflush(stdout);
    if (interrupted || fgets(out, (int)size, stdin) == NULL || interrupted) {
        printf("\n");
        return -1;
    }
    out[strcspn(out, "\r\n")] = '\0';
    char *start = out;
    while (*start == ' ' || *start == '\t')
        start++;
    memmove(out, start, strlen(start) + 1);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\t'))
        out[--n] = '\0';
    if (echo_input)
        printf("%s\n", out);
    return 0;
}

static void show_events(const Reply *r)
{
    printf("\n  %-4s %-24s %8s %11s %10s\n", "ID", "Event", "Capacity",
           "Registered", "Available");
    printf("  %-4s %-24s %8s %11s %10s\n", "----",
           "------------------------", "--------", "----------",
           "---------");
    for (int i = 0; i < r->count; i++) {
        if (strncmp(r->lines[i], "EVENT|", 6) != 0)
            continue;
        char copy[MAX_LINE], *save = NULL;
        snprintf(copy, sizeof copy, "%s", r->lines[i] + 6);
        char *id   = strtok_r(copy, "|", &save);
        char *name = strtok_r(NULL, "|", &save);
        char *cap  = strtok_r(NULL, "|", &save);
        char *reg  = strtok_r(NULL, "|", &save);
        char *left = strtok_r(NULL, "|", &save);
        if (!id || !name || !cap || !reg || !left)
            continue;
        printf("  %-4s %-24s %8s %11s %10s\n", id, name, cap, reg,
               atoi(left) > 0 ? left : "FULL");
    }
    printf("\n");
}

static void show_registration_result(const Reply *r, const char *event_id)
{
    if (r->ok)
        printf("  [SUCCESS] Registration confirmed: %s\n", r->text);
    else if (strcmp(r->code, "DUPLICATE") == 0)
        printf("  [REJECTED] Duplicate registration: %s\n", r->text);
    else if (strcmp(r->code, "EVENT_FULL") == 0)
        printf("  [REJECTED] Event full - no places left: %s\n", r->text);
    else if (strcmp(r->code, "NO_SUCH_EVENT") == 0)
        printf("  [REJECTED] Event %s is not available: %s\n", event_id,
               r->text);
    else
        printf("  [ERROR] %s\n", r->text);
}

static int authenticate(Connection *c, char *student_id, size_t size)
{
    Reply r;
    for (;;) {
        char id[64];
        if (prompt("Enter your student ID (or 'q' to quit): ", id,
                   sizeof id) < 0 || strcmp(id, "q") == 0)
            return 0;
        if (id[0] == '\0' || strchr(id, ' ') != NULL) {
            printf("  Please enter a single ID such as S1001.\n");
            continue;
        }
        if (request(c, &r, "LOGIN", id) < 0)
            return -1;

        if (r.ok) {
            snprintf(student_id, size, "%s", id);
            printf("\n  [SUCCESS] Authentication successful.");
            for (int i = 0; i < r.count; i++)
                if (strncmp(r.lines[i], "INFO ", 5) == 0)
                    printf(" %s", r.lines[i] + 5);
            printf("\n\nAvailable events:");
            show_events(&r);
            return 1;
        }
        if (strcmp(r.code, "TOO_MANY_ATTEMPTS") == 0) {
            printf("  [FAILED] Authentication failed: %s\n", r.text);
            return -1;
        }
        if (strcmp(r.code, "IN_USE") == 0)
            printf("  [FAILED] %s\n", r.text);
        else
            printf("  [FAILED] Authentication failed: %s\n", r.text);
    }
}

static int register_for_event(Connection *c)
{
    Reply r;
    char input[64];

    if (request(c, &r, "LIST", NULL) < 0)
        return -1;
    show_events(&r);

    if (prompt("Enter the event ID to register for (0 to cancel): ",
               input, sizeof input) < 0)
        return 1;
    if (strcmp(input, "0") == 0) {
        printf("  Registration cancelled.\n\n");
        return 0;
    }
    if (input[0] == '\0' || strspn(input, "0123456789") != strlen(input)) {
        printf("  [ERROR] '%s' is not a valid event ID (use a number such "
               "as 101).\n\n", input);
        return 0;
    }
    if (request(c, &r, "REGISTER", input) < 0)
        return -1;
    show_registration_result(&r, input);
    printf("\n");
    return 0;
}

static int show_my_events(Connection *c)
{
    Reply r;
    if (request(c, &r, "MYEVENTS", NULL) < 0)
        return -1;
    int n = 0;
    printf("\n  Your registrations:\n");
    for (int i = 0; i < r.count; i++)
        if (strncmp(r.lines[i], "MYEVENT|", 8) == 0) {
            char *name = strchr(r.lines[i] + 8, '|');
            if (name != NULL) {
                *name++ = '\0';
                printf("    %s  %s\n", r.lines[i] + 8, name);
                n++;
            }
        }
    if (n == 0)
        printf("    (none yet)\n");
    printf("\n");
    return 0;
}

static int menu(Connection *c)
{
    Reply r;
    for (;;) {
        char choice[16];
        printf("---------------- MENU ----------------\n"
               "  1. View events\n"
               "  2. Register for an event\n"
               "  3. View my registrations\n"
               "  4. Exit\n");
        if (prompt("Choose an option (1-4): ", choice, sizeof choice) < 0)
            return 0;

        int rc = 0;
        if (strcmp(choice, "1") == 0) {
            rc = request(c, &r, "LIST", NULL);
            if (rc == 0)
                show_events(&r);
        } else if (strcmp(choice, "2") == 0) {
            rc = register_for_event(c);
            if (rc == 1)
                return 0;
        } else if (strcmp(choice, "3") == 0) {
            rc = show_my_events(c);
        } else if (strcmp(choice, "4") == 0) {
            return 0;
        } else {
            printf("  Please enter a number from 1 to 4.\n\n");
        }
        if (rc < 0)
            return -1;
    }
}

int main(int argc, char *argv[])
{
    const char *host = argc > 1 ? argv[1] : DEFAULT_HOST;
    const char *port = argc > 2 ? argv[2] : DEFAULT_PORT;
    char student_id[64] = "";
    Connection conn = {0};
    Reply r;
    int status = EXIT_SUCCESS;

    setvbuf(stdout, NULL, _IOLBF, 0);
    echo_input = !isatty(STDIN_FILENO);
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    printf("Connecting to %s:%s ...\n", host, port);
    conn.fd = connect_to_server(host, port);
    if (conn.fd < 0) {
        fprintf(stderr, "Could not connect to the registration server at "
                        "%s:%s (%s).\n", host, port, strerror(errno));
        return EXIT_FAILURE;
    }

    if (read_reply(&conn, &r) < 0 || !r.ok) {
        printf("Server refused the connection: %s\n",
               r.count > 0 ? r.text : "connection closed");
        close(conn.fd);
        return EXIT_FAILURE;
    }
    printf("Connected. %s\n\n", r.text);

    int auth = authenticate(&conn, student_id, sizeof student_id);

    int result = auth == 1 ? menu(&conn) : auth;

    if (result < 0) {
        printf("The server closed the connection.\n");
        status = EXIT_FAILURE;
    } else {
        if (request(&conn, &r, "QUIT", NULL) == 0 && r.ok)
            printf("Server: %s\n", r.text);
    }

    shutdown(conn.fd, SHUT_RDWR);
    close(conn.fd);
    if (student_id[0] != '\0')
        printf("Registration session closed. Goodbye, %s.\n", student_id);
    else
        printf("Registration session closed. Goodbye.\n");
    return status;
}