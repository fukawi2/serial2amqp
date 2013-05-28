ifeq (${PREFIX},)
	PREFIX = "/usr/local"
endif

all: clean build

build: serial2amqp

install: bin/
	install -D -m0755 bin/serial2amqp $(PREFIX)/bin/serial2amqp

uninstall:
	rm -f $(PREFIX)/bin/serial2amqp

serial2amqp: serial2amqp.c
	gcc -Wall -g -lpthread -lrabbitmq -o bin/serial2amqp serial2amqp.c

clean:
	rm -f bin/*
