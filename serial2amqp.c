/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) 2012 Phillip Smith
 *
 * The Original Code is librabbitmq and amqptools:
 *  https://github.com/alanxz/rabbitmq-c
 *  https://github.com/rmt/amqptools
 * 
 * Portions of code based on examples within Serial Programming HOWTO
 * which is copyrighted (c) 1997 Peter Baumann, (c) 2001 Gary Frerking
 * and is distributed under the terms of the Linux Documentation Project (LDP)
 *  http://www.ibiblio.org/pub/Linux/docs/HOWTO/Serial-Programming-HOWTO
 *
 * The Initial Developers of the Original Code are LShift Ltd, Cohesive
 * Financial Technologies LLC, and Rabbit Technologies Ltd.  Portions
 * created before 22-Nov-2008 00:00:00 GMT by LShift Ltd, Cohesive
 * Financial Technologies LLC, or Rabbit Technologies Ltd are Copyright
 * (C) 2007-2008 LShift Ltd, Cohesive Financial Technologies LLC, and
 * Rabbit Technologies Ltd.
 *
 * Portions created by LShift Ltd are Copyright (C) 2007-2009 LShift
 * Ltd. Portions created by Cohesive Financial Technologies LLC are
 * Copyright (C) 2007-2009 Cohesive Financial Technologies
 * LLC. Portions created by Rabbit Technologies Ltd are Copyright (C)
 * 2007-2009 Rabbit Technologies Ltd.
 *
 * Portions created by Tony Garnock-Jones are Copyright (C) 2009-2010
 * LShift Ltd and Tony Garnock-Jones.
 *
 * All Rights Reserved.
 */

#include <stdio.h>    // perror() and printf()
#include <string.h>   // strchr()
#include <strings.h>  // bzero()
#include <fcntl.h>    // open()
#include <unistd.h>   // read()
#include <stdlib.h>   // exit()
#include <getopt.h>   // getopt_long()
#include <signal.h>   // handling signals
#include <termios.h>  // all serial port functions and constants
#include <syslog.h>   // logging
#include <pthread.h>  // threading
//#include <sys/types.h>
//#include <sys/stat.h>

#include <amqp.h>
#include <amqp_framing.h>

// baudrate settings defined in <asm/termbits.h> (included by <termios.h>)
#define BAUDRATE B38400

// default configuration constants
#define MODEMDEVICE "/dev/ttyS0"
#define DEBUGLEVEL 1
#define AMQP_HOSTNAME "amqpbroker";
#define AMQP_PORT 5672;
#define AMQP_USERNAME "guest";
#define AMQP_PASSWORD "guest";
#define AMQP_VHOST "/";
#define AMQP_EXCHANGE "amq.direct";
#define AMQP_ROUTINGKEY "serial2amqp";

#define AMQP_NOT_PERSISTENT 1;
#define AMQP_PERSISTENT 2;

/* maximum number of items in the internal queue between the serial operations
 * and amqp operations. and "item" is a canonical string of text received via
 * the serial port
 */
#define QUEUE_MAX_ITEMS 32;

// POSIX compliant source
#define _POSIX_SOURCE 1

// use strlcpy instead of strcpy
// https://security.web.cern.ch/security/recommendations/en/codetools/c.shtml
#ifndef strlcpy
#define strlcpy(dst,src,sz) snprintf((dst), (sz), "%s", (src))
#endif

#define FALSE 0
#define TRUE 1

volatile int STOP=FALSE;
int waiting_for_io_flag=TRUE;

// thread sychronization
pthread_mutex_t mtx_fifo_queue  = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  cond_fifo_queue = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mtx_serialio    = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  cond_serialio   = PTHREAD_COND_INITIALIZER;

char serial_device[100]     = MODEMDEVICE;
static int debug_level      = DEBUGLEVEL;
static int foreground_flag  = 0;
static int daemonized       = 0;

// amqp broker configuration
char amqp_hostname[255]   = AMQP_HOSTNAME;
int  amqp_port            = AMQP_PORT;
char amqp_vhost[64]       = AMQP_VHOST;
char amqp_username[64]    = AMQP_PASSWORD;
char amqp_password[64]    = AMQP_USERNAME;
char amqp_exchange[32]    = AMQP_EXCHANGE;
char amqp_routingkey[128] = AMQP_ROUTINGKEY;

// the original serial port settings to restore when ending
struct termios oldtio;
// file descriptior for open/read serial port
int fd;

