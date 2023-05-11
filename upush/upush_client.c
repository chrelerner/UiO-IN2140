#include "send_packet.h"
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

#define BUF_SIZE 1500

int set_next_ack (int ack) {
    
    if (ack == 0) {
        ack = 1;
    }
    else if (ack == 1) {
        ack = 0;
    }

    return ack;
}

char *construct_registration_message (int ack_num, char *nick);
int register_msg (char *nick, char *stored_msg, char *prepared_ack_msg, int previous_ack_num, struct msg_buffer **start);
int heartbeat (int socket_fd, int *server_ack_num, struct sockaddr_in *server_addr_ptr, char *nick, int timeout, struct client_addr **request_msg_list);

// Brukes bare for aa sende tilbake ACKs.
int test_format_text (char *message, char *nick);

char *construct_ack_correct (int ack_num);
char *construct_ack_wrong_name (int ack_num);
char *construct_ack_wrong_format (int ack_num);

int extract_pkt_text (char *buf);
char *extract_nick_text (char *buf);
char *extract_msg_text (char *buf);


// Brukes bare i stdin og lookup.
char *construct_lookup_message (int ack_num, char *nick);
int server_lookup_client (int socket_fd, int *server_ack_num, struct sockaddr_in *server_addr_ptr, struct client_addr **client_list, char *tonick, int timeout);

char *construct_text_message (int ack_num, char *from_nick, char *to_nick, char* text);
int send_text_message (int socket_fd, int *server_ack_num, struct sockaddr_in *server_addr_ptr, struct client_addr **client_list, char *fromnick, char *tonick, int timeout, struct client_addr **request_msg_list, struct client_addr *target_client, char *message);

char *extract_nick_server_lookup (char *buf);
char *extract_ip_server_lookup (char *buf);
int extract_port_server_lookup (char *buf);

char *extract_nick_stdin (char *buf);
char *extract_msg_stdin (char *buf);
char *extract_nick_stdin_block (char *buf);

int test_format_server_ack (char *message, int ack_num);
int test_format_client_ack (char *message, int ack_num);
int test_format_stdin (char *message);

int register_client(char *nick, char *request, int port, int ip, struct client_addr **start);
int block_client (char *nick, struct msg_buffer **blocked_list);
void unblock_client (char *nick, struct msg_buffer **blocked_list);




