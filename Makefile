ifeq (${RABBITMQCHOME},)
	AMQPTOOLS_RABBITHOME = "/usr/local/src/rabbitmq/rabbitmq-c"
endif

ifeq (${SERIAL2AMQP_INSTALLROOT},)
	SERIAL2AMQP_INSTALLROOT = "/usr/local/bin"
endif

all: clean build

build: serial2amqp

install: bin/
	install -D -m0755 bin/serial2amqp $(SERIAL2AMQP_INSTALLROOT)/serial2amqp

uninstall:
	rm -f $(SERIAL2AMQP_INSTALLROOT)/serial2amqp

serial2amqp: serial2amqp.c
	gcc -lpthread -o bin/serial2amqp serial2amqp.c -I$(RABBITMQCHOME)/librabbitmq $(RABBITMQCHOME)/librabbitmq/.libs/librabbitmq.so

clean:
	rm -f bin/*
