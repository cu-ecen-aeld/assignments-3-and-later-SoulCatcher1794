#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>

#define PORT "9000"
#define OUTPUT_FILE "/var/tmp/aesdsocketdata"

static volatile int active = 1;

static void signal_handler(int sig){
    syslog(LOG_DEBUG, "Caught signal, exiting");
    active = 0;
}

int setup_server(void){
    int server_fd, status, err;
    int optval = 1;
    struct addrinfo hints, *server_addr;
    
    // Initialize addrinfo structure with address information
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // Either IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // Indicates communication inside the same machine as host

    // Creation of address info structure using local communication on port defined
    if( (status = getaddrinfo(NULL, PORT, &hints, &server_addr)) != 0 ){
        syslog(LOG_ERR, "Creation of addrinfo structure failed: %s\n", gai_strerror(status));
        return -1;
    }
    
    // Creation of socket
    if( (server_fd = socket(server_addr->ai_family, server_addr->ai_socktype, server_addr->ai_protocol)) == -1 ){
        err = errno;
        syslog(LOG_ERR, "Socket creation failed: %s\n", strerror(err));
        freeaddrinfo(server_addr);
        return -1;
    }

    // Enable reuse of sockets for new connections
    if( (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))) == -1){
        err = errno;
        syslog(LOG_ERR, "Socket reuse setup failed: %s\n", strerror(err));
        freeaddrinfo(server_addr);
        close(server_fd);
        return -1;
    }

    // Association of socket with port on local machine, only applicable when acting as server
    if( (bind(server_fd, server_addr->ai_addr, server_addr->ai_addrlen)) == -1 ){
        err = errno;
        syslog(LOG_ERR, "Socket-port binding failed: %s\n", strerror(err));
        freeaddrinfo(server_addr);
        close(server_fd);
        return -1;
    }

    // Free server_addr struct now that is no longer needed
    freeaddrinfo(server_addr);
    return server_fd;
}

int client_handler(int server_fd){
    int client_fd, err;
    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);
    char client_ip[INET6_ADDRSTRLEN];

    // Accept incoming connection
    if( (client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len)) == -1 ){
        err = errno;
        syslog(LOG_ERR, "Incoming communication failed: %s\n", strerror(err));
        close(server_fd);
        return -1;
    }

    // Get client information and print client IP once connection is stablished
    if(client_addr.ss_family == AF_INET){
        struct sockaddr_in *s = (struct sockaddr_in *)&client_addr;
        inet_ntop(AF_INET, &s->sin_addr, client_ip, sizeof(client_ip));
    }else{
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&client_addr;
        inet_ntop(AF_INET6, &s->sin6_addr, client_ip, sizeof(client_ip));
    }
    syslog(LOG_DEBUG, "Accepted connection from %s", client_ip);
    return client_fd;
}

char *receive_data(int client_fd){
    // Define variables for data packet buffer
    int err;
    int buf_size = 1024; //Start with 1kb for buffer size
    char *buf = malloc(buf_size);
    int buf_len = 0;
    int bytes_received;
    char *newline_pos = NULL;

    if(buf == NULL){
        err = errno;
        syslog(LOG_ERR, "Memory allocation failed: %s\n", strerror(err));
        return NULL;
    }

    // Keep receiving data until packet has been fully received
    while( active && ( (bytes_received = recv(client_fd, (buf+buf_len), (buf_size-buf_len-1), 0)) > 0 ) ){
        // Throw error if data transfer fails
        if(bytes_received  == -1){
            err = errno;
            syslog(LOG_ERR, "Data transfer failed: %s\n", strerror(err));
            free(buf);
            return NULL;
        }

        // If client finalized connection, send back what has been received so far
        if(bytes_received == 0){
            syslog(LOG_DEBUG, "Client finalized connection\n");
            return buf;
        }

        buf_len += bytes_received; // Add bytes read to total buffer length
        buf[buf_len] = '\0'; // End buffer with the null character
        newline_pos = strchr(buf, '\n'); // Get position where newline character is located

        // Increse buffer size if needed
        if( buf_len >= (buf_size-1) ){
            buf_size *= 2; //Duplicate buffer size
            char *temp = realloc(buf, buf_size); // Reallocate buffer variable with new buffer size
            
            // Error handling for memory reallocation
            if(temp == NULL){
                err = errno;
                syslog(LOG_ERR, "Memory reallocation failed: %s\n", strerror(err));
                free(buf);
                return NULL; 
            }
            buf = temp;
        }
        
        // If newline character is found, end data transfer
        if(newline_pos != NULL){
            return buf;
        }
    }
    return buf;
}