int main (int argc, char *argv[]) {

    struct client_addr *client_list = NULL;
    struct client_addr *request_msg_list = NULL;
    struct msg_buffer *ack_msg_list = NULL;
    struct msg_buffer *blocked_client_list = NULL;

    if (argc != 6) {
        fprintf(stderr, "Invalid input. Try ./upush_client <nick> "
                        "<ip-address> <port> <timeout> <loss_probability>\n");

        exit(EXIT_SUCCESS);
    }

    char *nick = argv[1]; 
    char *server_ip_char = argv[2];
    int server_port = atoi(argv[3]);
    int timeout = atoi(argv[4]);


    // Fikser sannsynlighet for pakketap.
    double loss_probability = (double) atoi(argv[5]);
    if (loss_probability < 0 || loss_probability > 100) {
        fprintf(stderr, "Please set loss probability to a value from 0 to 100\n");
        exit(EXIT_SUCCESS);
    }
    float loss_probability_correct = (float) loss_probability / 100;
    set_loss_probability(loss_probability_correct);



    // Lager socketen som brukes til lytting og sending av meldinger.
    int ret, socket_fd;

    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    check_error(socket_fd, "socket", -1);



    // Lager adressen som brukes for kommunikasjon med serveren.
    struct sockaddr_in server_addr;
    struct in_addr server_ip;
    ret = inet_pton(AF_INET, server_ip_char, &server_ip.s_addr);
    if (ret == 0 || ret == -1) {
        perror("inet_pton");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    server_addr.sin_addr = server_ip;


    // Setter opp for select og timer.
    struct timeval time;
    time.tv_sec = timeout;
    time.tv_usec = 0;

    // Lager et sett, read_fds, til lesing for select.
    fd_set read_fds;
    fd_set temp_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_fd, &read_fds);
    FD_SET(STDIN_FILENO, &read_fds);


    // Registrering hos server. Server_ack initialiseres med 1 til å begynne med, siden funksjonen umiddelbart vil returnere 0.
    int server_ack = 1;
    server_ack = set_next_ack(server_ack);
    char *reg_message = construct_registration_message(server_ack, nick);

    ret = send_packet(socket_fd, reg_message, strlen(reg_message), 0, (struct sockaddr *) &server_addr, sizeof(struct sockaddr_in));
    free(reg_message);
    if (ret == -1) {
        perror("send_packet");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    // Kopierer originalsettet til et midlertidig sett som brukes i select,
    // siden select endrer paa selve settet.
    temp_fds = read_fds;

    ret = select(socket_fd + 1, &temp_fds, NULL, NULL, &time);
    if (ret == 0) {
        fprintf(stderr, "TIMEOUT, COULDN'T REGISTER WITH SERVER\n");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    if (FD_ISSET(socket_fd, &temp_fds)) {

        char buf[BUF_SIZE] = {'\0'};

        ret = recv(socket_fd, buf, BUF_SIZE - 1, 0);
        if (ret == -1) {
            perror("recv");
            close(socket_fd);
            exit(EXIT_FAILURE);
        }

        printf("%s\n", buf);
    }

    // For heartbeat.
    time.tv_sec = 10;

    // Main event loop
    while (1) {

        // Denne while-loopen haandterer meldinger som ble sendt i loepet av stop-and-wait, som ikke
        // kunne blitt behandlet umiddelbart. Dette er bare meldinger som ble sendt i loepet av en stop-and-wait
        // som ikke dreiet seg om lookup.
        struct client_addr *old_request;
        while ((old_request = poll_client(&request_msg_list)) != NULL) {

            char buf[BUF_SIZE] = {'\0'};
            strcpy(buf, old_request->stored_request);

            struct sockaddr_in client_addr;
            client_addr.sin_family = AF_INET;
            client_addr.sin_port = htons(old_request->port);
            client_addr.sin_addr.s_addr = old_request->ip;

            free(old_request->stored_request);
            free(old_request);

            int check_format = test_format_text(buf, nick);

            // Galt nick. Denne beskjeden skulle ikke hit.
            if (check_format == 0) {

                fprintf(stderr, "MESSAGE WITH WRONG DESTINATION RECEIVED\n");

                // Sjekker om PKT-en og text er fra en tidligere melding.
                int current_pkt = extract_pkt_text(buf);

                char *from_nick = extract_nick_text(buf);
                if (from_nick == NULL) {
                    perror("malloc");
                    close(socket_fd);
                    free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                    exit(EXIT_FAILURE);
                }

                struct msg_buffer *previous_ack_msg = find_specified_msg(from_nick, ack_msg_list);

                char *ack_to_send;

                // Denne brukeren har ikke sendt en melding hit foer naa.
                if (previous_ack_msg == NULL) {

                    ack_to_send = construct_ack_wrong_name(current_pkt);
                    char *malloced_buf = strdup(buf);
                    if (malloced_buf == NULL || ack_to_send == NULL) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(from_nick);
                        exit(EXIT_FAILURE);
                    }

                    int malloc_check = register_msg(from_nick, malloced_buf, ack_to_send, current_pkt, &ack_msg_list);

                    if (malloc_check == 0) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(from_nick);
                        free(ack_to_send);
                        free(malloced_buf);
                        exit(EXIT_FAILURE);
                    }

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        exit(EXIT_FAILURE);
                    }
                }

                // Hvis identisk, send gamle ACK paa nytt.
                else if (current_pkt == previous_ack_msg->previous_ack_num) {
                    free(from_nick);

                    ack_to_send = previous_ack_msg->prepared_ack_message;

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }
                }

                // Ikke identisk. Lag og send ny ACK-melding. Oppdater ACK-informasjon i from_nick_client.
                else {
                    free(from_nick);
                    free(previous_ack_msg->stored_msg);
                    free(previous_ack_msg->prepared_ack_message);

                    ack_to_send = construct_ack_wrong_name(current_pkt);
                    char *malloced_buf = strdup(buf);
                    if (malloced_buf == NULL || ack_to_send == NULL) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }

                    previous_ack_msg->stored_msg = malloced_buf;
                    previous_ack_msg->prepared_ack_message = ack_to_send;
                    previous_ack_msg->previous_ack_num = current_pkt;

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }
                }
            }

            // Galt format. Beskjeden ble sendt med ugyldig format.
            else if (check_format == 1) {

                fprintf(stderr, "MESSAGE WITH WRONG FORMAT RECEIVED\n");

                // Sjekker om PKT-en og text er fra en tidligere melding.
                int current_pkt = extract_pkt_text (buf);

                char *from_nick = extract_nick_text (buf);
                if (from_nick == NULL) {
                    perror("malloc");
                    close(socket_fd);
                    free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                    exit(EXIT_FAILURE);
                }

                struct msg_buffer *previous_ack_msg = find_specified_msg(from_nick, ack_msg_list);

                char *ack_to_send;

                // Denne brukeren har ikke sendt en melding hit foer naa.
                if (previous_ack_msg == NULL) {

                    ack_to_send = construct_ack_wrong_format(current_pkt);
                    char *malloced_buf = strdup(buf);
                    if (malloced_buf == NULL || ack_to_send == NULL) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(from_nick);
                        exit(EXIT_FAILURE);
                    }

                    int malloc_check = register_msg(from_nick, malloced_buf, ack_to_send, current_pkt, &ack_msg_list);

                    if (malloc_check == 0) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(from_nick);
                        free(ack_to_send);
                        free(malloced_buf);
                        exit(EXIT_FAILURE);
                    }

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        exit(EXIT_FAILURE);
                    }
                }

                // Hvis identisk, send gamle ACK paa nytt.
                else if (current_pkt == previous_ack_msg->previous_ack_num) {
                    free(from_nick);

                    ack_to_send = previous_ack_msg->prepared_ack_message;

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }
                }

                // Ikke identisk. Lag og send ny ACK-melding. Oppdater ACK-informasjon i from_nick_client.
                else {
                    free(from_nick);
                    free(previous_ack_msg->stored_msg);
                    free(previous_ack_msg->prepared_ack_message);

                    ack_to_send = construct_ack_wrong_format(current_pkt);
                    char *malloced_buf = strdup(buf);
                    if (malloced_buf == NULL || ack_to_send == NULL) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }

                    previous_ack_msg->stored_msg = malloced_buf;
                    previous_ack_msg->prepared_ack_message = ack_to_send;
                    previous_ack_msg->previous_ack_num = current_pkt;

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }
                }
            }

            // Beskjeden skulle hit, og hadde riktig format.
            else if (check_format == 2) {
                
                // Sjekker om PKT-en og text er fra en tidligere melding.
                int current_pkt = extract_pkt_text (buf);

                char *from_nick = extract_nick_text (buf);
                char *msg = extract_msg_text (buf);
                if (from_nick == NULL || msg == NULL) {
                    perror("malloc");
                    close(socket_fd);
                    free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                    exit(EXIT_FAILURE);
                }

                struct msg_buffer *previous_ack_msg = find_specified_msg(from_nick, ack_msg_list);

                char *ack_to_send;

                // Denne brukeren har ikke sendt en melding hit foer naa.
                if (previous_ack_msg == NULL) {

                    struct msg_buffer *is_blocked = find_specified_msg(from_nick, blocked_client_list);
                    if (is_blocked == NULL) {
                        printf("%s: %s\n", from_nick, msg);
                    }
                    free(msg);

                    ack_to_send = construct_ack_correct(current_pkt);
                    char *malloced_buf = strdup(buf);
                    if (malloced_buf == NULL || ack_to_send == NULL) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(from_nick);
                        exit(EXIT_FAILURE);
                    }

                    int malloc_check = register_msg(from_nick, malloced_buf, ack_to_send, current_pkt, &ack_msg_list);

                    if (malloc_check == 0) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(from_nick);
                        free(ack_to_send);
                        free(malloced_buf);
                        exit(EXIT_FAILURE);
                    }

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        exit(EXIT_FAILURE);
                    }
                }

                // Hvis identisk, send gamle ACK paa nytt.
                else if (current_pkt == previous_ack_msg->previous_ack_num) {
                    free(from_nick);
                    free(msg);

                    ack_to_send = previous_ack_msg->prepared_ack_message;

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }
                }

                // Ikke identisk. Lag og send ny ACK-melding. Oppdater ACK-informasjon i from_nick_client.
                else {

                    struct msg_buffer *is_blocked = find_specified_msg(from_nick, blocked_client_list);
                    if (is_blocked == NULL) {
                        printf("%s: %s\n", from_nick, msg);
                    }
                    free(msg);

                    free(from_nick);
                    free(previous_ack_msg->stored_msg);
                    free(previous_ack_msg->prepared_ack_message);

                    ack_to_send = construct_ack_correct(current_pkt);
                    char *malloced_buf = strdup(buf);
                    if (malloced_buf == NULL || ack_to_send == NULL) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }
                
                    previous_ack_msg->stored_msg = malloced_buf;
                    previous_ack_msg->prepared_ack_message = ack_to_send;
                    previous_ack_msg->previous_ack_num = current_pkt;

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }
                }
            }

            // Kunne ikke hente PKT num.
            else if (check_format == 3) {
                fprintf(stderr, "UNABLE TO MAKE A RESPONSE TO RECEIVED MESSAGE\n");
            }
        } // END - old requests.

        // Soerger for at heartbeat-meldinger vil sendes.
        if (time.tv_sec == 0) {
            time.tv_sec = 10;
        }
        temp_fds = read_fds;

        // Denne select-en brukes bare for aa sjekke brukerinput, og nye meldinger fra andre klienter. 
        // ACK-er som denne klienten forventer haandteres andre steder. Her er trengs ingen timer.
        ret = select(socket_fd + 1, &temp_fds, NULL, NULL, &time);

        // Det har gaatt minst 10 sekunder. Send en registreringsmelding til serveren. Forventer ikke ACK.
        if (ret == 0) {
            ret = heartbeat(socket_fd, &server_ack, &server_addr, nick, 1, &request_msg_list);
            if (ret == 2) {
                perror("send_packet");
                close(socket_fd);
                free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                exit(EXIT_FAILURE);
            }
            else if (ret == 3) {
                perror("recvfrom");
                close(socket_fd);
                free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                exit(EXIT_FAILURE);
            }
            else if (ret == 4) {
                perror("malloc");
                close(socket_fd);
                free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                exit(EXIT_FAILURE);
            }
        }

        if (FD_ISSET(socket_fd, &temp_fds)) {

            char buf[BUF_SIZE] = {'\0'};
            struct sockaddr_in client_addr;
            socklen_t addrlen = sizeof(struct sockaddr_in);

            ret = recvfrom(socket_fd, buf, BUF_SIZE - 1, 0, (struct sockaddr *) &client_addr, &addrlen);
            if (ret == -1) {
                perror("recvfrom");
                close(socket_fd);
                free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                exit(EXIT_FAILURE);
            }

            int check_format = test_format_text(buf, nick);

            // Galt nick. Denne beskjeden skulle ikke hit.
            if (check_format == 0) {

                fprintf(stderr, "MESSAGE WITH WRONG DESTINATION RECEIVED\n");

                // Sjekker om PKT-en og text er fra en tidligere melding.
                int current_pkt = extract_pkt_text(buf);

                char *from_nick = extract_nick_text(buf);
                if (from_nick == NULL) {
                    perror("malloc");
                    close(socket_fd);
                    free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                    exit(EXIT_FAILURE);
                }

                struct msg_buffer *previous_ack_msg = find_specified_msg(from_nick, ack_msg_list);

                char *ack_to_send;

                // Denne brukeren har ikke sendt en melding hit foer naa.
                if (previous_ack_msg == NULL) {

                    ack_to_send = construct_ack_wrong_name(current_pkt);
                    char *malloced_buf = strdup(buf);
                    if (malloced_buf == NULL || ack_to_send == NULL) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(from_nick);
                        exit(EXIT_FAILURE);
                    }

                    int malloc_check = register_msg(from_nick, malloced_buf, ack_to_send, current_pkt, &ack_msg_list);

                    if (malloc_check == 0) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(from_nick);
                        free(ack_to_send);
                        free(malloced_buf);
                        exit(EXIT_FAILURE);
                    }

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        exit(EXIT_FAILURE);
                    }
                }

                // Hvis identisk, send gamle ACK paa nytt.
                else if (current_pkt == previous_ack_msg->previous_ack_num) {
                    free(from_nick);

                    ack_to_send = previous_ack_msg->prepared_ack_message;

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }
                }

                // Ikke identisk. Lag og send ny ACK-melding. Oppdater ACK-informasjon i from_nick_client.
                else {
                    free(from_nick);
                    free(previous_ack_msg->stored_msg);
                    free(previous_ack_msg->prepared_ack_message);

                    ack_to_send = construct_ack_wrong_name(current_pkt);
                    char *malloced_buf = strdup(buf);
                    if (malloced_buf == NULL || ack_to_send == NULL) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }

                    previous_ack_msg->stored_msg = malloced_buf;
                    previous_ack_msg->prepared_ack_message = ack_to_send;
                    previous_ack_msg->previous_ack_num = current_pkt;

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }
                }
            }

            // Galt format. Beskjeden ble sendt med ugyldig format.
            else if (check_format == 1) {

                fprintf(stderr, "MESSAGE WITH WRONG FORMAT RECEIVED\n");

                // Sjekker om PKT-en og text er fra en tidligere melding.
                int current_pkt = extract_pkt_text (buf);

                char *from_nick = extract_nick_text (buf);
                if (from_nick == NULL) {
                    perror("malloc");
                    close(socket_fd);
                    free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                    exit(EXIT_FAILURE);
                }

                struct msg_buffer *previous_ack_msg = find_specified_msg(from_nick, ack_msg_list);

                char *ack_to_send;

                // Denne brukeren har ikke sendt en melding hit foer naa.
                if (previous_ack_msg == NULL) {

                    ack_to_send = construct_ack_wrong_format(current_pkt);
                    char *malloced_buf = strdup(buf);
                    if (malloced_buf == NULL || ack_to_send == NULL) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(from_nick);
                        exit(EXIT_FAILURE);
                    }

                    int malloc_check = register_msg(from_nick, malloced_buf, ack_to_send, current_pkt, &ack_msg_list);

                    if (malloc_check == 0) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(from_nick);
                        free(ack_to_send);
                        free(malloced_buf);
                        exit(EXIT_FAILURE);
                    }

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        exit(EXIT_FAILURE);
                    }
                }

                // Hvis identisk, send gamle ACK paa nytt.
                else if (current_pkt == previous_ack_msg->previous_ack_num) {
                    free(from_nick);

                    ack_to_send = previous_ack_msg->prepared_ack_message;

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }
                }

                // Ikke identisk. Lag og send ny ACK-melding. Oppdater ACK-informasjon i from_nick_client.
                else {
                    free(from_nick);
                    free(previous_ack_msg->stored_msg);
                    free(previous_ack_msg->prepared_ack_message);

                    ack_to_send = construct_ack_wrong_format(current_pkt);
                    char *malloced_buf = strdup(buf);
                    if (malloced_buf == NULL || ack_to_send == NULL) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }

                    previous_ack_msg->stored_msg = malloced_buf;
                    previous_ack_msg->prepared_ack_message = ack_to_send;
                    previous_ack_msg->previous_ack_num = current_pkt;

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }
                }
            }

            // Beskjeden skulle hit, og hadde riktig format.
            else if (check_format == 2) {
                
                // Sjekker om PKT-en og text er fra en tidligere melding.
                int current_pkt = extract_pkt_text (buf);

                char *from_nick = extract_nick_text (buf);
                char *msg = extract_msg_text (buf);
                if (from_nick == NULL || msg == NULL) {
                    perror("malloc");
                    close(socket_fd);
                    free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                    exit(EXIT_FAILURE);
                }

                struct msg_buffer *previous_ack_msg = find_specified_msg(from_nick, ack_msg_list);

                char *ack_to_send;

                // Denne brukeren har ikke sendt en melding hit foer naa.
                if (previous_ack_msg == NULL) {

                    struct msg_buffer *is_blocked = find_specified_msg(from_nick, blocked_client_list);
                    if (is_blocked == NULL) {
                        printf("%s: %s\n", from_nick, msg);
                    }
                    free(msg);

                    ack_to_send = construct_ack_correct(current_pkt);
                    char *malloced_buf = strdup(buf);
                    if (malloced_buf == NULL || ack_to_send == NULL) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(from_nick);
                        exit(EXIT_FAILURE);
                    }

                    int malloc_check = register_msg(from_nick, malloced_buf, ack_to_send, current_pkt, &ack_msg_list);

                    if (malloc_check == 0) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(from_nick);
                        free(ack_to_send);
                        free(malloced_buf);
                        exit(EXIT_FAILURE);
                    }

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        exit(EXIT_FAILURE);
                    }
                }

                // Hvis identisk, send gamle ACK paa nytt.
                else if (current_pkt == previous_ack_msg->previous_ack_num) {
                    free(from_nick);
                    free(msg);

                    ack_to_send = previous_ack_msg->prepared_ack_message;

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }
                }

                // Ikke identisk. Lag og send ny ACK-melding. Oppdater ACK-informasjon i from_nick_client.
                else {

                    struct msg_buffer *is_blocked = find_specified_msg(from_nick, blocked_client_list);
                    if (is_blocked == NULL) {
                        printf("%s: %s\n", from_nick, msg);
                    }
                    free(msg);

                    free(from_nick);
                    free(previous_ack_msg->stored_msg);
                    free(previous_ack_msg->prepared_ack_message);

                    ack_to_send = construct_ack_correct(current_pkt);
                    char *malloced_buf = strdup(buf);
                    if (malloced_buf == NULL || ack_to_send == NULL) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }
                
                    previous_ack_msg->stored_msg = malloced_buf;
                    previous_ack_msg->prepared_ack_message = ack_to_send;
                    previous_ack_msg->previous_ack_num = current_pkt;

                    ret = send_packet(socket_fd, ack_to_send, strlen(ack_to_send), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                        exit(EXIT_FAILURE);
                    }
                }
            }

            // Kunne ikke hente PKT num.
            else if (check_format == 3) {
                fprintf(stderr, "UNABLE TO MAKE A RESPONSE TO RECEIVED MESSAGE\n");
            }
        } // END - Read socket.





        // Sjekker om klienten vil skrive til en annen klient, eller avslutte.
        if (FD_ISSET(STDIN_FILENO, &temp_fds)) {

            char buf[BUF_SIZE] = {'\0'}; 

            ret = read(STDIN_FILENO, buf, BUF_SIZE - 1);
            if (ret == -1) {
                perror("read");
                close(socket_fd);
                free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                exit(EXIT_FAILURE);
            }

            if (buf[strlen(buf) - 1] == '\n') {
                buf[strlen(buf) - 1] = '\0';
            }

            int check_format = test_format_stdin(buf);

            // tonick meldinger.
            if (check_format == 1) {
                
                char *tonick = extract_nick_stdin(buf);
                char *message = extract_msg_stdin(buf);

                if (tonick == NULL || message == NULL) {
                    perror("malloc");
                    close(socket_fd);
                    free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                    exit(EXIT_FAILURE);
                }

                int is_blocked = 0;

                struct msg_buffer *blocked_client = find_specified_msg(tonick, blocked_client_list);
                if (blocked_client != NULL) {
                    is_blocked = 1;
                }

                struct client_addr *target_addr = find_specified_client(tonick, client_list);
                if (target_addr != NULL && is_blocked == 0) {

                    // Bruk denne adressen og send direkte.
                    int send_check = send_text_message(socket_fd, &server_ack, &server_addr, &client_list, nick, tonick, timeout, &request_msg_list, target_addr, message);

                    if (send_check == 2) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(tonick);
                        free(message);
                        exit(EXIT_FAILURE);
                    }

                    else if (send_check == 3) {
                        perror("recvfrom");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(tonick);
                        free(message);
                        exit(EXIT_FAILURE);
                    }

                    // Serveren responderer ikke.
                    else if (send_check == 4) {
                        free(tonick);
                        free(message);

                        break;
                    }

                    else if (send_check == 7) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(tonick);
                        free(message);
                        exit(EXIT_FAILURE);
                    }

                    free(tonick);
                    free(message);
                }
                else if (is_blocked == 0) {

                    printf("USER %s NOT KNOWN, ASKING SERVER\n", tonick);

                    // Forsoeker maks 3 ganger aa gjoere ett oppslag hos serveren.
                    int lookup_check = server_lookup_client(socket_fd, &server_ack, &server_addr, &client_list, tonick, timeout);

                    // Server har ikke info om klienten.
                    if (lookup_check == 0) {
                        fprintf(stderr, "NICK %s NOT REGISTERED\n", tonick);
                        free(tonick);
                        free(message);
                    }

                    // Klienten ble lagt til i client_list. Denne koden er lik for - if (target_addr != NULL) over.
                    else if (lookup_check == 1) {

                        printf("USER %s SAVED\n", tonick);

                        target_addr = find_specified_client(tonick, client_list);
                        
                        // Bruk denne adressen og send direkte.
                        int send_check = send_text_message(socket_fd, &server_ack, &server_addr, &client_list, nick, tonick, timeout, &request_msg_list, target_addr, message);

                        if (send_check == 2) {
                            perror("send_packet");
                            close(socket_fd);
                            free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                            free(tonick);
                            free(message);
                            exit(EXIT_FAILURE);
                        }

                        else if (send_check == 3) {
                            perror("recvfrom");
                            close(socket_fd);
                            free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                            free(tonick);
                            free(message);
                            exit(EXIT_FAILURE);
                        }

                        // Serveren responderer ikke.
                        else if (send_check == 4) {
                            free(tonick);
                            free(message);

                            break;
                        }

                        else if (send_check == 7) {
                            perror("malloc");
                            close(socket_fd);
                            free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                            free(tonick);
                            free(message);
                            exit(EXIT_FAILURE);
                        }

                        free(tonick);
                        free(message);
                    }

                    // Error med send_packet.
                    else if (lookup_check == 2) {
                        perror("send_packet");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(tonick);
                        free(message);
                        exit(EXIT_FAILURE);
                    }

                    // Error med recvfrom.
                    else if (lookup_check == 3) {
                        
                        perror("recvfrom");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(tonick);
                        free(message);
                        exit(EXIT_FAILURE);
                    }

                    // Serveren responderte aldri. Avslutt program.
                    else if (lookup_check == 4) {
                        free(tonick);
                        free(message);

                        break;
                    }

                    // Malloc feil.
                    else if (lookup_check == 5) {
                        perror("malloc");
                        close(socket_fd);
                        free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

                        free(tonick);
                        free(message);
                        exit(EXIT_FAILURE);
                    }
                }
                
                else if (is_blocked == 1) {
                    fprintf(stderr, "UNBLOCK %s TO SEND A MESSAGE\n", tonick);
                    free(tonick);
                    free(message);
                } // END - LOOKUP else.
            } // END - tonick if.

            // BLOCK. Nick_to_block skal brukes i structen og maa ikke frigjoeres her.
            else if (check_format == 3) {
                char *nick_to_block = extract_nick_stdin_block(buf);
                if (nick_to_block == NULL) {
                    perror("malloc");
                    close(socket_fd);
                    free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                    exit(EXIT_FAILURE);
                }

                int malloc_check = block_client(nick_to_block, &blocked_client_list);
                if (malloc_check == 0) {
                    perror("malloc");
                    close(socket_fd);
                    free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                    free(nick_to_block);
                    exit(EXIT_FAILURE);
                }
            }

            // UNBLOCK. Nick_to_unblock maa frigjoeres her.
            else if (check_format == 4) {
                char *nick_to_unblock = extract_nick_stdin_block(buf);
                if (nick_to_unblock == NULL) {
                    perror("malloc");
                    close(socket_fd);
                    free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);
                    exit(EXIT_FAILURE);
                }

                unblock_client(nick_to_unblock, &blocked_client_list);
                free(nick_to_unblock);
            }

            // Bryter seg ut av loopen og avslutter klienten.
            else if (strcmp(buf, "QUIT") == 0) {
                break;
            }
        }
    } // END - Main event loop.

    printf("Ending program\n");

    free_structs(client_list, request_msg_list, ack_msg_list, blocked_client_list);

    close(socket_fd);
    return EXIT_SUCCESS;
}












