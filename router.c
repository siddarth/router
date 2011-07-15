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

#define NUM_THREADS 5
#define SUCCESS 0
#define FAILURE -1

#define MAX_INT 35

#define TIMEOUT 1

#define HOST "localhost"

#define NEIGHBOR_LAG 3

#define UNSET -1

/* Different packet types */
#define PING 1
#define MSG 2
#define PV 3
#define DATA 4

#define TRUE 1
#define FALSE 0

/*
 * Struct Neighbor, stores the ID, port #, and time 
 * that it was last heard from.
 */
struct Neighbor {
  char key[10];
  int id;
  int is_paired;
  int port;
  long int last_seen;
};
typedef struct Neighbor Neighbor;

/*
 * Struct Path_vector contains the destination
 * and path of the vector
 */
struct Path_vector {
  int dest;
  int path[20];
};

/*
 * Packet used to ping neighbors
 */
struct Ping_packet {
  int sender_LS_port;
  int sender_PV_port;
  int sender_id;
  long int timestamp;
};
typedef struct Ping_packet Ping_packet;

/*
 * Packet used to send messages to routers (via the console)
 */
struct Msg_packet {
  int dest;
};
typedef struct Msg_packet Msg_packet;

/*
 * Packet that contains the path vector, along with key
 */
struct Pv_packet {
  int key[10];
  int sender_PV_port;
  struct Path_vector pv;
};
typedef struct Pv_packet Pv_packet;

/*
 * Data packet used for communicating link-state information
 * */
struct Link_state_packet {
  Neighbor neighbors[20];
  int num_neighbors;
  int seen_by[20];
  int sender_LS_port; 
  int sender_id;
  long int timestamp;
};
typedef struct Link_state_packet Link_state_packet;

/*
* All the information relevant to the router.
*/ 
struct Router {
	int routing_table[20][20];
  Neighbor neighbors[20];
  int border_router_neighbors[5];
  int id;
  int is_border_router;
  int is_preferred[20];
  int is_rejected[20];
  int myLSport; // terrible naming, but sticking with the specs
  int myPVport;
  int num_border_neighbors;
  int num_neighbors;
  long int network_matrix[20][20];
  long int uses_path_vector[20];
};
typedef struct Router Router;

/*
 * Global variables
 */
Router router;
/* Following are variables while sending and initialized in initalize() */
int sender_socket = -1;
struct sockaddr_in sender_sin;

/* Functions */
int dijkstra(int init);
int initialize(int argc, char **argv);
void check_timestamps();
void create_peering_session(int id, int port, char key[10]);
void handle_stdin(char buff[80]);
void ping_neighbors();
void print_neighbors();
void print_router();
void print_routing_table();
void process_link_state_packet(Link_state_packet p);
void process_msg_packet(Msg_packet p);
void process_pv_packet(Pv_packet p);
void recv_and_handle();
void reject(int id);
void send_data_packets();
void send_msg(int dest);
void send_one_packet(int port, int packet_type, Ping_packet pp,
                     Msg_packet sp, Pv_packet pvp, Link_state_packet dpp);

