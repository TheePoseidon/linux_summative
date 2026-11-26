/*
 * library_reservation.c - Library book reservation system using POSIX
 *                          threads, a mutex and condition variables.
 *
 * Build:  gcc -Wall -Wextra -pthread -o library_reservation library_reservation.c
 * Run:    ./library_reservation
 *
 * Threads
 *   Student (producer)  : creates 15 reservations, one per second, and puts
 *                         them in a shared queue of capacity 5.
 *   Staff   (consumer)  : takes reservations from the queue and spends
 *                         3 seconds on each. Even book IDs are approved,
 *                         odd book IDs are rejected.
 *   Monitor             : every 4 seconds prints the system statistics.
 *   main                : creates the threads, waits for them to finish,
 *                         prints a final summary and destroys the
 *                         synchronisation objects.
 *
 * ---------------------------------------------------------------------------
 * HOW SHARED MEMORY IS PROTECTED
 *   All shared data (the queue and every counter) lives in one struct,
 *   `lib`, and is guarded by ONE mutex, lib.lock. Rule: no thread reads or
 *   writes any field of `lib` (except the synchronisation objects
 *   themselves) unless it holds lib.lock. Using a single lock for the queue
 *   and the counters means they can never be seen in an inconsistent state,
 *   e.g. a request counted as submitted but not yet in the queue.
 *
 * HOW LOCKING IS APPLIED
 *   Locks are held only for short critical sections: changing the queue,
 *   updating counters, or copying statistics. The slow parts - the 1 s
 *   submission delay, the 3 s processing delay and printing the monitor
 *   report - happen with the mutex UNLOCKED, so the threads really run
 *   in parallel and never block each other for long.
 *
 * HOW CONDITION VARIABLES COORDINATE THE THREADS
 *   not_full   Student waits here while the queue holds 5 requests.
 *              Staff signals it after removing a request.
 *   not_empty  Staff waits here while the queue is empty.
 *              Student signals it after adding a request.
 *   finished   Monitor sleeps on it with a 4-second timeout. Staff
 *              broadcasts it when the last reservation is processed, so
 *              the monitor wakes immediately and exits instead of waiting
 *              for its next 4-second tick.
 *   pthread_cond_wait() atomically releases the mutex while sleeping and
 *   re-acquires it before returning. Every wait is inside a while loop that
 *   re-checks the condition, which handles spurious wake-ups and guarantees
 *   the condition really holds before the thread continues.
 *
 * FULL AND EMPTY QUEUE
 *   Full : while (count == CAPACITY) the student waits on not_full, so the
 *          queue can never exceed 5 requests.
 *   Empty: while (count == 0) the staff thread waits on not_empty, so it
 *          never removes from an empty queue.
 *
 * APPROVAL / REJECTION
 *   Simulation rule: an even book ID means the book is available
 *   (APPROVED); an odd book ID means it is unavailable (REJECTED).
 *
 * MONITOR SAFETY
 *   The monitor locks the mutex only long enough to copy the counters and
 *   queue size into a local snapshot, unlocks, then prints the snapshot.
 *   The numbers in each report are therefore consistent with each other,
 *   and printing never delays the student or staff threads.
 *
 * SAFE TERMINATION
 *   The run is limited to TOTAL_RESERVATIONS (15). The student thread stops
 *   after submitting 15; the staff thread stops after processing 15 and
 *   then sets lib.all_done and broadcasts `finished`; the monitor sees
 *   all_done and stops. Each thread ends with pthread_exit(). main uses
 *   pthread_join() to wait for all three, so no thread is cancelled while
 *   holding the lock, then destroys the mutex and condition variables.
 */

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
#define FIRST_ID            1001        /* first reservation ID */

/* Stop the program with a clear message if a pthread call fails.
 * pthread functions return an error number instead of setting errno. */