// HER ER ALLE FUNKSJONER SOM ER LAGET I UPUSH_CLIENT.


// Returnerer 1 hvis ACK ble mottatt, og 0 hvis brukeren ikke er registrert. Errno-feil vil returnere 2 eller 3.
// Hvis serveren ikke responderer returneres 4. Hvis brukeren ikke kan naas, returneres 5.
// Galt format vil returnere 6. Malloc feil vil returnere 7.
int send_text_message (int socket_fd, int *server_ack_num, struct sockaddr_in *server_addr_ptr, struct client_addr **client_list, char *fromnick, char *tonick, int timeout, struct client_addr **request_msg_list, struct client_addr *target_client, char *message) {

    target_client->current_pkt_num = set_next_ack(target_client->current_pkt_num);
    int temp_ack = target_client->current_pkt_num;
    int ret;

    struct sockaddr_in target_addr;

    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(target_client->port);
    target_addr.sin_addr.s_addr = target_client->ip;

    char *text_message = construct_text_message(temp_ack, fromnick, tonick, message);
    if (text_message == NULL) {
        return 7;
    }

    for (int i = 0; i < 4; i++) {

        // Gjoer et lookup til serveren og oppdater adressen. Hvis den ikke lenger finnes, returner fra send_text_message.
        if (i == 2) {
            int lookup_check = server_lookup_client(socket_fd, server_ack_num, server_addr_ptr, client_list, tonick, timeout);

            // Server har ikke info om klienten.
            if (lookup_check == 0) {
                free(text_message);
                fprintf(stderr, "NICK %s NOT REGISTERED\n", tonick);
                return 0;
            }

            // Klienten ble lagt til i client_list. Oppdaterer target_addr med ny informasjon.
            else if (lookup_check == 1) {
                target_client = find_specified_client(tonick, *client_list);
                target_addr.sin_port = htons(target_client->port);
                target_addr.sin_addr.s_addr = target_client->ip;
            }

            // Error med send_packet.
            else if (lookup_check == 2) {
                free(text_message);
                return 2;
            }

            // Error med recvfrom.
            else if (lookup_check == 3) {
                free(text_message);
                return 3;
            }

            // Serveren responderte aldri. Avslutt program.
            else if (lookup_check == 4) {
                free(text_message);
                return 4;
            }

            // Malloc feil.
            else if (lookup_check == 5) {
                free(text_message);
                return 7;
            }
        }

        ret = send_packet(socket_fd, text_message, strlen(text_message), 0, (struct sockaddr *) &target_addr, sizeof(struct sockaddr_in));
        if (ret == -1) {
            return 2;
        }

        int while_check = 1;

        // Midlertidig timer for aa sjekke om klienten responderer med riktig ACK.
        struct timeval temp_time;
        temp_time.tv_sec = timeout;
        temp_time.tv_usec = 0;

        while (while_check) {

            // Midlertidig fd_set for en in-loop select.
            fd_set temp_fds;
            FD_ZERO(&temp_fds);
            FD_SET(socket_fd, &temp_fds);

            ret = select(socket_fd + 1, &temp_fds, NULL, NULL, &temp_time);

            // Her maa det sjekkes om det er brukeren som responderer.
            if (ret == 0) {
                fprintf(stderr, "TIMEOUT! USER DID NOT RESPOND WITH ACK (%d of 4).\n", i + 1);
                while_check = 0;
            }
            else {

                char temp_buf[BUF_SIZE] = {'\0'}; 
                struct sockaddr_in temp_addr;
                socklen_t temp_addrlen = sizeof(struct sockaddr_in);

                ret = recvfrom(socket_fd, temp_buf, BUF_SIZE - 1, 0, (struct sockaddr *) &temp_addr, &temp_addrlen);
                if (ret == -1) {
                    return 3;
                }

                // Sjekker om det er brukerem som responderte, hvis saa sjekk ack. Hvis ikke, lagre melding.
                if (temp_addr.sin_port == target_addr.sin_port) {
                    if (temp_addr.sin_addr.s_addr == target_addr.sin_addr.s_addr) {

                        // Sjekk svaret fra brukeren.
                        int ack_check = test_format_client_ack(temp_buf, temp_ack);
                        
                        // Ugyldig ACK.
                        if (ack_check == 0) {
                            fprintf(stderr, "RECEIVED INVALID ACK FROM USER!\n");
                        }

                        // Riktig ACK. Suksess!
                        else if (ack_check == 1) {
                            free(text_message);
                            fprintf(stderr, "MESSAGE SENT SUCCESSFULLY.\n");
                            return 1;
                        }

                        // Feil navn. Brukeren var ikke riktig.
                        else if (ack_check == 2) {
                            fprintf(stderr, "MESSAGE TO USER WAS SENT TO WRONG ADDRESS!\n");
                        }

                        // Melding ble sendt med feil format.
                        else if (ack_check == 3) {
                            free(text_message);
                            fprintf(stderr, "MESSAGE WAS SENT WITH WRONG FORMAT!\n");
                            return 6;
                        }

                    }
                }

                // En annen bruker enn den forventede svarte. Lagrer meldingen.
                else {

                    char *request = strdup(temp_buf);

                    // Malloc feil.
                    if (request == NULL) {
                        free(text_message);
                        return 7;
                    }

                    int sender_port = ntohs(temp_addr.sin_port);
                    int sender_ip = temp_addr.sin_addr.s_addr;
                    int malloc_check = register_client(NULL, request, sender_port, sender_ip, request_msg_list);

                    // Malloc feil.
                    if (malloc_check == 0) {
                        return 7;
                    }

                }
            }
        } // END - while loop.
    }

    free(text_message);
    fprintf(stderr, "NICK %s UNREACHABLE\n", tonick);
    return 5;
}