/*
* int
* initialize
*
* Initalizes global variables, checks validity of command line arguments,
* and fills the globar `router` variable
*/
int initialize(int argc, char **argv) {

  /* Initialize the global variable `sender_sin` */
  int myPVport, id, myLSport, i=1;
  struct hostent *hp;
  char host[10] = "localhost";
  memset(&sender_sin, 0, sizeof(sender_sin));
  sender_sin.sin_family = AF_INET;
  hp = gethostbyname(host);
  memcpy(&sender_sin.sin_addr, hp->h_addr, sizeof(sender_sin.sin_addr));

  /* Initialize global variable sender_socket */  
	sender_socket = socket(AF_INET, SOCK_DGRAM, 0);

  /* Make sure there are at least three arguments */
  if(argc <= 3) {
    printf("Too few arguments.\n"); 
    return FAILURE;
  }

  /* Check to see if -b is passed */
  if(strncmp(argv[1],"-b", 2) == 0) {
    router.is_border_router = TRUE;

    /* Get myPVport */
    if(sscanf(argv[2], "%d", &myPVport) == 0) {
      return FAILURE;
    }
    router.myPVport = myPVport;

    i=3;
  }
  else
    router.is_border_router = FALSE;

  /* Get ID */
  if(sscanf(argv[i++], "%d", &id) == 0) {
    return FAILURE;
  }
  if(id < 0 || id > 19) {
    printf("Router ID should be an integer in [0,19]");
    return FAILURE;
  }
  router.id = id;

  /* Get myLSport */
  if(sscanf(argv[i++], "%d", &myLSport) == 0) {
    return FAILURE;
  }
  router.myLSport = myLSport;

  /* Process all the neighbors */
  /* FIXME: Kinda hacked -- maybe there's a more beautiful way? */
  int j=0;
  int num_neighbors = argc - i;
  router.num_neighbors = num_neighbors;
  while(num_neighbors--) {
    int neighbor_port;
    if(sscanf(argv[i++], "%d", &(neighbor_port)) == 0) {
      return FAILURE;
    }
    else {
			router.neighbors[j].id = -1;
      router.neighbors[j].port = neighbor_port;
      router.neighbors[j].last_seen = -1;
      router.neighbors[j].is_paired = FALSE;
      j++;
    }
  }

  /* Initialize the router_matrix with 0s */
  for(int i=0; i<20; i++) {
    router.routing_table[i][0] = -1;
    for(int j=0; j<20; j++)
      router.network_matrix[i][j] = 0;
  }

  /* Initialize the is_preferred and is_rejected array with FALSEs */
  for(int i=0; i<20; i++) {
    router.is_preferred[i] = FALSE;
    router.is_rejected[i] = FALSE;
  }

  /* Initialize the routing table of this router to itself */
  router.routing_table[router.id][0] = router.id;

  return SUCCESS;
}

/*
 * void
 * print_router
 *
 * For debugging. Prints all values associated with a router.
 */
void print_router() {
  printf("Border Router?: %d\n\n", router.is_border_router);
  if(router.is_border_router == TRUE)
    printf("myPVport: %d\n\n", router.myPVport);
  printf("ID: %d\n\n", router.id);
  printf("myLSport: %d\n\n", router.myLSport);
  printf("Number of neighbors: %d\n\n", router.num_neighbors);
  printf("Neighbors: ");
  for(int i=0; i < router.num_neighbors; i++) {
    printf("%d (%ld)\t", router.neighbors[i].port, router.neighbors[i].last_seen);
  }
  printf("\n\n");
  printf("Current Neighbors:");
  for(int i=0; i<20; i++) {
    if(i%4 == 0)
      printf("\n");
    printf("%d\t", router.network_matrix[router.id][i]);
  }
  printf("\n\n");

  printf("Neighbor pairs:\n");
  for(int i=0; i<20; i++)
    for(int j=0; j<20; j++)
      if(router.network_matrix[i][j] != 0)
        printf("%d:%d = %d\n", i, j, router.network_matrix[i][j]);
    
  printf("Is rejected...");
  for(int i=0; i<20; i++)
    printf("%d -> %d \n", i, router.uses_path_vector[i]);
  printf("\n");
}


/*
 * void
 * check_timestamps
 *
 * Looks through the network matrix, and eliminates all old entries.
 */
void check_timestamps() {
  struct timeval now;
  gettimeofday(&now, NULL);
  long int current_time = now.tv_sec;
  for(int i=0; i<20; i++) {
    for(int j=0; j<20; j++) {
      long int value = router.network_matrix[i][j];
      if(value != 0) {
        if(value < (current_time - NEIGHBOR_LAG)) {
          router.network_matrix[i][j] = 0;
          if(i == router.id) {
            router.routing_table[j][0] = UNSET;
          }
        }
      }
    }
  }
}

