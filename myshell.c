// This is the myshell.c file, it implements that part of the shell that is responsible for sub-process creation.
// The file contains 2 parts - a small part that deals with the incoming arguments and decides how to run them
// and a second part that actually runs them (parse vs. execution).

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/wait.h>

#define ERRNO_ASSERT_WITH_VALUE(assertion, errno_value)  	\
	if (!(assertion)) {							\
		printf("%s\n", strerror(errno_value));	\
		exit(-1);								\
	}
#define ERRNO_ASSERT(assertion) ERRNO_ASSERT_WITH_VALUE(assertion, errno);

#define NO_PIPE_FOUND (-1)

// These are globals that are used to control if and how we wait for child processes
struct sigaction g_old_action = {0};
struct sigaction g_new_action = {
		  .sa_handler = SIG_DFL,
		  .sa_flags = SA_NOCLDWAIT
		};
bool g_handling_sigchild_signals = true;
bool g_saved_old_action = false;

// An enum used to identify how to run a child process
typedef enum RunMode_e {
	RM_Backgorund,
	RM_Forground,
} RunMode;


// This function returns the index of the pipe symbol in the arglist, or NO_PIPE_FOUND if no pipe symbol is found
int find_pipe_char(int count, char** arglist);

// Says weather the command should run in the background or if the shell should wait for the command to finish
// This just checks if the last argument in arglist is an "&" string
RunMode run_in_background(int count, char** arglist);

// arglist - a list of char* arguments (words) provided by the user
// it contains count+1 items, where the last item (arglist[count]) and *only* the last is NULL
// RETURNS - 1 if should cotinue, 0 otherwise
int process_arglist(int count, char** arglist);

// Runs a single program, either as a background program or as a synchronized program
void run_program(int count, char** arglist, RunMode run_mode);

// Runs two programs, piping the stdout from the first to the second
void run_piped_programs(int count, char** arglist, int pipe_index);

// This function waits for a single child to finish running (and makes sure that the child we are waiting for is the
// process that stopped running and not some backgound job).
void wait_for_child(pid_t child);

// This function will wait for both children to finish (and make sure that they are the finishing children, and
// not some background job that was still running).
void wait_for_children(pid_t first_child, pid_t second_child);


int find_pipe_char(int count, char** arglist) {
	int found_pipe_str = NO_PIPE_FOUND;
	for (int i = 0; i < count; i++) {
		if (0 == strcmp(arglist[i], "|")) {
			found_pipe_str = i;
			break;
		}
	}
	return found_pipe_str;
}

RunMode run_in_background(int count, char** arglist) {
	int result = strcmp(arglist[count-1], "&");
	if (0 == result) {
		return RM_Backgorund;
	}
	return RM_Forground;
}

int process_arglist(int count, char** arglist) {
	int pipe_index = find_pipe_char(count, arglist);
	if (NO_PIPE_FOUND == pipe_index) {
		RunMode run_mode = run_in_background(count, arglist);
		run_program(count, arglist, run_mode);
	}
	else {
		run_piped_programs(count, arglist, pipe_index);
	}

	return 1;
}

void set_run_mode(RunMode mode) {
	if (RM_Backgorund == mode) {
		// We don't want to wait for the child process to end, so we set up the signal handler to prevent
		// turning the child into a zombie
		// If this is the first time we're going through this path, we should save the original signal handler
		if (g_handling_sigchild_signals) {
			int result;
			if (!g_saved_old_action) {
				result = sigaction(SIGCHLD, NULL, &g_old_action);
				ERRNO_ASSERT(0 == result);
				g_saved_old_action = true;
			}
			result = sigaction(SIGCHLD, &g_new_action, NULL);
			ERRNO_ASSERT(0 == result);
			g_handling_sigchild_signals = false;
		}
	}
	else {
		assert(RM_Forground == mode);
		// We want to wait for the child process to finish, so we set the signal handler back to it's original form
		if (!g_handling_sigchild_signals) {
			int result = sigaction(SIGCHLD, &g_old_action, NULL);
			ERRNO_ASSERT(0 == result);
			g_handling_sigchild_signals = true;
		}
	}
}

void run_program(int count, char** arglist, RunMode run_mode) {
	if (RM_Backgorund == run_mode) {
		// We don't want to pass the "&" to the called function
		arglist[count - 1] = NULL;
	}

	set_run_mode(run_mode);

	pid_t pid = fork();
	ERRNO_ASSERT(-1 != pid);

	if (0 < pid) {
		// This is the logic that runs in the parent
		// If the process doesn't run in the background, we just wait for it.
		if (RM_Forground == run_mode) {
			wait_for_child(pid);
		}
	}
	else if (0 == pid) {
		// This is the logic that runs in the child
		char** program_arguments = arglist;
		char* program_name = program_arguments[0];
		execvp(program_name, program_arguments);
	}
	else { assert(false); }
}

void run_piped_programs(int count, char** arglist, int pipe_index) {
	char** first_program_args = arglist;
	char* first_program = first_program_args[0];
	// This will double as a NULL argumrny for execvp for the first program, and will separate the programs
	arglist[pipe_index] = NULL;
	char** second_program_args = arglist + pipe_index + 1;
	char* second_program = second_program_args[0];

	set_run_mode(RM_Forground);

	int pipes[2] = {0};
	int result = pipe(pipes);
	ERRNO_ASSERT(-1 != result);

	pid_t first_pid = fork();
	ERRNO_ASSERT(-1 != first_pid);

	if (0 < first_pid) {
		// This is the father's logic
		close(pipes[1]);
		pid_t second_pid = fork();
		ERRNO_ASSERT(-1 != second_pid);
		if (0 < second_pid) {
			// This is again the father's logic
			wait_for_children(first_pid, second_pid);
		}
		else if (0 == second_pid) {
			// This is the seconds child's logic
			result = dup2(pipes[0], STDIN_FILENO);
			ERRNO_ASSERT(-1 != result);
			execvp(second_program, second_program_args);
		}
		else { assert(false); }

	}
	else if (0 == first_pid) {
		// This is the first child's logic
		result = dup2(pipes[1], STDOUT_FILENO);
		ERRNO_ASSERT(-1 != result);
		close(pipes[0]);
		execvp(first_program, first_program_args);
	}
	else { assert(false); }
}

void wait_for_child(pid_t child) {
	pid_t waiting_pid = 0;
	// The loop is in case a previous child that was run in the background finishes at the same time
	while (waiting_pid != child) {
		waiting_pid = wait(NULL);
		ERRNO_ASSERT(-1 != waiting_pid);
	}
}

void wait_for_children(pid_t first_child, pid_t second_child) {
	bool found_first_child = false;
	bool found_second_child = false;

	// We loop untill we've found both children
	while (!(found_first_child && found_second_child)) {
		pid_t waiting_pid = wait(NULL);
		ERRNO_ASSERT(-1 != waiting_pid);

		// We check to see both children so we won't run into some other old child that's just dying now
		if (first_child == waiting_pid) { found_first_child = true; }
		else if (second_child == waiting_pid) { found_second_child = true; }
	}
}