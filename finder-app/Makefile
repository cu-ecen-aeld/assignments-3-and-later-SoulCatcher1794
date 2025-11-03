TARGET = writer
CFLAGS = -g -Werror -Wall -std=c99

ifeq ($(CROSS_COMPILE),aarch64-none-linux-gnu-) 
	CC := $(CROSS_COMPILE)gcc
else
	CC := gcc
endif

default: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) -o $@ $<

.PHONY: clean
clean:
	rm -f $(TARGET) *.o
