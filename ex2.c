#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h> // for open flags
#include <unistd.h>
#include <time.h> // for time measurement
#include <sys/time.h>
#include <assert.h>
#include <errno.h> 
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#define SECTORS_PER_BLOCK (4)
#define SECTOR_SIZE (4 * 1024)
#define IO_SIZE (SECTOR_SIZE)
#define INVALID_DEVICE (-1)
#define ARRAYSIZE(arr) (sizeof(arr)/sizeof(arr[0]))

int		g_num_dev;
int*	g_dev_status;
char 	g_io_buffer[IO_SIZE];
char**	g_argv;
int 	g_argc;
int 	g_last_bad_device;

typedef struct {
	int device_index;
	int stripe_number;
	int place_in_block;
	bool is_parity;
} PhysicalLocation;

char* device_string(int device_index) {
	return g_argv[device_index + 1];
}

int get_logical_sector_stripe(int logical_sector) {
	return ((logical_sector / (g_num_dev - 1)) / SECTORS_PER_BLOCK);
}

int get_parity_index_in_stripe(int logical_stripe) {
	return (g_num_dev - 1 - (logical_stripe % g_num_dev));
}

int get_sector_index_in_stripe(int logical_sector, int parity_in_stripe) {
	logical_sector /= SECTORS_PER_BLOCK;
	int index_in_logical_stripe = logical_sector % (g_num_dev - 1);
	int device_index = index_in_logical_stripe;
	if (index_in_logical_stripe >= parity_in_stripe) {
		device_index++;
	}
	return device_index;
}

PhysicalLocation get_physical_sector(int logical_sector) {
	PhysicalLocation location = {0};
	int logical_stripe = get_logical_sector_stripe(logical_sector);
	int parity_in_stripe = get_parity_index_in_stripe(logical_stripe);
	int device_index = get_sector_index_in_stripe(logical_sector, parity_in_stripe);
	location.device_index = device_index;
	location.stripe_number = logical_stripe;
	location.place_in_block = logical_sector % SECTORS_PER_BLOCK;
	location.is_parity = false;
	return location;
}

PhysicalLocation get_relevant_parity_sector(PhysicalLocation desired_write_sector) {
	int parity_in_stripe = get_parity_index_in_stripe(desired_write_sector.stripe_number);
	PhysicalLocation parity = {0};
	parity.device_index = parity_in_stripe;
	parity.stripe_number = desired_write_sector.stripe_number;
	parity.place_in_block = desired_write_sector.place_in_block;
	parity.is_parity = true;
	return parity;
}

void get_backup_sectors(PhysicalLocation bad_sector, PhysicalLocation* backup_data) {
	int parity_in_stripe = get_parity_index_in_stripe(bad_sector.stripe_number);

	for (int i = 0, current_device = 0; i < g_num_dev - 1; i++, current_device++) {
		if (current_device == bad_sector.device_index) {
			current_device++;
		}
		backup_data[i].device_index = current_device;
		backup_data[i].stripe_number = bad_sector.stripe_number;
		backup_data[i].place_in_block = bad_sector.place_in_block;
		backup_data[i].is_parity = (parity_in_stripe == current_device);
	}
}

int physical_location_to_sector(PhysicalLocation location) {
	return ((location.stripe_number * SECTORS_PER_BLOCK) + location.place_in_block);
}

off_t physical_location_to_offset(PhysicalLocation location) {
	return (physical_location_to_sector(location) * SECTOR_SIZE);
}

void close_device(int device_index) {
	assert(device_index >= 0 && device_index < g_num_dev);

	if (INVALID_DEVICE != g_dev_status[device_index]) {
		close(g_dev_status[device_index]);
		g_dev_status[device_index] = INVALID_DEVICE;
	}
}

void print_operated_on_device(PhysicalLocation real_sector) {
	printf("Operation on device %d, sector %d\n", real_sector.device_index, physical_location_to_sector(real_sector));
}

void print_bad_operation_on_device() {
		printf("Operation on bad device %d\n", g_last_bad_device);
}

typedef ssize_t (*io_func)(int fd, void* buf, size_t count);
bool io_operation(PhysicalLocation io_position, io_func operation, char* operation_name) {
	// Saving the last bad device is based on answers from the forum that say to print only the last device that was bad
	int dev_num = io_position.device_index;
	if (g_dev_status[dev_num] < 0) {
		g_last_bad_device = dev_num;
		return false;
	}

	off_t offset_in_device = physical_location_to_offset(io_position);
	off_t resulting_offset = lseek(g_dev_status[dev_num], offset_in_device, SEEK_SET);
	if (resulting_offset != offset_in_device) {
		printf("Seek operation failed on bad device %d with error %s\n", dev_num, strerror(errno));
		g_last_bad_device = dev_num;
		close_device(io_position.device_index);
		return false;
	}

	ssize_t result = operation(g_dev_status[dev_num], g_io_buffer, IO_SIZE);
	if (result != IO_SIZE) {
		printf("%s operation failed on bad device %s (index %d) with error %s\n", operation_name, device_string(dev_num), dev_num, strerror(errno));
		g_last_bad_device = dev_num;
		close_device(io_position.device_index);
		return false;
	}

	print_operated_on_device(io_position);
	return true;	
}

bool read_physical(PhysicalLocation to_read) {
	return io_operation(to_read, (io_func)read, "Read");
}

bool write_physical(PhysicalLocation to_write) {
	return io_operation(to_write, (io_func)write, "Write");
}

