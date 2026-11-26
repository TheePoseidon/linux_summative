#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define QUEUE_CAPACITY      5
#define TOTAL_RESERVATIONS  15
#define SUBMIT_SECONDS      1
#define PROCESS_SECONDS     3
#define MONITOR_SECONDS     4
#define FIRST_ID            1001

#define CHECK(call)                                                       \
    do {                                                                  \
        int rc_ = (call);                                                 \
        if (rc_ != 0) {                                                   \
            fprintf(stderr, "%s failed: %s\n", #call, strerror(rc_));     \
            exit(EXIT_FAILURE);                                           \
        }                                                                 \
    } while (0)

typedef struct {
    int id;
    int book_id;
} Reservation;

typedef struct {
    Reservation items[QUEUE_CAPACITY];
    int head;
    int tail;
    int count;
} ReservationQueue;

typedef struct {
    ReservationQueue queue;

    int submitted;
    int processed;
    int approved;
    int rejected;
    int all_done;

    pthread_mutex_t lock;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
    pthread_cond_t  finished;
} LibrarySystem;

static LibrarySystem lib;
static struct timespec start_time;

static double elapsed(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start_time.tv_sec) +
           (double)(now.tv_nsec - start_time.tv_nsec) / 1e9;
}

static void LOG(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void LOG(const char *fmt, ...)
{
    char line[256];
    int len = snprintf(line, sizeof line, "[%5.1fs] ", elapsed());
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + len, sizeof line - (size_t)len, fmt, ap);
    va_end(ap);
    strncat(line, "\n", sizeof line - strlen(line) - 1);
    fputs(line, stdout);
}

static void queue_push(ReservationQueue *q, Reservation r)
{
    q->items[q->tail] = r;
    q->tail = (q->tail + 1) % QUEUE_CAPACITY;
    q->count++;
}

static Reservation queue_pop(ReservationQueue *q)
{
    Reservation r = q->items[q->head];
    q->head = (q->head + 1) % QUEUE_CAPACITY;
    q->count--;
    return r;
}

static int book_is_available(int book_id)
{
    return book_id % 2 == 0;
}

static void *student_thread(void *arg)
{
    unsigned int seed = *(unsigned int *)arg;

    for (int i = 0; i < TOTAL_RESERVATIONS; i++) {
        Reservation r;
        r.id = FIRST_ID + i;
        r.book_id = 100 + (int)(rand_r(&seed) % 100);

        sleep(SUBMIT_SECONDS);

        CHECK(pthread_mutex_lock(&lib.lock));

        if (lib.queue.count == QUEUE_CAPACITY)
            LOG("STUDENT : queue FULL (%d/%d) - reservation #%d waiting",
                lib.queue.count, QUEUE_CAPACITY, r.id);
        int waited = 0;
        while (lib.queue.count == QUEUE_CAPACITY) {
            CHECK(pthread_cond_wait(&lib.not_full, &lib.lock));
            waited = 1;
        }
        if (waited)
            LOG("STUDENT : space available - resuming with reservation #%d",
                r.id);

        queue_push(&lib.queue, r);
        lib.submitted++;
        LOG("STUDENT : submitted reservation #%d for book %d   "
            "| queue %d/%d", r.id, r.book_id,
            lib.queue.count, QUEUE_CAPACITY);

        CHECK(pthread_cond_signal(&lib.not_empty));

        CHECK(pthread_mutex_unlock(&lib.lock));
    }

    CHECK(pthread_mutex_lock(&lib.lock));
    LOG("STUDENT : all %d reservations submitted - student thread exiting",
        TOTAL_RESERVATIONS);
    CHECK(pthread_mutex_unlock(&lib.lock));
    pthread_exit(NULL);
}

static void *staff_thread(void *arg)
{
    (void)arg;

    for (int done = 0; done < TOTAL_RESERVATIONS; done++) {
        CHECK(pthread_mutex_lock(&lib.lock));

        if (lib.queue.count == 0)
            LOG("STAFF   : queue EMPTY - waiting for reservations");
        while (lib.queue.count == 0)
            CHECK(pthread_cond_wait(&lib.not_empty, &lib.lock));

        Reservation r = queue_pop(&lib.queue);
        LOG("STAFF   : processing reservation #%d for book %d "
            "| queue %d/%d", r.id, r.book_id,
            lib.queue.count, QUEUE_CAPACITY);

        CHECK(pthread_cond_signal(&lib.not_full));

        CHECK(pthread_mutex_unlock(&lib.lock));

        sleep(PROCESS_SECONDS);

        int approved = book_is_available(r.book_id);

        CHECK(pthread_mutex_lock(&lib.lock));
        lib.processed++;
        if (approved)
            lib.approved++;
        else
            lib.rejected++;
        LOG("STAFF   : reservation #%d %s (book %d is %s)", r.id,
            approved ? "APPROVED" : "REJECTED", r.book_id,
            approved ? "available" : "unavailable");

        if (lib.processed == TOTAL_RESERVATIONS) {
            lib.all_done = 1;
            CHECK(pthread_cond_broadcast(&lib.finished));
        }
        CHECK(pthread_mutex_unlock(&lib.lock));
    }

    CHECK(pthread_mutex_lock(&lib.lock));
    LOG("STAFF   : all %d reservations processed - staff thread exiting",
        TOTAL_RESERVATIONS);
    CHECK(pthread_mutex_unlock(&lib.lock));
    pthread_exit(NULL);
}

