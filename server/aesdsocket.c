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
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>

static volatile int active = 1;

static void signal_handler(int sig){
    active = 0;
    syslog(LOG_DEBUG, "Caught signal, exiting");
}

int main(int argc, char* argv[]){
    int server_fd, status, err;
    int optval = 1;
    struct addrinfo hints, *server_addr;
    
    openlog(NULL, 0, LOG_USER);

    // Initialize addrinfo structure with address information
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC; // Either IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE; // Indicates communication inside the same machine as host

    // Creation of address info structure using local communication on port defined
    if( (status = getaddrinfo(NULL, "9000", &hints, &server_addr)) != 0 ){
        syslog(LOG_ERR, "Creation of addrinfo structure failed: %s\n", gai_strerror(status));
        closelog();
        return -1;
    }
    
    // Creation of socket
    if( (server_fd = socket(server_addr->ai_family, server_addr->ai_socktype, server_addr->ai_protocol)) == -1 ){
        err = errno;
        syslog(LOG_ERR, "Socket creation failed: %s\n", strerror(err));
        freeaddrinfo(server_addr);
        closelog();
        return -1;
    }

    // Enable reuse of sockets for new connections
    if( (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval))) == -1){
        err = errno;
        syslog(LOG_ERR, "Socket reuse setup failed: %s\n", strerror(err));
        freeaddrinfo(server_addr);
        close(server_fd);
        closelog();
        return -1;
    }

    // Association of socket with port on local machine, only applicable when acting as server
    if( (bind(server_fd, server_addr->ai_addr, server_addr->ai_addrlen)) == -1 ){
        err = errno;
        syslog(LOG_ERR, "Socket-port binding failed: %s\n", strerror(err));
        freeaddrinfo(server_addr);
        close(server_fd);
        closelog();
        return -1;
    }

    // Free server_addr struct now that is no longer needed
    freeaddrinfo(server_addr);

    // Catching signals and providing special handling for terminations and interrupts
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = signal_handler;
    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);

    // If argumnet '-d' is provided to program, create connection as a daemon
    if (argv[1] == '-d'){
        pid_t pid;
        pid = fork();
        if(pid < 0){
            exit(EXIT_FAILURE);
        }else if (pid == 0){
            exit(EXIT_SUCCESS);
        }
    }

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
        int client_fd, file_fd;
        struct sockaddr_storage client_addr;
        socklen_t client_len = sizeof(client_addr);
        char client_ip[INET6_ADDRSTRLEN];
        char *output_file = "/var/tmp/aesdsocketdata";
        int bytes_received;

        // Accept incoming connection
        if( (client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len)) == -1 ){
            err = errno;
            syslog(LOG_ERR, "Incoming communication failed: %s\n", strerror(err));
            close(server_fd);
            closelog();
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

        // Open file and create it if it doesn't exist
        file_fd = open(output_file, O_RDWR | O_APPEND | O_CREAT, 0644);
        if(file_fd == -1){
            err = errno;
            syslog(LOG_ERR, "Failure while opening file: %s\n", strerror(err));
            close(server_fd);
            close(client_fd);
            closelog();
            return -1;
        }
        
        // Allocate memory for buffer 
        char *buf = malloc(1024);
        if(buf == NULL){
           err = errno;
           syslog(LOG_ERR, "Memory allocation failed: %s\n", strerror(err));
           close(server_fd);
           close(client_fd);
           close(file_fd);
           closelog();
           return -1;
        }

        // Define variables for data packet buffer
        int buf_size = 1024; //Start with 1kb for buffer size
        int buf_len = 0;
        int bytes_received;
        char *newline_pos = NULL;

        // Keep receiving data packets until there's no more data to be received
        while( active && (bytes_received = recv(client_fd, buf+buf_len, buf_size-buf_len-1, 0)) > 0 ){
            
            buf_len += bytes_received; // Add bytes read to total buffer length
            buf[buf_len] = '\0'; // End buffer with the null character
            newline_pos = strchr(buf, '\n'); // Get position where newline character is located

            // Increse buffer size if needed
            if(buf_len >= buf_size -1){
                buf_size *= 2; //Duplicate buffer size
                char *temp = realloc(buf, buf_size); // Reallocate buffer variable with new buffer size
                if(temp == NULL){
                    err = errno;
                    syslog(LOG_ERR, "Memory reallocation failed: %s\n", strerror(err));
                    free(buf);
                    close(server_fd);
                    close(client_fd);
                    close(file_fd);
                    closelog();
                    return -1; 
                }
                buf = temp;
            }
            
            
            // If newline character is found, end data transfer
            if(newline_pos != NULL){
                break;
            }

   
        }

        // Throw error if data transfer fails
        if(bytes_received  == -1){
            err = errno;
            syslog(LOG_ERR, "Data transfer failed: %s\n", strerror(err));
            free(buf);
            close(file_fd);
            close(client_fd);
            close(server_fd);
            closelog();
            return -1;
        }else if(bytes_received == 0){
            syslog(LOG_DEBUG, "Client finalized connection\n");
            free(buf);
            close(client_fd);
            close(file_fd);
            continue;
        }

        // Free buffer memory and prepare for new connection
        free(buf);
        close(file_fd);
        close(client_fd);
    }

    close(server_fd);
    return 0;
}