CC = gcc
CFLAGS = -std=c23 \
         -O2 \
         -Werror \
         -Wall \
         -Wcast-align \
         -Wcast-qual \
         -Wconversion \
         -Wdouble-promotion \
         -Wextra \
         -Wfloat-equal \
         -Wformat=2 \
         -Wimplicit-fallthrough \
         -Wmissing-declarations \
         -Wmissing-prototypes \
         -Wnull-dereference \
         -Wold-style-definition \
         -Wpedantic \
         -Wpointer-arith \
         -Wredundant-decls \
         -Wshadow \
         -Wsign-conversion \
         -Wstrict-prototypes \
         -Wswitch-enum \
         -Wundef \
         -Wvla \
         -Wwrite-strings \
         -pedantic-errors
LDFLAGS =

all:
	$(CC) -o sfs $(CFLAGS) $(LDFLAGS) main.c
	strip sfs