/*
 * void
 * set_prefer_policy
 *
 * Takes a string of the form "P <num_stops> <stop1> <stop2> ..."
 * and updates the routing table accordingly.
 */
void set_prefer_policy(char buff[80]) {
  char *token = strtok(buff, " ");
  token = strtok(NULL, " ");
  int path_length = atoi(token);
  int path[path_length];
  token = strtok(NULL, " ");
  int i=0;
  while(token != NULL)
  {
    path[i] = atoi(token);
    token = strtok(NULL, " ");
    i++;
  }
  int dest = path[path_length - 1];
  for(i=0; i<path_length; i++)
    router.routing_table[dest][i] = path[i];

  router.is_preferred[dest] = TRUE;

  printf("Prefer policy set: ");
  printf("%d -> ", router.id);
  for(i=0; i< path_length-1; i++)
    printf("%d -> ", path[i]);
  printf("%d\n\n", dest);
}

/*
 * void
 * handle_stdin
 *
 * Handle the standard input from the user and call the appropriate
 * functions
 */
void handle_stdin(char buff[80]) {  
  char c;
  int i;

  /* If only one character is present */
  if(sscanf(buff, "%c", &c) == 1) {
    switch(buff[0]) {
      case 'N':
        print_neighbors();
        return;
      case 'T':
        print_routing_table();
        return;
      case 'p':
        print_router();
        return;
    }
  }

  /* To send messages */
  if(sscanf(buff, "%d", &i) == 1) {
    if(i >= 0 && i<=19)
      send_msg(i);
      return;
  }

  /* Reject policy */
 	if(sscanf(buff, "R %d", &i) == 1) {
    if(i < 0 || i > 19)
      printf("Invalid router ID specified.");
		else if(router.is_border_router == FALSE) {
			printf("Reject commands can be run only on border routers.\n");
		}
		else {
			printf("Router rejected: %d.", i);
      reject(i);
    }
    printf("\n\n");
		fflush(stdout);
		return;
	}

  /* Prefer policy */
  if(buff[0] == 'P') {
		if(router.is_border_router == FALSE) {
			printf("Commands to set preferred paths can be run only on \
              border routers.\n\n");
      fflush(stdout);
      return;
		}

    set_prefer_policy(buff);

    return;
  }

  int id, port;
  char key[10];
  /* Create a peering session */
  if(sscanf(buff, "S %d %d %s", &id, &port, key) == 3) {
		if(router.is_border_router == FALSE) {
			printf("Commands to create a peering session can be run only on \
              border routers.");
    }
    else {
      create_peering_session(id, port, key);
    }
    printf("\n\n");
    fflush(stdout);
    return;
  
  }

  printf("Invalid command.\n\n");
  return;
      
}

/*
 * void
 * reject
 *
 * Sets up reject policy for router with id=`id`
 */
void reject(int id) {
  router.is_rejected[id] = TRUE;

  /* Update routing table */
  for(int i=0; i<20; i++)
    for(int j=0; j<20; j++)
      if(router.routing_table[i][j] == id)
        router.routing_table[id][0] = -1;

}

/*
 * void
 * create_peering_session
 *
 * Sets up a peering session w/ router with given args
 */
void create_peering_session(int id, int port, char key[10]) {
  int num_neighbors = router.num_neighbors;
  router.neighbors[num_neighbors].id = id;
  router.neighbors[num_neighbors].port = port;
  router.neighbors[num_neighbors].is_paired = TRUE;
  strncpy(router.neighbors[num_neighbors].key, key, 10);
  router.num_neighbors = num_neighbors + 1;
  printf("Session created with router %d at port %d using key %s.", 
          id, port, key);
}

/*
 * void
 * send_msg
 *
 * Send a message to `dest` (as part of a msg sent via the console)
 */