// Denne metoden returnerer 1 hvis en klient ble funnet og lagt paa client_list. Returnerer 0 hvis ikke funnet.
// Hvis serveren ikke responderer returneres 4. Errno-feil vil returnere 2 eller 3. Malloc feil returnerer 5.
int server_lookup_client (int socket_fd, int *server_ack_num, struct sockaddr_in *server_addr_ptr, struct client_addr **client_list, char *tonick, int timeout) {

    *server_ack_num = set_next_ack(*server_ack_num);

    struct sockaddr_in server_addr = *server_addr_ptr;
    int ret;

    int temp_ack = *server_ack_num;

    char *lookup_message = construct_lookup_message(temp_ack, tonick);
    if (lookup_message == NULL) {
        return 5;
    }

    for (int i = 0; i < 3; i++) {

        // Gjoer en lookup til serveren, og lagre adressen i client_list hvis den finnes.

        ret = send_packet(socket_fd, lookup_message, strlen(lookup_message), 0, (struct sockaddr *) &server_addr, sizeof(struct sockaddr_in));
        if (ret == -1) {
            free(lookup_message);
            return 2;
        }

        int while_check = 1;

        // Midlertidig timer for aa sjekke om serveren responderer.
        struct timeval temp_time;
        temp_time.tv_sec = timeout;
        temp_time.tv_usec = 0;

        // Loopen stopper hvis det blir en timeout i den indre select-en.
        while (while_check) {

            // Midlertidig fd_set for en in-loop select.
            fd_set temp_fds;
            FD_ZERO(&temp_fds);
            FD_SET(socket_fd, &temp_fds);

            ret = select(socket_fd + 1, &temp_fds, NULL, NULL, &temp_time);

            // Her maa det sjekkes om det er serveren som responderer.
            if (ret == 0) {
                fprintf(stderr, "TIMEOUT! SERVER DID NOT RESPOND REGARDING LOOKUP (%d of 3).\n", i + 1);
                while_check = 0;
            }
            else {

                char temp_buf[BUF_SIZE] = {'\0'}; 
                struct sockaddr_in temp_addr;
                socklen_t temp_addrlen = sizeof(struct sockaddr_in);

                ret = recvfrom(socket_fd, temp_buf, BUF_SIZE - 1, 0, (struct sockaddr *) &temp_addr, &temp_addrlen);
                if (ret == -1) {
                    free(lookup_message);
                    return 3;
                }

                // Sjekker om det er serveren som responderte, hvis saa lagre klient-info. Hvis ikke, ignorer melding.
                if (temp_addr.sin_port == server_addr.sin_port) {
                    if (temp_addr.sin_addr.s_addr == server_addr.sin_addr.s_addr) {

                        // Sjekk svaret fra serveren.
                        int ack_check = test_format_server_ack(temp_buf, temp_ack);

                        // Ugyldig ACK.
                        if (ack_check == 0) {
                            fprintf(stderr, "SERVER RESPONSE INVALID\n");
                        }

                        // Klient funnet. Lagrer klienten i client_list.
                        else if (ack_check == 1) {

                            char *target_nick  = extract_nick_server_lookup(temp_buf);
                            int target_port = extract_port_server_lookup(temp_buf);
                            char *target_ip_string = extract_ip_server_lookup(temp_buf);

                            // Malloc feil.
                            if (target_nick == NULL || target_ip_string == NULL) {
                                free(lookup_message);
                                return 5;
                            }

                            struct in_addr target_ip;
                            ret = inet_pton(AF_INET, target_ip_string, &target_ip.s_addr);

                            int malloc_check = register_client(target_nick, NULL, target_port, (int) target_ip.s_addr, client_list);

                            // Malloc feil.
                            if (malloc_check == 0) {
                                return 5;
                            }

                            free(target_ip_string);

                            free(lookup_message);
                            return 1;
                        }

                        // Klient ikke funnet.
                        else if (ack_check == 2) {
                            free(lookup_message);
                            return 0;
                        }

                        // Ugyldig format.
                        else if (ack_check == 3) {
                            fprintf(stderr, "SERVER RESPONSE INVALID\n");
                        }
                    }
                }
            }
        } // END - while loop.

    } // END - attempt 2 more times for loop.

    free(lookup_message);
    return 4;   
}