void print_help(const char *program_name) {
  fprintf(stderr, "Usage: %s [options]\n", program_name);
  fprintf(stderr, "Options:\n");
  fprintf(stderr, "  --device/-D /dev/ttyXX   specify the serial device to list on (default: \"/dev/ttyS0\")\n");
  fprintf(stderr, "  --debug/-d X             set the debug verbosity level\n");
  fprintf(stderr, "  --quiet/-q               no output\n");
  fprintf(stderr, "  --host/-H amqpbroker     hostname of the AMQP broker\n");
  fprintf(stderr, "  --port/-p 5762           port to connect to the AMQP broker\n");
  fprintf(stderr, "  --user/-U guest          username for AMQP broker\n");
  fprintf(stderr, "  --pass/-P guest          password for AMQP broker\n");
  fprintf(stderr, "  --exchange/-E amq.direct exhange to publish to\n");
  fprintf(stderr, "  --key/-K default         routing key to publish to\n");
  fprintf(stderr, "  --vhost/-V /             amqp virtual host to publish to\n");
  fprintf(stderr, "  --foreground/-f          do not daemonize\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "The following environment variables may also be set:\n");
  fprintf(stderr, "  S2A_DEVICE, AMQP_HOSTNAME, AMQP_PORT, AMQP_USERNAME, AMQP_PASSWORD\n");
  fprintf(stderr, "  AMQP_VHOST, AMQP_EXCHANGE, AMQP_ROUTINGKEY\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "IMPORTANT: Using the --pass option could allow other users to discover your\n");
  fprintf(stderr, "password by looking at process list. You should use the AMQP_PASSWORD environment\n");
  fprintf(stderr, "variable where possible to guard against this.\n");
}



// Define the function to be called when ctrl-c (SIGINT) signal is sent to process
void signal_callback_handler(int signum)
{
  fprintf(stderr, "Caught signal %d\n", signum);

  // global var to tell loops when to exit so
  // we can finish up gracefully.
  STOP=TRUE;

  // signal the serial IO thread to stop waiting for serial data
  pthread_cond_signal( &cond_serialio );
}



// helper for fatal errors
void bomb(int ecode, const char *msg)
{
  char fmsg[255];  // formatted msg
  snprintf(fmsg, sizeof(fmsg), "ERROR: %s", msg);

  // print to stderr if we haven't daemonized
  if (0 == daemonized)
    fprintf(stderr, "%s\n", fmsg);

  // log to syslog
  syslog(LOG_MAKEPRI(LOG_DAEMON, LOG_NOTICE), fmsg);

  exit(ecode);
}


// helper to log debug messages
void debug_print(int msg_lvl, const char *msg)
{
  /* log to syslog
   * add 4 to the message level value to approximate
   * a syslog notice level:
   * Notice = 5 (1 + 4)
   * Information = 6 (2 + 4)
   * Debug = 7 (3 + 4)
   */
  int syslog_lvl = msg_lvl+4;
  if (syslog_lvl > 7)
    syslog_lvl = 7;
  syslog(LOG_MAKEPRI(LOG_DAEMON, syslog_lvl), msg);

  // abort if the message is higher debug than the user
  // wants to see.
  if (msg_lvl > debug_level) return;

  // print to stderr, only if we haven't already daemonized
  if (0 == daemonized) {
    char fmsg[255];  // formatted msg
    snprintf(fmsg, sizeof(fmsg), "DEBUG%d: %s", msg_lvl, msg);
    fprintf(stderr, fmsg);
    fprintf(stderr, "\n");
  }
}


/******************************************************************************
 * Ringbuffer/FIFO Queue implementation
 * http://akomaenablog.blogspot.com.au/2008/03/round-fifo-queue-in-c.html
 *****************************************************************************/
int queue_head;
int queue_tail;
int queue_items_max;
int queue_items_avl;
int queue_lock_intent;
int queue_locked;
void **table;

// init queue
void fifo_init (int size) {
  queue_head  = 0;
  queue_tail  = 0;
  queue_items_avl = 0;
  queue_items_max = size;
  table=(void**)malloc(queue_items_max * sizeof(void*));
}

// free memory
void fifo_destroy() {
  int i;
  if( ! fifo_is_empty() ) {
    for ( i=queue_tail; i<queue_head; i++ ) {
      free(table[i]);
    }
  }
  free(table);
}

// boolean if the queue is empty or not
int fifo_is_empty() {
  return(queue_items_avl==0);
}

// insert element
int fifo_push(void *next) {
  debug_print(4, "Pushing onto queue");
  if ( queue_items_avl == queue_items_max ) {
    // queue full
    return(0);
  }

  table[queue_head]=next;
  queue_items_avl++;
  queue_head=(queue_head+1)%queue_items_max;
  return(1);
}

// return next element
void* fifo_unshift() {
  debug_print(4, "Unshifting from queue");
  void* get;
  if (queue_items_avl>0) {
    get=table[queue_tail];
    queue_tail=(queue_tail+1)%queue_items_max;
    queue_items_avl--;
    return(get);
  }
}

/******************************************************************************
 * AMQP FUNCTIONALITY
 *****************************************************************************/

void die_on_error(int x, char const *context) {
  if (x < 0) {
    char *errstr = amqp_error_string(-x);
    fprintf(stderr, "%s: %s\n", context, errstr);
    free(errstr);
    exit(1);
  }
}

void die_on_amqp_error(amqp_rpc_reply_t x, char const *context) {
  switch (x.reply_type) {
    case AMQP_RESPONSE_NORMAL:
      return;

    case AMQP_RESPONSE_NONE:
      fprintf(stderr, "%s: missing RPC reply type!\n", context);
      break;

    case AMQP_RESPONSE_LIBRARY_EXCEPTION:
      fprintf(stderr, "%s: %s\n", context, amqp_error_string(x.library_error));
      break;

    case AMQP_RESPONSE_SERVER_EXCEPTION:
      switch (x.reply.id) {
  case AMQP_CONNECTION_CLOSE_METHOD: {
    amqp_connection_close_t *m = (amqp_connection_close_t *) x.reply.decoded;
    fprintf(stderr, "%s: server connection error %d, message: %.*s\n",
      context,
      m->reply_code,
      (int) m->reply_text.len, (char *) m->reply_text.bytes);
    break;
  }
  case AMQP_CHANNEL_CLOSE_METHOD: {
    amqp_channel_close_t *m = (amqp_channel_close_t *) x.reply.decoded;
    fprintf(stderr, "%s: server channel error %d, message: %.*s\n",
      context,
      m->reply_code,
      (int) m->reply_text.len, (char *) m->reply_text.bytes);
    break;
  }
  default:
    fprintf(stderr, "%s: unknown server error, method id 0x%08X\n", context, x.reply.id);
    break;
      }
      break;
  }

  exit(1);
}

void *thr_amqp_publish() {
  int amqp_channel = 1; // TODO: handle dynamic channel number

  // open a connection to the server
  debug_print(2, "AMQP: Connecting to broker");
  amqp_connection_state_t conn;
  int sockfd;
  conn = amqp_new_connection();
  die_on_error(sockfd = amqp_open_socket(amqp_hostname, amqp_port), "Opening socket");

  // authenticate
  debug_print(2, "AMQP: Authenticating");
  amqp_set_sockfd(conn, sockfd);
  die_on_amqp_error(
      amqp_login(
        conn, amqp_vhost, 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, amqp_username, amqp_password
        ),
      "Authenticating");

  // open a channel
  debug_print(3, "AMQP: Opening a channel");
  amqp_channel_open(conn, amqp_channel);
  die_on_amqp_error(amqp_get_rpc_reply(conn), "Opening channel");

  debug_print(1, "AMQP: Ready");
  while (1) {
    // lock the fifo mutex before checking it
    pthread_mutex_lock( &mtx_fifo_queue );
    if ( fifo_is_empty() ) {
      // block until the serial io thread signals us that
      // it has pushed data into the fifo
      debug_print(6, "AMQP: fifo queue empty; unlocking mutex and issuing pthread_cond_wait");
      pthread_cond_wait( &cond_fifo_queue, &mtx_fifo_queue );

      // once the fifo queue is empty, then we can gracefully exit this sub if
      // the global var STOP is true (controlled by the signal handler)
      // this check needs to be after the pthread_cond_wait. if it is before,
      // then when the signal handler signals us, we miss this check and go on
      // to try and unshift the (empty) fifo queue.
      if ( STOP==TRUE )
        break;
    }

    // fifo must finally have something; grab it and publish it
    debug_print(5, "AMQP: fifo queue has something (I hope); unshifting item");
    char *msg = fifo_unshift();

    // build the message body
    amqp_bytes_t messagebody;
    messagebody = amqp_cstring_bytes(msg);

    // build the message frame
    debug_print(3, "AMQP: Building message frame");
    amqp_basic_properties_t props;
    props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
    props.content_type = amqp_cstring_bytes("text/plain");
    props.delivery_mode = AMQP_PERSISTENT;

    // send the message frame
    debug_print(2, "AMQP: Publishing message to exchange/routing key");
    debug_print(2, amqp_exchange);
    debug_print(2, amqp_routingkey);
    die_on_error(amqp_basic_publish(conn,
                                    amqp_channel,
                                    amqp_cstring_bytes(amqp_exchange),
                                    amqp_cstring_bytes(amqp_routingkey),
                                    0,
                                    0,
                                    &props,
                                    messagebody),
                  "Sending message");

    pthread_mutex_unlock( &mtx_fifo_queue );
  }

  // cleanup by closing all our connections
  debug_print(4, "AMQP: Cleaning up connection");
  debug_print(5, "AMQP: Closing channel");
  die_on_amqp_error(
      amqp_channel_close(conn, amqp_channel, AMQP_REPLY_SUCCESS),
      "Closing channel");
  debug_print(5, "AMQP: Closing connection");
  die_on_amqp_error(
      amqp_connection_close(conn, AMQP_REPLY_SUCCESS),
      "Closing connection");
  debug_print(5, "AMQP: Ending connection");
  die_on_error(
      amqp_destroy_connection(conn),
      "Ending connection");

  debug_print(1, "AMQP: Thread Exiting");
  pthread_exit(0);
}


/******************************************************************************
 * Serial Port Operations
 *****************************************************************************/
void signal_handler_IO (int status) {
  debug_print(5, "SIGIO: Setting waiting_for_io_flag = false and Signalling cond_serialio");
  waiting_for_io_flag = FALSE;
  pthread_cond_signal( &cond_serialio );
}         

void *thr_read_from_serial() {
  int res;  // throaway value for capturing results of functions
  struct termios newtio;  // the settings we want for the serial port
  /*
   * Open modem device for reading and writing and not as controlling tty
   * because we don't want to get killed if linenoise sends CTRL-C.
  */
  debug_print(2, "SERIO: Opening serial device:");
  debug_print(2, serial_device);
  fd = open(serial_device, O_RDONLY | O_NOCTTY );
  if (fd < 0) { perror(serial_device); exit(-1); }  // exit if fail to open

  // install the signal handler before making the device asynchronous
  struct sigaction saio;           /* definition of signal action */
  saio.sa_handler = signal_handler_IO;
  //saio.sa_mask = 0;
  saio.sa_flags = 0;
  saio.sa_restorer = NULL;
  sigaction(SIGIO, &saio, NULL);
  // allow the process to receive SIGIO
  fcntl(fd, F_SETOWN, getpid());
  // Make the file descriptor asynchronous (the manual page says only
  // O_APPEND and O_NONBLOCK, will work with F_SETFL...)
  fcntl(fd, F_SETFL, FASYNC);

  debug_print(3, "SERIO: Saving existing serial port settings");
  tcgetattr(fd, &oldtio);         // save current serial port settings
  bzero(&newtio, sizeof(newtio)); // clear struct for new port settings

  /*
   * BAUDRATE: Set bps rate. You could also use cfsetispeed and cfsetospeed.
   * CRTSCTS : output hardware flow control (only used if the cable has
   *           all necessary lines. See sect. 7 of Serial-HOWTO)
   * CS8     : 8n1 (8bit,no parity,1 stopbit)
   * CLOCAL  : local connection, no modem contol
   * CREAD   : enable receiving characters
  */
  newtio.c_cflag = BAUDRATE | CRTSCTS | CS8 | CLOCAL | CREAD;

  /*
  IGNPAR  : ignore bytes with parity errors
  ICRNL   : map CR to NL (otherwise a CR input on the other computer
            will not terminate input) otherwise make device raw
  */
  newtio.c_iflag = IGNPAR | ICRNL;

  // Raw output.
  newtio.c_oflag = 0;

  /*
   * ICANON  : enable canonical input
   * disable all echo functionality, and don't send signals to calling program
  */
  newtio.c_lflag = ICANON;

  /*
   * initialize all control characters
   * default values can be found in /usr/include/termios.h, and are given
   * in the comments, but we don't need them here
  */
  newtio.c_cc[VINTR]    = 0;  // Ctrl-c
  newtio.c_cc[VQUIT]    = 0;  // Ctrl-\   */
  newtio.c_cc[VERASE]   = 0;  // del
  newtio.c_cc[VKILL]    = 0;  // @
  newtio.c_cc[VEOF]     = 4;  // Ctrl-d
  newtio.c_cc[VTIME]    = 0;  // inter-character timer unused
  newtio.c_cc[VMIN]     = 1;  // blocking read until 1 character arrives
  newtio.c_cc[VSWTC]    = 0;  // '\0'
  newtio.c_cc[VSTART]   = 0;  // Ctrl-q
  newtio.c_cc[VSTOP]    = 0;  // Ctrl-s
  newtio.c_cc[VSUSP]    = 0;  // Ctrl-z
  newtio.c_cc[VEOL]     = 0;  // '\0'
  newtio.c_cc[VREPRINT] = 0;  // Ctrl-r
  newtio.c_cc[VDISCARD] = 0;  // Ctrl-u
  newtio.c_cc[VWERASE]  = 0;  // Ctrl-w
  newtio.c_cc[VLNEXT]   = 0;  // Ctrl-v
  newtio.c_cc[VEOL2]    = 0;  // '\0'

  // now clean the modem line and activate the settings for the port
  debug_print(3, "SERIO: Cleaning line and activating port settings");
  tcflush(fd, TCIFLUSH);
  tcsetattr(fd, TCSANOW, &newtio);

  // serial port settings done, now handle input
  debug_print(1, "SERIO: Thread Ready");
  char buf[255];
  char dbgmsg[1024];
  while (STOP == FALSE) {
    if ( waiting_for_io_flag==TRUE ) {
      debug_print(6, "SERIO: Waiting for IO; issuing pthread_cond_wait");
      pthread_cond_wait( &cond_serialio, &mtx_serialio );
      continue;
    }
    /* read blocks program execution until a line terminating character is
     * input, even if more than 255 chars are input. If the number
     * of characters read is smaller than the number of chars available,
     * subsequent reads will return the remaining chars. res will be set
     * to the actual number of characters actually read
    */
    res = read(fd, buf, 512);

    // switch the trailing newline for null
    buf[res-1] = '\0';

    // don't output blank lines
    if (res-1 < 1) {
      continue;
    }

    // show debug info
    snprintf(dbgmsg, sizeof(dbgmsg), "SERIO: Recv %d characters: %s", res, buf);
    debug_print(1, dbgmsg);

    // output just the received data to stdout
    pthread_mutex_lock( &mtx_fifo_queue );
    res = fifo_push(buf);
    if ( res ) {
      debug_print(2, "SERIO: Message Queued");
    } else {
      debug_print(1, "SERIO: WARNING: fifo queue FULL. Dropped message.");
    }
    pthread_cond_signal( &cond_fifo_queue );
    pthread_mutex_unlock( &mtx_fifo_queue );

    waiting_for_io_flag=TRUE;
  }

  // restore the old port settings
  tcsetattr(fd, TCSANOW, &oldtio);

  // signal the AMQP thread that it can exit; we aren't
  // going to send any more data to the queue
  pthread_cond_signal( &cond_fifo_queue );
  close(fd);

  debug_print(1, "SERIO: Thread Exiting");
  pthread_exit(0);
}

/******************************************************************************
 * MAIN
 *****************************************************************************/
int main(int argc, char **argv) {
  // Register signal and signal handler
  signal(SIGINT, signal_callback_handler);
  signal(SIGTERM, signal_callback_handler);

  // first we need to check environment variables for our config
  if (NULL != getenv("S2A_DEVICE"))         { strlcpy(serial_device, getenv("S2A_DEVICE"), sizeof(serial_device)); }
  if (NULL != getenv("AMQP_HOSTNAME"))      { strlcpy(amqp_hostname, getenv("AMQP_HOSTNAME"), sizeof(amqp_hostname)); }
  if (NULL != getenv("AMQP_PORT"))          { amqp_port = atoi(getenv("AMQP_PORT")); }
  if (NULL != getenv("AMQP_USERNAME"))      { strlcpy(amqp_username, getenv("AMQP_USERNAME"), sizeof(amqp_username)); }
  if (NULL != getenv("AMQP_PASSWORD"))      { strlcpy(amqp_password, getenv("AMQP_PASSWORD"), sizeof(amqp_password)); }
  if (NULL != getenv("AMQP_VHOST"))         { strlcpy(amqp_vhost, getenv("AMQP_VHOST"), sizeof(amqp_vhost)); }
  if (NULL != getenv("AMQP_EXCHANGE"))      { strlcpy(amqp_exchange, getenv("AMQP_EXCHANGE"), sizeof(amqp_exchange)); }
  if (NULL != getenv("AMQP_ROUTINGKEY"))    { strlcpy(amqp_routingkey, getenv("AMQP_ROUTINGKEY"), sizeof(amqp_routingkey)); }

  // overwriting current config with command line options?
  while(1) {
    struct option long_options[] =
    {
      {"device",      required_argument,  0,  'D'},
      {"debug",       required_argument,  0,  'd'},
      {"quiet",       no_argument,        0,  'q'},
      {"help",        no_argument,        0,  '?'},
      {"host",        required_argument,  0,  'H'},
      {"port",        required_argument,  0,  'p'},
      {"user",        required_argument,  0,  'U'},
      {"pass",        required_argument,  0,  'P'},
      {"exchange",    required_argument,  0,  'E'},
      {"key",         required_argument,  0,  'K'},
      {"vhost",       required_argument,  0,  'V'},
      {"foreground",  no_argument,        0,  'f'},
      {0, 0, 0, 0}
    };
    int c;
    int option_index = 0;
    c = getopt_long(argc, argv, "D:d:H:p:U:P:E:K:V:fq?",
                    long_options, &option_index);
    if(c == -1)
      break;

    switch(c) {
      case 0: // no_argument
        break;
      case 'D':
        strlcpy(serial_device, optarg, sizeof(serial_device));
        break;
      case 'H':
        strlcpy(amqp_hostname, optarg, sizeof(amqp_hostname));
        break;
      case 'p':
        amqp_port = atoi(optarg);
        break;
      case 'U':
        strlcpy(amqp_username, optarg, sizeof(amqp_username));
        break;
      case 'P':
        debug_print(1, "WARNING: password supplied on command line; Use AMQP_PASSWORD instead");
        strlcpy(amqp_password, optarg, sizeof(amqp_password));
        break;
      case 'E':
        strlcpy(amqp_exchange, optarg, sizeof(amqp_exchange));
        break;
      case 'K':
        strlcpy(amqp_routingkey, optarg, sizeof(amqp_routingkey));
        break;
      case 'V':
        strlcpy(amqp_vhost, optarg, sizeof(amqp_vhost));
        break;
      case 'd':
        debug_level = atoi(optarg);
        debug_level = debug_level >= 0 ? debug_level : DEBUGLEVEL;
        debug_level = debug_level <= 9 ? debug_level : DEBUGLEVEL;
        break;
      case 'f':
        foreground_flag = 1;
        break;
      case 'q':
        debug_level = -1;
        break;
      case '?':
      default:
        print_help(argv[0]);
        exit(1);
    }
  }

  // validate config
  if (amqp_port < 0)      { bomb(1, "Bad port"); }
  if (amqp_port > 65535)  { bomb(1, "Bad port"); }
  
  // initialize the internal ringbuffer/fifo queue
  int queue_size = QUEUE_MAX_ITEMS;
  fifo_init(queue_size);

  // ready to handle input; daemonize if required
  // refer: http://www.danielhall.me/2010/01/writing-a-daemon-in-c/
  if (0 == foreground_flag) {
    pid_t pid;
    pid = fork();
    // If the pid is less than zero, something went wrong when forking
    if (pid < 0)
      bomb(EXIT_FAILURE, "Failed to fork child process");
    /* If the pid we got back was greater than zero, then the clone was
       successful and we are the parent. */
    if (pid > 0)
      exit(EXIT_SUCCESS);

    // If execution reaches this point we are the child
    daemonized = 1;
    umask(0);
    pid_t sid;
    sid = setsid();
    // set session id for child process
    if (sid < 0)
      bomb(EXIT_FAILURE, "Could not create process group for child process");
    // chdir to root
    if ((chdir("/")) < 0)
      bomb(EXIT_FAILURE, "Could not change working directory to /");
    // Close the standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
  }


  // Refer: http://www.yolinux.com/TUTORIALS/LinuxTutorialPosixThreads.html
  pthread_t thread1, thread2;
  int iret1, iret2;
  
  // Create independent threads
  debug_print(3, "Creating SERIO thread");
  iret1 = pthread_create( &thread1, NULL, &thr_read_from_serial, NULL);
  debug_print(3, "Creating AMQP thread");
  iret2 = pthread_create( &thread2, NULL, &thr_amqp_publish, NULL);

  /* Wait till threads are complete before main continues. Unless we
   * wait we run the risk of executing an exit which will terminate
   * the process and all threads before the threads have completed.
   */
  debug_print(3, "Joining serial port operations thread");
  pthread_join( thread1, NULL);
  debug_print(3, "Joining AMQP operations thread");
  pthread_join( thread2, NULL);
}
