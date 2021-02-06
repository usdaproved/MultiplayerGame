#include <stdlib.h>
#include <cstdio>

#include <syslog.h>
#include <signal.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sys/stat.h>

#include <errno.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <string.h>
#include <poll.h>

#include "../game_types.h"

#define DAEMON_MODE 0

#define PORT "28976"

void signal_handler(int signal) {
  switch(signal){
  case SIGKILL:
  case SIGTERM:
    // daemon has been killed, cleanup.
    closelog();
    
    exit(0);
    break;
  default:
    break;
  }
}

void sigchild_handler(int s){
  // waitpd() might overwrite errno, so we want to save and restore it.
  int saved_errno = errno;

  // TODO(Trystan): look up what this is.
  while(waitpid(-1, NULL, WNOHANG) > 0);

  errno = saved_errno;
}

// Get sockaddr, IPv4 or IPv6
void * get_in_addr(struct sockaddr *sa){
  if(sa->sa_family == AF_INET){
    return &(((struct sockaddr_in *)sa)->sin_addr);
  }

  return &(((struct sockaddr_in6 *)sa)->sin6_addr);
}

struct square {
  v3 position;
  b32 connected;
};

#define TOTAL_CONNECTIONS 8

volatile square squares[TOTAL_CONNECTIONS];

// A few things to think about structure-wise:
// If we wait on all connections and then only act once we receive input,
// then two possibilities can happen, we can get spammed with messages and
// respond constantly. OR we will be waiting until some connection comes back,
// which is *kind of* fine, as nothing has changed so things don't need updated anyway.

// Our target is to hit 30 packets sent to each client a second.

struct network_host;

struct network_client{
  network_host *host;
  struct sockaddr address; // Address of the client.
  u32 address_length;
};

struct network_host{
  int socket;
  network_client *clients;
  int client_count;
};

// NOTE(Trystan): Maybe we can just get away with using the buffer for everything.
struct network_packet{
  uint8 *data;
  uint32 data_length;
};

// NOTE(Trystan): Just getting network_buffer to a round 256 bits, not thought out at all.
#define MAX_DATA_LENGTH 224
struct network_buffer{
  char data[MAX_DATA_LENGTH];
  uint32 data_length;
};

network_host * host_create(int client_count){
  network_host *host = (network_host *)malloc(sizeof(network_host));
  assert(host != NULL);
  memset(host, 0, sizeof(network_host));

  host->clients = (network_client *)malloc(client_count * sizeof(network_client));
  assert(host->clients != NULL);
  memset(host->clients, 0, client_count * sizeof(network_client));

  struct addrinfo hints = {};
  // NOTE(Trystan): AF_INET6 if we want to try IPv6
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_PASSIVE; // Use my IP

  struct addrinfo *server_info = NULL;
  getaddrinfo(NULL, PORT, &hints, &server_info);

  for(struct addrinfo *p = server_info; p != NULL; p = p->ai_next){
    host->socket = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if(host->socket != -1){
      // We got the socket we wanted.
      bind(host->socket, p->ai_addr, p->ai_addrlen);
      break;
    }
  }

  assert(host->socket != 0);

  freeaddrinfo(server_info);
  
  // Set the socket as non-blocking.
  int yes = 1;
  ioctl(host->socket, FIONBIO, &yes);
  setsockopt(host->socket, SOL_SOCKET, SO_BROADCAST, (char *)&yes, sizeof(int));

  network_client *current_client;
  host->client_count = client_count;

  for(current_client = host->clients;
      current_client < &host->clients[host->client_count];
      ++current_client){

    current_client->host = host;
  }

  return host;
}

void socket_receive(int socket, network_buffer *buffer){
  struct sockaddr_storage sending_address;
  socklen_t address_length = sizeof(sending_address);

  
  int received_length = recvfrom(socket, buffer->data, MAX_DATA_LENGTH-1, 0,
                                 (struct sockaddr *)&sending_address, &address_length);

  if(received_length > 0){
    char s[INET_ADDRSTRLEN];
    printf("Got packet from %s\n", inet_ntop(sending_address.ss_family,
                                             get_in_addr((struct sockaddr *)&sending_address),
                                             s, sizeof(s)));
    printf("Packet is %d bytes long\n", received_length);
    buffer->data[received_length] = '\0';
    printf("Packet contains \"%s\"\n", buffer->data);
  }
}

int socket_wait(int socket, u32 timeout){
  struct pollfd poll_socket;

  poll_socket.fd = socket;
  poll_socket.events = POLL_IN;

  int poll_count = poll(&poll_socket, 1, timeout);

  if (poll_count == 0){
    return 0;
  }

  if (poll_socket.revents & POLL_IN){
    return (int)POLL_IN;
  }

  return 0;
}

int main(int argc, char **argv){
#if DAEMON_MODE
  pid_t pid, sid;

  pid = fork();
  if(pid < 0){
    return -1;
  }

  if(pid > 0){
    // Succesfully forked, close the parent process.
    return 0;
  }

  umask(0);

  sid = setsid();
  if(sid < 0){
    return -1;
  }

  if((chdir("/etc/gamed")) < 0){
    syslog(LOG_NOTICE, "couldn't change dir to /etc/gamed\n");
    return -1;
  }

  signal(SIGKILL, signal_handler);
  signal(SIGTERM, signal_handler);
#endif

  // I believe this is for only when we are forking child processes to handle individual connections.
  // with poll() we are handling all connections from the main thread.
#if 0
  struct sigaction sa = {};
  sa.sa_handler = sigchild_handler; // reap all dead processes.
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = SA_RESTART;
  if(sigaction(SIGCHLD, &sa, NULL) == -1){
    perror("sigaction");
    exit(1);
  }

#endif

  // I want a similar thing where we get all the memory we need at startup.
  // I want this thing to be able to run without crashing after we pass initialization. 
  // Handle connects and disconnects.
  // Run the server at 128 ticks, sending out packets 60 times per second.
  network_host *host = host_create(TOTAL_CONNECTIONS);

  printf("server: waiting for connections...\n");

  for(;;){
    // Check every 100 milliseconds if a message is pending.
    int wait_condition = socket_wait(host->socket, 100);
    if(wait_condition == POLL_IN){
      network_buffer buffer = {};
      socket_receive(host->socket, &buffer);
    }
  }

  return 0;
}
