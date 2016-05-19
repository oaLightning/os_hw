#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h> // for open flags
#include <unistd.h>
#include <time.h> // for time measurement
#include <sys/time.h>
#include <signal.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#define IN
#define OUT

#define FIFO_FILE_PERMISSIONS (S_IRWXU | S_IRWXG | S_IRWXO)
#define WRITE_AREA_SIZE (0x1000)

#define ERRNO_ASSERT_WITH_VALUE(assertion, errno_value)  	\
	if (!(assertion)) {							\
		printf("%s\n", strerror(errno_value));	\
		exit(-1);								\
	}
#define ERRNO_ASSERT(assertion) ERRNO_ASSERT_WITH_VALUE(assertion, errno);


char g_write_area[WRITE_AREA_SIZE];
int g_fifo_fd;
const char* g_fifo_path;

bool is_fifo_file(IN const char* filePath) {
	struct stat statData = {0};
	int result = stat(filePath, &statData);
	// Note - This stat can't fail because it's already after we made sure it's a file in does_file_exist
	ERRNO_ASSERT(0 == result);

	return S_ISFIFO(statData.st_mode);
}

int make_fifo_file(IN const char* filePath, IN bool make_file) {
	if (make_file) {
		int result = mkfifo(filePath, FIFO_FILE_PERMISSIONS);
		ERRNO_ASSERT(0 == result);
	}

	int fd = open(filePath, O_WRONLY);
	ERRNO_ASSERT(-1 != fd);
	return fd;
}

bool does_file_exist(IN const char* filePath) {
	struct stat statData = {0};
	int result = stat(filePath, &statData);
	return (0 == result);
}

void delete_file(IN const char* filePath) {
	int result = unlink(filePath);
	ERRNO_ASSERT(0 == result);
}

bool read_from_stdin(OUT ssize_t* dataLength) {
	memset(g_write_area, 0, WRITE_AREA_SIZE);
	char* result = fgets(g_write_area, WRITE_AREA_SIZE, stdin);
	if (NULL == result) {
		int saved_errno = errno;
		if (feof(stdin)) {
			*dataLength = strlen(g_write_area);
			return true;
		}
		ERRNO_ASSERT_WITH_VALUE(false, saved_errno);
	}
	*dataLength = strlen(g_write_area);
	return false;
}

void write_to_file(IN ssize_t dataLength) {
	ssize_t result = write(g_fifo_fd, g_write_area, dataLength);
	if (result != dataLength) {
		if (EPIPE == errno) {
			printf("Recovering from bad pipe\n");
			result = write(g_fifo_fd, g_write_area, dataLength);
		}
	}
	ERRNO_ASSERT(result == dataLength);
}

void close_file(IN int fd) {
	close(fd);
}

void read_write_loop() {
	while (true) {
		ssize_t dataLength = 0;
		bool gotEOF = read_from_stdin(&dataLength);
		write_to_file(dataLength);
		if (gotEOF) {
			break;
		}
	}
}

int get_fifo_file_fd(IN const char* filePath) {
	bool make_fifo = true;
	if (does_file_exist(filePath)) {
		make_fifo = !is_fifo_file(filePath);
		if (make_fifo) {
			delete_file(filePath);
		}
	}
	return make_fifo_file(filePath, make_fifo);
}

void exit_cleanly() {
	delete_file(g_fifo_path);

	if (0 != g_fifo_fd) {
		close_file(g_fifo_fd);
		g_fifo_fd = 0;
	}
}

void pipe_signal_handler(int signal_number) {
	exit_cleanly();
	g_fifo_fd = get_fifo_file_fd(g_fifo_path);
}

void exit_signal_handler(int signal_number) {
	exit_cleanly();
}

typedef void (*signal_handler)(int signal_number);

void register_signal_handler(IN int signal_number, signal_handler handler) {
	struct sigaction new_action = {0};
	struct sigaction old_action = {0};

	new_action.sa_handler = handler;
	int result = sigemptyset(&new_action.sa_mask);
	ERRNO_ASSERT(0 == result);
	new_action.sa_flags = 0;

	result = sigaction(signal_number, NULL, &old_action);
	ERRNO_ASSERT(0 == result);
	if (old_action.sa_handler != SIG_IGN) {
		result = sigaction(signal_number, &new_action, NULL);
		ERRNO_ASSERT(0 == result);
	}
}

void register_signal_handlers() {
	register_signal_handler(SIGINT, exit_signal_handler);
	register_signal_handler(SIGTERM, exit_signal_handler);
	register_signal_handler(SIGPIPE, pipe_signal_handler);
}

int main(int argc, char** argv)
{
	assert(argc == 2);

	register_signal_handlers();

	g_fifo_fd = get_fifo_file_fd(argv[1]);
	g_fifo_path = argv[1];

	read_write_loop();

	exit_cleanly();
}