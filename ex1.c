#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h> // for time measurement
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <math.h>
#include <string.h>
#include <errno.h>

typedef enum Parameters_t {
	ProgramPath = 0,
	TargetFile,
	ParameterCount,
} Parameters;

typedef enum FileType_t {
	FTFile = 0,
	FTDir,
	FTNeither,
} FileType;

typedef enum ErrorCodes_t {
	ECSuccess 						= 0,
	ECNotFileOrDir 					= -1,
	ECFailedToGetFileStats 			= -2,
	ECFailedToOpenForTruncate 		= -3,
	ECFailedToWriteDuringSetup 		= -4,
	ECFailedToGetTimeStart 			= -5,
	ECFailedToGetTimeEnd 			= -6,
	ECFailedToSeekRandomPosition 	= -7,
	ECFailedToWriteAllData 			= -8,
	ECBadArgumentCount 				= -9,
	ECFileIsADirectory 				= -10,
	ECFailedOpenToWriteDuringSetup	= -11,
	ECFailedToCreateFile			= -12,
} ErrorCodes;

#define VERIFY(condition, message, errorCode) 	\
	if (!(condition))	{						\
		printf(message);						\
		exit((int)errorCode);					\
	}

#define VERIFY_ERRNO(condition, message, errorCode) 	\
	if (!(condition))	{						\
		printf((message), strerror(errno));		\
		exit((int)errorCode);					\
	}

#define ARRAYSIZE(arr) (sizeof(arr)/sizeof(arr[0]))

static char buf[1024*1024] __attribute__ ((__aligned__ (4096)));
#define TARGET_FILE_SIZE (256 * 1024 * 1024)
#define KILOBYTE(num) (num * 1024)
#define MEGABYTE(num) (KILOBYTE(num) * 1024)
#define WRITE_SIZE (MEGABYTE(1))
#define NUMBER_OF_TESTS_FOR_AVEREGE (5)


bool doesFileExist(const char * filePath) {
	struct stat data = {0};
	int result = stat(filePath, &data);
	return (-1 != result);
}

void getStat(const char * path, struct stat * data) {
	int result = stat(path, data);
	VERIFY_ERRNO(-1 != result, "Failed to get file stats: %s\n", ECFailedToGetFileStats);
}

FileType getFileType(const char * filePath) {
	struct stat data = {0};
	getStat(filePath, &data);
	if (S_ISREG(data.st_mode)) {
		return FTFile;
	}
	else if (S_ISDIR(data.st_mode)) {
		return FTDir;
	}
	return FTNeither;
}

void printFileType(FileType fileType) {
	VERIFY(FTNeither != fileType, "Not a regular file or directory\n", ECNotFileOrDir);
	if (FTFile == fileType) {
		printf("It is a regular file\n");
	}
	else if (FTDir == fileType) {
		printf("It is a directory\n");	
	}
}

off_t getRandomOffsetInFile(size_t writeSize) {
	// Note - This assumes a file size of 256 MB
	size_t randomValue = (size_t)random();
	size_t possibleAllignedWrites = TARGET_FILE_SIZE / writeSize;
	return (off_t)((randomValue % possibleAllignedWrites) * writeSize);
}

off_t getFileSize(const char * path) {
	struct stat data = {0};
	getStat(path, &data);
	return data.st_size;
}

void makeGlobalBufferRandom(long dataToRandomize) {
	int runsToMake = dataToRandomize / sizeof(long);
	long* bufAsLong = (long*)buf;
	for (int i = 0; i < runsToMake; i++) {
		bufAsLong[i] = random();
	}
}

void appendRandomDataToFile(const char * path, off_t dataSize) {
	int fd = open(path, O_APPEND | O_RDWR);
	VERIFY_ERRNO(-1 != fd, "Failed to open the file to make it 256MB: %s\n", ECFailedOpenToWriteDuringSetup);

	while (dataSize > 0) {
		int dataToWrite = (dataSize < sizeof(buf)) ? (dataSize) : (sizeof(buf));
		makeGlobalBufferRandom(dataToWrite);
		int result = write(fd, buf, dataToWrite);
		VERIFY_ERRNO(-1 != result, "Failed writing to buffer to make it 256MB: %s\n", ECFailedToWriteDuringSetup);
		dataSize -= dataToWrite;
	}
	close(fd);
}

void verifyFileExists(const char * path) {
	if (!doesFileExist(path)) {
		int fd = open(path, O_RDWR | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO);
		VERIFY_ERRNO(-1 != fd, "Failed to create the file for appending: %s\n", ECFailedToCreateFile);
		close(fd);
	}
}