void send_msg(int dest) {

  /* initialize packet */
  Msg_packet p;
  p.dest = htonl(dest);

  Link_state_packet dp; Ping_packet pp; Pv_packet pvp;

  int next_hop;

  dijkstra(router.id);

	next_hop = router.routing_table[dest][0];

  /* If the node is unreachable, drop the packet */
	if((next_hop == MAX_INT) || (next_hop == UNSET)) {
		printf("Unable to send message to %d.\n\n", dest);
		return;
	}

  /* If not, send it on to the next hop */
  for(int i=0; i<router.num_neighbors; i++) {
    if(router.neighbors[i].id == next_hop) {
      send_one_packet(router.neighbors[i].port, MSG, pp, p, pvp, dp);
      break;
    }
  }

  printf("%d\n\n", next_hop);
  fflush(stdout);

}

/*
 * Ping all the neighbors of the router
 */
void ping_neighbors() {

  /* Initialize packet */
  Ping_packet p;
  Link_state_packet dp; Msg_packet mp; Pv_packet pvp;

  /* Get current time and set timestamp */
  struct timeval now;
  gettimeofday(&now, NULL);
  long int current_time = now.tv_sec;
  p.timestamp = htonl(current_time);

  /* Set the other credentials */
  p.sender_id = htonl(router.id);
  p.sender_LS_port = htonl(router.myLSport);
  p.sender_PV_port = htonl(router.myPVport);

  /* Ping each neighbor with the packet */
  for(int i=0; i < router.num_neighbors; i++)
    send_one_packet(router.neighbors[i].port, PING,  p, mp, pvp, dp);
}

/*
 * void
 * send_path_vector_packets()
 *
 * Send all paired neighbors path vectors
 */
void send_path_vector_packets() {

  /* Initialize packet with credentials */
  Pv_packet p;
  p.sender_PV_port = htonl(router.myPVport);
 
  Link_state_packet dp; Msg_packet mp; Ping_packet pp;

  for(int i=0; i<router.num_neighbors; i++) {
    /* If a session is set up, send a Path Vector packet */
    if(router.neighbors[i].is_paired == TRUE) {

      /* Set the key of the  connection */
      for(int j=0; j<10; j++)
        p.key[j] = htonl(router.neighbors[i].key[j]);

      for(int j=0; j<20; j++) {

        /* If there is some path to a given router */
        if(router.routing_table[j][0] != UNSET) {

          /* Set the destination of the path */
          p.pv.dest = htonl(j);

          /* 
           * Set the first element of the path to itself, so that we
           * don't have to worry about processing it at the other end.
           */
          p.pv.path[0] = htonl(router.id);

          int k;

          /* Copy the path over from the routing table */
          for(k=0; k<20; k++) {
            p.pv.path[k+1] = htonl(router.routing_table[j][k]);

            /* Break when  we reach the destination */
            if(router.routing_table[j][k] == j)
              break;
          }

          send_one_packet(router.neighbors[i].port, PV, pp, mp, p, dp);
        }
      }
    }
  }
}

/*
 * void
 * send_data_packets
 *
 * Send data packets to all neighbors
 */
void send_data_packets() {  
  Link_state_packet p;

  Pv_packet pvp; Ping_packet pp; Msg_packet mp;
  /* set timestamp */
  struct timeval now;
  gettimeofday(&now, NULL);
  long current_time = now.tv_sec;
  p.timestamp = htonl(current_time);

  /* Set sender_LS_port and ID */
  p.sender_LS_port = htonl(router.myLSport);
  p.sender_id = htonl(router.id);

  /* Set the seen by flags for all routers except this one to FALSE */
  for(int i=0; i<20; i++)
    p.seen_by[i] = htonl(FALSE);
  p.seen_by[router.id] = htonl(TRUE);

  /* Set the neighbors */
  for(int i=0; i<router.num_neighbors; i++) {
    p.neighbors[i].port = htonl(router.neighbors[i].port);
    p.neighbors[i].last_seen = htonl(router.neighbors[i].last_seen);
    p.neighbors[i].id = htonl(router.neighbors[i].id);
  }
  p.num_neighbors = htonl(router.num_neighbors);

  for(int i=0; i<router.num_neighbors; i++)
    if(router.neighbors[i].is_paired == FALSE)
      send_one_packet(router.neighbors[i].port, DATA, pp, mp, pvp, p);

}

