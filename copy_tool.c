#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <fcntl.h> // for open flags
#include <unistd.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define IN
#define OUT

char* g_error_string = NULL;

#define VERIFY_NOT_AND_CLEANUP(to_verify)		\
	if ((to_verify)) {							\
		if (NULL == g_error_string) {			\
			g_error_string = strerror(errno);	\
		}										\
		goto cleanup;							\
	}

off_t min(off_t a, off_t b) {
	if (a < b) { return a; }
	return b;
}

bool does_file_exist(IN const char* filePath) {
	struct stat statData = {0};
	int result = stat(filePath, &statData);
	return (0 == result);
}

void copy_block(IN char* src, IN OUT char* dst, IN size_t size) {
	for (size_t i = 0; i < size; i++) {
		dst[i] = src[i];
	}
}

bool copy_part(IN int src, IN int dst, IN off_t data_to_copy, IN off_t offset_in_file) {
	bool result = false;
	void* src_memory = MAP_FAILED;
	void* dst_memory = MAP_FAILED;

	src_memory = mmap(NULL, data_to_copy, PROT_READ | PROT_WRITE, MAP_PRIVATE, src, offset_in_file);
	VERIFY_NOT_AND_CLEANUP(MAP_FAILED == src_memory);

	dst_memory = mmap(NULL, data_to_copy, PROT_READ | PROT_WRITE, MAP_SHARED, dst, offset_in_file);
	VERIFY_NOT_AND_CLEANUP(MAP_FAILED == dst_memory)

	copy_block(src_memory, dst_memory, data_to_copy);
	result = true;

cleanup:
	if (MAP_FAILED != src_memory) {
		munmap(src_memory, data_to_copy);
	}

	if (MAP_FAILED != dst_memory) {
		munmap(dst_memory, data_to_copy);
	}

	return result;
}

bool get_file_size(IN int fd, OUT off_t* size) {
	struct stat st = {0};
	bool return_value = false;
	int result = fstat(fd, &st);
	VERIFY_NOT_AND_CLEANUP(0 != result);

	*size = st.st_size;
	return_value = true;

cleanup:
	return return_value;
}

bool copy_file(IN int src, IN int dst) {
	bool return_value = false;
	off_t file_size = 0;

	bool result = get_file_size(src, &file_size);
	VERIFY_NOT_AND_CLEANUP(!result);

	int int_result = ftruncate(dst, file_size);
	VERIFY_NOT_AND_CLEANUP(-1 == int_result);

	long page_size = sysconf(_SC_PAGESIZE);
	off_t current_position = 0;

	while (current_position < file_size) {
		off_t to_copy = min(file_size - current_position, 4 * page_size);
		result = copy_part(src, dst, to_copy, current_position);
		VERIFY_NOT_AND_CLEANUP(!result);
		current_position += to_copy;
	}

	return_value = true;

cleanup:
	return return_value;
}

int main(int argc, char** argv)
{
	assert(argc == 3);
	int return_value = -1;
	char* src_path = argv[1];
	char* dst_path = argv[2];
	int src_fd = -1;
	int dst_fd = -1;

	if (!does_file_exist(src_path)) {
		printf("Source file does not exist\n");
		goto cleanup;
	}

	src_fd = open(src_path, O_RDWR);
	VERIFY_NOT_AND_CLEANUP(-1 == src_fd);

	dst_fd = open(dst_path, O_RDWR | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO);
	VERIFY_NOT_AND_CLEANUP(-1 == dst_fd);

	bool result = copy_file(src_fd, dst_fd);
	VERIFY_NOT_AND_CLEANUP(!result);

	return_value = 0;

cleanup:
	if (-1 != src_fd) {
		close(src_fd);
	}

	if (-1 != dst_fd) {
		close(dst_fd);
	}

	if (NULL != g_error_string) {
		printf("%s\n", g_error_string);
	}

	return return_value;
}