# Compiler and flags
CC = gcc
CFLAGS = -Wall -g -Werror

# Targets
TARGET = fs
OBJS = fs-sim.o

# Build the executable
all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

fs-sim.o: fs-sim.c fs-sim.h
	$(CC) $(CFLAGS) -c fs-sim.c

# Clean the build
clean:
	rm -f $(TARGET) $(OBJS)