/*
 * void
 * dijkstra
 *
 * Runs Dijkstra's algorithm on all the nodes beginning with
 * `init`
 */
int dijkstra(int init) {

  int dist[20];
  int visited_nodes[20];
  int previous[20];

  /* set all the distances except from the initial node to MAX_INT */
  for(int i=0; i<20; i++) {
    dist[i] = MAX_INT;
    visited_nodes[i] = FALSE;
    previous[i] = -1;
  }
  dist[init] = 0;

  int curr = init;
  while(1) {

    /* Mark current node as visited */
    visited_nodes[curr] = TRUE;

    /* For each of the neighbors of current node which are alive */
    for(int i=0; i<20; i++) {
      if((router.network_matrix[curr][i] != 0) && 
         (router.network_matrix[i][curr] != 0)) {
        /* 
				 * check if new path is cheaper than old path, if so update the 
				 * distance 
				 */ 
        int new_dist = dist[curr] + 1;
        if(new_dist < dist[i]) {
          dist[i] = new_dist;
          previous[i] = curr;
        }
      }
    }
    int min_dist = MAX_INT;
    int min_node = -1;

    /* 
		 * Check to see if there's an unvisited node, that has been reached from 
		 * one of the nodes we've looked through
		 */
    for(int i=0; i<20; i++) {
      if(visited_nodes[i] == FALSE) {
        if(dist[i] < min_dist) {
          min_dist = dist[i];
          min_node = i;
        }
      }
    }
    /* If so, set the node with the least distance as current. */
    if(min_node == -1)
      break;
    else
      curr = min_node;
  }

  int prev;
	/* 
	 * Look through each of the distances we have calculated, and
	 * update the routing table appropriately.
	 */
  for(int i=0; i<20; i++) {
		/* If the node was reachable and is not itself */
    if((dist[i] != MAX_INT) &&
       (dist[i] != 0) &&
       (router.is_preferred[i] != TRUE) &&
       (router.uses_path_vector[i] != TRUE)) {
			/* calculate the path back to the initial node */
      prev = i;
      int path[dist[i]];
      int counter = dist[i] - 1;
      while(prev != init) {
        router.routing_table[i][counter] = prev;
        prev = previous[prev];
        counter--;
      }
    } 
    /* It was unreachable, hence reset the routing table */
		else if(router.uses_path_vector[i] != TRUE)
			router.routing_table[i][0] = UNSET;
  }
}

/*
 * void
 * print_neighbors
 *
 * Print all neighbors of the current node per the format specified in
 * the spec.
 */
void print_neighbors() {
    int router_id = router.id;
    for(int i=0; i<20; i++) {
      if(router.network_matrix[router_id][i] != 0)
        printf("%d ", i);
    }
    printf("\n\n");
}

/*
 * void
 * print_routing_table
 *
 * Prints the routes to the reachable neighbors.
 */
void print_routing_table() {

  dijkstra(router.id);

	for(int i=0; i<20; i++) {
		if(router.routing_table[i][0] != UNSET)
			for(int j=0; j<20; j++) {
				printf("%d ", router.routing_table[i][j]);
				if(router.routing_table[i][j] == i) {
					printf("\n");
					break;
				}
			}
  }
  printf("\n");
}

/*
 * If a given packet was of type PING, we can be sure that is from one of the
 * router's neighbors. So, simply update the last seen of that neighbor, and
 * don't forward the packet
 */
