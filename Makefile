CC = gcc
CFLAGS = -pedantic -Wall -Wextra -Werror
LDFLAGS =

all:
	$(CC) -o sfs $(CFLAGS) $(LDFLAGS) main.c
	strip sfs
