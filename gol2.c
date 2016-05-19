// This is the file gol.c which implements a simple game of life, without using any threads.
// The file contains both function for loading/saving the matrix representation and for the actual updating

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>

typedef unsigned char Cell;
typedef Cell** Matrix;
typedef enum CellState_e {
	DEAD = 0,
	ALIVE = 1,
} CellState;

typedef struct _Task {
	int x;
	int y;
	int dx;
	int dy;
	struct _Task* next;
	struct _Task* prev;
} Task;

#define MAX_SURROUNDING_CELLS 		(3)
#define MINIMUM_SURROUNDING_CELLS 	(2)
#define MAKE_ALIVE_THRESHOLD		(3)

#define ASSERT(assertion, message)  						\
	if (!(assertion)) {										\
		printf("%d - %s\n", __LINE__, message);				\
		exit(-1);											\
	}

#define ERRNO_ASSERT(assertion)  							\
	if (!(assertion)) {										\
		printf("%d - %s\n", __LINE__, strerror(errno));		\
		exit(-1);											\
	}

#define PTHREAD_ASSERT(value)	  							\
	if (0 != (value)) {										\
		printf("%d -  %s\n", __LINE__, strerror(value));	\
		exit(-1);											\
	}

// The matrix for the game of life
Matrix g_matrix = NULL;
// A utility matrix used to work so we could update all the cells at once
Matrix g_workspace_matrix = NULL;
// Holds the size of the matrix (ie - How long each array is, the matrix is realy g_matrix_size*g_matrix_size)
int g_matrix_size = 0;
// The square of the matrix size is held as an optimization because it would be needed a lot
int g_matrix_size_square = 0;
// Points to the next task that needs to be done
Task* g_queueFront = NULL;
// Points to the last task that needs to be done (held for fast insertions)
Task* g_queueEnd = NULL;
// The number of cells updated so we can know when we finish a generation
int g_cellsUpdated = 0;
// The lock guarding all access to the queue
pthread_mutex_t g_queueLock;
// Says wheather the queue has any operations left in it, and a cond to enable sleeping while waiting for the change
pthread_cond_t g_hasTasks;
// We should note that g_hasTasksVal is always protected by g_queueLock and therfor doesn't need to be accessed atomically
bool g_hasTasksVal = false;
// The lock used to wait for the g_finishedProcessing condition
pthread_mutex_t g_finishedLock;
// Tells the main thread that we finished processing all the tasks
pthread_cond_t g_finishedProcessing;
// Tells the other threads started from the main thread that they need to exit.
// This is done because otherwise they'd wait while holding the synchronization
// primitives, and we want to clean them up properly
bool g_shouldExit = false;
// Handles to all the started threads, so we could clean them up when we finish
pthread_t* g_threads;

// This function updates a single cell in the g_workspace_matrix based on
// the result of the cells in g_matrix
void update_cell(int i, int j);

// Updates the entire matrix and returns the time it took to update all the cells in miliseconds
double update_matrix();

// Allocates a matrix of the size found in g_matrix_size for use in the g_workspace_matrix global
void allocate_workspace_matrix();

// Free a matrix that isn't used any more and sets the pointed-to variable to NULL
void free_matrix(Matrix* to_free);

// Loads the matrix from a file into the g_matrix global variable
void load_matrix(char* load_from);

// Prints the matrix held in the g_matrix global variable
void print_matrix();

// Initializes all the synchornization primitives
void init_resources();

// Cleans up all the allocated memory and the synchornization primitives
void cleanup();

// This function gets the size of each row\collumn in the matrix without using the standard sqrt function
// It assumes the size is a power of 4. The need for the function is because using math.h's sqrt requires
// linking agains the math so, but we need to use the default gcc parameters which don't link it in...
int get_row_size(int matrix_size);

// Removes the top most task from the queue
Task* deque_task();

// Adds a task (or a number of tasks) to the queu
void enque_task(Task* task);

// This function handles the logic that needs to be done for each task
void process_task(Task* task);

// This function creates a task with the given inital values
Task* create_task(int x, int y, int dx, int dy);

// Sets the task's members to indicate that the second task comes after the first in the queue
void chain_tasks(Task* first, Task* second);

// This is the function that implements the logic for the worker threads that do tasks posted to the queue
void* queue_worker_logic(void* unused);

// Handles all the things required to start all the worker threads
void start_worker_threads(int workers_to_make);

// Tells all the worker threads to exit (so we won't have any issues releasing the resources)
void stop_worker_threads(int workers_to_make);

