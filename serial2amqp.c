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
#include <stdint.h>   // avoid error: unknown type name on arm architecture
#include <string.h>   // strchr()
#include <strings.h>  // bzero()
#include <fcntl.h>    // open()
#include <unistd.h>   // read()
#include <stdlib.h>   // exit()
#include <getopt.h>   // getopt_long()
#include <signal.h>   // handling signals
#include <termios.h>  // all serial port functions and constants
#include <syslog.h>   // logging
#include <time.h>     // gmtime for timestamping msgs
//#include <sys/types.h>
#include <sys/stat.h> // umask()

#include <amqp_tcp_socket.h>
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

  // restore the old port settings
  tcsetattr(fd, TCSANOW, &oldtio);

  // Terminate program
  exit(signum);
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
 * AMQP FUNCTIONALITY
 *****************************************************************************/

void die_on_error(int x, char const *context) {
  if (x < 0) {
    char *errstr = amqp_error_string2(x);
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
      fprintf(stderr, "%s: %s\n", context, amqp_error_string2(x.library_error));
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



int amqpsend(const char *msg) {
  int amqp_channel = 1; // TODO: handle dynamic channel number
  int status;
  amqp_socket_t *socket = NULL;
  amqp_connection_state_t conn;
  amqp_bytes_t messagebody;

  // build the message body
  messagebody = amqp_cstring_bytes(msg);

  // open a connection to the server
  debug_print(2, "Connecting to AMQP Broker");
  conn = amqp_new_connection();
  socket = amqp_tcp_socket_new(conn);
  if (!socket) { 
    debug_print(2, "ERROR creating TCP socket");
    return 1;
  }
  status = amqp_socket_open(socket, amqp_hostname, amqp_port);
  if (status) {
    debug_print(2, "ERROR opening TCP socket");
    return 1;
  }

  // authenticate
  debug_print(2, "Authenticating");
  die_on_amqp_error(
      amqp_login(
        conn, amqp_vhost, 0, 131072, 0, AMQP_SASL_METHOD_PLAIN, amqp_username, amqp_password
        ),
      "Authenticating");

  // open a channel
  debug_print(3, "Opening a channel");
  amqp_channel_open(conn, amqp_channel);
  die_on_amqp_error(amqp_get_rpc_reply(conn), "Opening channel");

  // build the message frame
  debug_print(3, "Building AMQP Message Frame");
  amqp_basic_properties_t props;
  props._flags = AMQP_BASIC_CONTENT_TYPE_FLAG | AMQP_BASIC_DELIVERY_MODE_FLAG;
  props.content_type = amqp_cstring_bytes("text/plain");
  props.delivery_mode = AMQP_PERSISTENT;

  // send the message frame
  debug_print(2, "Publishing message to exchange/routing key");
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

  // cleanup by closing all our connections
  debug_print(4, "Cleaning up AMQP Connection");
  //debug_print(5, "Closing connection");
  //die_on_amqp_error(
  //    amqp_connection_close(conn, AMQP_REPLY_SUCCESS),
  //    "Closing connection");
  debug_print(5, "Ending connection");
  die_on_error(
      amqp_destroy_connection(conn),
      "Ending connection");

  return 0;
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

  int res;  // throaway value for capturing results of functions
  struct termios newtio;  // the settings we want for the serial port
  /*
   * Open modem device for reading and writing and not as controlling tty
   * because we don't want to get killed if linenoise sends CTRL-C.
  */
  debug_print(1, "Opening serial device:");
  debug_print(1, serial_device);
  fd = open(serial_device, O_RDONLY | O_NOCTTY );
  if (fd < 0) { perror(serial_device); exit(-1); }  // exit if fail to open

  debug_print(3, "Saving existing serial port settings");
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
  debug_print(3, "Cleaning line and activating port settings");
  tcflush(fd, TCIFLUSH);
  tcsetattr(fd, TCSANOW, &newtio);

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

  // serial port settings done, now handle input
  debug_print(2, "Begining loop");
  char buf[512];
  char dbgmsg[1024];
  char publish_buffer[1024];
  while (STOP == FALSE) {
    /* read blocks program execution until a line terminating character is
     * input, even if more than sizeof(buf) chars are input. If the number
     * of characters read is smaller than the number of chars available,
     * subsequent reads will return the remaining chars. res will be set
     * to the actual number of characters actually read
    */
    res = read(fd, buf, sizeof(buf));

    // switch the trailing newline for null
    buf[res-1] = '\0';

    // don't output blank lines
    if (res-1 < 1) {
      continue;
    }

    // log the raw input to file
    FILE * logfile;
    logfile = fopen("msglog.txt", "a");  
    if (logfile != 0) {
      fwrite(buf, 1, sizeof(buf), logfile);
      fclose(logfile);
    }
    free(logfile);

    // prepend a gmt timestamp to the msg before we publish it
    time_t    now;
    struct tm ts;
    char      tz[80];
    time(&now);
    ts = *localtime(&now);
    strftime(tz, sizeof(tz), "%Y-%m-%d %H:%M:%S%z,", &ts); // the ; is the delimiter for the published msg

    // build the buffer that we want to publish (with unix timestamp prepended)
    publish_buffer[0] = '\0';
    strncat(publish_buffer, tz, sizeof(publish_buffer));
    strncat(publish_buffer, buf, sizeof(publish_buffer));

    // show debug info
    snprintf(dbgmsg, sizeof(dbgmsg), "Recv %d characters: %s", res, publish_buffer);
    debug_print(1, dbgmsg);

    // output just the received data to stdout
    printf("%s\n", publish_buffer);
    int res = -1;
    while ( res != 0 ) {
      // just dumb keep retrying
      res = amqpsend(publish_buffer);
      sleep(2);
    }
  }

  // once we get here we are done so we can exit
  return 0;
}
