CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -pthread
TARGET  = server
SRC     = server.c

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)
	@echo "✔ Servidor compilado: ./$(TARGET)"

clean:
	rm -f $(TARGET)

run: $(TARGET)
	./$(TARGET) 8080 server.log
