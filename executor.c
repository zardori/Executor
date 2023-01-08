#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint-gcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdatomic.h>

#include "err.h"
#include "utils.h"

#define INSTRUCTION_BUFFER_SIZE 512

#define MAX_TASKS 4096

// Size of the buffer used to read line from a task.
#define LINE_BUFF_SIZE 1024

struct Task {

    // Variable used only to be referenced to in thread creation.
    // It will be the same as index in task array.
    int id;

    int pid;

    atomic_bool finished;

    // Descriptors which will be used to read from the task.
    int stdout_desc;
    int stderr_desc;

    // Place for threads which will handle task output reading.
    pthread_t stdout_thread;
    pthread_t stderr_thread;

    char stdout_buff_1[LINE_BUFF_SIZE];
    char stdout_buff_2[LINE_BUFF_SIZE];

    char stderr_buff_1[LINE_BUFF_SIZE];
    char stderr_buff_2[LINE_BUFF_SIZE];

    char* which_stdout_to_print;
    char* which_stderr_to_print;

    sem_t stdout_buff_switch_mutex;
    sem_t stderr_buff_switch_mutex;

};

struct TaskAndStatus{
    int task_num;
    int status;
};


struct TaskList {
    struct TaskAndStatus tasks_arr[MAX_TASKS];
    int size;
};



struct Task tasks[MAX_TASKS];

sem_t main_mutex;

int curr_task_num;

bool handling_command;

struct TaskList ended_to_be_printed;



void addToEndedList(int task_num, int status) {
    ended_to_be_printed.tasks_arr[ended_to_be_printed.size].task_num = task_num;
    ended_to_be_printed.tasks_arr[ended_to_be_printed.size].status = status;

    ended_to_be_printed.size++;
}


void printTaskStartMessage(int task_num, pid_t pid) {
    printf("Task %d started: pid %ld.\n", task_num, (long)pid);
}


void printTaskEndMessage(int task_num, int status) {
    if (WIFEXITED(status)) {
        printf("Task %d ended: status %d.\n", task_num, WEXITSTATUS(status));
    } else {
        printf("Task %d ended: signalled.\n", task_num);
    }
}



void taskStructInit(int task_num) {

    tasks[task_num].id = task_num;

    tasks[task_num].finished = false;

    // fill first buffer with empty string
    // and make it ready to print
    tasks[task_num].stdout_buff_1[0] = '\0';
    tasks[task_num].which_stdout_to_print = tasks[task_num].stdout_buff_1;

    sem_init(&tasks[task_num].stdout_buff_switch_mutex, 0, 1);


    // Same with stderr
    tasks[task_num].stderr_buff_1[0] = '\0';
    tasks[task_num].which_stderr_to_print = tasks[task_num].stderr_buff_1;

    sem_init(&tasks[task_num].stderr_buff_switch_mutex, 0, 1);


}


void readerLoop(FILE* stream, char* which_to_write_to, char** which_to_print, sem_t* buff_switch_mutex) {

    char* temp;

    size_t chars_read;

    while(read_line(which_to_write_to, LINE_BUFF_SIZE, stream)) {

        chars_read = strlen(which_to_write_to);
        assert(chars_read <= LINE_BUFF_SIZE - 2);

        // Discard \n from the end of the string if it there
        if (which_to_write_to[chars_read - 1] == '\n') {
            which_to_write_to[chars_read - 1] = '\0';
        }

        sem_wait(buff_switch_mutex);

        // swap buffers
        temp = *which_to_print;
        *which_to_print = which_to_write_to;
        which_to_write_to = temp;

        sem_post(buff_switch_mutex);

    }


}


void* stderrReaderMain(void* task_num_ptr) {

    int task_num = *(int*)task_num_ptr;

    char* which_to_write_to = tasks[task_num].stderr_buff_2;

    FILE* stream = fdopen(tasks[task_num].stderr_desc, "r");

    readerLoop(stream, which_to_write_to, &tasks[task_num].which_stderr_to_print,
        &tasks[task_num].stderr_buff_switch_mutex);

    // If we are here, it means that EOF was reached.
    fclose(stream);

    sem_destroy(&tasks[task_num].stderr_buff_switch_mutex);

}


void* stdoutReaderMain(void* task_num_ptr) {

    int task_num = *(int*)task_num_ptr;

    char* which_to_write_to = tasks[task_num].stdout_buff_2;

    FILE* stream = fdopen(tasks[task_num].stdout_desc, "r");

    readerLoop(stream, which_to_write_to, &tasks[task_num].which_stdout_to_print,
        &tasks[task_num].stdout_buff_switch_mutex);

    // If we are here it means that EOF was reached
    fclose(stream);


    // Wait for the task termination to print information about exit status.
    int status;
    waitpid(tasks[task_num].pid, &status, 0);
    tasks[task_num].finished = true;

    sem_wait(&main_mutex);

    if (handling_command) {

        // If we are currently handling command just add task_num and its exit status
        // to the list. It will be printed by main thread when the command is finished.
        addToEndedList(task_num, status);

    } else {
        printTaskEndMessage(task_num, status);
    }

    sem_post(&main_mutex);

    sem_destroy(&tasks[task_num].stdout_buff_switch_mutex);


}