// Sender bare registrering 1 gang. Venter paa svar fra serveren i maks 2 sekunder.
// Lagrer meldinger fra andre klienter i en liste.
// Returnerer 1 hvis serveren svarte eller timeout. 2 og 3 ved errno. Returnerer 4 ved malloc feil.
int heartbeat (int socket_fd, int *server_ack_num, struct sockaddr_in *server_addr_ptr, char *nick, int timeout, struct client_addr **request_msg_list) {

    *server_ack_num = set_next_ack(*server_ack_num);

    struct sockaddr_in server_addr = *server_addr_ptr;
    int ret;

    int temp_ack = *server_ack_num;

    char *registration_message = construct_registration_message(temp_ack, nick);
    if (registration_message == NULL) {
        return 4;
    }

    ret = send_packet(socket_fd, registration_message, strlen(registration_message), 0, (struct sockaddr *) &server_addr, sizeof(struct sockaddr_in));
    free(registration_message);
    if (ret == -1) {
        return 2;
    }

    // Midlertidig timer for aa sjekke om serveren responderer.
    struct timeval temp_time;
    temp_time.tv_sec = timeout;
    temp_time.tv_usec = 0;

    while (1) {

        // Midlertidig fd_set for en in-loop select.
        fd_set temp_fds;
        FD_ZERO(&temp_fds);
        FD_SET(socket_fd, &temp_fds);

        ret = select(socket_fd + 1, &temp_fds, NULL, NULL, &temp_time);

        // Timeout, returnerer fra heartbeat.
        if (ret == 0) {
            return 1;
        }
        else {

            char temp_buf[BUF_SIZE] = {'\0'}; 
            struct sockaddr_in temp_addr;
            socklen_t temp_addrlen = sizeof(struct sockaddr_in);

            ret = recvfrom(socket_fd, temp_buf, BUF_SIZE - 1, 0, (struct sockaddr *) &temp_addr, &temp_addrlen);
            if (ret == -1) {
                free(registration_message);
                return 3;
            }

            // Sjekker om det er serveren som responderte, hvis saa returner.
            if (temp_addr.sin_port == server_addr.sin_port) {
                if (temp_addr.sin_addr.s_addr == server_addr.sin_addr.s_addr) {
                    return 1;
                }
            }
            else {
                char *request = strdup(temp_buf);

                // Malloc feil.
                if (request == NULL) {
                    free(registration_message);
                    return 4;
                }

                int sender_port = ntohs(temp_addr.sin_port);
                int sender_ip = temp_addr.sin_addr.s_addr;
                int malloc_check = register_client(NULL, request, sender_port, sender_ip, request_msg_list);

                // Malloc feil.
                if (malloc_check == 0) {
                    return 4;
                }
            }
        }
    } // END - while loop.
}