void process_ping_packet(Ping_packet p) {
  int sender_id, sender_LS_port, sender_PV_port;
  long int timestamp;

  /* Get all the values stored in the packet */
  sender_id = ntohl(p.sender_id);

  /* Drop if the id is one that we have rejected */
  if(router.is_rejected[sender_id] == TRUE)
    return;

  sender_LS_port = ntohl(p.sender_LS_port);
  sender_PV_port = ntohl(p.sender_PV_port);
  timestamp = ntohl(p.timestamp);

  /* Update the last seen for that neighbor */
  int num_neighbors = router.num_neighbors;
  for(int i=0; i < num_neighbors; i++) {
    if((router.neighbors[i].port == sender_LS_port) ||
       ((router.neighbors[i].port == sender_PV_port) && 
        (router.neighbors[i].is_paired == TRUE))) {
      router.neighbors[i].last_seen = timestamp;
      router.neighbors[i].id = sender_id;
      /* Update the network adjacency matrix */
      router.network_matrix[router.id][sender_id] = timestamp;
      router.network_matrix[sender_id][router.id] = timestamp;
      break;
    }
  }

}

/*
 * If a given packet is of type MSG, if this router is not the destination
 * of the message, send it along its path
 */
void process_msg_packet(Msg_packet p) {
  int dest;
  dest = ntohl(p.dest);

  /* Make sure the destination is a valid id */
  if((dest < 0) || (dest > 19)) {
    printf("Invalid destination.\n");
    return;
  }

  /* If this router is the destinatioon of the msg, just return */
  if(dest == router.id)
      return;
 
  send_msg(dest);
}

/*
 * void
 * process_pv_packet
 *
 * Process path vector packet and update routing table 
 */
void process_pv_packet(Pv_packet p) {
  
  int sender_PV_port, dest;
  long int timestamp;

  char key[10];
  
  /* Get credentials from packet */
  sender_PV_port = ntohl(p.sender_PV_port);
  dest = ntohl(p.pv.dest);

  for(int i=0; i<10; i++)
    key[i] = ntohl(p.key[i]);

  int valid = FALSE;

  /* Ensure that the packet is from a paired neighbor */
  for(int i=0; i<router.num_neighbors; i++) {
    if((router.neighbors[i].port == sender_PV_port) &&
       (router.neighbors[i].is_paired == TRUE)) {

      if(strncmp(key, router.neighbors[i].key, 10) == 0)
        valid = TRUE;
     }
   }
   if(valid == FALSE)
     return;

  /* If the destination is this router itself, drop it */
  if(dest == router.id)
    return;

  /* Don't bother if you already have a preferred path */
  if(router.is_preferred[dest])
    return;

  /* Get the advertised_path from the packet */
  int advertised_path[20];
  for(int i=0; i<20; i++)
    advertised_path[i] = ntohl(p.pv.path[i]);

  /* Get the length of the current path */
  int current_path_length = 0;
  if(router.routing_table[dest][0] != UNSET) {
    for(int i=0; i<20; i++) {
      current_path_length++;
      if(router.routing_table[dest][i] == dest)
        break;
    }
  }
  
  /* Get the length of the advertised path */
  int advertised_path_length = 0;
  for(int i=0; i<20; i++) {
    advertised_path_length++;
    if(advertised_path[i] == dest)
      break;
    else
      continue;
  }

  /* Don't bother if the current length is smaller */
  if((router.uses_path_vector[dest] != FALSE) &&
     ((current_path_length <= advertised_path_length) &&
     (current_path_length != 0)))
    return;

  /* Ensure that none of the rejected routers on the path */
  for(int i=0; i<20; i++) {
    int hop = advertised_path[i];
    if(router.is_rejected[hop] == TRUE)
      return;
    if(hop == dest)
      break;
  }

  /* Set the uses_path_vector for that dest to be true */
  router.uses_path_vector[dest] = timestamp;

  /* Actually copy it all over */
  for(int i=0; i < advertised_path_length; i++) {
    router.routing_table[dest][i] = advertised_path[i];
  }

  return;
}

/*
 * void
 * process_link_state_packet
 *
 * Process link state packet and update the network_matrix
 */