#define CHECK(call)                                                       \
    do {                                                                  \
        int rc_ = (call);                                                 \
        if (rc_ != 0) {                                                   \
            fprintf(stderr, "%s failed: %s\n", #call, strerror(rc_));     \
            exit(EXIT_FAILURE);                                           \
        }                                                                 \
    } while (0)

/* ------------------------------------------------------------------------
 * Data types
 * --------------------------------------------------------------------- */

typedef struct {
    int id;                             /* unique, incremental */
    int book_id;
} Reservation;

/* Circular buffer: `head` is the next slot to read, `tail` the next slot
 * to write. Both wrap around with % QUEUE_CAPACITY, so the fixed array is
 * reused without shifting elements. `count` tells full from empty. */
typedef struct {
    Reservation items[QUEUE_CAPACITY];
    int head;
    int tail;
    int count;
} ReservationQueue;

/* All shared state, protected by `lock`. */
typedef struct {
    ReservationQueue queue;

    int submitted;                      /* statistics */
    int processed;
    int approved;
    int rejected;
    int all_done;                       /* set when everything is processed */

    pthread_mutex_t lock;
    pthread_cond_t  not_full;
    pthread_cond_t  not_empty;
    pthread_cond_t  finished;
} LibrarySystem;

static LibrarySystem lib;
static struct timespec start_time;      /* for elapsed-time stamps */

/* ------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

/* Seconds since the program started, used to timestamp every message. */
static double elapsed(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)(now.tv_sec - start_time.tv_sec) +
           (double)(now.tv_nsec - start_time.tv_nsec) / 1e9;
}

/* Print one message with a "[ 12.0s] " time prefix. The whole line is
 * formatted first and then written with a single fputs call, so lines from
 * different threads never mix (stdio locks each call internally). */
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

/* Queue operations. The CALLER MUST HOLD lib.lock and must already have
 * checked that the queue is not full (push) or not empty (pop). */
static void queue_push(ReservationQueue *q, Reservation r)
{
    q->items[q->tail] = r;
    q->tail = (q->tail + 1) % QUEUE_CAPACITY;   /* wrap around */
    q->count++;
}

static Reservation queue_pop(ReservationQueue *q)
{
    Reservation r = q->items[q->head];
    q->head = (q->head + 1) % QUEUE_CAPACITY;   /* wrap around */
    q->count--;
    return r;
}

/* Availability rule for the simulation. */
static int book_is_available(int book_id)
{
    return book_id % 2 == 0;                    /* even = available */
}

/* ========================================================================
 * Student thread (producer)
 * ===================================================================== */
static void *student_thread(void *arg)
{
    unsigned int seed = *(unsigned int *)arg;   /* private RNG state */

    for (int i = 0; i < TOTAL_RESERVATIONS; i++) {
        /* Build the request. The ID counter is only used by this thread,
         * so it needs no lock. Book IDs are random in 100..199. */
        Reservation r;
        r.id = FIRST_ID + i;
        r.book_id = 100 + (int)(rand_r(&seed) % 100);

        sleep(SUBMIT_SECONDS);                  /* 1 s to submit, unlocked */

        CHECK(pthread_mutex_lock(&lib.lock));   /* ---- critical section */

        /* FULL QUEUE: wait until the staff thread frees a slot.
         * pthread_cond_wait releases the lock while waiting, so the staff
         * thread can take the lock and remove an item. */
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

        queue_push(&lib.queue, r);              /* guaranteed not full */
        lib.submitted++;
        LOG("STUDENT : submitted reservation #%d for book %d   "
            "| queue %d/%d", r.id, r.book_id,
            lib.queue.count, QUEUE_CAPACITY);

        /* Wake the staff thread if it is waiting on an empty queue. */
        CHECK(pthread_cond_signal(&lib.not_empty));

        CHECK(pthread_mutex_unlock(&lib.lock)); /* ---- end critical section */
    }

    CHECK(pthread_mutex_lock(&lib.lock));
    LOG("STUDENT : all %d reservations submitted - student thread exiting",
        TOTAL_RESERVATIONS);
    CHECK(pthread_mutex_unlock(&lib.lock));
    pthread_exit(NULL);
}

/* ========================================================================
 * Library staff thread (consumer)
 * ===================================================================== */