void handleRun(const char* program_and_args) {


    //fprintf(stderr, "Started handling run command with args: \"%s\" \n", program_and_args);

    // Assume there will be at most MAX_TASKS programs to run.
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


    // Additional initialisation.
    taskStructInit(curr_task_num);


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

    // Parent process

    // start reading threads
    pthread_create(&tasks[curr_task_num].stdout_thread, NULL, stdoutReaderMain,
        &tasks[curr_task_num].id);

    pthread_create(&tasks[curr_task_num].stderr_thread, NULL, stderrReaderMain,
        &tasks[curr_task_num].id);


    // pid variable should contain pid of newly created child
    tasks[curr_task_num].pid = pid;

    // Close write descriptors, because the executor process will only read from
    // the created pipes.
    close(stdout_write_desc);
    close(stderr_write_desc);

    free_split_string(split_str);

    printTaskStartMessage(curr_task_num, pid);

    //fprintf(stderr, "Ended handling run command with args: \"%s\" \n", program_and_args);

}


void handleOut(const char* program_number) {

    //fprintf(stderr, "Started handling out command with args: \"%s\" \n", program_number);

    int task_num;

    ASSERT_SYS_OK(sscanf(program_number, "%d", &task_num));

    assert(task_num >= 0 && task_num < 4096);


    sem_wait(&tasks[task_num].stdout_buff_switch_mutex);

    printf("Task %d stdout: '%s'.\n", task_num, tasks[task_num].which_stdout_to_print);

    sem_post(&tasks[task_num].stdout_buff_switch_mutex);


    //fprintf(stderr, "Ended handling out command with args: \"%s\" \n", program_number);

}


void handleErr(const char* program_number) {

    //fprintf(stderr, "Started handling err command with args: \"%s\" \n", program_number);


    int task_num;

    ASSERT_SYS_OK(sscanf(program_number, "%d", &task_num));

    assert(task_num >= 0 && task_num < 4096);


    sem_wait(&tasks[task_num].stderr_buff_switch_mutex);

    printf("Task %d stderr: '%s'.\n", task_num, tasks[task_num].which_stderr_to_print);

    sem_post(&tasks[task_num].stderr_buff_switch_mutex);


    //fprintf(stderr, "Ended handling err command with args: \"%s\" \n", program_number);

}

void handleKill(const char* program_number) {

    //fprintf(stderr, "Started handling kill command with args: \"%s\" \n", program_number);

    int task_num;
    
    ASSERT_SYS_OK(sscanf(program_number, "%d", &task_num));

    assert(task_num >= 0 && task_num < 4096);

    if (!tasks[task_num].finished) {
        kill(tasks[task_num].pid, SIGINT);
    }
    

    //fprintf(stderr, "Ended handling kill command with args: \"%s\" \n", program_number);

}

void handleSleep(const char* time) {

    //fprintf(stderr, "Started handling sleep command with args: \"%s\" \n", time);


    int milliseconds = 0;
    sscanf(time, "%d", &milliseconds);
    usleep(milliseconds * 1000);

    //fprintf(stderr, "Ended handling sleep command with args: \"%s\" \n", time);

}

void handleQuit() {

    //fprintf(stderr, "Started handling quit command.");

    // Kill all unfinished tasks.
    for (int i = 0; i <= curr_task_num; i++) {
        if (!tasks[i].finished) {
            kill(tasks[i].pid, SIGKILL);
        }
        pthread_join(tasks[i].stdout_thread, NULL);
        pthread_join(tasks[i].stderr_thread, NULL);
    }

    //fprintf(stderr, "Ended handling quit command.");

    exit(0);


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
        exit(EXIT_FAILURE);
    }

}


int main() {

    sem_init(&main_mutex, 0, 1);

    handling_command = false;

    curr_task_num = -1;

    char buffer[INSTRUCTION_BUFFER_SIZE];

    char *buff_ptr = buffer;

    size_t size = INSTRUCTION_BUFFER_SIZE;

/*
    if (getline(&buff_ptr, &size, stdin) == -1) {
        printf("error in reading the line\n");
    }

    printf("\n");

    printf("%s", buffer);

    printf("\n Hello world\n");
*/

    ssize_t chars_read;

    bool quit = false;

    // main loop
    while (true) {

        chars_read = getline(&buff_ptr, &size, stdin);

        if (chars_read == -1) {
            // EOF encountered
            quit = true;
        } else if (buff_ptr[chars_read - 1] == '\n') {
            if (chars_read == 1) {
                // If there is single '\n', continue to the next instruction
                continue;
            } else {
                // remove '\n' from the end of the string
                buff_ptr[chars_read - 1] = '\0';
            }
        }

        sem_wait(&main_mutex);

        handling_command = true;

        sem_post(&main_mutex);

        if (quit) {
            handleQuit();
        } else {
            handleInstruction(buff_ptr);
        }

        sem_wait(&main_mutex);

        handling_command = false;

        // Print info about tasks that ended during handling command.
        for (int i = 0; i < ended_to_be_printed.size; i++) {
            printTaskEndMessage(ended_to_be_printed.tasks_arr[i].task_num,
                ended_to_be_printed.tasks_arr[i].status);
        }
        ended_to_be_printed.size = 0;


        sem_post(&main_mutex);
    }


    handleQuit();

}