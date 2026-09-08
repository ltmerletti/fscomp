#ifndef FSCOMP_QUEUE_H
#define FSCOMP_QUEUE_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  void **items;
  size_t capacity;
  size_t head_index;
  size_t tail_index;
  size_t current_count;
  bool is_shutdown;

  pthread_mutex_t lock;
  pthread_cond_t not_empty_condition;
  pthread_cond_t not_full_condition;
} task_queue_t;

/**
 * Allocates internal ring buffer memory and initializes queue mutexes.
 */
bool queue_initialize(task_queue_t *queue, size_t capacity);

/**
 * Shuts down the queue and frees ring buffer memory and synchronization primitives.
 */
void queue_destroy(task_queue_t *queue);

/**
 * Appends a task item to the back of the queue, blocking if the queue is full.
 */
bool queue_push(task_queue_t *queue, void *task_item);

/**
 * Appends a task item without waiting, returning false if the queue is full or shut down.
 */
bool queue_try_push(task_queue_t *queue, void *task_item);

/**
 * Removes a task item from the front of the queue, blocking until an item is ready.
 */
bool queue_pop(task_queue_t *queue, void **output_task_item);

/**
 * Signals all waiting threads that the queue has shut down and wakes them up.
 */
void queue_signal_shutdown(task_queue_t *queue);

/**
 * Returns the number of task items currently waiting in the queue.
 */
size_t queue_get_count(task_queue_t *queue);

#endif /* FSCOMP_QUEUE_H */