bool read_backup(PhysicalLocation sector_to_read) {
	int backup_sector_count = g_num_dev - 1;
	PhysicalLocation backup_locations[backup_sector_count];
	get_backup_sectors(sector_to_read, backup_locations);
	for (int i = 0; i < backup_sector_count; i++) {
		bool read_ok = read_physical(backup_locations[i]);
		if (!read_ok) {
			// TODO - Should probably print here that we are canceling the operation
			return false;
		}
	}
	return true;
}

void read_operation(int sector) {
	PhysicalLocation real_sector = get_physical_sector(sector);

	bool read_ok = read_physical(real_sector);
	if (!read_ok) {
		read_ok = read_backup(real_sector);
	}

	if (!read_ok) {
		print_bad_operation_on_device();
	}
}

bool standard_write(PhysicalLocation real_sector, PhysicalLocation parity_sector) {
	// Based on answers from here http://moodle.tau.ac.il/mod/forum/discuss.php?d=49760
	if (INVALID_DEVICE != g_dev_status[parity_sector.device_index]) {
		assert(real_sector.device_index != parity_sector.device_index);
		PhysicalLocation* lower_index = (real_sector.device_index < parity_sector.device_index) ? (&real_sector) : (&parity_sector);
		PhysicalLocation* higher_index = (real_sector.device_index > parity_sector.device_index) ? (&real_sector) : (&parity_sector);

		bool io_result = read_physical(*lower_index);
		if (!io_result) return false;
		io_result = write_physical(*lower_index);
		if (!io_result) return false;

		io_result = read_physical(*higher_index);
		if (!io_result) return false;
		io_result = write_physical(*higher_index);
		if (!io_result) return false;
		return true;
	}
	else {
		return write_physical(real_sector);
	}

	/*
	// This is the old way I used to write to a sector which mimicked the way actual raid5 implementations would do it
	bool read_success = true;
	if (INVALID_DEVICE != g_dev_status[parity_sector.device_index]) {
		read_success = read_physical(real_sector);
	}

	bool physical_result = write_physical(real_sector);
	if (!physical_result) {
		return false;
	}

	if (read_success) {
		physical_result = read_physical(parity_sector);
		if (physical_result) {
			write_physical(parity_sector);
		}	
	}

	return true;
	*/
}

bool error_state_write(PhysicalLocation real_sector) {
	// Based on answers from here http://moodle.tau.ac.il/mod/forum/discuss.php?d=49760
	int backup_sector_count = g_num_dev - 1;
	PhysicalLocation backup_locations[backup_sector_count];
	get_backup_sectors(real_sector, backup_locations);

	// Note - We don't need to read the parity because we only need the old data from the other devices and the new data in order to
	// Note - calculate it (and we don't need the old data from the bad device at all)
	for (int i = 0; i < backup_sector_count; i++) {
		bool io_ok = true;
		if (backup_locations[i].is_parity) {
			io_ok = write_physical(backup_locations[i]);
		}
		else {
			io_ok = read_physical(backup_locations[i]);
		}
		if (!io_ok) {
			return false;
		}
	}

	return true;
}

void write_operation(int sector) {
	PhysicalLocation real_sector = get_physical_sector(sector);
	PhysicalLocation parity_sector = get_relevant_parity_sector(real_sector);

	bool write_succeeded = false;
	if (INVALID_DEVICE != g_dev_status[real_sector.device_index]) {
		write_succeeded = standard_write(real_sector, parity_sector);
	}
	if (!write_succeeded) {
		write_succeeded = error_state_write(real_sector);
	}
	
	if (!write_succeeded) {
		print_bad_operation_on_device();
	}
}

void open_device(int device_index) {
	assert(device_index <= g_num_dev && device_index >= 0);

	g_dev_status[device_index] = open(device_string(device_index), O_RDWR );
	// TODO - Should check if we need to print an error here in case of failure
	if (INVALID_DEVICE == g_dev_status[device_index]) {
		printf("Failed to open device %s index %d with error %s\n", device_string(device_index), device_index, strerror(errno));
	}
}

void repair_device(int device_index) {
	// Based on answers here http://moodle.tau.ac.il/mod/forum/discuss.php?d=50752
	assert(device_index <= g_num_dev && device_index >= 0);

	int old_device = g_dev_status[device_index];
	open_device(device_index);
	if (INVALID_DEVICE == g_dev_status[device_index]) {
		g_dev_status[device_index] == old_device;
	} 
	else {
		close(old_device);
	}

}

void open_devices() {
	for (int i = 0; i < g_num_dev; i++) {
		open_device(i);
	}
}

void close_devices() {
	for (int i = 0; i < g_num_dev; i++) {
		close_device(i);
	}
}

struct {
	char* op_name;
	void (*func)(int param);
} functions[] = {
	{"READ", read_operation},
	{"WRITE", write_operation},
	{"REPAIR", repair_device},
	{"KILL", close_device},
};

int main(int argc, char** argv)
{
	assert(argc >= 4);
	
	// number of devices == number of arguments (ignore 1st)
	g_argc = argc;
	g_argv = argv;
	g_num_dev = argc - 1;
	int _dev_status[g_num_dev];
	g_dev_status = _dev_status;
	open_devices(argv+1);
	
	// vars for parsing input line
	char input_line[1024];
	char given_command[0x20];
	int command_param;
	
	// read input lines to get command of type "<CMD> <PARAM>"
	while (fgets(input_line, 1024, stdin) != NULL) {
		sscanf(input_line, "%s %d", given_command, &command_param);

		bool found = false;

		for (int i = 0; i < ARRAYSIZE(functions); i++) {
			if (!strcmp(given_command, functions[i].op_name)) {
				found = true;
				functions[i].func(command_param);
				break;
			}
		}

		if (!found) {
			printf("Invalid command: %s\n", given_command);
		}
	}

	close_devices();
}