// Siden nick allerede er lagret paa heapen, trengs ikke strdup.
// Returnerer 0 ved malloc feil.
int register_client (char *nick, char *request, int port, int ip, struct client_addr **start) {

    struct client_addr *client = malloc(sizeof(struct client_addr));
    if (client == NULL) {
        return 0;
    }

    client->nick = nick;
    client->stored_request = request;
    client->port = port;
    client->ip = ip;
    client->next = NULL;

    client->current_pkt_num = 0;

    // Nick er NULL for alle i request_msg_list. Disse toemmes via poll_client.
    if (nick != NULL) {
        struct client_addr *old_addr = remove_specified_client(nick, start);
        if (old_addr != NULL) {

            if (old_addr->nick != NULL) {
                free(old_addr->nick);
            }
            if (old_addr->stored_request != NULL) {
                free(old_addr->stored_request);
            }

            client->current_pkt_num = old_addr->current_pkt_num;

            free(old_addr);
        }
    }

    push_client(client, start);
    return 1;
}

// Returnerer 0 ved malloc feil.
int block_client (char *nick, struct msg_buffer **blocked_list) {

    struct msg_buffer *old_blocked_client = remove_specified_msg(nick, blocked_list);

    if (old_blocked_client != NULL) {
        free(old_blocked_client->nick);
        free(old_blocked_client);
    }

    struct msg_buffer *blocked_client = malloc(sizeof(struct msg_buffer));
    if (blocked_client == NULL) {
        return 0;
    }

    blocked_client->nick = nick;
    blocked_client->stored_msg = NULL;
    blocked_client->prepared_ack_message = NULL;
    blocked_client->next = NULL;

    push_msg(blocked_client, blocked_list);

    printf("USER %s BLOCKED\n", nick);
    return 1;
}

void unblock_client (char *nick, struct msg_buffer **blocked_list) {
    
    struct msg_buffer *unblocked_client = remove_specified_msg(nick, blocked_list);

    if (unblocked_client != NULL) {
        free(unblocked_client->nick);
        free(unblocked_client);
    }

    printf("USER %s UNBLOCKED\n", nick);
}

// Siden nick og message allerede er lagret paa heapen, trengs ikke strdup.
// Returnerer 0 ved malloc feil.
int register_msg (char *nick, char *stored_msg, char *prepared_ack_msg, int previous_ack_num, struct msg_buffer **start) {

    struct msg_buffer *msg = malloc(sizeof(struct msg_buffer));
    if (msg == NULL) {
        return 0;
    }

    msg->nick = nick;
    msg->stored_msg = stored_msg;
    msg->prepared_ack_message = prepared_ack_msg;
    msg->previous_ack_num = previous_ack_num;
    msg->next = NULL;

    struct msg_buffer *old_msg = remove_specified_msg(nick, start);
    if (old_msg != NULL) {
        if (old_msg->prepared_ack_message != NULL) {
            free(old_msg->prepared_ack_message);
        }

        if (old_msg->stored_msg != NULL) {
            free(old_msg->stored_msg);
        }

        if (old_msg->nick != NULL) {
            free(old_msg->nick);
        }
        free(old_msg);
    }

    push_msg(msg, start);
    return 1;
}