static void *staff_thread(void *arg)
{
    (void)arg;

    for (int done = 0; done < TOTAL_RESERVATIONS; done++) {
        CHECK(pthread_mutex_lock(&lib.lock));   /* ---- critical section */

        /* EMPTY QUEUE: wait until the student adds a request. */
        if (lib.queue.count == 0)
            LOG("STAFF   : queue EMPTY - waiting for reservations");
        while (lib.queue.count == 0)
            CHECK(pthread_cond_wait(&lib.not_empty, &lib.lock));

        Reservation r = queue_pop(&lib.queue);  /* guaranteed not empty */
        LOG("STAFF   : processing reservation #%d for book %d "
            "| queue %d/%d", r.id, r.book_id,
            lib.queue.count, QUEUE_CAPACITY);

        /* A slot is free: wake the student if it is waiting on a full
         * queue. */
        CHECK(pthread_cond_signal(&lib.not_full));

        CHECK(pthread_mutex_unlock(&lib.lock)); /* ---- end critical section */

        sleep(PROCESS_SECONDS);                 /* 3 s to process, unlocked */

        /* APPROVAL / REJECTION: even book ID = available. */
        int approved = book_is_available(r.book_id);

        CHECK(pthread_mutex_lock(&lib.lock));   /* update statistics */
        lib.processed++;
        if (approved)
            lib.approved++;
        else
            lib.rejected++;
        LOG("STAFF   : reservation #%d %s (book %d is %s)", r.id,
            approved ? "APPROVED" : "REJECTED", r.book_id,
            approved ? "available" : "unavailable");

        /* Last reservation: tell the monitor to stop now. */
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

/* ========================================================================
 * Monitoring thread
 * ===================================================================== */
static void *monitor_thread(void *arg)
{
    (void)arg;
    struct timespec next_report;

    /* Reports are due at start + 4 s, 8 s, 12 s ... Using absolute times
     * (rather than "sleep 4 s after each report") prevents drift. */
    next_report = start_time;
    next_report.tv_sec += MONITOR_SECONDS;

    CHECK(pthread_mutex_lock(&lib.lock));
    while (!lib.all_done) {
        /* Sleep until the next report time or until `finished` is
         * broadcast. The lock is released while sleeping, so the other
         * threads are never blocked by the monitor. */
        int rc = pthread_cond_timedwait(&lib.finished, &lib.lock,
                                        &next_report);
        if (lib.all_done)
            break;                          /* woken for shutdown */
        if (rc == EINTR || rc == 0)
            continue;                       /* spurious wake-up */
        if (rc != ETIMEDOUT) {
            fprintf(stderr, "pthread_cond_timedwait: %s\n", strerror(rc));
            exit(EXIT_FAILURE);
        }

        /* Report time. Copy the statistics while holding the lock so the
         * numbers are consistent with each other ... */
        int submitted = lib.submitted;
        int processed = lib.processed;
        int approved  = lib.approved;
        int rejected  = lib.rejected;
        int queued    = lib.queue.count;
        next_report.tv_sec += MONITOR_SECONDS;

        /* ... then release it before printing. */
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

/* ========================================================================
 * main - set up, run the threads, wait for them, clean up
 * ===================================================================== */
int main(void)
{
    pthread_t student, staff, monitor;
    pthread_condattr_t cond_attr;
    unsigned int seed = (unsigned int)time(NULL);

    setvbuf(stdout, NULL, _IOLBF, 0);       /* print each line at once */
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    /* Initialise shared state and synchronisation objects. The `finished`
     * condition uses CLOCK_MONOTONIC for its timeout so that changes to the
     * system clock cannot shorten or lengthen the monitor's 4 s period. */
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

    /* Thread creation */
    CHECK(pthread_create(&staff, NULL, staff_thread, NULL));
    CHECK(pthread_create(&student, NULL, student_thread, &seed));
    CHECK(pthread_create(&monitor, NULL, monitor_thread, NULL));

    /* Wait for every thread to finish (graceful termination: no thread is
     * cancelled, each one leaves its loop and calls pthread_exit). */
    CHECK(pthread_join(student, NULL));
    CHECK(pthread_join(staff, NULL));
    CHECK(pthread_join(monitor, NULL));

    /* All threads have ended, so the shared data can be read without the
     * lock. */
    printf("\n[%5.1fs] FINAL SUMMARY\n"
           "          Total submitted : %d\n"
           "          Total processed : %d\n"
           "          Approved        : %d\n"
           "          Rejected        : %d\n"
           "          Queue size      : %d/%d\n",
           elapsed(), lib.submitted, lib.processed, lib.approved,
           lib.rejected, lib.queue.count, QUEUE_CAPACITY);

    /* Destroy synchronisation objects (only safe once no thread uses
     * them). */
    CHECK(pthread_cond_destroy(&lib.finished));
    CHECK(pthread_cond_destroy(&lib.not_empty));
    CHECK(pthread_cond_destroy(&lib.not_full));
    CHECK(pthread_mutex_destroy(&lib.lock));

    printf("All threads joined; mutex and condition variables destroyed. "
           "Program terminated successfully.\n");
    return EXIT_SUCCESS;
}