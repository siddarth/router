#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define SUCCESS 0
#define FAILURE -1

#define DEBUGGING_LOW 0
#define DEBUGGING_HIGH 0

#define TRUE 1
#define FALSE 0

#define TIMEOUT 1

/*
 * Packet format.
 *
 * Based on echoing the size of packets received, it looks like they're
 * exactly 1472 bytes.
 *
 */
struct Packet {
  char payload[1472];
};
typedef struct Packet Packet;

/*
 * The Target struct, consists of the infomration provided in the command
 * line.
 */
struct Target {
  int raw_port;
  float target_rate;
  int shaped_port;
  int tokens;
};

/*
 * The Shaper struct to store the number of targets, and the targets
 * themselves.
 */
struct Shaper {
  struct Target targets[10];
  int num_targets;
};
typedef struct Shaper Shaper;
Shaper shaper;

/* Global var for sending packets */
struct sockaddr_in sin_sender;
int sender_socket;

int initialize(int argc, char **argv);
void print_shaper();
void send_one_packet(int port, Packet p);
void shape();

/*
* int
* initialize
*
* Checks validity of command line arguments, and fills the
* global `shaper` struct 
*/
int initialize(int argc, char **argv) {
  int s;
  struct timeval now;
  struct hostent *hp;
  gettimeofday(&now, NULL);

  /* Initialize globar var sin_sender */
  char host[10] = "localhost";
  memset(&sin_sender, 0, sizeof(sin_sender));
  sin_sender.sin_family = AF_INET;
  hp = gethostbyname(host);
  memcpy(&sin_sender.sin_addr, hp->h_addr, sizeof(sin_sender.sin_addr));

  /* Initialize global var sender_socket */
  sender_socket = socket(AF_INET, SOCK_DGRAM, 0);

  /* Parse cmd line arguments and fill `shaper` */
  shaper.num_targets = 0;

  /* Max ten flows */
  if(argc > 10) {
    printf("Error: maximum of 10 flows.\n");
    return FAILURE;
  }

  int raw_port, shaped_port;
  float target_rate;
  for(int i=1; i<argc; i++) {
    if(sscanf(argv[i], "%d:%f:%d",&raw_port, &target_rate, &shaped_port) != 3) {
      printf("Error: invalid arguments.\n");
      return FAILURE;
    }

    shaper.num_targets++;

    shaper.targets[i-1].raw_port = raw_port;
    shaper.targets[i-1].target_rate = target_rate;
    shaper.targets[i-1].shaped_port = shaped_port;
  }

  return SUCCESS;
}

/*
 * void
 * print_shaper
 *
 * Prints out the targets of our shaper
 */
void print_shaper() {
  for(int i=0; i< shaper.num_targets; i++) {

    if(i%3 == 0 && i!=0)
      printf("\n");
    printf("Target[%d] = %d:%f:%d;\t",
            i,
            shaper.targets[i].raw_port,
            shaper.targets[i].target_rate,
            shaper.targets[i].shaped_port);
   }
}
  

/*
 * void
 * shape
 *
 * Listens and receives packet on the raw ports as specified in the cmd line.
 * Upon receiving packets, checks the token buckets, and forwards.
 *
 * A lot of the code below is from the example pa-one-recv.c file.
 */
void shape() {

  fd_set mask;
  Packet p;
  int n, id, s[10], len, isBound=0, cc;
  struct timeval tv;

  int num_targets = shaper.num_targets;

  for(int i=0; i<num_targets; i++)
    s[i] = socket(AF_INET, SOCK_DGRAM, 0);

  if(s < 0){
    perror("pa-one-recv: socket");
    exit(1);
  }

  /* Bind to each raw port */
  struct sockaddr_in sin;
  for(int i=0; i < num_targets; i++) {
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    int port = shaper.targets[i].raw_port;
    sin.sin_port = htons(port);
    if(bind(s[i], (struct sockaddr *)&sin, sizeof(sin)) < 0){
      perror("shape: bind");
    }
  }

  /* Set initial timeout for select() */
  tv.tv_sec = TIMEOUT;
  tv.tv_usec = 0;

  while(1){

    FD_ZERO(&mask);
    for(int i=0; i<num_targets; i++)
      FD_SET(s[i], &mask);

    /* select() the highest file descriptor */
    n = select(s[num_targets - 1] + 1, &mask, (fd_set*)0, (fd_set*)0, &tv);
    if(n < 0){
      perror("select");
      exit(1);
    }

    if(n == 0){

      /* Reset the number of tokens */
      for(int i=0; i<num_targets; i++)
        shaper.targets[i].tokens = shaper.targets[i].target_rate*125*1000*TIMEOUT;

      /* Reset timeout */
      tv.tv_sec = TIMEOUT; 
      tv.tv_usec = 0;

      continue;
    }

    /* If a flag is set on any of the file descriptors */
    for(int i=0; i<num_targets; i++) {
      if(FD_ISSET(s[i], &mask)) {

        len = sizeof(sin);

        cc = recvfrom(s[i], &p, sizeof(p), 0,
                      (struct sockaddr *)&sin, &len);
        if(cc < 0){
          perror("shape: recvfrom");
          exit(1);
        }

        if(cc == sizeof(p)){
          /* 
           * Check to see if tokens are available in the bucket. Update the
           * number of tokens, and send forward to shaped port.
           */
          shaper.targets[i].tokens -= sizeof(p);
          if(shaper.targets[i].tokens > 0) {
            send_one_packet(shaper.targets[i].shaped_port, p);
          }
          else {
            /* Drop packet */
          }
        } else {
          printf("  The length is wrong.\n");
        }
        fflush(stdout);
      }
    }
  }
}

/*
 * void
 * send_one_packet
 *
 * send_one_packet sends a packet to the specified port on HOST
 */
void send_one_packet(int port, Packet p) {
  sin_sender.sin_port = htons(port);

  if(sender_socket < 0){
    perror("send_one_packet: socket");
    exit(1);
  }

  if(sendto(sender_socket, &p, sizeof(p), 0, (struct sockaddr *)&sin_sender, sizeof(sin_sender)) < 0){
    perror("send_one_packet: sendto");
  }
}

int main(int argc, char **argv) {

  /* Load arguments */
  if(initialize(argc, argv) != SUCCESS) {
    printf("Usage:./shaper raw_port1:target_rate1:shaped_port1 raw_port2:target_rate2:shaped_port2 ...");
    exit(-1);
  }
  
  /* Begin shaping */
  shape();

  return 0;
}