// Returnerer NULL ved malloc feil.
char *construct_registration_message (int ack_num, char *nick) {
    char *result = malloc(sizeof(char) * BUF_SIZE);
    if (result == NULL) {
        return NULL;
    }

    memset(result, '\0', BUF_SIZE);

    int result_counter = 0;
    char ack_char = ack_num + '0';
    
    memcpy(result, "PKT ", 4);
    result_counter += 4;

    result[result_counter] = ack_char;
    result_counter += 1;

    memcpy(&result[result_counter], " REG ", 5);
    result_counter += 5;

    memcpy(&result[result_counter], nick, strlen(nick));

    return result;
}

// Returnerer NULL ved malloc feil.
char *construct_lookup_message (int ack_num, char *nick) {
    char *result = malloc(sizeof(char) * BUF_SIZE);
    if (result == NULL) {
        return NULL;
    }

    memset(result, '\0', BUF_SIZE);

    int result_counter = 0;
    char ack_char = ack_num + '0';
    
    memcpy(result, "PKT ", 4);
    result_counter += 4;

    result[result_counter] = ack_char;
    result_counter += 1;

    memcpy(&result[result_counter], " LOOKUP ", 8);
    result_counter += 8;

    memcpy(&result[result_counter], nick, strlen(nick));

    return result;
}

// Returnerer NULL ved malloc feil.
char *construct_text_message (int ack_num, char *from_nick, char *to_nick, char* text) {
    char *result = malloc(sizeof(char) * BUF_SIZE);
    if (result == NULL) {
        return NULL;
    }

    memset(result, '\0', BUF_SIZE);

    int result_counter = 0;
    char ack_char = ack_num + '0';
    
    memcpy(result, "PKT ", 4);
    result_counter += 4;

    result[result_counter] = ack_char;
    result_counter += 1;

    memcpy(&result[result_counter], " FROM ", 6);
    result_counter += 6;

    memcpy(&result[result_counter], from_nick, strlen(from_nick));
    result_counter += strlen(from_nick);

    memcpy(&result[result_counter], " TO ", 4);
    result_counter += 4;

    memcpy(&result[result_counter], to_nick, strlen(to_nick));
    result_counter += strlen(to_nick);

    memcpy(&result[result_counter], " MSG ", 5);
    result_counter += 5;

    memcpy(&result[result_counter], text, strlen(text));
    result_counter += strlen(text);

    return result;
}

// Returnerer NULL ved malloc feil.
char *construct_ack_correct (int ack_num) {
    char *result = malloc(sizeof(char) * BUF_SIZE);
    if (result == NULL) {
        return NULL;
    }

    memset(result, '\0', BUF_SIZE);

    int result_counter = 0;
    char ack_char = ack_num + '0';
    
    memcpy(result, "ACK ", 4);
    result_counter += 4;

    result[result_counter] = ack_char;
    result_counter += 1;

    memcpy(&result[result_counter], " OK", 3);
    
    return result;
}

// Returnerer NULL ved malloc feil.
char *construct_ack_wrong_name (int ack_num) {
    char *result = malloc(sizeof(char) * BUF_SIZE);
    if (result == NULL) {
        return NULL;
    }

    memset(result, '\0', BUF_SIZE);

    int result_counter = 0;
    char ack_char = ack_num + '0';
    
    memcpy(result, "ACK ", 4);
    result_counter += 4;

    result[result_counter] = ack_char;
    result_counter += 1;

    memcpy(&result[result_counter], " WRONG NAME", 11);
    
    return result;
}

// Returnerer NULL ved malloc feil.
char *construct_ack_wrong_format (int ack_num) {
    char *result = malloc(sizeof(char) * BUF_SIZE);
    if (result == NULL) {
        return NULL;
    }

    memset(result, '\0', BUF_SIZE);

    int result_counter = 0;
    char ack_char = ack_num + '0';
    
    memcpy(result, "ACK ", 4);
    result_counter += 4;

    result[result_counter] = ack_char;
    result_counter += 1;

    memcpy(&result[result_counter], " WRONG FORMAT", 13);
    
    return result;
}

// Returnerer 0 hvis ugyldig ack, 1 hvis klient funnet, 2 hvis klient ikke funnet, 3 hvis ugyldig format.
int test_format_server_ack (char *message, int ack_num) {

    char copy[BUF_SIZE] = {'\0'};
    strcpy(copy, message);

    char *delimiter = " ";

    char *buf1 = strtok(copy, delimiter);
    char *buf2 = strtok(NULL, delimiter);
    char *buf3 = strtok(NULL, delimiter);

    if (strcmp(buf1, "ACK") == 0) {

        if (buf2[0] == (ack_num + '0')) {

            // Klient var funnet.
            if (strcmp(buf3, "NICK") == 0) {
                return 1;
            }

            // Klient var ikke funnet.
            else if (strcmp(buf3, "NOT") == 0) {

                return 2;
            }
        }

        // Ugyldig ACK.
        else {
            return 0;
        }
    }
    return 3;
}

// Returnerer 0 hvis ugyldig ACK, 1 hvis riktig. 2 hvis den er fra en annen bruker, 3 hvis ugyldig format.
// Returnerer 4 hvis mottakeren av ACK-en mottok ACK med galt format.
int test_format_client_ack (char *message, int ack_num) {

    char copy[BUF_SIZE] = {'\0'};
    strcpy(copy, message);

    char *delimiter = " ";

    char *buf1 = strtok(copy, delimiter);
    char *buf2 = strtok(NULL, delimiter);
    char *buf3 = strtok(NULL, delimiter);
    char *buf4 = strtok(NULL, delimiter);

    if (strcmp(buf1, "ACK") == 0) {

        if (buf2[0] == (ack_num + '0')) {

            if (strcmp(buf3, "OK") == 0) {
                return 1;
            }

            else if (strcmp(buf3, "WRONG") == 0) {

                if (strcmp(buf4, "NAME") == 0) {
                    return 2;
                }

                else if (strcmp(buf4, "FORMAT") == 0) {
                    return 3;
                }
            }
        }
        // Ugyldig ACK.
        else {
            return 0;
        }
    }
    return 4;
}

// Stoler paa at nick ikke skrives med mellomrom.
// Returnerer 1 ved tonick, 2 ved galt input, 3 ved block, 4 ved unblock, og 5 ved QUIT. 
int test_format_stdin (char *message) {
    char copy[BUF_SIZE] = {'\0'};
    strcpy(copy, message);

    char *delimiter = " ";

    char *buf1 = strtok(copy, delimiter);
    char *buf2 = strtok(NULL, delimiter);

    if (buf1 == NULL) {
        fprintf(stderr, "WRONG FORMAT FROM INPUT!\n");
        return 2;
    }

    if (buf2 == NULL) {
        if (strcmp(buf1, "QUIT") == 0) {
            return 5;
        }
        else {
            fprintf(stderr, "WRONG FORMAT FROM INPUT!\n");
            return 2;
        }
    }

    char space = 20;
    char tab = 9;
    char return_char = 13; 

    if (buf1[0] == '@') {
        
        char *buf3 = &buf1[1];
        int len3 = strlen(buf3);

        for (int i = 0; i < (int) len3; i++) {
            if (isascii(buf3[i]) == 0 || buf3[i] == space || buf3[i] == tab || buf3[i] == return_char) {
                fprintf(stderr, "NICK MUST CONTAIN ASCII\n");
                return 2;
            }
        }
        return 1;
    }

    char *buf3 = strtok(NULL, delimiter);
    if (buf3 != NULL) {
        fprintf(stderr, "WRONG FORMAT FROM INPUT!\n");
        return 2;
    }

    int len2 = strlen(buf2);


    if (strcmp(buf1, "BLOCK") == 0) {
        for (int i = 0; i < (int) len2; i++) {
            if (isascii(buf2[i]) == 0 || buf2[i] == space || buf2[i] == tab || buf2[i] == return_char) {
                fprintf(stderr, "WRONG FORMAT FROM INPUT!\n");
                return 2;
            }
        }
        return 3;
    }

    if (strcmp(buf1, "UNBLOCK") == 0) {
        for (int i = 0; i < (int) len2; i++) {
            if (isascii(buf2[i]) == 0 || buf2[i] == space || buf2[i] == tab || buf2[i] == return_char) {
                fprintf(stderr, "WRONG FORMAT FROM INPUT!\n");
                return 2;
            }
        }
        return 4;
    }

    fprintf(stderr, "WRONG FORMAT FROM INPUT!\n");
    return 2;
}

