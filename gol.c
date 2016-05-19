// This is the file gol.c which implements a simple game of life, without using any threads.
// The file contains both function for loading/saving the matrix representation and for the actual updating

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>

typedef unsigned char Cell;
typedef Cell** Matrix;
typedef enum CellState_e {
	DEAD = 0,
	ALIVE = 1,
} CellState;

#define MAX_SURROUNDING_CELLS 		(3)
#define MINIMUM_SURROUNDING_CELLS 	(2)
#define MAKE_ALIVE_THRESHOLD		(3)

#define ASSERT(assertion, message)  			\
	if (!(assertion)) {							\
		printf("%s\n", message);				\
		exit(-1);								\
	}

#define ERRNO_ASSERT(assertion)  			\
	if (!(assertion)) {						\
		printf("%s\n", strerror(errno));	\
		exit(-1);							\
	}

// The matrix for the game of life
Matrix g_matrix = NULL;
// A utility matrix used to work so we could update all the cells at once
Matrix g_workspace_matrix = NULL;
// Holds the size of the matrix (ie - How long each array is, the matrix is realy g_matrix_size*g_matrix_size)
int g_matrix_size = 0;

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

// Cleans up all the allocated memory
void cleanup();

// This function gets the size of each row\collumn in the matrix without using the standard sqrt function
// It assumes the size is a power of 4. The need for the function is because using math.h's sqrt requires
// linking agains the math so, but we need to use the default gcc parameters which don't link it in...
int get_row_size(int matrix_size);



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
	struct timeval start_time = {0};
	struct timeval end_time = {0};
	int start_result = gettimeofday(&start_time, NULL);

	for (int i = 0; i < g_matrix_size; i++) {
		for (int j = 0; j < g_matrix_size; j++) {
			update_cell(i, j);
		}
	}

	Matrix temp_holder = g_matrix;
	g_matrix = g_workspace_matrix;
	g_workspace_matrix = temp_holder;

	int end_result = gettimeofday(&end_time, NULL);

	// We test the return values of the gettimeofday function only here so that we won't affect the running time for the part we want to test
	ASSERT(0 == start_result && 0 == end_result, "Failed to measure the time\n");
	double time_in_milliseconds = ((end_time.tv_sec - start_time.tv_sec) * 1000) + ((end_time.tv_usec - start_time.tv_usec) / 1000);
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

void cleanup() {
	free_matrix(&g_matrix);
	free_matrix(&g_workspace_matrix);
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

int main(int argc, char** argv) {
	assert(argc == 3);

	char* file_name = argv[1];
	int generation_to_run = atoi(argv[2]);

	load_matrix(file_name);
	//print_matrix();

	double time_to_run = 0;
	for (int i = 0; i < generation_to_run; i++) {
		time_to_run += update_matrix();
		//print_matrix();
	}
	cleanup();

	printf("It took %f miliseconds to run\n", time_to_run);
	return 0;
}