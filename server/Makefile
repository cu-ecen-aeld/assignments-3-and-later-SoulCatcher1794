TARGET=aesdsocket
CFLAGS=-g -Werror -Wall -std=c99

ifdef CROSS_COMPILE
	CC:=$(CROSS_COMPILE)gcc
else
	CC:=gcc
endif

all:default

default:$(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY:clean

clean:
	rm -f $(TARGET) *.o