# Project 4: Developing a Multithreaded Library Book Reservation System in C

`library_reservation.c` simulates a library where a student thread submits 15 reservations into a shared queue of capacity 5, a staff thread processes them, and a monitor thread reports statistics every 4 seconds.

## Environment, Build and Run

GCC 15.2.0, glibc 2.43 (POSIX threads), Ubuntu 26.04 LTS (x86-64).

```
gcc -Wall -Wextra -pthread -o library_reservation library_reservation.c
./library_reservation
```

## Design

| Thread | Function | Behaviour |
|---|---|---|
| Student (producer) | `student_thread` | Creates IDs 1001 to 1015 and random book IDs 100 to 199, takes 1 s per submission, waits on `not_full` when the queue holds 5 |
| Staff (consumer) | `staff_thread` | Takes a request, waits on `not_empty` when the queue is empty, takes 3 s per request; even book ID is APPROVED, odd is REJECTED |
| Monitor | `monitor_thread` | Uses `pthread_cond_timedwait` on `finished` to wake every 4 s and print submitted, processed, approved, rejected and queue size |

- **Queue:** a circular buffer (`head`, `tail`, `count`, wrapped with `% QUEUE_CAPACITY`).
- **Shared data:** the queue and all counters are in one struct, `lib`, protected by one `pthread_mutex_t lock`.
  - Locks are held only while the queue or the counters change.
  - The 1 s and 3 s delays run without the lock.
- **Condition variables:**
  - `not_full`: the student waits here and the staff thread signals it.
  - `not_empty`: the staff thread waits here and the student signals it.
  - `finished`: the staff thread broadcasts it after the last reservation.
  - Every wait is inside a `while` loop, so a spurious wake-up is handled.
- **Monitor safety:** the monitor copies the counters while holding the lock, unlocks, then prints, so it never delays the other threads.
- **Termination:**
  - The student thread stops after 15 submissions, and the staff thread stops after 15 have been processed.
  - The staff thread then sets `all_done` and broadcasts `finished`, so the monitor exits.
  - Each thread calls `pthread_exit()`, and `main` uses `pthread_join()` for all three, then destroys the mutex and the 3 condition variables.
- **Logging:** each line gets a timestamp and is written with a single `fputs`, so lines from different threads never mix.

## Sample Execution Output (from `output.txt`)

Reservations being generated and processed:
```
[  0.0s] STAFF   : queue EMPTY - waiting for reservations
[  1.0s] STUDENT : submitted reservation #1001 for book 175   | queue 1/5
[  1.0s] STAFF   : processing reservation #1001 for book 175 | queue 0/5
[  4.0s] STAFF   : reservation #1001 REJECTED (book 175 is unavailable)
[  7.0s] STAFF   : reservation #1002 APPROVED (book 162 is available)
```

Queue full, so the student waits:
```
[  8.0s] STUDENT : submitted reservation #1008 for book 127   | queue 5/5
[  9.0s] STUDENT : queue FULL (5/5) - reservation #1009 waiting
[ 10.0s] STAFF   : reservation #1003 APPROVED (book 198 is available)
[ 10.0s] STAFF   : processing reservation #1004 for book 175 | queue 4/5
[ 10.0s] STUDENT : space available - resuming with reservation #1009
[ 10.0s] STUDENT : submitted reservation #1009 for book 177   | queue 5/5
```

Monitoring report:
```
[ 12.0s] MONITOR : ---------- system report ----------
                    Total submitted   : 9
                    Total processed   : 3
                    Approved          : 2
                    Rejected          : 1
                    Queue size        : 5/5
                    -----------------------------------
```

Termination:
```
[ 46.0s] STAFF   : reservation #1015 APPROVED (book 164 is available)
[ 46.0s] STAFF   : all 15 reservations processed - staff thread exiting
[ 46.0s] MONITOR : all reservations processed - monitor thread exiting

[ 46.0s] FINAL SUMMARY
          Total submitted : 15
          Total processed : 15
          Approved        : 9
          Rejected        : 6
          Queue size      : 0/5
All threads joined; mutex and condition variables destroyed. Program terminated successfully.
```

The queue never goes above 5/5. Book IDs are random, so the approved and rejected counts change from run to run.
