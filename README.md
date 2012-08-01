serial2amqp
================================

Download and compile supporting code:
  git clone git://github.com/alanxz/rabbitmq-c.git
  cd rabbitmq-c
  git submodule init
  git submodule update
  autoreconf -i
  ./configure
  make

No need to actually install the supporting packages; the compiled packages
just need to be available for amqptools.

Compile serial2amqp:
  make RABBITMQCHOME=../rabbitmq-c
