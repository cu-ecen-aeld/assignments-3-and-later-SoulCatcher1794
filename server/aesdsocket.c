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
#include <pthread.h>
#include <time.h>
#include "queue.h"

#define PORT "9000"
#define BACKLOG 10
#define OUTPUT_FILE "/var/tmp/aesdsocketdata"

// Global variables
static volatile sig_atomic_t active = 1;

// Mutexes for file and list operations
static pthread_mutex_t file_mutex;
static pthread_mutex_t list_mutex;

// Client data structure for thread pool
struct thread_data{
    pthread_t thread_id;
    int thread_complete;
    int client_fd;
    SLIST_ENTRY(thread_data) thread_pool;
};

// Define head for thread pool linked list
SLIST_HEAD(thread_pool_head, thread_data) head;

// Signal handler for SIGINT and SIGTERM
static void signal_handler(int sig){
    syslog(LOG_DEBUG, "Caught signal, exiting");
    active = 0;
}

// Function to setup server socket
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
    syslog(LOG_DEBUG, "Server socket file descriptor: %d", server_fd);

    // Free server_addr struct now that is no longer needed
    freeaddrinfo(server_addr);
    return server_fd;
}

// Function to handle client connection and return client file descriptor
int client_setup(int server_fd){
    int client_fd, err;
    char client_ip[INET6_ADDRSTRLEN];
    struct sockaddr_storage client_addr;
    socklen_t client_len = sizeof(client_addr);

    // Accept incoming connection
    if( (client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len)) == -1 ){
        err = errno;
        // Don't log error if interrupted by signal
        if(err != EINTR){
            syslog(LOG_ERR, "Incoming communication failed: %s\n", strerror(err));
        }
        return -1;
    }

    // Get client information and print client IP once connection is stablished
    if(client_addr.ss_family == AF_INET){
        struct sockaddr_in *s = (struct sockaddr_in *)&client_addr;
        inet_ntop(AF_INET, &s->sin_addr, client_ip, INET6_ADDRSTRLEN);
    }else{
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)&client_addr;
        inet_ntop(AF_INET6, &s->sin6_addr, client_ip, INET6_ADDRSTRLEN);
    }

    syslog(LOG_DEBUG, "Accepted connection from IP: %s", client_ip);
    syslog(LOG_DEBUG, "Client file descriptor: %d", client_fd);
    return client_fd;
}

