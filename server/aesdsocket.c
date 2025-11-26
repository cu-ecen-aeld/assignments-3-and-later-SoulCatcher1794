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

static volatile sig_atomic_t active = 1;

static void signal_handler(int sig){
    int err;
    syslog(LOG_DEBUG, "Caught signal, exiting");
    // Delete the output file
    if(unlink(OUTPUT_FILE) == -1){
        err = errno;
        if(err != ENOENT){  // Ignore error if file doesn't exist
            syslog(LOG_ERR, "Failed to delete output file: %s\n", strerror(err));
        }
    }
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

int client_handler(int server_fd, char *client_ip_out){
    int client_fd, err;
    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);

    // Accept incoming connection
    if( (client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len)) == -1 ){
        err = errno;
        syslog(LOG_ERR, "Incoming communication failed: %s\n", strerror(err));
        return -1;
    }

    // Get client information and print client IP once connection is stablished
    if(client_addr.ss_family == AF_INET){
        struct sockaddr_in *s = (struct sockaddr_in *)&client_addr;
        inet_ntop(AF_INET, &s->sin_addr, client_ip_out, INET6_ADDRSTRLEN);
    }else{
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&client_addr;
        inet_ntop(AF_INET6, &s->sin6_addr, client_ip_out, INET6_ADDRSTRLEN);
    }
    syslog(LOG_DEBUG, "Accepted connection from %s", client_ip_out);
    return client_fd;
}

int receive_data(int client_fd, int file_fd){
    // Define variables for data packet buffer
    int err;
    int buf_size = 1024; //Start with 1kb for buffer size
    char *buf = malloc(buf_size);
    int buf_len = 0;
    int bytes_received;
    char *newline_pos = NULL;
    int packet_complete = 0;
    int written_so_far = 0; // Track how much we've already written

    if(buf == NULL){
        err = errno;
        syslog(LOG_ERR, "Memory allocation failed: %s\n", strerror(err));
        return -1;
    }

    // Keep receiving data until packet has been fully received
    while( active && !packet_complete && ((bytes_received = recv(client_fd, (buf+buf_len), (buf_size-buf_len-1), 0)) > 0) ){
        buf_len += bytes_received; // Add bytes read to total buffer length
        buf[buf_len] = '\0'; // End buffer with the null character
        
        // Check if we have a complete packet (newline found)
        newline_pos = strchr(buf + written_so_far, '\n'); // Search from where we left off
        if(newline_pos != NULL){
            packet_complete = 1;
            // Write only the remaining data up to and including the newline
            int remaining_to_write = (newline_pos - (buf + written_so_far)) + 1;
            int bytes_written = 0;
            
            while(bytes_written < remaining_to_write){
                int result = write(file_fd, (buf + written_so_far) + bytes_written, remaining_to_write - bytes_written);
                if(result == -1){
                    err = errno;
                    syslog(LOG_ERR, "Writing to file failed: %s\n", strerror(err));
                    free(buf);
                    return -1;
                }
                bytes_written += result;
            }
        } else {
            // Write only the new data we haven't written yet
            int new_data_len = buf_len - written_so_far;
            int bytes_written = 0;
            
            while(bytes_written < new_data_len){
                int result = write(file_fd, (buf + written_so_far) + bytes_written, new_data_len - bytes_written);
                if(result == -1){
                    err = errno;
                    syslog(LOG_ERR, "Writing to file failed: %s\n", strerror(err));
                    free(buf);
                    return -1;
                }
                bytes_written += result;
            }
            written_so_far = buf_len; // Update what we've written
        }

        // Increase buffer size if needed and no packet complete yet
        if( !packet_complete && buf_len >= (buf_size-1) ){
            buf_size *= 2; //Double buffer size
            char *temp = realloc(buf, buf_size); // Reallocate buffer variable with new buffer size
            
            // Error handling for memory reallocation
            if(temp == NULL){
                err = errno;
                syslog(LOG_ERR, "Memory reallocation failed: %s\n", strerror(err));
                free(buf);
                return -1; 
            }
            buf = temp;
        }
    }

    syslog(LOG_DEBUG, "Data reception from client complete");

    // Handle receive errors
    if(bytes_received == -1){
        err = errno;
        syslog(LOG_ERR, "Data transfer failed: %s\n", strerror(err));
        free(buf);
        return -1;
    }

    // If client closed connection before sending complete packet
    if(bytes_received == 0 && !packet_complete){
        syslog(LOG_DEBUG, "Client closed connection before sending complete packet\n");
        // Write any remaining data to file
        if(buf_len > 0){
            int bytes_written = 0;
            while(bytes_written < buf_len){
                int result = write(file_fd, buf + bytes_written, buf_len - bytes_written);
                if(result == -1){
                    err = errno;
                    syslog(LOG_ERR, "Writing remaining data to file failed: %s\n", strerror(err));
                    free(buf);
                    return -1;
                }
                bytes_written += result;
            }
        }
    }

    free(buf);
    return 0;
}

