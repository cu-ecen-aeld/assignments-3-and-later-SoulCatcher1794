#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

int main (int argc, char* argv[]){
    openlog(NULL, 0, LOG_USER);

    // If arguments are less than 3: program_name, writefile, writestr -> Exit with error
    if (argc < 3){
        syslog(LOG_ERR, "One of the arguments was not specified");
        closelog();
        return 1;
    }

    // Define variables for easy access of program parameters
    char *writefile = argv[1];
    const char *writestr = argv[2];
    const char *buf = writestr;
    size_t len = strlen(writestr); // Used to capture initial string length and afterwards how many written bytes are missing

    // Open file with access mode to read/write and if file does not exist, create it with owner having read/write permissions
    int fd = open(writefile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    // If file can't be openend, create error log and terminate program
    if(fd == -1){
        int err = errno;
        syslog(LOG_ERR, "Open function failed: %s\n", strerror(err));
        closelog();
        return 1;
    }

    // Auxiliary variable used to capture how many bytes were written
    ssize_t ret;
    // Loop to enforce complete write function
    while (len > 0){
        ret = write(fd, buf, len);
        // If write operation failed, review failure mode
        if (ret == -1){
            int err = errno;
            // If error was that function call was interrupted, loop back and document error
            if (err == EINTR){
                syslog(LOG_WARNING, "Write function call interrupted, will try again");
                continue;
            }
            //Otherwise, create error log and terminate program
            syslog(LOG_ERR, "Write function failed: %s",strerror(err));
            close(fd);
            closelog();
            return 1;
        }
        len -= (size_t)ret; // Every time bytes are written, substract byte nums to len variable
        buf += ret; // Move buf pointer to the next byte after previous bytes of the string were written
    }
    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);
    close(fd);
    closelog();
    return 0;
}