void update_cell(int i, int j) {
	unsigned char living_neighbours = 0;
	for (int i_offset = -1; i_offset <= 1; i_offset++) {
		for (int j_offset = -1; j_offset <= 1; j_offset++) {
			if (0 == i_offset && 0 == j_offset) { continue; } // This is to skip the current cell
			int current_i = i + i_offset;
			int current_j = j + j_offset;
			if (current_i >= 0 && current_i < g_matrix_size && current_j >= 0 && current_j < g_matrix_size) {
				living_neighbours += g_matrix[current_i][current_j];
			}
		}
	}
	if (g_matrix[i][j]) {
		if (living_neighbours < MINIMUM_SURROUNDING_CELLS || living_neighbours > MAX_SURROUNDING_CELLS) {
			g_workspace_matrix[i][j] = DEAD;
		}
		else {
			g_workspace_matrix[i][j] = ALIVE;
		}
	}
	else {
		if (living_neighbours == MAKE_ALIVE_THRESHOLD) {
			g_workspace_matrix[i][j] = ALIVE;
		}
		else {
			g_workspace_matrix[i][j] = DEAD;
		}
	}
}

double update_matrix() {
	// We assume that the current matrix is in g_matrix and we will write it to g_workspace_matrix, and finally when we
	// finish we will switch the pointers between g_workspace_matrix and g_matrix (this way we replace all the values at the same time).
	// We assume both matrices are already allocated when we start, and we don't care about the values in g_workspace_matrix at all.
	// We return the time in miliseconds it takes to run the calculation

	// We start by preparing all the metadata we'll need ahead of time, so we won't count the creating time during
	// our clock counts. We also take the finished lock here that we'll need in order to wait for the final task
	// to signal that it finished processing
	g_cellsUpdated = 0;
	Task* initialTask = create_task(0, 0, g_matrix_size, g_matrix_size);
	int result = pthread_mutex_lock(&g_finishedLock);
	PTHREAD_ASSERT(result);

	clock_t start_time = clock();

	enque_task(initialTask);

	// We don't need to check the variable in a loop here because we are the only consumer, and the variable can't have changed untill now
	result = pthread_cond_wait(&g_finishedProcessing, &g_finishedLock);
	PTHREAD_ASSERT(result);

	// Once we finished processing we can update the matrix pointers
	Matrix temp_holder = g_matrix;
	g_matrix = g_workspace_matrix;
	g_workspace_matrix = temp_holder;

	clock_t end_time = clock();

	// We release the lock here to not count the lock's time in the clock
	result = pthread_mutex_unlock(&g_finishedLock);
	PTHREAD_ASSERT(result);

	// We test the return values of the clock function only here so that we won't affect the running time for the part we want to test
	ASSERT(-1 != start_time && -1 != end_time, "Failed to measure the time\n");
	double time_in_milliseconds = (end_time - start_time) / (CLOCKS_PER_SEC / 1000);
	return time_in_milliseconds;
}

void allocate_workspace_matrix() {
	ASSERT(NULL == g_workspace_matrix, "The matrix is already allocated for some reason\n");
	g_workspace_matrix = calloc(g_matrix_size, sizeof(Cell*));

	for (int i = 0; i < g_matrix_size; i++) {
		g_workspace_matrix[i] = calloc(g_matrix_size, sizeof(Cell));
		ASSERT(NULL != g_workspace_matrix[i], "Failed to allocate the matrix\n");
	}
}

void free_matrix(Matrix* to_free) {
	ASSERT(NULL != to_free, "Incorrect usage of the function free_matrix\n");
	ASSERT(NULL != *to_free, "Incorrect usage of the function free_matrix\n");

	for (int i = 0; i < g_matrix_size; i++) {
		if (NULL != (*to_free)[i]) {
			free((*to_free)[i]);
			(*to_free)[i] = NULL;
		}
	}

	free(*to_free);
	*to_free = NULL;
}

void load_matrix(char* load_from) {
	int fd = open(load_from, O_RDONLY);
	ERRNO_ASSERT(-1 != fd);

	struct stat stat_data = {0};
	int result = fstat(fd, &stat_data);
	ERRNO_ASSERT(-1 != result);

	g_matrix_size = get_row_size(stat_data.st_size);
	g_matrix_size_square = g_matrix_size * g_matrix_size;
	g_matrix = malloc(g_matrix_size * sizeof(Cell*));
	ASSERT(NULL != g_matrix, "Failed to allocate the matrix\n");

	// Note - We read it in parts in order to make sure we don't allocate to much data at one time
	// so we won't break the "Allocate less than 1MB memory at a time" instruction as much as possible
	for (int i = 0; i < g_matrix_size; i++) {
		int allocation_size = sizeof(Cell) * g_matrix_size;
		g_matrix[i] = malloc(allocation_size);
		ASSERT(NULL != g_matrix[i], "Failed to allocate the matrix\n");

		ssize_t read_size = read(fd, g_matrix[i], allocation_size);
		ERRNO_ASSERT(-1 != read_size);
		ASSERT(read_size == allocation_size, "Didn't read all the data from the file\n");
	}
	allocate_workspace_matrix();
	close(fd);
}