// Returnerer 0 hvis galt nick, 1 hvis galt format, 2 hvis ok.
// Returnerer 3 hvis PKT num ikke kan hentes.
int test_format_text (char *message, char *nick) {
    char copy[BUF_SIZE] = {'\0'};
    strcpy(copy, message);

    char *delimiter = " ";

    char *buf1 = strtok(copy, delimiter);
    char *buf2 = strtok(NULL, delimiter);
    char *buf3 = strtok(NULL, delimiter);
    char *buf4 = strtok(NULL, delimiter);
    char *buf5 = strtok(NULL, delimiter);
    char *buf6 = strtok(NULL, delimiter);
    char *buf7 = strtok(NULL, delimiter);
    char *buf8 = strtok(NULL, delimiter);

    if (buf1 == NULL || buf2 == NULL || buf3 == NULL || buf4 == NULL) {
        return 3;
    }

    if (buf5 == NULL || buf6 == NULL || buf7 == NULL || buf8 == NULL) {
        return 3;
    }

    if (strcmp(buf1, "PKT") == 0) {

        if (buf2[0] == (0 + '0') || buf2[0] == (1 + '0')) {

            if (strcmp(buf3, "FROM") == 0) {

                if (strcmp(buf5, "TO") == 0) {

                    if (strcmp(buf6, nick) == 0) {

                        // Gyldig melding.
                        if (strcmp(buf7, "MSG") == 0) {
                            
                            int len8 = strlen(buf8);

                            for (int i = 0; i < len8; i++) {
                                if (isascii(buf8[i]) == 0) {
                                    return 1;
                                }
                            }

                            while ((buf8 = strtok(NULL, delimiter)) != NULL) {
                                len8 = strlen(buf8);

                                for (int i = 0; i < len8; i++) {
                                    if (isascii(buf8[i]) == 0) {
                                        return 1;
                                    }
                                }
                            }
                            
                            return 2;
                        }
                        return 1;
                    }
                    // Galt nick.
                    return 0;
                }
                return 1;
            }
            return 1;
        }
        return 3;
    }
    return 3;
}

// Returnerer NULL ved malloc feil.
char *extract_nick_stdin (char *buf) {
    char copy[BUF_SIZE] = {'\0'};
    strcpy(copy, &buf[1]);

    char *delimiter = " ";
    char *buf1 = strtok(copy, delimiter);

    char *nick = strdup(buf1);

    return nick;
}

// Soerger for at meldingen ikke er lengre enn 1400 bytes.
// Returnerer NULL ved malloc feil.
char *extract_msg_stdin (char *buf) {
    
    char copy[BUF_SIZE] = {'\0'};
    strcpy(copy, buf);

    char *delimiter = " ";
    char *buf1 = strtok(copy, delimiter);

    int proper_start = strlen(buf1) + 1;

    if (strlen(&buf[proper_start]) > 1400) {
        buf[proper_start + 1400] = '\0';
    }

    char *msg = strdup(&buf[proper_start]);

    return msg;
}

// Returnerer NULL ved malloc feil.
char *extract_nick_stdin_block(char *buf) {
    char copy[BUF_SIZE] = {'\0'};
    strcpy(copy, buf);

    char *delimiter = " ";
    char *buf1 = strtok(copy, delimiter);
    buf1 = strtok(NULL, delimiter);

    char *nick = strdup(buf1);

    return nick;
}

// Returnerer NULL ved malloc feil.
char *extract_nick_server_lookup (char *buf) {
    char copy[BUF_SIZE] = {'\0'};
    strcpy(copy, buf);

    char *delimiter = " ";
    char *buf1 = strtok(copy, delimiter);
    buf1 = strtok(NULL, delimiter);
    buf1 = strtok(NULL, delimiter);
    buf1 = strtok(NULL, delimiter);

    char *nick = strdup(buf1);

    return nick;
}

// Returnerer NULL ved malloc feil.
char *extract_ip_server_lookup (char *buf) {
    char copy[BUF_SIZE] = {'\0'};
    strcpy(copy, buf);

    char *delimiter = " ";
    char *buf1 = strtok(copy, delimiter);
    buf1 = strtok(NULL, delimiter);
    buf1 = strtok(NULL, delimiter);
    buf1 = strtok(NULL, delimiter);
    buf1 = strtok(NULL, delimiter);
    buf1 = strtok(NULL, delimiter);

    char *ip = strdup(buf1);

    return ip;
}

int extract_port_server_lookup (char *buf) {
    char copy[BUF_SIZE] = {'\0'};
    strcpy(copy, buf);

    char *delimiter = " ";
    char *buf1 = strtok(copy, delimiter);
    buf1 = strtok(NULL, delimiter);
    buf1 = strtok(NULL, delimiter);
    buf1 = strtok(NULL, delimiter);
    buf1 = strtok(NULL, delimiter);
    buf1 = strtok(NULL, delimiter);
    buf1 = strtok(NULL, delimiter);
    buf1 = strtok(NULL, delimiter);

    int port = atoi(buf1);

    return port;
}

int extract_pkt_text (char *buf) {
    char copy[BUF_SIZE] = {'\0'};
    strcpy(copy, buf);

    char *delimiter = " ";
    char *buf1 = strtok(copy, delimiter);
    buf1 = strtok(NULL, delimiter);

    int pkt = atoi(buf1);

    return pkt;
}

// Returnerer NULL ved malloc feil.
char *extract_nick_text (char *buf) {
    char copy[BUF_SIZE] = {'\0'};
    strcpy(copy, buf);

    char *delimiter = " ";
    char *buf1 = strtok(copy, delimiter);
    buf1 = strtok(NULL, delimiter);
    buf1 = strtok(NULL, delimiter);
    buf1 = strtok(NULL, delimiter);

    char *nick = strdup(buf1);

    return nick;
}

// Returnerer NULL ved malloc feil.
char *extract_msg_text (char *buf) {
    char copy[BUF_SIZE] = {'\0'};
    strcpy(copy, buf);

    char *delimiter = " ";
    char *buf1 = strtok(copy, delimiter);
    char *buf2 = strtok(NULL, delimiter);
    char *buf3 = strtok(NULL, delimiter);
    char *buf4 = strtok(NULL, delimiter);
    char *buf5 = strtok(NULL, delimiter);
    char *buf6 = strtok(NULL, delimiter);
    char *buf7 = strtok(NULL, delimiter);

    int proper_start = strlen(buf1) + 1 + strlen(buf2) + 1 + strlen(buf3) + 1 + strlen(buf4) + 1;
    proper_start += strlen(buf5) + 1 + strlen(buf6) + 1 + strlen(buf7) + 1;

    char *msg = strdup(&buf[proper_start]);

    return msg;
}