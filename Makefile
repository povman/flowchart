CC      = gcc
CFLAGS  = $(shell pkg-config --cflags gtk4) -Wall -Wextra -Wpedantic -g -O2
LDFLAGS = $(shell pkg-config --libs gtk4) -lm
TARGET  = flowchart
SOURCES = flowchart.c
OBJECTS = $(SOURCES:.c=.o)

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)

%.o: %.c flowchart.h
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJECTS) flowchart.png *.flow

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)