int write_to_file(int file_fd, char *buf){
    int bytes_written = 0;
    int total_bytes = strlen(buf);
    int err;

    // Write entire buffer to file
    while(bytes_written < total_bytes){
        int result = write(file_fd, buf + bytes_written, total_bytes - bytes_written);
        if(result == -1){
            err = errno;
            syslog(LOG_ERR, "Writing to file failed: %s\n", strerror(err));
            return -1;
        }
        bytes_written += result;
    }
    return 0;
}

int send_data(int client_fd, int file_fd, int buf_size){
    int err, bytes_read, bytes_sent;
    int len = buf_size;
    char *buf = malloc(len);

    // Initialize bytes counters
    bytes_sent = 0;
    bytes_read = 0;

    // Error handling for memory allocation
    if(buf == NULL){
        err = errno;
        syslog(LOG_ERR, "Memory allocation failed: %s\n", strerror(err));
        return -1;
    }

    // Read data from file
    while( (bytes_read = read(file_fd, buf, len)) != 0 ){
        if(bytes_read == -1){
            err = errno;
            if(err == EINTR){
                continue; // Retry read operation if interrupted by signal
            }
            syslog(LOG_ERR, "Reading from file failed: %s\n", strerror(err));
            free(buf);
            return -1;
        }
        len -= bytes_read; // Decrease remaining length to read
        buf += bytes_read; // Move buffer pointer forward
    }
    buf -= (buf_size - len); // Reset buffer pointer to the beginning

    while(bytes_sent < bytes_read){
            int result = send(client_fd, buf + bytes_sent, bytes_read - bytes_sent, 0);
            if(result == -1){
                err = errno;
                syslog(LOG_ERR, "Sending data to client failed: %s\n", strerror(err));
                free(buf);
                return -1;
            }
            bytes_sent += result;
    }

    free(buf);
    return 0;
}

int main(int argc, char* argv[]){
    int server_fd, client_fd, file_fd, err;
    openlog(NULL, 0, LOG_USER);

    // Catching signals and providing special handling for terminations and interrupts
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = signal_handler;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    // Setup server socket
    server_fd = setup_server();
    if(server_fd == -1){
        closelog();
        return -1;
    }

    /*
    // If argumnet '-d' is provided to program, listen for connections as a daemon
    if (argv[1] == '-d'){
        pid_t pid;
        pid = fork();
        if(pid ==  -1){
            err = errno;
            syslog(LOG_ERR, "Daemon process fork failed: %s\n", strerror(err));
            exit(EXIT_FAILURE);
        }else if (pid == 0){
            exit(EXIT_SUCCESS);
        }else{
            syslog(LOG_DEBUG, "Running as daemon process");
        }
    }
    */

    // Listen for incoming connections
    if( (listen(server_fd, 10)) == -1 ){
        err = errno;
        syslog(LOG_ERR, "Listening for incoming connections failed: %s\n", strerror(err));
        close(server_fd);
        closelog();
        return -1;
    }

    // Start requesting connection request until signal is detected
    while(active){
        // Setting up client connection
        client_fd = client_handler(server_fd);
        if(client_fd == -1){
            close(server_fd);
            closelog();
            return -1;
        }
        
        // Receive data packets from client
        char *buf = receive_data(client_fd);
        if(buf == NULL){
            close(server_fd);
            close(client_fd);
            closelog();
            return -1;
        }

        // Open output file for writing received data, or create it if it doesn't exist
        file_fd = open(OUTPUT_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if(file_fd == -1){
            err = errno;
            syslog(LOG_ERR, "Opening output file failed: %s\n", strerror(err));
            free(buf);
            close(server_fd);
            close(client_fd);
            closelog();
            return -1;
        }

        // Write received data to output file
        if( (write_to_file(file_fd, buf)) == -1 ){
            free(buf);
            close(file_fd);
            close(client_fd);
            close(server_fd);
            closelog();
            return -1;
        }

        // Send back data saved in output file to client
        if( (send_data(client_fd, file_fd, sizeof(buf))) == -1 ){
            free(buf);
            close(file_fd);
            close(client_fd);
            close(server_fd);
            closelog();
            return -1;
        }

        free(buf); // Free buffer memory and prepare for new connection
        close(file_fd); // Close output file after writing is done
        close(client_fd); // Close client connection after data transfer is done
    }
    close(server_fd);
    return 0;
}