void process_link_state_packet(Link_state_packet p) {
  int seen_by[20], sender_LS_port, sender_id, num_neighbors;
  long int timestamp;
  Neighbor neighbors[20];
  
  /* Get current time */
  struct timeval now;
  gettimeofday(&now, NULL);
  long int current_time = now.tv_sec;

  /* Get time stamp, and if the packet is really old, drop it */
  timestamp = ntohl(p.timestamp);
  sender_LS_port = ntohl(p.sender_LS_port);
  sender_id = ntohl(p.sender_id);

  if(router.is_rejected[sender_id] == TRUE)
    return;

  /* If the packet is really old, drop it */
  if((current_time - timestamp) > (NEIGHBOR_LAG + 3))
    return;

  /* Get the number of neighbors */
  num_neighbors = ntohl(p.num_neighbors);

  /* Get all the neighbors */
  for(int i=0; i<num_neighbors; i++) {
    neighbors[i].id = ntohl(p.neighbors[i].id);
    neighbors[i].port = ntohl(p.neighbors[i].port);
    neighbors[i].last_seen = ntohl(p.neighbors[i].last_seen);
  }

  /* Update the network_matrix to the last time the neighbors were seen */
  for(int i=0; i<num_neighbors; i++) {
    if(neighbors[i].id != UNSET) {
      router.network_matrix[sender_id][neighbors[i].id] = neighbors[i].last_seen;
      router.network_matrix[neighbors[i].id][sender_id] = neighbors[i].last_seen;
    }
  }

  /* Get a list of who all have seen the router, and forward */
  for(int i=0; i<20; i++)
    seen_by[i] = ntohl(p.seen_by[i]);

  for(int i=0; i<router.num_neighbors; i++) {
    int neighbor_port = router.neighbors[i].port;
    int neighbor_id = router.neighbors[i].id;

    /* If it has been seen by this neighbor, skip it */
    if(seen_by[neighbor_id] == TRUE)
      continue;
    else {
      /* Mark as seen by this router */
      p.seen_by[router.id] = htonl(TRUE);

      /* Create empty packets, and send_one_packet */
      Ping_packet pp; Msg_packet mp; Pv_packet pvp;
      send_one_packet(neighbor_port, DATA, pp, mp, pvp, p);
    }
  }

  return;
}

/*
 * void
 * recv_and_handle
 *
 * Receives, and handles packets received on the link-state port
 * and the path vector port (if it is a border router)
 *
 * A lot of the code below is from the example pa-one-recv.c file.
 */
