CC = gcc
TARGET = build/sgf

SRC = main.c src/bitmap.c
INCLUDES = -Iinclude
CFLAGS = -finput-charset=UTF-8 -Wall $(INCLUDES)

$(shell mkdir build)

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC)

clean:
	del $(TARGET).exe 2>nul || exit 0