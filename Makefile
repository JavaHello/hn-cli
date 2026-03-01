CC = cc
CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L -Wall -Wextra -Werror -O2 -Iinclude $(shell pkg-config --cflags json-c)
LDFLAGS = $(shell pkg-config --libs json-c) -lcurl

TARGET = hn-cli
SOURCES = src/main.c src/cli.c src/http.c src/hn_api.c src/deepseek.c src/text.c
OBJECTS = $(SOURCES:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJECTS)

.PHONY: all clean
