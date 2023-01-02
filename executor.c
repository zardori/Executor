#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <semaphore.h>

#include "utils.h"
#include "err.h"

const int MAX_INSTRUCTION_SIZE = 512;

const int MAX_TASKS = 4096;

struct Task {

    // Descriptors which will be used to read from the task
    int stdout_desc;
    int stderr_desc;


};


struct Task tasks[MAX_TASKS];



sem_t main_mutex;


void handleRun(const char* program_and_args) {


    fprintf(stderr, "Started handling run command with args: \"%s\" \n", program_and_args);

    // Assume there will be at most MAX_TASKS programs to run.
    static int curr_task_num = -1;
    curr_task_num++;
    assert(curr_task_num < MAX_TASKS);

    // Assume that command is correct and there are no extra spaces.
    assert(*program_and_args != ' ');

    pid_t pid;

    char** split_str = split_string(program_and_args);

    char* program_name = split_str[0];

    int descriptors[2];

    // for stdout
    pipe(descriptors);

    set_close_on_exec(descriptors[0], true);
    set_close_on_exec(descriptors[1], true);

    tasks[curr_task_num].stdout_desc = descriptors[0];

    int stdout_write_desc = descriptors[1];

    // for stderr
    pipe(descriptors);

    set_close_on_exec(descriptors[0], true);
    set_close_on_exec(descriptors[1], true);

    tasks[curr_task_num].stderr_desc = descriptors[0];

    int stderr_write_desc = descriptors[1];



    pid = fork();
    ASSERT_SYS_OK(pid);
    if (!pid) {

        // Child process

        // Replace stdout and stderr descriptors to make the program write to
        // the newly created pipes
        dup2(stdout_write_desc, STDOUT_FILENO);
        dup2(stderr_write_desc, STDERR_FILENO);

        // All other descriptors should be closed on exec function

        // First argument of the newly started task should be the name of the task,
        // so we pass whole split_str to the exec function.
        ASSERT_SYS_OK(execvp(program_name, split_str));

    }

    // Close write descriptors, because the executor process will only read from
    // the created pipes.
    close(stdout_write_desc);
    close(stderr_write_desc);

    free_split_string(split_str);

    fprintf(stderr, "Ended handling run command with args: \"%s\" \n", program_and_args);

}


void handleOut(const char* program_number) {

    fprintf(stderr, "Started handling out command with args: \"%s\" \n", program_number);



    fprintf(stderr, "Ended handling out command with args: \"%s\" \n", program_number);

}


void handleErr(const char* program_number) {

    fprintf(stderr, "Started handling err command with args: \"%s\" \n", program_number);



    fprintf(stderr, "Ended handling err command with args: \"%s\" \n", program_number);

}

void handleKill(const char* program_number) {

    fprintf(stderr, "Started handling kill command with args: \"%s\" \n", program_number);



    fprintf(stderr, "Ended handling kill command with args: \"%s\" \n", program_number);

}

void handleSleep(const char* time) {

    fprintf(stderr, "Started handling sleep command with args: \"%s\" \n", time);

    int seconds = 0;
    sscanf(time, "%d", seconds);
    sleep(seconds);

    fprintf(stderr, "Ended handling sleep command with args: \"%s\" \n", time);

}

void handleQuit() {

    fprintf(stderr, "Started handling quit command.");



    fprintf(stderr, "Ended handling quit command.");

}


void handleInstruction(const char* instruction) {

    char command[10];

    sscanf(instruction, "%s", command);

    int command_len = (int)strlen(command);

    // Calculate pointer to the next part of the instruction.
    // Add 1 for the single space between the keyword (command) and its arguments.
    const char* further_instruction_ptr = instruction + command_len + 1;

    if (strcmp(command, "run") == 0) {
        handleRun(further_instruction_ptr);
    } else if (strcmp(command, "out") == 0) {
        handleOut(further_instruction_ptr);
    } else if (strcmp(command, "err") == 0) {
        handleErr(further_instruction_ptr);
    } else if (strcmp(command, "kill") == 0) {
        handleKill(further_instruction_ptr);
    } else if (strcmp(command, "sleep") == 0) {
        handleSleep(further_instruction_ptr);
    } else if (strcmp(command, "quit") == 0) {
        handleQuit();
    } else {
        fprintf(stderr, "Wrong command.\n");
    }



}




int main() {

    sem_init(&main_mutex, 0, 1);



    char buffer[MAX_INSTRUCTION_SIZE];


    // main loop
    while (true) {

        fgets(buffer, MAX_INSTRUCTION_SIZE, stdin);

        sem_wait(&main_mutex);

        // handle command


        sem_post(&main_mutex);



/*
        scanf("%s", command);

        if (strcmp(command, "run") == 0) {

        } else if (strcmp(command, "out") == 0) {

        } else if (strcmp(command, "err") == 0) {

        } else if (strcmp(command, "kill") == 0) {

        } else if (strcmp(command, "sleep") == 0) {

        } else if (strcmp(command, "quit") == 0) {

        } else {
            fprintf(stderr, "Wrong command.\n");
        }
*/
    }




    return 0;
}