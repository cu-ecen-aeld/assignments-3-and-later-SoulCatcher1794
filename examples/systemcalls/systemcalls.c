#include "systemcalls.h"

/**
 * @param cmd the command to execute with system()
 * @return true if the command in @param cmd was executed
 *   successfully using the system() call, false if an error occurred,
 *   either in invocation of the system() call, or if a non-zero return
 *   value was returned by the command issued in @param cmd.
*/
bool do_system(const char *cmd){
    /*
    * TODO  add your code here
    *  Call the system() function with the command set in the cmd
    *   and return a boolean true if the system() call completed with success
    *   or false() if it returned a failure
    */
    int ret, err;
    ret = system(cmd);
    openlog(NULL, 0, LOG_USER);

    if(ret == -1){
        err = errno;
        syslog(LOG_ERR, "System function failed: %s", strerror(err));
        closelog();
        return false;
    }

    syslog(LOG_DEBUG, "System function call completed with success");
    closelog();
    return true;
}

/**
* @param count -The numbers of variables passed to the function. The variables are command to execute.
*   followed by arguments to pass to the command
*   Since exec() does not perform path expansion, the command to execute needs
*   to be an absolute path.
* @param ... - A list of 1 or more arguments after the @param count argument.
*   The first is always the full path to the command to execute with execv()
*   The remaining arguments are a list of arguments to pass to the command in execv()
* @return true if the command @param ... with arguments @param arguments were executed successfully
*   using the execv() call, false if an error occurred, either in invocation of the
*   fork, waitpid, or execv() command, or if a non-zero return value was returned
*   by the command issued in @param arguments with the specified arguments.
*/
bool do_exec(int count, ...){
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;

    for(i=0; i<count; i++){
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;

/*
 * TODO:
 *   Execute a system command by calling fork, execv(),
 *   and wait instead of system (see LSP page 161).
 *   Use the command[0] as the full path to the command to execute
 *   (first argument to execv), and use the remaining arguments
 *   as second argument to the execv() command.
 *
*/
    va_end(args);
    openlog(NULL, 0, LOG_USER);

    if(command[0][0] != '/'){
            syslog(LOG_ERR, "Absolute path was not provided for command: %s", command[0]);
            closelog();
            return false;
    }

    for (i=1; i<count; i++){
        if(command[i][0] != '-' && command[i][0] != '/'){
            syslog(LOG_ERR, "Absolute path was not provided for file or command: %s", command[i]);
            closelog();
            return false;
        }
    }

    int status, err;
    pid_t pid;

    pid = fork();
    if(pid == -1){
        err = errno;
        syslog(LOG_ERR, "Creation of new process failed: %s", strerror(err));
        return false;
    }else if (pid == 0){
        syslog(LOG_DEBUG, "Command %s to be executed at child process %d", command[0], getpid());
        execv(command[0], command);
        exit(-1); // Error handle for execv not being able to execute command
    }

    if(waitpid(pid, &status, 0) == -1){
        err = errno;
        syslog(LOG_ERR, "Child process %d could not be terminated: %s", pid, strerror(err));
        closelog();
        return false;
    }else if (WIFEXITED(status)){
        syslog(LOG_DEBUG, "Child process %d terminated normally", pid);
        closelog();
        return true;
    }else{
        syslog(LOG_DEBUG, "Child process %d terminated in an unexpected way", pid);
        closelog();
        return false;
    }
}

/**
* @param outputfile - The full path to the file to write with command output.
*   This file will be closed at completion of the function call.
* All other parameters, see do_exec above
*/
bool do_exec_redirect(const char *outputfile, int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;

/*
 * TODO
 *   Call execv, but first using https://stackoverflow.com/a/13784315/1446624 as a refernce,
 *   redirect standard out to a file specified by outputfile.
 *   The rest of the behaviour is same as do_exec()
 *
*/
    va_end(args);
    openlog(NULL, 0, LOG_USER);

    if(command[0][0] != '/'){
            syslog(LOG_ERR, "Absolute path was not provided for command: %s", command[0]);
            closelog();
            return false;
    }

    syslog(LOG_DEBUG, "Output file: %s", outputfile);

    int status, err;
    pid_t pid;

    // Opening output file where output of executed command will be redirected to
    int fd = open(outputfile, O_WRONLY|O_CREAT|O_TRUNC, 0644);
    if (fd == -1){
        err = errno;
        syslog(LOG_ERR, "File %s failed to be opened: %s", outputfile, strerror(err));
        closelog();
        return false;
    }

    syslog(LOG_ERR, "Parent pid %d opened file descriptor %d", getpid(), fd);

    // Creating new process
    pid = fork();
    // If creation of child process fails, throw error message
    if(pid == -1){
        err = errno;
        syslog(LOG_ERR, "Creation of new process failed: %s", strerror(err));
        closelog();
        return false;
    // If child process is running, execute command provided
    }else if (pid == 0){
        // If duplicate file descriptor fails to be created, throw error
        if(dup2(fd, STDOUT_FILENO) == -1){
            err = errno;
            syslog(LOG_ERR, "Creation of duplicate file descriptor failed: %s", strerror(err));
            exit(-1);
        }

        close(fd);
        syslog(LOG_DEBUG, "Command %s to be executed at child process %d with file descriptor %d", command[0], getpid(), fd);
        execv(command[0], command);
        err = errno;
        syslog(LOG_ERR, "Execution of command failed: %s", strerror(err));
        exit(-1); // Error handle for execv not being able to execute command
    }
    
    close(fd);

    if(waitpid(pid, &status, 0) == -1){
        err = errno;
        syslog(LOG_ERR, "Child process %d could not be terminated: %s", pid, strerror(err));
        closelog();
        return false;
    }else if (WIFEXITED(status)){
        syslog(LOG_DEBUG, "Child process %d terminated normally", pid);
        closelog();
        return true;
    }else{
        syslog(LOG_DEBUG, "Child process %d terminated in an unexpected way", pid);
        closelog();
        return false;
    }
}
