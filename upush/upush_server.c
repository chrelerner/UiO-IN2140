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

#define BUF_SIZE 300

int register_client (char *nick, int port, int ip, int current_time, struct client_addr **start);
struct client_addr *lookup_client (char *nick, struct client_addr *start);

int test_format (char *message);
char *extract_nick (char *buf);
int extract_ACK (char *buf);

char *construct_registration_response (int ack_num);
char *construct_lookup_response (int ack_num, char *nick, int port, int ip, int client_found);

int main (int argc, char *argv[]) {

    struct client_addr *client_list = NULL;

    if (argc != 3) {
        fprintf(stderr, "Invalid input. Try ./upush_server <port> <loss_probability>\n");
        exit(EXIT_SUCCESS);
    }

    // Fikser sannsynlighet for pakketap.
    double loss_probability = (double) atoi(argv[2]);
    if (loss_probability < 0 || loss_probability > 100) {
        fprintf(stderr, "Please set loss probability to a value from 0 to 100\n");
        exit(EXIT_SUCCESS);
    }
    float loss_probability_correct = (float) loss_probability / 100;
    set_loss_probability(loss_probability_correct);

    // Lager socketen som brukes til lytting, og binder den til porten spesifisert.
    int ret, socket_fd;

    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    check_error(socket_fd, "socket", -1);

    struct sockaddr_in my_addr;
    int port = atoi(argv[1]);

    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(port);
    my_addr.sin_addr.s_addr = INADDR_ANY;

    ret = bind(socket_fd, (struct sockaddr *) &my_addr, sizeof(struct sockaddr_in));
    if (ret == -1) {
        perror("bind");
        close(socket_fd);
        exit(EXIT_FAILURE);
    }

    // Lager et sett, read_fds, til lesing for select.
    fd_set read_fds;
    fd_set temp_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_fd, &read_fds);
    FD_SET(STDIN_FILENO, &read_fds);

    while (1) {

        // Kopierer originalsettet til et midlertidig sett som brukes i select,
        // siden select endrer paa selve settet.
        temp_fds = read_fds;

        ret = select(socket_fd + 1, &temp_fds, NULL, NULL, NULL);

        // Haandterer registrering og oppsoek av klienter.
        if (FD_ISSET(socket_fd, &temp_fds)) {

            char buf[BUF_SIZE] = {'\0'};
            struct sockaddr_in client_addr;
            socklen_t addrlen = sizeof(struct sockaddr_in);

            ret = recvfrom(socket_fd, buf, BUF_SIZE - 1, 0, (struct sockaddr *) &client_addr, &addrlen);
            if (ret == -1) {
                perror("recvfrom");
                close(socket_fd);
                free_clients(client_list);
                exit(EXIT_FAILURE);
            }

            // Used for debugging.
            if (buf[strlen(buf) - 1] == '\n') {
                buf[strlen(buf) - 1] = '\0';
            }

            printf("\n%s\n", buf);

            int check_format = test_format(buf);
            
            if (check_format != 3) {
            
                char *nick = extract_nick(buf);
                if (nick == NULL) {
                    perror("malloc");
                    close(socket_fd);
                    free_clients(client_list);
                    exit(EXIT_FAILURE);
                }

                // Registrering av klient. Nick og klient legges paa heap og maa frigjoeres.
                // Siden registrering ikke tar lang tid, gjoeres det uansett hva ACK-nummeret er.
                if (check_format == 1) {
                    unsigned short port = ntohs(client_addr.sin_port);
                    int ip = client_addr.sin_addr.s_addr;

                    struct timeval time;
                    ret = gettimeofday(&time, NULL);

                    int malloc_check = register_client(nick, port, ip, time.tv_sec, &client_list);
                    if (malloc_check == 0) {
                        perror("malloc");
                        close(socket_fd);
                        free_clients(client_list);
                        free(nick);
                        exit(EXIT_FAILURE);
                    }

                    // Sender tilbake en ACK.
                    int ack_num = extract_ACK(buf);

                    char *response = construct_registration_response(ack_num);
                    if (response == NULL) {
                        perror("malloc");
                        close(socket_fd);
                        free_clients(client_list);
                        exit(EXIT_FAILURE);
                    }

                    printf("%s\n", response);

                    ret = send_packet(socket_fd, response, strlen(response), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                    free(response);
                    if (ret == -1) {
                        perror("send_packet");
                        close(socket_fd);
                        free_clients(client_list);
                        exit(EXIT_FAILURE);
                    }
                }

                // Oppsoek av klient.
                if (check_format == 2) {

                    int ack_num = extract_ACK(buf);

                    struct client_addr *target_client = lookup_client(nick, client_list);

                    // Denne nick-en trengs ikke mer, og frigjoeres heller ingen andre steder.
                    free(nick);

                    int client_found;

                    // Sender ikke info om klienten hvis det har gaatt mer enn 30 sekunder siden registrering.
                    if (target_client != NULL) {
                        struct timeval time;
                        ret = gettimeofday(&time, NULL);

                        int current_time = time.tv_sec;
                        int registration_time = target_client->registration_time;

                        int time_elapsed = current_time - registration_time;

                        if (time_elapsed > 30) {
                            target_client = NULL;
                        }
                    }

                    if (target_client == NULL) {

                        client_found = 0;
                        
                        char *response = construct_lookup_response(ack_num, "nonsense", 0, 0, client_found);
                        if (response == NULL) {
                            perror("malloc");
                            close(socket_fd);
                            free_clients(client_list);
                            exit(EXIT_FAILURE);
                        }

                        printf("%s\n", response);
                        ret = send_packet(socket_fd, response, strlen(response), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                        free(response);
                        if (ret == -1) {
                            perror("send_packet");
                            close(socket_fd);

                            free_clients(client_list);

                            exit(EXIT_FAILURE);
                        }
                    }
                    else {

                        client_found = 1;

                        char *target_nick = target_client->nick;
                        int target_ip = target_client->ip;
                        int target_port = target_client->port;

                        char *response = construct_lookup_response(ack_num, target_nick, target_port, target_ip, client_found);
                        if (response == NULL) {
                            perror("malloc");
                            close(socket_fd);
                            free_clients(client_list);
                            exit(EXIT_FAILURE);
                        }

                        printf("%s\n", response);

                        ret = send_packet(socket_fd, response, strlen(response), 0, (struct sockaddr *) &client_addr, sizeof(struct sockaddr_in));
                        free(response);
                        if (ret == -1) {
                            perror("send_packet");
                            close(socket_fd);
                            
                            free_clients(client_list);
                            
                            exit(EXIT_FAILURE);
                        }
                    }
                }
            }

            // Ugyldig meldingsformat.
            else {
                fprintf(stderr, "CLIENT REQUEST FORMAT INVALID.\n");
            }
        }


        // Sjekker om serveren vil avslutte.
        if (FD_ISSET(STDIN_FILENO, &temp_fds)) {

            char buf_quit[BUF_SIZE] = {'\0'}; 

            ret = read(STDIN_FILENO, buf_quit, BUF_SIZE - 1);
            if (ret == -1) {
                perror("recvfrom");
                close(socket_fd);
                free_clients(client_list);
                exit(EXIT_FAILURE);
            }

            // Bryter seg ut av loopen og avslutter serveren.
            if (strcmp(buf_quit, "QUIT\n") == 0) {
                break;
            }
        }
    }

    printf("Ending program\n");

    free_clients(client_list);

    close(socket_fd);
    return EXIT_SUCCESS;
} // END - main.



// Siden nick allerede er lagret paa heapen, trengs ikke strdup.
// Returnerer 0 ved malloc feil.
int register_client(char *nick, int port, int ip, int current_time, struct client_addr **start) {

    struct client_addr *client = malloc(sizeof(struct client_addr));
    if (client == NULL) {
        return 0;
    }

    client->nick = nick;
    client->stored_request = NULL;
    client->port = port;
    client->ip = ip;
    client->registration_time = current_time;

    client->next = NULL;

    struct client_addr *old_addr = remove_specified_client(nick, start);
    if (old_addr != NULL) {

        free(old_addr->nick);
        free(old_addr);
    }

    push_client(client, start);

    return 1;
}

struct client_addr *lookup_client(char *nick, struct client_addr *start) {
    struct client_addr *result = find_specified_client(nick, start);
    return result;
}


// Sjekker om klienter sender beskjeder i riktig format.
// Sjekker ogsaa om brukernavn har en passende lengde og er ASCII.
int test_format(char *message) {

    char copy[BUF_SIZE] = {'\0'};
    strcpy(copy, message);

    char *delimiter = " ";

    char *buf1 = strtok(copy, delimiter);
    char *buf2 = strtok(NULL, delimiter);
    char *buf3 = strtok(NULL, delimiter);
    char *buf4 = strtok(NULL, delimiter);

    if (buf1 == NULL || buf2 == NULL || buf3 == NULL || buf4 == NULL) {
        return 3;
    }

    // Forsikrer seg om at et navn med mellomrom ikke kommer igjennom.
    if (strtok(NULL, delimiter) != NULL) {
        return 3;
    }

    int len4 = strlen(buf4);

    len4 = strlen(buf4);

    if (strcmp(buf1, "PKT") == 0) {
        
        if (strlen(buf2) == 1 && isdigit(buf2[0])) {

            char space = 20;
            char tab = 9;
            char return_char = 13; 

            // Registrering av klient format.
            if (strcmp(buf3, "REG") == 0) {

                if (len4 <= 20 && len4 > 0) {
                    for (int i = 0; i < (int) len4; i++) {
                        if (isascii(buf4[i]) == 0 || buf4[i] == space || buf4[i] == tab || buf4[i] == return_char) {
                            return 3;
                        }
                    }
                    return 1;
                }
            } // END - Registrering av klient format.


            // Oppsoek for klient format.
            if (strcmp(buf3, "LOOKUP") == 0) {

                if (len4 <= 20 && len4 > 0) {
                    for (int i = 0; i < (int) len4; i++) {
                        if (isalpha(buf4[i]) == 0 || buf4[i] == space || buf4[i] == tab || buf4[i] == return_char) {
                            return 3;
                        }
                    }
                    return 2;
                }
            } // END - Oppsoek for klient format.
        } // END - if 2.
    } // END - if 1.
    return 3;
}

// Er noedt til aa lagre nick paa heapen, som maa frigjoeres.
// Returnerer NULL ved malloc feil.
char *extract_nick (char *buf) {

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

int extract_ACK (char *buf) {

    char copy[BUF_SIZE] = {'\0'};
    strcpy(copy, buf);

    char *delimiter = " ";
    char *buf1 = strtok(copy, delimiter);
    buf1 = strtok(NULL, delimiter);

    int ack_num = atoi(buf1);

    return ack_num;
}

// Returnerer NULL ved malloc feil.
char *construct_registration_response (int ack_num) {

    // ACK number OK - Har alltid 9 bytes med nullbyte inkludert.
    char *result = malloc(sizeof(char) * 9);
    if (result == NULL) {
        return NULL;
    }

    memset(result, '\0', 9);

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
char *construct_lookup_response(int ack_num, char *nick, int port, int ip, int client_found) {
    char *result = malloc(sizeof(char) * BUF_SIZE);
    if (result == NULL) {
        return NULL;
    }

    memset(result, '\0', BUF_SIZE);

    int result_counter = 0;

    if (client_found) {

        // Constructing the looked up client's IP in a string format.
        char ip_string[INET_ADDRSTRLEN] = {'\0'};
        inet_ntop(AF_INET, &ip, ip_string, INET_ADDRSTRLEN);

        // Constructing the looked up client's port in a string format.
        int port_string_len = snprintf(NULL, 0, "%d", port);
        char port_string[port_string_len + 1];
        snprintf(port_string, port_string_len + 1, "%d", port);

        // Appending all necessary information to the result string.
        memcpy(result, "ACK ", 4);
        result_counter += 4;

        char ack_char = ack_num + '0';
        result[result_counter] = ack_char;
        result_counter += 1;

        memcpy(&result[result_counter], " NICK ", 6);
        result_counter += 6;

        memcpy(&result[result_counter], nick, strlen(nick));
        result_counter += strlen(nick);

        memcpy(&result[result_counter], " IP ", 4);
        result_counter += 4;

        memcpy(&result[result_counter], ip_string, strlen(ip_string));
        result_counter += strlen(ip_string);

        memcpy(&result[result_counter], " PORT ", 6);
        result_counter += 6;

        memcpy(&result[result_counter], port_string, strlen(port_string));
        result_counter += strlen(port_string);
    }
    else {
        // Appending all necessary information to the result string.
        memcpy(result, "ACK ", 4);
        result_counter += 4;

        char ack_char = ack_num + '0';
        result[result_counter] = ack_char;
        result_counter += 1;

        memcpy(&result[result_counter], " NOT FOUND", 10);
    }

    return result;
}