int send_data(int client_fd, int file_fd){
    int err, bytes_read, bytes_sent;
    int buf_size = 1024;
    char *buf = malloc(buf_size);

    // Error handling for memory allocation
    if(buf == NULL){
        err = errno;
        syslog(LOG_ERR, "Memory allocation failed: %s\n", strerror(err));
        return -1;
    }

    // Seek to beginning of file to read complete content
    if( (lseek(file_fd, 0, SEEK_SET) == -1) ){
        err = errno;
        syslog(LOG_ERR, "File seek failed: %s\n", strerror(err));
        free(buf);
        return -1;
    }

    bytes_sent = 0;
    // Read and send data from file in chunks
    while( (bytes_read = read(file_fd, buf, buf_size)) > 0 ){
        int total_sent = 0;
        while(total_sent < bytes_read){
            int sent = send(client_fd, buf + total_sent, bytes_read - total_sent, 0);
            if(sent == -1){
                err = errno;
                syslog(LOG_ERR, "Sending data to client failed: %s\n", strerror(err));
                free(buf);
                return -1;
            }
            total_sent += sent;
            bytes_sent += sent;
        }
    }

    if(bytes_read == -1){
        err = errno;
        syslog(LOG_ERR, "Reading from file failed: %s\n", strerror(err));
        free(buf);
        return -1;
    }

    free(buf);
    return 0;
}

int main(int argc, char* argv[]){
    int server_fd, err;
    openlog(NULL, 0, LOG_USER);

    // Catching signals and providing special handling for terminations and interrupts
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);  // Clear signal mask
    action.sa_flags = 0;            // No special flags

    if(sigaction(SIGINT, &action, NULL) == -1){
        syslog(LOG_ERR, "Failed to set SIGINT handler");
    }
    if(sigaction(SIGTERM, &action, NULL) == -1){
        syslog(LOG_ERR, "Failed to set SIGTERM handler");
    }

    // Setup server socket
    server_fd = setup_server();
    if(server_fd == -1){
        closelog();
        return -1;
    }
    syslog(LOG_DEBUG, "Server socket file descriptor: %d", server_fd);

    // If argumnet '-d' is provided to program, listen for connections as a daemon
    if ( (argc > 1) && (strcmp(argv[1], "-d") == 0)){
        pid_t pid;

        pid = fork();

        if(pid ==  -1){
            err = errno;
            syslog(LOG_ERR, "Daemon process fork failed: %s\n", strerror(err));
            exit(EXIT_FAILURE);
        }else if (pid == 0){
            syslog(LOG_DEBUG, "Running as daemon process");
            if( (setsid()) == -1 ){
                err = errno;
                syslog(LOG_ERR, "Creating new session for daemon failed: %s\n", strerror(err));
                close(server_fd);
                closelog();
                return -1;
            }

            if( (chdir("/")) == -1 ){
                err = errno;
                syslog(LOG_ERR, "Changing working directory for daemon failed: %s\n", strerror(err));
                close(server_fd);
                closelog();
                return -1;
            }
            // Close standard file descriptors
            close(STDIN_FILENO);
            close(STDOUT_FILENO);
            close(STDERR_FILENO);
        }else if(pid > 0){
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
    syslog(LOG_DEBUG, "Server listening for incoming connections");

    // Start requesting connection request until signal is detected
    while(active){
        int client_fd, file_fd;
        char client_ip[INET6_ADDRSTRLEN];

        // Setting up client connection and return client IP address
        client_fd = client_handler(server_fd, client_ip);
        if(client_fd == -1){
            if(!active){
                break; // Exit loop if signal was caught
            }
            continue;
        }
        syslog(LOG_DEBUG, "Client file descriptor: %d", client_fd);
        
        // Open output file for writing received data, or create it if it doesn't exist
        file_fd = open(OUTPUT_FILE, O_RDWR | O_CREAT | O_APPEND, 0644);
        if(file_fd == -1){
            err = errno;
            syslog(LOG_ERR, "Opening output file failed: %s\n", strerror(err));
            close(server_fd);
            close(client_fd);
            closelog();
            return -1;
        }

        // Receive data packets from client and write to file immediately
        if( (receive_data(client_fd, file_fd)) == -1 ){
            close(file_fd);
            close(client_fd);
            close(server_fd);
            closelog();
            return -1;
        }

        // Send back data saved in output file to client
        if( (send_data(client_fd, file_fd)) == -1 ){
            close(file_fd);
            close(client_fd);
            close(server_fd);
            closelog();
            return -1;
        }

        // Log closed connection with client IP
        syslog(LOG_DEBUG, "Closed connection from %s", client_ip);
        close(file_fd); // Close output file after writing is done
        close(client_fd); // Close client connection after data transfer is done
    }
    
    close(server_fd);
    syslog(LOG_DEBUG, "Server socket closed");
    closelog();
    return 0;
}