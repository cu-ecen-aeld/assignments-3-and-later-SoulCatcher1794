TARGET ?= aesdsocket
CFLAGS ?= -g -Werror -Wall -std=c99
LDFLAGS ?= -lpthread -lrt

ifdef CROSS_COMPILE
	CC ?= $(CROSS_COMPILE)gcc
else
	CC ?= gcc
endif

all:default

default:$(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

.PHONY:clean

clean:
	rm -f $(TARGET) *.o