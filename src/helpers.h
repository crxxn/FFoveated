/*
 * Copyright (C) 2019 Oliver Wiedemann
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <SDL2/SDL.h>

/**
 * Print formatted error message referencing the affeted source file,
 * line and the errno status through perror (3), then exit with EXIT_FAILURE.
 * Likely used through the pexit macro for comfort.
 *
 * @param msg error message
 * @param file usually the __FILE__ macro
 * @param line usually the __LINE__ macro
 */
void pexit_(const char *msg, const char *file, const int line);

/**
 * Convenience macro to report runtime errors with debug information.
 */
#define pexit(s) pexit_(s, __FILE__, __LINE__)

/**
 * Container for a generic queue and associated metadata.
 *
 * Signalling is implemented by having data[rear] always point to an
 * unused location, therefore capacity+1 elements have to be allocated.
 */
typedef struct Queue {
	void **data;
	size_t capacity;
	unsigned int front;
	unsigned int rear;
	SDL_mutex *mutex;
	SDL_cond *full;
	SDL_cond *empty;
} Queue;

/**
 * Create and initialize a Queue structure.
 *
 * Allocates storage on the heap.
 * Calls pexit in case of failure.
 * Use free_queue to dispose of pointers acquired through this function.
 * @param capacity number of elements the queue is able to store.
 * @return Queue* ready to use queue. See enqueue, dequeue, free_queue.
 */
Queue *create_queue(size_t capacity);

/**
 * Free and dismantle a Queue structure.
 *
 * The mutex and the full/empty condition variables are destroyed.
 * Calls free on both q->data and subsequently q itself.
 * This function does not take care of any remaining elements in the queue,
 * which have to be handled manually. Caution: This can lead to data leaks.
 */
void free_queue(Queue *q);

/**
 * Add data to end of the queue.
 *
 * Blocks if there is no space left in q, waiting for SDL_CondSignal to be
 * called on the full condition variable.
 * Calls pexit in case of a failure.
 * @param q pointer to a valid Queue structure.
 * @param data will be added to q->data.
 */
void enqueue(Queue *q, void *data);

/**
 * Extract the first element of the queue.
 *
 * Blocks if there is no element in q, waiting for SDL_CondSignal to be called
 * on the empty condition variable.
 * Elements are not safely removed from the queue (read: not overwritten)
 * and might still be accessible at a later point in time.
 * Calls pexit in case of a failure.
 * @param q pointer to a valid Queue struct.
 * @return void* the formerly first element of q.
 */
void *dequeue(Queue *q);

/**
 * Parse a file line by line.
 *
 * At most PATH_MAX characters per line are supported, the main purpose
 * is to parse a file containing pathnames.
 * Each line is sanitized: A trailing newline characters are replaced
 * with a nullbyte. The returned pointer array is also NULL terminated.
 * All contained pointers and the array itself must be passed to free()
 * Calls pexit in case of a failure.
 * @param pathname path to an ascii file to be opened and parsed
 * @return NULL-terminated array of char* to line contents
 */
char **parse_file_lines(const char *pathname);