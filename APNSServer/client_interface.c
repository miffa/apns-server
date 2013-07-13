//
//  client_interface.c
//  APNSServer
//
//  Created by Tristan Seifert on 12/07/2013.
//  Copyright (c) 2013 Squee! Apps. All rights reserved.
//

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>

#include "json.h"
#include "msg.h"
#include "msg_queue.h"

#include "ansi_terminal_defs.h"
#include "apns_config.h"

static int client_socket = 0;
static int client_should_run = 0;
static pthread_t client_service_thread;

void *client_interface_listening_thread(void *param);
void *client_interface_connection_handler(void *connection);
extern inline inline char* copy_json_info(json_value *value);

/*
 * Sets up a socket on which we listen for clients. Also creates the listening
 * thread.
 */
int client_interface_set_up() {
    char ourname[HOSTNAME_BUF_SIZE+1];
    struct sockaddr_in sa;
    struct hostent *hp;
    
    memset(&sa, 0, sizeof(struct sockaddr_in));
    gethostname(ourname, HOSTNAME_BUF_SIZE);
    
    hp = gethostbyname(ourname);
    if(hp == NULL) { // we apparently don't exist - what is this, the matrix?
        perror("Finding hostname");
        return -1;
    }
    
    sa.sin_family = hp->h_addrtype;
    sa.sin_port = htons(CLIENT_LISTEN_PORT);
    
    // create socket
    if((client_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Creating listening socket");
        return 255;
    }
    
    // bind socket
    if(bind(client_socket, (const struct sockaddr *) &sa, sizeof(struct sockaddr_in)) < 0) {
        close(client_socket);
        perror("Binding listening socket");
        return 512;
    }
    
    listen(client_socket, MAX_NUM_QUEUED_CLIENTS);
    
    // Set up the listening thread
    client_should_run = 1;
    
    int error = pthread_create(&client_service_thread, NULL, client_interface_listening_thread, NULL);
    
    if(error) {
        client_should_run = 0;
        close(client_socket);
        
        perror("Creating listening thread");
        
        return error | 0x8000;
    }
    
    return 0;
}

/*
 * Waits for a connection to come in on the socket and accepts it, then
 * establishes a socket with the client.
 */
int client_interface_get_connection() {
    int t;
    if((t = accept(client_socket, NULL, NULL)) < 0) {
        return -1;
    }
    
    return t;
}

/*
 * Cleans up and closes the socket created previously.
 * If mercy == 0, the thread is just killed and socket closed.
 */
void client_interface_stop(int mercy) {
    if(!mercy) {
        pthread_cancel(client_service_thread);
        close(client_socket);
    } else {
        client_should_run = 0;
    }
}

/*
 * This thread processes connections.
 */
void *client_interface_listening_thread(void *param) {
    int t; // holds the connection we establish with clients
    
    printf("Client listening thread active. Listening for clients on port %i...\n"
           , CLIENT_LISTEN_PORT);
    fflush(stdout);
    
    while(client_should_run) {
        if((t = client_interface_get_connection()) < 0) {
            if(errno == EINTR) { // The socket might sometimes get EINTR
                continue;
            }
            
            perror("Accept client connection"); // die, we got an unknown error
        }
        
        pthread_t thread; // we don't really care about this thread later, it'll die eventually
        pthread_create(&thread, NULL, client_interface_connection_handler, (void *) &t);
    }
    
    close(client_socket);
    
    return NULL;
}

/*
 * This function is called in a new thread to process a client that connects. It
 * handles the protocol and all that other fun stuff to translate messages the
 * clients send into a push notification we can send out.
 */
void *client_interface_connection_handler(void *connection) {
    int sock = *((int *) connection);

    printf("Client connected.\n");
    
    char *msg_buf = calloc(MAX_CLIENT_MSG_SIZE + 1, sizeof(char));
    char *msg_buf_write_ptr = msg_buf;
    
    long bytes_read_total = 0;
    long bytes_read = 0;
    
    // read one byte at a time
    while((bytes_read = read(sock, msg_buf_write_ptr, 1)) > 0) {
        bytes_read_total += bytes_read;
        
        // check if we have \n
        if(strcmp(msg_buf_write_ptr, "\n") == 0) {
//            printf("Received \\n.\n");
            break;
        }
        
        msg_buf_write_ptr += bytes_read;
        
        // have we got MAX_CLIENT_MSG_SIZE bytes and no \n?
        if(bytes_read_total == MAX_CLIENT_MSG_SIZE) {
            write(sock, "err", 3);
            printf(ANSI_COLOR_RED
                   "Client tried to write over 4K request data - potential"
                   "buffer overflow exploit attempt!" ANSI_RESET);
            
            write(sock, "overflow", 8);
            
            // Close socket
            free(msg_buf);
            close(sock);
            
            pthread_exit(NULL);
        }
    }
    
//    printf("Read %li bytes from client.\n%s\n", bytes_read_total, msg_buf);
    
    // try and parse JSON
    char *jsonErr = calloc(512, sizeof(char));
    json_settings settings = { 0 };
    json_value *parsed = json_parse_ex(&settings, msg_buf, MAX_CLIENT_MSG_SIZE, jsonErr);
    
    if(!parsed) {
        write(sock, "err", 3);
        printf("Error parsing JSON: %s\n", jsonErr);
        
        // free memory, close socket
        free(jsonErr);
        free(msg_buf);
        close(sock);
        
        pthread_exit(NULL);
    } else {
        if(parsed->type != json_object) {
            write(sock, "err_type", 8);
            printf("Expected json_object, got %i", parsed->type);
            
            // free memory, close socket
            free(parsed);
            free(jsonErr);
            free(msg_buf);
            close(sock);
            
            pthread_exit(NULL);
        }
    }
    
    // allocate memory for the message
    push_msg *message = malloc(sizeof(push_msg));
    memset(message, 0x00, sizeof(push_msg));

    if(message == NULL) {
        write(sock, "nomem", 5);
        
        printf("Could not allocate push_msg object!\n");
        
        // free memory, close socket
        free(parsed);
        free(jsonErr);
        free(msg_buf);
        close(sock);
        
        pthread_exit(NULL);        
    }
    
    // loop through all the items in the objectoid
    for (int i = 0; i < parsed->u.object.length; i++) {
        char *name = parsed->u.object.values[i].name;
        json_value *value = parsed->u.object.values[i].value;

        // compare name against all keys
        if(strcmp("text", name) == 0 && value->type == json_string) {
            message->text = copy_json_info(value);
        } else if(strcmp("sound", name) == 0 && value->type == json_string) {
            message->sound = copy_json_info(value);
        } else if(strcmp("badge", name) == 0 && value->type == json_integer) {
            message->badgeNumber = (int) value->u.integer;
        } else if(strcmp("custom", name) == 0 && value->type == json_string) {
            message->custPayload = copy_json_info(value);
        } else if(strcmp("key", name) == 0 && value->type == json_string) {
            message->deviceID = copy_json_info(value);
        } else {
            printf("Encountered unexpected token: %s\n", name);
        }
    }
    
    write(sock, "ok", 2);
    
    // Free memory, close socket
    free(parsed);
    free(msg_buf);
    free(jsonErr);
    close(sock);
    
//    printf("Text: %s\nDevice: %s\n", message->text, message->deviceID);
    
    int error = msg_queue_insert(message); // shove message into queue
    if(error) {
        printf("Error adding to queue: %i\n\n", error);
    }
    
    
    fflush(stdout);
    
    return NULL;
}

/*
 * This inline copies a string from the json_value struct to the destination
 * string buffer, ensuring that memory is allocated.
 */
inline char* copy_json_info(json_value *value) {
    char *destBuff = malloc(value->u.string.length + 2); // alloc mem + 2
    memset(destBuff, 0x00, sizeof(value->u.string.length + 2));
    strncpy(destBuff, value->u.string.ptr, value->u.string.length); // copy
    
    return destBuff; // return
}