void print_matrix() {
	printf("Priniting matrix----------------\n");
	for (int i = 0; i < g_matrix_size; i++) {
		for (int j = 0; j < g_matrix_size; j++) {
			if (ALIVE == g_matrix[i][j]) {
				printf("*");
			}
			else {
				printf("-");
			}
		}
		printf("\n");
	}
	printf("--------------------------------\n");
}

void init_resources() {
	int result = pthread_mutex_init(&g_queueLock, NULL);
	PTHREAD_ASSERT(result);
	result = pthread_mutex_init(&g_finishedLock, NULL);
	PTHREAD_ASSERT(result);
	result = pthread_cond_init(&g_hasTasks, NULL);
	PTHREAD_ASSERT(result);
	result = pthread_cond_init(&g_finishedProcessing, NULL);
	PTHREAD_ASSERT(result);
}

void cleanup() {
	free_matrix(&g_matrix);
	free_matrix(&g_workspace_matrix);
	int result = pthread_mutex_destroy(&g_queueLock);
	PTHREAD_ASSERT(result);
	result = pthread_mutex_destroy(&g_finishedLock);
	PTHREAD_ASSERT(result);
	result = pthread_cond_destroy(&g_hasTasks);
	PTHREAD_ASSERT(result);
	result = pthread_cond_destroy(&g_finishedProcessing);
	PTHREAD_ASSERT(result);
}

int get_row_size(int matrix_size) {
	int NUMBER_OF_BITS_IN_BYTE = 8;
	int NUMBER_OF_BITS_IN_SIZE = sizeof(matrix_size) * NUMBER_OF_BITS_IN_BYTE;
	for (int bit_count = 2; bit_count < NUMBER_OF_BITS_IN_SIZE; bit_count += 2) {
		int current_size_estimate = 1 << bit_count;
		if (current_size_estimate == matrix_size) {
			return 1 << (bit_count / 2);
		}
	}
	assert(0);
}

Task* deque_task() {
	// Take the lock on the queue and wait for there to be more tasks
	int result = pthread_mutex_lock(&g_queueLock);
	PTHREAD_ASSERT(result);

	while (!g_hasTasksVal) {
		result = pthread_cond_wait(&g_hasTasks, &g_queueLock);
		PTHREAD_ASSERT(result);
	}

	// If we should exit then we release the mutex and signal for the next threads to continue
	if (g_shouldExit) {
		result = pthread_mutex_unlock(&g_queueLock);
		PTHREAD_ASSERT(result);

		result = pthread_cond_signal(&g_hasTasks);
		PTHREAD_ASSERT(result);

		pthread_exit(NULL);
	}

	// We know there are more tasks, so we take out the first task
	Task* task = g_queueFront;
	ASSERT(NULL != task, "We should never be able to dequeu when the queue is empty\n");
	ASSERT(NULL == task->prev, "This should never have a predecssor\n");
	// Remove the task from the queue
	g_queueFront = task->next;
	if (g_queueEnd == task) {
		g_queueEnd = task->next;
	}
	task->next = NULL;
	bool signal_more_actions = false;
	// If there are more actions to run then signal so (and make sure they don't remember this task)
	if (NULL != g_queueFront) {
		g_queueFront->prev = NULL;
		signal_more_actions = true;
	}
	else {
		// Otherwise mark that there are no more tasks, if some late thread suddenly wakes up
		g_hasTasksVal = false;
	}

	// Release the mutex on the queue (and g_hasTasksVal)
	result = pthread_mutex_unlock(&g_queueLock);
	PTHREAD_ASSERT(result);

	// Signal that there are still more tasks so the next thread would run the next task
	if (signal_more_actions) {
		result = pthread_cond_signal(&g_hasTasks);
		PTHREAD_ASSERT(result);
	}

	return task;
}

