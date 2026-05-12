CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11
TARGET  = fs_tool

all: $(TARGET)

$(TARGET): fs.c
	$(CC) $(CFLAGS) -o $(TARGET) fs.c

clean:
	rm -f $(TARGET)

.PHONY: all clean
