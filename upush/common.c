#include "common.h"
#include <ctype.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netdb.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <arpa/inet.h>

// Brukes bare i starten av programmene naar ingen structs har blitt initialisert.
void check_error(int ret, char *msg, int fault_cond) {

    if (ret == fault_cond) {
        perror(msg);
        exit(EXIT_FAILURE);
    }
}

// Brukes bare paa klient-siden.
void free_structs(struct client_addr *client_start1, struct client_addr *client_start2, struct msg_buffer *msg_start1, struct msg_buffer *msg_start2) {
    free_clients(client_start1);
    free_clients(client_start2);
    free_msg(msg_start1);
    free_msg(msg_start2);
}


// Struct client_addr metoder.

void push_client (struct client_addr *node, struct client_addr **start) {

    struct client_addr *temp = *start;

    if (*start == NULL) {
        *start = node;
    }
    else {
        while (temp->next != NULL) {
            temp = temp->next;
        }
        temp->next = node;
    }
}

struct client_addr *poll_client(struct client_addr **start) {

    struct client_addr *temp = *start;

    if (*start == NULL) {
        return NULL;
    }
    else {
        *start = (*start)->next;
        return temp;
    }
}

struct client_addr *find_specified_client (char *nick, struct client_addr *start) {
    
    struct client_addr *temp = start;
    if (temp == NULL) {
        return NULL;
    }

    while (temp->next != NULL) {
        if (strcmp(temp->nick, nick) == 0) {
            return temp;
        }
        temp = temp->next;
    }

    if (strcmp(temp->nick, nick) == 0) {
            return temp;
    }
    
    return NULL;
}

struct client_addr *remove_specified_client (char *nick, struct client_addr **start) {

    struct client_addr *temp = *start;
    struct client_addr *temp2;
    if (temp == NULL) {
        return NULL;
    }

    if (strcmp(temp->nick, nick) == 0) {
        *start = temp->next;
        return temp;
    }

    while (temp->next != NULL) {
        temp2 = temp->next;

        if (strcmp(temp2->nick, nick) == 0) {
            temp ->next = temp2->next;
            return temp2;
        }
        temp = temp->next;
    }

    return NULL;
}

void free_clients (struct client_addr *start) {

    struct client_addr *temp = start;
    struct client_addr *temp_prev;

    if (temp == NULL) {
        return;
    }

    while (temp->next != NULL) {
        temp_prev = temp;
        temp = temp->next;

        if (temp_prev->nick != NULL) {
            free(temp_prev->nick);
        }
        if (temp_prev->stored_request != NULL) {
            free(temp_prev->stored_request);
        }
        
        free(temp_prev);
    }

    if (temp->nick != NULL) {
        free(temp->nick);
    }
    if (temp->stored_request != NULL) {
        free(temp->stored_request);
    }

    free(temp);
}

// END - struct client_addr metoder.


// Struct msg_buffer metoder.

void push_msg (struct msg_buffer *node, struct msg_buffer **start) {

    struct msg_buffer *temp = *start;

    if (*start == NULL) {
        *start = node;
    }
    else {
        while (temp->next != NULL) {
            temp = temp->next;
        }
        temp->next = node;
    }
}

struct msg_buffer *poll_msg(struct msg_buffer **start) {

    struct msg_buffer *temp = *start;

    if (*start == NULL) {
        return NULL;
    }
    else {
        *start = (*start)->next;
        return temp;
    }
}

struct msg_buffer *find_specified_msg (char *nick, struct msg_buffer *start) {
    

    struct msg_buffer *temp = start;

    if (temp == NULL) {
        return NULL;
    }


    while (temp->next != NULL) {
        if (strcmp(temp->nick, nick) == 0) {
            return temp;
        }
        temp = temp->next;
    }

    if (strcmp(temp->nick, nick) == 0) {
        return temp;
    }
    
    return NULL;
}

struct msg_buffer *remove_specified_msg (char *nick, struct msg_buffer **start) {

    struct msg_buffer *temp = *start;
    struct msg_buffer *temp2;
    if (temp == NULL) {
        return NULL;
    }

    if (strcmp(temp->nick, nick) == 0) {
        *start = temp->next;
        return temp;
    }

    while (temp->next != NULL) {
        temp2 = temp->next;

        if (strcmp(temp2->nick, nick) == 0) {
            temp ->next = temp2->next;
            return temp2;
        }
        temp = temp->next;
    }

    return NULL;
}

void free_msg (struct msg_buffer *start) {

    struct msg_buffer *temp = start;
    struct msg_buffer *temp_prev;

    if (temp == NULL) {
        return;
    }

    while (temp->next != NULL) {
        temp_prev = temp;
        temp = temp->next;

        if (temp_prev->prepared_ack_message != NULL) {
            free(temp_prev->prepared_ack_message);
        }
        if (temp_prev->stored_msg != NULL) {
            free(temp_prev->stored_msg);
        }
        if (temp_prev->nick != NULL) {
            free(temp_prev->nick);
        }

        free(temp_prev);
    }

    if (temp->prepared_ack_message != NULL) {
        free(temp->prepared_ack_message);
    }
    if (temp->stored_msg != NULL) {
        free(temp->stored_msg);
    }
    if (temp->nick != NULL) {
        free(temp->nick);
    }

    free(temp);
}

// END - struct msg_buffer metoder.