void enque_task(Task* task) {
	// We first find the last task enqueue in the current batch
	Task* first = task;
	Task* last = first;
	while (NULL != last->next) {
		last = last->next;
	}

	// We take the lock on the queue and surrounding variables
	int result = pthread_mutex_lock(&g_queueLock);
	PTHREAD_ASSERT(result);

	// We update the queue with the new tasks
	if (NULL == g_queueEnd) {
		g_queueFront = first;
		g_queueEnd = last;
	}
	else {
		chain_tasks(g_queueEnd, first);
		g_queueEnd = last;
	}

	// We signal that we inserted new tasks.
	// We change the boolean value inside the lock to protect it using the lock
	// But we send the signal outside in order to hold on to the lock as little as possible
	g_hasTasksVal = true;

	result = pthread_mutex_unlock(&g_queueLock);
	PTHREAD_ASSERT(result);

	result = pthread_cond_signal(&g_hasTasks);
	PTHREAD_ASSERT(result);
}

void process_task(Task* task) {
	if (task->dx == 1 && task->dy == 1) {
		// We first update the cell
		update_cell(task->x, task->y);
		// We use the __sync_add_and_fetch function to update the count and know how many cells were updated including this one
		int current_count = __sync_add_and_fetch_4(&g_cellsUpdated, 1);
		if (current_count == g_matrix_size_square) {
			// If we finished updating all the cells than we notify the main thread
			int result = pthread_cond_signal(&g_finishedProcessing);
			PTHREAD_ASSERT(result);
		}
	}
	else {
		// If there is more than a single cell then split the task to smaller parts and enque them all
		int xDelta = task->dx/2;
		int yDelta = task->dy/2;
		Task* topLeft = create_task(task->x, task->y, task->dx/2, task->dy/2);
		Task* bottomLeft = create_task(task->x + xDelta, task->y, task->dx/2, task->dy/2);
		Task* topRight = create_task(task->x, task->y + yDelta, task->dx/2, task->dy/2);
		Task* bottomRight = create_task(task->x + xDelta, task->y + yDelta, task->dx/2, task->dy/2);

		chain_tasks(topLeft, bottomLeft);
		chain_tasks(bottomLeft, topRight);
		chain_tasks(topRight, bottomRight);

		// This enques all the needed tasks
		enque_task(topLeft);
	}

	// Finaly free the task (it was finished after all...)
	ASSERT(NULL == task->next, "How can a finished task be part of a queue 1\n");
	ASSERT(NULL == task->prev, "How can a finished task be part of a queue 2\n");
	free(task);
}

Task* create_task(int x, int y, int dx, int dy) {
	Task* task = malloc(sizeof(*task));
	ASSERT(NULL != task, "Failed to allocate space for task\n");

	task->x = x;
	task->y = y;
	task->dx = dx;
	task->dy = dy;
	task->next = NULL;
	task->prev = NULL;

	return task;
}

void chain_tasks(Task* first, Task* second) {
	ASSERT(NULL == first->next, "Will destroy the queue otherwise 1\n");
	first->next = second;
	ASSERT(NULL == second->prev, "Will destroy the queue otherwise 2\n");
	second->prev = first;
}

void* queue_worker_logic(void* unused) {
	while (true) {
		Task* task = deque_task();
		process_task(task);
	}
}

void start_worker_threads(int workers_to_make) {
	g_threads = malloc(workers_to_make * sizeof(*g_threads));
	ASSERT(NULL != g_threads, "Failed to allocate memory for thread handles\n");

	for (int i = 0; i < workers_to_make; i++) {
		int result = pthread_create(&(g_threads[i]), NULL, queue_worker_logic, NULL);
		PTHREAD_ASSERT(result);
	}
}

void stop_worker_threads(int workers_to_make) {
	g_shouldExit = true;
	g_hasTasksVal = true;

	int result = pthread_cond_signal(&g_hasTasks);
	PTHREAD_ASSERT(result);

	for (int i = 0; i < workers_to_make; i++) {
		result = pthread_join(g_threads[i], NULL);
		PTHREAD_ASSERT(result);
	}

	free(g_threads);
}

int main(int argc, char** argv) {
	assert(argc == 4);

	char* file_name = argv[1];
	int generation_to_run = atoi(argv[2]);
	int threads_to_start = atoi(argv[3]);

	init_resources();
	start_worker_threads(threads_to_start);
	load_matrix(file_name);
	//print_matrix();

	double time_to_run = 0;
	for (int i = 0; i < generation_to_run; i++) {
		time_to_run += update_matrix();
		//print_matrix();
	}

	stop_worker_threads(threads_to_start);
	cleanup();

	printf("It took %f miliseconds to run\n", time_to_run);
	return 0;
}