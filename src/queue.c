#include "queue.h"

#include <stdlib.h>
#include <string.h>

/**
 * Allocates internal ring buffer memory and initializes queue mutexes.
 */
bool queue_initialize(task_queue_t *queue, size_t capacity) {
  if (!queue || capacity == 0) return false;

  memset(queue, 0, sizeof(*queue));
  queue->items = calloc(capacity, sizeof(*queue->items));
  if (!queue->items) return false;

  queue->capacity = capacity;
  queue->head_index = 0;
  queue->tail_index = 0;
  queue->current_count = 0;
  queue->is_shutdown = false;

  if (pthread_mutex_init(&queue->lock, NULL) != 0) {
    free(queue->items);
    queue->items = NULL;
    return false;
  }

  if (pthread_cond_init(&queue->not_empty_condition, NULL) != 0) {
    pthread_mutex_destroy(&queue->lock);
    free(queue->items);
    queue->items = NULL;
    return false;
  }

  if (pthread_cond_init(&queue->not_full_condition, NULL) != 0) {
    pthread_cond_destroy(&queue->not_empty_condition);
    pthread_mutex_destroy(&queue->lock);
    free(queue->items);
    queue->items = NULL;
    return false;
  }

  return true;
}

/**
 * Shuts down the queue and frees ring buffer memory and synchronization primitives.
 */
void queue_destroy(task_queue_t *queue) {
  if (!queue) return;

  pthread_mutex_lock(&queue->lock);
  queue->is_shutdown = true;
  pthread_cond_broadcast(&queue->not_empty_condition);
  pthread_cond_broadcast(&queue->not_full_condition);
  pthread_mutex_unlock(&queue->lock);

  pthread_cond_destroy(&queue->not_empty_condition);
  pthread_cond_destroy(&queue->not_full_condition);
  pthread_mutex_destroy(&queue->lock);

  free(queue->items);
  queue->items = NULL;
  queue->capacity = 0;
  queue->current_count = 0;
}

/**
 * Appends a task item to the back of the queue, blocking if the queue is full.
 */
bool queue_push(task_queue_t *queue, void *task_item) {
  if (!queue || !task_item) return false;

  pthread_mutex_lock(&queue->lock);

  while (queue->current_count == queue->capacity && !queue->is_shutdown)
    pthread_cond_wait(&queue->not_full_condition, &queue->lock);

  if (queue->is_shutdown) {
    pthread_mutex_unlock(&queue->lock);
    return false;
  }

  queue->items[queue->tail_index] = task_item;
  queue->tail_index = (queue->tail_index + 1) % queue->capacity;
  queue->current_count++;

  pthread_cond_signal(&queue->not_empty_condition);
  pthread_mutex_unlock(&queue->lock);
  return true;
}

/**
 * Appends a task item without waiting, returning false if the queue is full or shut down.
 */
bool queue_try_push(task_queue_t *queue, void *task_item) {
  if (!queue || !task_item) return false;

  pthread_mutex_lock(&queue->lock);
  if (queue->current_count == queue->capacity || queue->is_shutdown) {
    pthread_mutex_unlock(&queue->lock);
    return false;
  }

  queue->items[queue->tail_index] = task_item;
  queue->tail_index = (queue->tail_index + 1) % queue->capacity;
  queue->current_count++;

  pthread_cond_signal(&queue->not_empty_condition);
  pthread_mutex_unlock(&queue->lock);
  return true;
}

/**
 * Removes a task item from the front of the queue, blocking until an item is ready.
 */
bool queue_pop(task_queue_t *queue, void **output_task_item) {
  if (!queue || !output_task_item) return false;

  pthread_mutex_lock(&queue->lock);

  while (queue->current_count == 0 && !queue->is_shutdown)
    pthread_cond_wait(&queue->not_empty_condition, &queue->lock);

  if (queue->current_count == 0 && queue->is_shutdown) {
    pthread_mutex_unlock(&queue->lock);
    return false;
  }

  *output_task_item = queue->items[queue->head_index];
  queue->items[queue->head_index] = NULL;
  queue->head_index = (queue->head_index + 1) % queue->capacity;
  queue->current_count--;

  pthread_cond_signal(&queue->not_full_condition);
  pthread_mutex_unlock(&queue->lock);
  return true;
}

/**
 * Signals all waiting threads that the queue has shut down and wakes them up.
 */
void queue_signal_shutdown(task_queue_t *queue) {
  if (!queue) return;

  pthread_mutex_lock(&queue->lock);
  queue->is_shutdown = true;
  pthread_cond_broadcast(&queue->not_empty_condition);
  pthread_cond_broadcast(&queue->not_full_condition);
  pthread_mutex_unlock(&queue->lock);
}

/**
 * Returns the number of task items currently waiting in the queue.
 */
size_t queue_get_count(task_queue_t *queue) {
  if (!queue) return 0;

  pthread_mutex_lock(&queue->lock);
  size_t count = queue->current_count;
  pthread_mutex_unlock(&queue->lock);
  return count;
}
