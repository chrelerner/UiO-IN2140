#ifndef COMMON_H
#define COMMON_H

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

// Brukes for aa lagre informasjon om klienter, og deres ACK-er.
// Brukes av baade server og klienter.
struct client_addr {

    // stored_request brukes bare paa klient-siden. Lagrer meldinger som kom samtidig
    // som en send-operasjon utenfor lookup.
    char *stored_request;

    char *nick;
    int port;
    int ip;

    // Initialiseres med gettimeofday() paa serversiden. Naar lookup skjer, 
    // returneres ACK number NOT FOUND hvis det har gaatt mer enn 30 sekunder.
    int registration_time;

    // Brukes for kommunikasjon mellom klienter
    int current_pkt_num;

    struct client_addr *next;
};

// Bruker dobbeltpekere slik at man kan direkte endre start-variabelen i hovedprogrammene.
void push_client (struct client_addr *node, struct client_addr **start);
struct client_addr *poll_client (struct client_addr **start);
struct client_addr *find_specified_client (char *nick, struct client_addr *start);
struct client_addr *remove_specified_client (char *nick, struct client_addr **start);
void free_clients (struct client_addr *start);


// Brukes for aa lagre meldinger som ble ignorert som foelge av stop-and-wait funksjonaliteten.
// Brukes bare av klienter.
struct msg_buffer {
    
    char *nick;
    char *stored_msg;
    char *prepared_ack_message;

    int previous_ack_num;
    
    struct msg_buffer *next;
};

// Bruker dobbeltpekere slik at man kan direkte endre start-variabelen i hovedprogrammene.
void push_msg (struct msg_buffer *node, struct msg_buffer **start);
struct msg_buffer *poll_msg(struct msg_buffer **start); 
struct msg_buffer *find_specified_msg (char *nick, struct msg_buffer *start);
struct msg_buffer *remove_specified_msg (char *nick, struct msg_buffer **start);
void free_msg (struct msg_buffer *start);


// Feilsjekking og enkel frigjoering.
void check_error(int ret, char *msg, int fault_cond);
void free_structs(struct client_addr *client_start1, struct client_addr *client_start2, struct msg_buffer *msg_start1, struct msg_buffer *msg_start2);

#endif