void makeFileSize(const char * path, off_t size) {
	verifyFileExists(path);

	off_t currentFileSize = getFileSize(path);
	off_t dataToAdd = 0;
	if (currentFileSize < size) {
		dataToAdd = size - currentFileSize;
	}
	else if (currentFileSize > size) {
		int fd = open(path, O_TRUNC | O_RDWR);
		VERIFY_ERRNO(-1 != fd, "Failed to open the file for truncating: %s\n", ECFailedToOpenForTruncate);
		close(fd);
		dataToAdd = size;
	}
	else {
		// Note - File is just the right size, we don't need to do anythig
	}

	if (size != 0) {
		appendRandomDataToFile(path, dataToAdd);
	}
}

long rewriteFile(const char * path, bool useOdirect, size_t writeSize) {
	struct timeval start = {0};
	struct timeval end = {0};
	int seekResult = 0;
	ssize_t writeResult = 0;

	int flags = (useOdirect) ? (O_RDWR | O_DIRECT) : (O_RDWR);

	VERIFY_ERRNO(0 == gettimeofday(&start, NULL), "Error getting start time: %s\n", ECFailedToGetTimeStart);

	size_t numberOfWrites = TARGET_FILE_SIZE / writeSize;

	int fd = open(path, flags);

	for (int i = 0; i < numberOfWrites; i++) {
		off_t offsetInFile = getRandomOffsetInFile(writeSize);
		seekResult = lseek(fd, offsetInFile, SEEK_SET);
		VERIFY_ERRNO(-1 != seekResult, "Failed to seek to the correct location: %s\n", ECFailedToSeekRandomPosition);

		writeResult = write(fd, buf, sizeof(buf));
		VERIFY_ERRNO(-1 != writeResult, "Failed to write all the data to the file: %s\n", ECFailedToWriteAllData);
	}

	close(fd);

	VERIFY_ERRNO(0 == gettimeofday(&end, NULL), "Error getting end time: %s\n", ECFailedToGetTimeEnd);

	long seconds  = end.tv_sec  - start.tv_sec;
    long useconds = end.tv_usec - start.tv_usec;
    long mtime = (seconds*1000 + useconds/1000.0);
    return mtime;
}

void printWriteStatistics(const char* path, size_t writeSize) {
	double timeWithDirect = 0;
	double timeWithoutDirect = 0;

	for (int i = 0; i < NUMBER_OF_TESTS_FOR_AVEREGE; i++) {
		makeGlobalBufferRandom(writeSize);
		timeWithDirect += rewriteFile(path, true, writeSize);
		makeGlobalBufferRandom(writeSize);
		timeWithoutDirect += rewriteFile(path, false, writeSize);
	}

	double averageTimeDirect = timeWithDirect/NUMBER_OF_TESTS_FOR_AVEREGE;
	double throughputDirect = MEGABYTE(256) / MEGABYTE(averageTimeDirect);
	double averageTimeNoDirect = timeWithoutDirect/NUMBER_OF_TESTS_FOR_AVEREGE;
	double throughputNoDirect = MEGABYTE(256) / MEGABYTE(averageTimeNoDirect);
	printf("Average time for write size %d with direct is %f and thoughput is %f\n", writeSize, averageTimeDirect, throughputDirect);
	printf("Average time for write size %d without direct is %f and thoughput is %f\n", writeSize, averageTimeNoDirect, throughputNoDirect);
}

int main(int argc, char const *argv[])
{
	VERIFY(argc == ParameterCount, "Bad parameter count\n", ECBadArgumentCount);

	const char * filePath = argv[TargetFile];

	if (doesFileExist(filePath)) {
		printf("Input file exists\n");
		FileType fileType = getFileType(filePath);
		printFileType(fileType);
		VERIFY(FTDir != fileType, "Can't add data to a directory\n", ECFileIsADirectory);
		makeFileSize(filePath, TARGET_FILE_SIZE);
	}
	else {
		printf("Input file does not exist\n");
		makeFileSize(filePath, TARGET_FILE_SIZE);
	}

	/* This is the code used for making the graph
	size_t writeSizes[] = {MEGABYTE(1),KILOBYTE(256), KILOBYTE(64), KILOBYTE(16), KILOBYTE(4)};
	for (int i = 0; i < ARRAYSIZE(writeSizes); i++) {
		printWriteStatistics(filePath, writeSizes[i]);
	}
	
	printf("**************\n");
	*/
	
	printWriteStatistics(filePath, WRITE_SIZE);

	return 0;
}