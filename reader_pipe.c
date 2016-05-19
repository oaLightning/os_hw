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

#define FILE_PERMISSIONS (S_IRWXU | S_IRWXG | S_IRWXO)
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
struct sigaction g_original_sigterm;
struct sigaction g_original_sigint;

bool is_fifo_file(IN const char* filePath) {
	struct stat statData = {0};
	int result = stat(filePath, &statData);
	// Note - This stat can't fail because it's already after we made sure it's a file in does_file_exist
	ERRNO_ASSERT(0 == result);

	return S_ISFIFO(statData.st_mode);
}

bool does_file_exist(IN const char* filePath) {
	struct stat statData = {0};
	int result = stat(filePath, &statData);
	return (0 == result);
}

void print_to_screen() {
	printf("%s", g_write_area);
}

bool read_from_fifo_file() {
	memset(g_write_area, 0, WRITE_AREA_SIZE);
	errno = 0;
	ssize_t result = read(g_fifo_fd, g_write_area, WRITE_AREA_SIZE-1);
	ERRNO_ASSERT(-1 != result);
	return (0 == result);
}

void ignore_signal(IN int signal_number, OUT struct sigaction* save_action) {
	int result = sigaction(signal_number, NULL, save_action);
	ERRNO_ASSERT(0 == result);

	struct sigaction new_action = *save_action;
	new_action.sa_handler = SIG_IGN;
	result = sigaction(signal_number, &new_action, NULL);
	ERRNO_ASSERT(0 == result);
}

void restore_signal(IN int signal_number, IN struct sigaction* saved_action) {
	int result = sigaction(signal_number, saved_action, NULL);
	ERRNO_ASSERT(0 == result);
}

void ignore_signals() {
	ignore_signal(SIGINT, &g_original_sigint);
	ignore_signal(SIGTERM, &g_original_sigterm);
}

void restore_signals() {
	restore_signal(SIGINT, &g_original_sigint);
	restore_signal(SIGTERM, &g_original_sigterm);
}


void close_fifo_file() {
	if (0 != g_fifo_fd) {
		close(g_fifo_fd);
		g_fifo_fd = 0;
		restore_signals();
	}
}

void read_write_loop() {
	while (true) {
		ssize_t dataLength = 0;
		bool gotEOF = read_from_fifo_file(&dataLength);
		if (gotEOF) {
			break;
		}
		else {
			print_to_screen(dataLength);
		}
	}
	close_fifo_file();
}

int get_fifo_file_fd(IN const char* filePath) {
	while (true) {
		if (does_file_exist(filePath) && is_fifo_file(filePath)) {
			int fd = open(filePath, O_RDONLY);
			if (-1 != fd) {
				ignore_signals();
				return fd;
			}
			ERRNO_ASSERT(ENOENT == errno);
		}
		sleep(1);
	}
}

int main(int argc, char** argv) {
	assert(argc == 2);

	g_fifo_path = argv[1];

	while(true) {
		g_fifo_fd = get_fifo_file_fd(argv[1]);
		read_write_loop();
	}

	return 0;
}