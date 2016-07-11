#include <stdio.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>

#include "ringbuffer.h"

#ifdef __MACH__
#include <mach/clock.h>
#include <mach/mach.h>
#include <mach/kern_return.h>
#endif



/*
 * As the mutex lock is stored in global (static) memory it can be 
 * initialized with PTHREAD_MUTEX_INITIALIZER.
 * If we had allocated space for the mutex on the heap, 
 * then we would have used pthread_mutex_init(ptr, NULL)
 */
static pthread_mutex_t      ring_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ringbuff_body *ring;

static int                  body_idx;
static struct ringbuff_body bodies[NUM_OF_BUFFER];

/*
 * when there is a ring to write.
 */
sem_t               *sem_ring_avail;
sem_t               *sem_ring_notify;

static FILE         *log_file;


static int _ring_buffer_dequeue(void)
{
    int rval = -1;

    pthread_mutex_lock(&ring_lock);

    if (ring) {
        for (int i = 0; i < ring->writer_idx; i++)
        {
            struct ringbuff_cell *cell = &ring->cell[i];

            fprintf(log_file,"%ld.%9lds %d\n", (long)cell->timestamp.tv_sec 
                                             , cell->timestamp.tv_nsec
                                             , cell->curr_heap_size);
        }
        memset(ring, 0, sizeof(*ring));
        rval = 0;
    }

    pthread_mutex_unlock(&ring_lock);

    return rval;
}

void ring_buffer_enqueue(void *value)
{
    static int reentrant = 0;
    struct ringbuff_body *r;

    assert(reentrant == 0);
    reentrant++;

    r = &bodies[body_idx];

    if (value != NULL)
        r->cell[r->writer_idx++] = *(struct ringbuff_cell *)value;

    if (r->writer_idx == NUM_OF_CELL || value == NULL) {

        if (r->writer_idx > 0) {
            // wait until next ring is released
            sem_wait(sem_ring_avail);

            pthread_mutex_lock(&ring_lock);
            ring     = r;
            body_idx = (body_idx + 1) % NUM_OF_BUFFER;
            pthread_mutex_unlock(&ring_lock);

        } else {
            pthread_mutex_lock(&ring_lock);
            ring = NULL;
            pthread_mutex_unlock(&ring_lock);
        }

        // notify consumer that a ring has been prepared
        sem_post(sem_ring_notify);
    }

    reentrant--;
}

void *ring_buffer_writer(void *ptr)
{
    int i = 0;
    int count = *(int *)ptr;
    struct ringbuff_cell temp;
    for (i = 0; i < count; ++i)
    {
        clock_get_monotonic_time(&temp.timestamp);
        temp.curr_heap_size = i;
        ring_buffer_enqueue(&temp);
    }
    return NULL;
}

void *ring_buffer_reader(void *ptr)
{
    // post 2 available rings
    sem_post(sem_ring_avail);
    sem_post(sem_ring_avail);

    while (1)
    {
        if (sem_wait(sem_ring_notify) < 0) {
            assert(errno == 0);
        }
        if (_ring_buffer_dequeue() < 0)
            break;
        sem_post(sem_ring_avail);
    }

    fflush(log_file);
    fclose(log_file);
    return NULL;
}

void ring_buffer_init(void)
{
    sem_unlink("ring_available");
    sem_ring_avail = sem_open("ring_available", O_CREAT, 0700, 0);

    if (sem_ring_avail == SEM_FAILED) {
        printf("%s\n", strerror(errno));
        exit(-1);
    }

    sem_unlink("ring_notify");
    sem_ring_notify = sem_open("ring_notify", O_CREAT, 0700, 0);

    if (sem_ring_notify == SEM_FAILED) {
        printf("%s\n", strerror(errno));
        exit(-1);
    }

    log_file = fopen(FILE_NAME,"w");
    setvbuf(log_file, NULL, _IONBF, 0);
}

void ring_buffer_done(void)
{
    ring_buffer_enqueue(NULL);
    ring_buffer_enqueue(NULL);
}

