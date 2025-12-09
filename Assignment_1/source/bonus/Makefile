CC = gcc
CFLAGS = -Wall -O2
TARGET = pstree
SRC = pstree.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET) *.o
