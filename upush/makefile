CFLAGS = -g -Wall -Wextra -std=gnu11
VFLAGS = --track-origins=yes --malloc-fill=0x40 --free-fill=0x23 --leak-check=full --show-leak-kinds=all
BIN = upush_client upush_server
OBJECT = upush_client.o upush_server.o send_packet.o common.o

all: $(BIN)


upush_client: upush_client.o send_packet.o send_packet.h common.o common.h
	gcc $(CFLAGS) upush_client.o send_packet.o common.o -o upush_client

upush_server: upush_server.o send_packet.o send_packet.h common.o common.h
	gcc $(CFLAGS) upush_server.o send_packet.o common.o -o upush_server


upush_client.o: upush_client.c
	gcc $(CFLAGS) -c upush_client.c

upush_server.o: upush_server.c
	gcc $(CFLAGS) -c upush_server.c


common.o: common.c
	gcc $(CFLAGS) -c common.c

send_packet.o: send_packet.c
	gcc $(CFLAGS) -c send_packet.c

valgrind_server:
	valgrind $(VFLAGS) ./upush_server 5000 0

valgrind_client_mario:
	valgrind $(VFLAGS) ./upush_client Mario 127.0.0.1 5000 10 0

valgrind_client_luigi:
	valgrind $(VFLAGS) ./upush_client Luigi 127.0.0.1 5000 10 0

valgrind_client_peach:
	valgrind $(VFLAGS) ./upush_client Peach 127.0.0.1 5000 10 0

valgrind_client_yoshi:
	valgrind $(VFLAGS) ./upush_client Yoshi 127.0.0.1 5000 10 0

clean:
	rm -f $(BIN) $(OBJECT)