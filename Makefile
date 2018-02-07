CC = $(CROSS_COMPILER)gcc
INCLUDES = -I. -Iinclude
LIBS = -Llib -L../lib/.libs -lfuse3
OUT = AVServer

default: $(OUT)

$(OUT): clean
	$(CC) -Wall -D_FILE_OFFSET_BITS=64 $(INCLUDES) $(LIBS) -o $(OUT) $(filter-out test_av.c, $(wildcard *.c))

clean:
	rm -f $(OUT)