// Function to receive data from client and write to file
int receive_data(int client_fd, int file_fd){
    // Define variables for data packet buffer
    int err;
    int buf_size = 1024; //Start with 1kb for buffer size
    char *buf = malloc(buf_size);
    int packet_size = 0;
    int bytes_received;
    char *newline_pos = NULL;

    if(buf == NULL){
        err = errno;
        syslog(LOG_ERR, "Memory allocation failed: %s\n", strerror(err));
        return -1;
    }

    // Keep receiving data until client closes connection
    while(active && ((bytes_received = recv(client_fd, (buf+packet_size), (buf_size-packet_size-1), 0)) > 0)){
        packet_size += bytes_received; // Add bytes read to total packet size
        buf[packet_size] = '\0'; // End buffer with the null character
        
        // Check if we have a complete packet (newline found)
        newline_pos = strchr(buf, '\n');
        // Found new line character which indicates a single packet has been completely recieved
        if(newline_pos != NULL){
            int packet_length = (newline_pos - buf) + 1; // Calculate length of complete packet (buffer size - position of newline + 1 to include newline)
            int bytes_written = 0; // Track number of bytes written to file

            pthread_mutex_lock(&file_mutex); // Lock file for writing
            while(bytes_written < packet_length){
                int result = write(file_fd, (buf + bytes_written), (packet_length - bytes_written));
                if(result == -1){
                    err = errno;
                    syslog(LOG_ERR, "Writing to file failed: %s\n", strerror(err));
                    free(buf);
                    pthread_mutex_unlock(&file_mutex); // Unlock file after writing attempt
                    return -1; // Exit with error
                }
                bytes_written += result;
            }
            free(buf); // Free buffer memory
            pthread_mutex_unlock(&file_mutex); // Unlock file after writing to file
            return 0; // Packet fully received and written to file
        } 

        // Increase buffer size if needed and no packet complete yet
        if(packet_size >= (buf_size-1)){
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

    // Handle receive errors
    if(bytes_received == -1){
        err = errno;
        syslog(LOG_ERR, "Data transfer failed: %s\n", strerror(err));
        free(buf);
        return -1;
    }

    // Client closed connection, finalize data reception and 
    syslog(LOG_DEBUG, "Data reception from client finalized");
    free(buf); // Free buffer memory of unwritten data of last packet
    return 0;
}

// Function to send data from file back to client
int send_data(int client_fd, int file_fd){
    int err, bytes_read;
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

    pthread_mutex_lock(&file_mutex); // Lock file for reading
    // Read and send data from file in chunks
    while( (bytes_read = read(file_fd, buf, buf_size)) > 0 ){
        int total_sent = 0;
        // Send all bytes read from file
        while(total_sent < bytes_read){
            int sent = send(client_fd, buf + total_sent, bytes_read - total_sent, 0);
            if(sent == -1){
                err = errno;
                syslog(LOG_ERR, "Sending data to client failed: %s\n", strerror(err));
                free(buf);
                pthread_mutex_unlock(&file_mutex); // Unlock file after sending attempt
                return -1;
            }
            total_sent += sent;
        }
    }
    pthread_mutex_unlock(&file_mutex); // Unlock file after reading

    if(bytes_read == -1){
        err = errno;
        syslog(LOG_ERR, "Reading from file failed: %s\n", strerror(err));
        free(buf);
        return -1;
    }

    free(buf);
    return 0;
}

// Define thread handler function
void *client_handler(void *args){
    struct thread_data *data = (struct thread_data *)args;
    int client_fd = data->client_fd;
    int file_fd, err;

    // Open output file for writing received data, or create it if it doesn't exist
    file_fd = open(OUTPUT_FILE, O_RDWR | O_CREAT | O_APPEND, 0644);
    if(file_fd == -1){
        err = errno;
        syslog(LOG_ERR, "Opening output file failed: %s\n", strerror(err));
        close(client_fd);
        pthread_mutex_lock(&list_mutex);
        data->thread_complete = 1;
        pthread_mutex_unlock(&list_mutex);
        pthread_exit(NULL);
    }

    // Receive data packets from client and write to file immediately
    if( (receive_data(client_fd, file_fd)) == -1 ){
        close(client_fd);
        close(file_fd);
        pthread_mutex_lock(&list_mutex);
        data->thread_complete = 1;
        pthread_mutex_unlock(&list_mutex);
        pthread_exit(NULL);
    }

    // Send back data saved in output file to client
    if( (send_data(client_fd, file_fd)) == -1 ){
        close(client_fd);
        close(file_fd);
        pthread_mutex_lock(&list_mutex);
        data->thread_complete = 1;
        pthread_mutex_unlock(&list_mutex);
        pthread_exit(NULL);
    }

    close(file_fd); // Close output file after writing is done
    close(client_fd); // Close client connection after data transfer is done

    pthread_mutex_lock(&list_mutex);
    data->thread_complete = 1;
    pthread_mutex_unlock(&list_mutex);
    return NULL;
}

void *stamper_handler(void *args){
    int err;

    while(active){
        time_t now = time(NULL); // Get current time
        struct tm *tm_info = localtime(&now); // Convert to local time structure
        char time_buffer[512]; // Buffer to hold formatted time string

        strftime(time_buffer, sizeof(time_buffer), "timestamp:%a, %d %b %Y %T %z\n", tm_info); // Format time string

        int file_fd = open(OUTPUT_FILE, O_RDWR | O_CREAT | O_APPEND, 0644);
        if(file_fd == -1){
            err = errno;
            syslog(LOG_ERR, "Opening output file for timestamp failed: %s\n", strerror(err));
            break;
        }

        pthread_mutex_lock(&file_mutex);
        if(write(file_fd, time_buffer, strlen(time_buffer)) == -1){
            err = errno;
            syslog(LOG_ERR, "Writing timestamp to file failed: %s\n", strerror(err));
            close(file_fd);
            pthread_mutex_unlock(&file_mutex);
            break;
        }

        close(file_fd);
        pthread_mutex_unlock(&file_mutex);

        // Sleep for 10 seconds, checking active flag each second for improved responsiveness
        for(int i = 0; i < 10; i++){
            if(!active){
                break;
            }
            sleep(1);
        }   
    }

    return NULL;
}

int main(int argc, char* argv[]){
    int server_fd, err;
    openlog(NULL, 0, LOG_USER);

    // Initialize mutexes without attributes (null)
    pthread_mutex_init(&file_mutex, NULL);
    pthread_mutex_init(&list_mutex, NULL);

    // Initialize the head of the thread pool linked list
    SLIST_INIT(&head);

    // Catching signals and providing special handling for terminations and interrupts
    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);  // Clear signal mask
    action.sa_flags = 0;           // No special flags

    if(sigaction(SIGINT, &action, NULL) == -1){
        syslog(LOG_ERR, "Failed to set SIGINT handler");
    }
    if(sigaction(SIGTERM, &action, NULL) == -1){
        syslog(LOG_ERR, "Failed to set SIGTERM handler");
    }

    // Setup server socket
    server_fd = setup_server();
    if(server_fd == -1){
        pthread_mutex_destroy(&file_mutex);
        pthread_mutex_destroy(&list_mutex);
        closelog();
        return -1;
    }

    // If argumnet '-d' is provided to program, listen for connections as a daemon
    if ( (argc > 1) && (strcmp(argv[1], "-d") == 0)){
        pid_t pid;

        pid = fork();
        if(pid ==  -1){
            err = errno;
            syslog(LOG_ERR, "Daemon process fork failed: %s\n", strerror(err));
            close(server_fd);
            pthread_mutex_destroy(&file_mutex);
            pthread_mutex_destroy(&list_mutex);
            closelog();
            exit(EXIT_FAILURE);
        }else if (pid == 0){
            syslog(LOG_DEBUG, "Running as daemon process");
            if( (setsid()) == -1 ){
                err = errno;
                syslog(LOG_ERR, "Creating new session for daemon failed: %s\n", strerror(err));
                pthread_mutex_destroy(&file_mutex);
                pthread_mutex_destroy(&list_mutex);
                close(server_fd);
                closelog();
                return -1;
            }

            if( (chdir("/")) == -1 ){
                err = errno;
                syslog(LOG_ERR, "Changing working directory for daemon failed: %s\n", strerror(err));
                pthread_mutex_destroy(&file_mutex);
                pthread_mutex_destroy(&list_mutex);
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

    // Start stamper thread to add timestamps to output file every 10 seconds
    pthread_t stamper_thread;
    if( (pthread_create(&stamper_thread, NULL, stamper_handler, NULL)) != 0){
        err = errno;
        syslog(LOG_ERR, "Stamper thread creation failed: %s\n", strerror(err));
        pthread_mutex_destroy(&file_mutex);
        pthread_mutex_destroy(&list_mutex);
        close(server_fd);
        closelog();
        return -1;
    }

    // Listen for incoming connections
    if( (listen(server_fd, BACKLOG)) == -1 ){
        err = errno;
        syslog(LOG_ERR, "Listening for incoming connections failed: %s\n", strerror(err));
        pthread_mutex_destroy(&file_mutex);
        pthread_mutex_destroy(&list_mutex);
        close(server_fd);
        closelog();
        return -1;
    }
    syslog(LOG_DEBUG, "Server listening for incoming connections");

    // Start requesting connection request until signal is detected
    while(active){
        int client_fd;
        struct thread_data *new_client;

        // Setting up client connection
        client_fd = client_setup(server_fd);
        if(client_fd == -1){
            if(!active){
                break; // Exit loop if signal was caught
            }
            continue; //If this connection failed, try for another request
        }

        // Start handling client connection
        new_client = malloc(sizeof(struct thread_data));
        if(new_client == NULL){
            err = errno;
            syslog(LOG_ERR, "Memory allocation for thread data failed: %s\n", strerror(err));
            close(client_fd);
            continue; //If this connection failed, try for another request
        }
        
        // Create new thread to handle each individual client and add to linked list
        new_client->client_fd = client_fd;
        new_client->thread_complete = 0;
        if( (pthread_create(&new_client->thread_id, NULL, client_handler, new_client)) != 0){
            err = errno;
            syslog(LOG_ERR, "Thread creation failed: %s\n", strerror(err));
            close(client_fd);
            free(new_client);
            continue; //If this connection failed, try for another request
        }
        SLIST_INSERT_HEAD(&head, new_client, thread_pool);

        // Join thread if status is complete, remove from list and free memory
        pthread_mutex_lock(&list_mutex);
        struct thread_data *current_client,*temp_client;
        SLIST_FOREACH_SAFE(current_client, &head, thread_pool, temp_client){
            if(current_client->thread_complete == 1){
                pthread_join(current_client->thread_id, NULL);
                SLIST_REMOVE(&head, current_client, thread_data, thread_pool);
                free(current_client);
            }
        }
        pthread_mutex_unlock(&list_mutex);
    }

    // Clean up remaining client threads once server shutdowm signal is received
    pthread_mutex_lock(&list_mutex);
    while(!SLIST_EMPTY(&head)){
        struct thread_data *temp_thread = SLIST_FIRST(&head);
        pthread_join(temp_thread->thread_id, NULL);
        SLIST_REMOVE_HEAD(&head, thread_pool);
        free(temp_thread);
    }
    pthread_mutex_unlock(&list_mutex);

    // Wait for stamper thread to finish
    pthread_join(stamper_thread, NULL);

    pthread_mutex_destroy(&file_mutex);
    pthread_mutex_destroy(&list_mutex);
    
    close(server_fd);
    syslog(LOG_DEBUG, "Server socket closed");

    // Delete the output file
    if(unlink(OUTPUT_FILE) == -1){
        err = errno;
        if(err != ENOENT){  // Ignore error if file doesn't exist
            syslog(LOG_ERR, "Failed to delete output file: %s\n", strerror(err));
        }
    }

    closelog();
    return 0;
}