void recv_and_handle() {
  fd_set mask;

  char recv_buffer[700];

  char buff[512];
  int n, id, s[2], len, isBound=0, cc;
  struct sockaddr_in sin;
  struct timeval tv;

  /* Initialize the sockets */
  for(int i=0; i<2; i++) {
    s[i] = socket(AF_INET, SOCK_DGRAM, 0);
    if(s[i] < 0){
      perror("pa-one-recv: socket");
      exit(1);
    }
  }

  /* Set the header */
  memset(&sin, 0, sizeof(sin));
  sin.sin_family = AF_INET;

  /* If it is a border router, bind to the path vector port */
  if(router.is_border_router) {
    sin.sin_port = htons(router.myPVport);
    if(bind(s[0], (struct sockaddr *)&sin, sizeof(sin)) < 0) {
      perror("pa-one-recv: bind");
      exit(1);
    }
  }

  /* Bind to the link state port */
  sin.sin_port = htons(router.myLSport);
  if(bind(s[1], (struct sockaddr *)&sin, sizeof(sin)) < 0){
    perror("pa-one-recv: bind");
    exit(1);
  }

  /* Set the initial timeouts */
  tv.tv_sec = TIMEOUT;
  tv.tv_usec = 0;

  while(1){

    /* Check the time stamps on the network matrix */
    check_timestamps();

  
    FD_ZERO(&mask);
    FD_SET(fileno(stdin), &mask);

    /* Reset flags */
    for(int i=0; i<2; i++)
      FD_SET(s[i], &mask);

    /* Select the highest socket file descriptor */
    n = select(s[1]+1, &mask, (fd_set*)0, (fd_set*)0, &tv);

    /* If there was an error selecting */
    if(n < 0){
      perror("select");
      exit(1);
    }

    /* On time out */
    if(n == 0){

      /* Update `tv` back to TIMEOUT */
      tv.tv_sec = TIMEOUT;
      tv.tv_usec = 0;

      /* Ping all the neighbors */
      ping_neighbors();

      /* Send path vector updates to all peered border routers */
      send_path_vector_packets();

      /* Send the data packets w/ all the neighbors to all neighbors */
      send_data_packets();
 
      dijkstra(router.id);
      continue;
    }

    /* If some text was entered in the console */
    if(FD_ISSET(fileno(stdin), &mask)) {
      fgets(buff, sizeof(buff), stdin);
      handle_stdin(buff);
    }

    /* If either of the ports (path vector, or link state) received a packet */
    for(int i=0; i<2; i++) {
      if(FD_ISSET(s[i], &mask)) {
        len = sizeof(sin);

        /* Receive and store in the recv_buffer */
        cc = recvfrom(s[i], &recv_buffer, sizeof(recv_buffer), 0,
                      (struct sockaddr *)&sin, &len);

        if(cc < 0){
          perror("pa-one-recv: recvfrom");
          exit(1);
        }
        /* Based on the size of the packet, process it accordingly */
        if(cc == sizeof(Ping_packet)){
          Ping_packet p;
          memcpy(&p, recv_buffer, sizeof(p));
          process_ping_packet(p);
        }
        else if(cc == sizeof(Msg_packet)) {
          Msg_packet p;
          memcpy(&p, recv_buffer, sizeof(p));
          process_msg_packet(p);
        }
        else if(cc == sizeof(Pv_packet)) {
          Pv_packet p;
          memcpy(&p, recv_buffer, sizeof(p));
          process_pv_packet(p);
        }
        else if(cc == sizeof(Link_state_packet)) {
          Link_state_packet p;
          memcpy(&p, recv_buffer, sizeof(p));
          process_link_state_packet(p);
        }
        else {
          printf("  The length %d, is wrong.\n", cc);
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
 * Sends a packet of type `packet_type` to `port`
 */
void send_one_packet(int port,
                     int packet_type,
                     Ping_packet pp,
                     Msg_packet mp,
                     Pv_packet pvp, 
                     Link_state_packet dp) {

  /* Set the port of the header to the passed port */
  sender_sin.sin_port = htons(port);

  if(sender_socket < 0){
    perror("pa-one-send: socket");
    exit(1);
  }

  /* Based on the packet_type, send the packet */
  switch(packet_type) {
    case PING:
      if(sendto(sender_socket, &pp, sizeof(pp), 0,
               (struct sockaddr *)&sender_sin, sizeof(sender_sin)) < 0) {
        perror("pa-one-send: ping");
        exit(-1);
      }
      break;
    case MSG:
      if(sendto(sender_socket, &mp, sizeof(mp), 0, (struct sockaddr *)&sender_sin, sizeof(sender_sin)) < 0){
        perror("pa-one-send: msg");
        exit(-1);
      }
      break;

    case PV:
      if(sendto(sender_socket, &pvp, sizeof(pvp), 0, (struct sockaddr *)&sender_sin, sizeof(sender_sin)) < 0){
        perror("pa-one-send: path-vector");
        exit(-1);
      }
      break;

    case DATA:
      if(sendto(sender_socket, &dp, sizeof(dp), 0, (struct sockaddr *)&sender_sin, sizeof(sender_sin)) < 0){
        perror("pa-one-send: data-packet");
        exit(-1);
      }
      break;
  }
}

int main(int argc, char **argv) {
  if(initialize(argc, argv) != SUCCESS) {
    printf("Error: enter valid arguments.\n");
    printf("Usage:\n./router ID myLSport port1 [port2 ...], OR\n");
    printf("./router -b myPVport ID myLSport port1 [port2 ...]\n");
    exit(-1);
  }

  recv_and_handle();

  return 0;
}