static void *monitor_thread(void *arg)
{
    (void)arg;
    struct timespec next_report;

    next_report = start_time;
    next_report.tv_sec += MONITOR_SECONDS;

    CHECK(pthread_mutex_lock(&lib.lock));
    while (!lib.all_done) {
        int rc = pthread_cond_timedwait(&lib.finished, &lib.lock,
                                        &next_report);
        if (lib.all_done)
            break;
        if (rc == EINTR || rc == 0)
            continue;
        if (rc != ETIMEDOUT) {
            fprintf(stderr, "pthread_cond_timedwait: %s\n", strerror(rc));
            exit(EXIT_FAILURE);
        }

        int submitted = lib.submitted;
        int processed = lib.processed;
        int approved  = lib.approved;
        int rejected  = lib.rejected;
        int queued    = lib.queue.count;
        next_report.tv_sec += MONITOR_SECONDS;

        CHECK(pthread_mutex_unlock(&lib.lock));

        printf("[%5.1fs] MONITOR : ---------- system report ----------\n"
               "                    Total submitted   : %d\n"
               "                    Total processed   : %d\n"
               "                    Approved          : %d\n"
               "                    Rejected          : %d\n"
               "                    Queue size        : %d/%d\n"
               "                    -----------------------------------\n",
               elapsed(), submitted, processed, approved, rejected,
               queued, QUEUE_CAPACITY);

        CHECK(pthread_mutex_lock(&lib.lock));
    }
    LOG("MONITOR : all reservations processed - monitor thread exiting");
    CHECK(pthread_mutex_unlock(&lib.lock));
    pthread_exit(NULL);
}

int main(void)
{
    pthread_t student, staff, monitor;
    pthread_condattr_t cond_attr;
    unsigned int seed = (unsigned int)time(NULL);

    setvbuf(stdout, NULL, _IOLBF, 0);
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    memset(&lib, 0, sizeof lib);
    CHECK(pthread_mutex_init(&lib.lock, NULL));
    CHECK(pthread_cond_init(&lib.not_full, NULL));
    CHECK(pthread_cond_init(&lib.not_empty, NULL));
    CHECK(pthread_condattr_init(&cond_attr));
    CHECK(pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC));
    CHECK(pthread_cond_init(&lib.finished, &cond_attr));
    CHECK(pthread_condattr_destroy(&cond_attr));

    printf("Library reservation system: %d reservations, queue capacity %d\n"
           "Submit: %d s each | Process: %d s each | Monitor: every %d s\n"
           "Rule: even book ID = available (approve), "
           "odd book ID = unavailable (reject)\n\n",
           TOTAL_RESERVATIONS, QUEUE_CAPACITY, SUBMIT_SECONDS,
           PROCESS_SECONDS, MONITOR_SECONDS);

    CHECK(pthread_create(&staff, NULL, staff_thread, NULL));
    CHECK(pthread_create(&student, NULL, student_thread, &seed));
    CHECK(pthread_create(&monitor, NULL, monitor_thread, NULL));

    CHECK(pthread_join(student, NULL));
    CHECK(pthread_join(staff, NULL));
    CHECK(pthread_join(monitor, NULL));

    printf("\n[%5.1fs] FINAL SUMMARY\n"
           "          Total submitted : %d\n"
           "          Total processed : %d\n"
           "          Approved        : %d\n"
           "          Rejected        : %d\n"
           "          Queue size      : %d/%d\n",
           elapsed(), lib.submitted, lib.processed, lib.approved,
           lib.rejected, lib.queue.count, QUEUE_CAPACITY);

    CHECK(pthread_cond_destroy(&lib.finished));
    CHECK(pthread_cond_destroy(&lib.not_empty));
    CHECK(pthread_cond_destroy(&lib.not_full));
    CHECK(pthread_mutex_destroy(&lib.lock));

    printf("All threads joined; mutex and condition variables destroyed. "
           "Program terminated successfully.\n");
    return EXIT_SUCCESS;
}