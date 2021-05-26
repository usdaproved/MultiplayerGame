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

#include <time.h>

#include "../game_types.h"
#include "../game_math.h"
#include "../game_network.h"

#define DAEMON_MODE 1

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

#define TOTAL_CONNECTIONS 8
#define SQUARE_LENGTH 300
#define SCREEN_WIDTH 1920
#define SCREEN_HEIGHT 1080

// These map directly to the host->clients.
v3 squares[TOTAL_CONNECTIONS] = {};

// Copied over from the win32 code.
f32 square_speed = 1000.0f;

void process_player_input(network_client_normal_packet *data){
  v3 *square = &squares[data->player_id];
  if(data->button_durations[network_button_type_left]){
    square->x -= square_speed * data->button_durations[network_button_type_left];
  }
  if(data->button_durations[network_button_type_right]){
    square->x += square_speed * data->button_durations[network_button_type_right];
  }
  if(data->button_durations[network_button_type_up]){
    square->y -= (square_speed * data->button_durations[network_button_type_up]);
  }
  if(data->button_durations[network_button_type_down]){
    square->y += (square_speed * data->button_durations[network_button_type_down]);
  }

  if(square->x > SCREEN_WIDTH - SQUARE_LENGTH) square->x = SCREEN_WIDTH - SQUARE_LENGTH;
  if(square->x < 0) square->x = 0;
  if(square->y > SCREEN_HEIGHT - SQUARE_LENGTH) square->y = SCREEN_HEIGHT - SQUARE_LENGTH;
  if(square->y < 0) square->y = 0;
}

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
  b32 is_connected;
  f32 time_since_last_message;
};

struct network_host{
  int socket;
  network_client *clients;
  u32 client_count;
  u32 connected_client_count;
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

void send_packet(network_client *client, network_buffer *buffer){
  sendto(client->host->socket, buffer->data, buffer->data_length, 0, &client->address, client->address_length);
}

void handle_incoming_packets(network_host *host){
  struct sockaddr sending_address;
  socklen_t address_length = sizeof(sending_address);

  network_buffer buffer = {};
  int received_length = recvfrom(host->socket, buffer.data, MAX_DATA_LENGTH, 0,
                                 &sending_address, &address_length);

  if(!buffer.data){
    return;
  }
  uint32 packet_type = unpack_uint32(&buffer);
  switch(packet_type){
  case network_packet_type_connect:
    {
      // NOTE(Trystan): This isn't an error, we don't want the first position to be used.
      // We want 0 to be the uninitalized ID state, so we don't want that to ever be handed out.
      int insertion_index = 0;
      for(int i = 1; i < host->client_count; ++i){
        if(!host->clients[i].is_connected){
          insertion_index = i;
          break;
        }
      }

      if(!insertion_index){
        syslog(LOG_NOTICE, "Attempted connection failed. Server full.\n");
        break;
      }
      
      syslog(LOG_NOTICE, "New connection received\n");
      ++host->connected_client_count;
      host->clients[insertion_index].address = sending_address;
      host->clients[insertion_index].address_length = address_length;
      host->clients[insertion_index].is_connected = 1;
      host->clients[insertion_index].time_since_last_message = 0.0f;
      
      network_buffer message = {};
      pack_uint32(&message, (u32)network_packet_type_connect);
      pack_uint32(&message, (u32)insertion_index);
      send_packet(&host->clients[insertion_index], &message);
    } break;
  case network_packet_type_normal:
    {
      // Figure out who sent the packet,
      // update their square.
      network_client_normal_packet packet = {};
      packet.player_id = unpack_uint32(&buffer);
      // If a message comes through from a disconnected client, don't process.
      if(!host->clients[packet.player_id].is_connected){
        break;
      }
      host->clients[packet.player_id].time_since_last_message = 0.0f;
      for(int i = 0; i < network_button_type_count; ++i){
        u32 encoded_duration = unpack_uint32(&buffer);
        f32 duration = net_decode_32(encoded_duration);
        packet.button_durations[i] = duration;
      }
      process_player_input(&packet);
    } break;
  case network_packet_type_disconnect:
    {
      // Clear out the previously used client slot.
      uint32 client_id = unpack_uint32(&buffer);
      if(!client_id || !host->clients[client_id].is_connected){
        break;
      }
      
      syslog(LOG_NOTICE, "Connection ended\n");
      --host->connected_client_count;
      host->clients[client_id] = {};
      host->clients[client_id].host = host;
      squares[client_id] = {};
    } break;
  default:
    syslog(LOG_NOTICE, "invalid packet\n");
    break;
  }
}

// Idea for the layout of a packet the server sends:
// u64 packet_number // If an older number is received than previous it's stale.
// u32 player_count
// [ // This repeats player_count times
//   u32 player_id
//   f32 x
//   f32 y
// ]

// On the opposite side here is the client sent packet:
// u32 packet_type // connecting or regular message
// --- That is all a connecting packet will contain, below is when regular messages
// u32 player_id
// f32 x
// f32 y

int socket_wait(int socket, u32 timeout){
  struct pollfd poll_socket;

  poll_socket.fd = socket;
  poll_socket.events = POLL_IN;

  // NOTE(Trystan): poll_count will only ever be 1 or 0. As we are only polling 1 socket.
  int poll_count = poll(&poll_socket, 1, timeout);

  if (poll_count == 0){
    return 0;
  }

  if (poll_socket.revents & POLL_IN){
    return poll_count;
  }

  return 0;
}

inline struct timespec linux_get_wall_clock(){
  struct timespec result;
  clock_gettime(CLOCK_MONOTONIC, &result);
  return result;
}

inline f32 linux_get_seconds_elapsed(struct timespec start, struct timespec end){
  struct timespec temp = {};
  temp.tv_sec  = end.tv_sec  - start.tv_sec;
  temp.tv_nsec = end.tv_nsec - start.tv_nsec;
  if (temp.tv_nsec < 0) {
    --temp.tv_sec;
    temp.tv_nsec += 1000000000L;
  }
  
  f32 result = temp.tv_sec + (f32)(temp.tv_nsec / 1000000000.0f);
  return result;
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

  syslog(LOG_NOTICE, "server: waiting for connections...\n");

  struct timespec last_counter = linux_get_wall_clock();

  uint32 tick_rate = 60;

  u32 expected_frames_per_update = 1;
  f32 target_seconds_per_tick = (f32)expected_frames_per_update / (f32)tick_rate;
  f32 time_since_last_packet_sent = 0.0f;
  u64 packet_number = 0;
  b32 resuming_from_sleep = 0;
  for(;;){
    f32 dt_for_tick = target_seconds_per_tick;
    time_since_last_packet_sent += dt_for_tick;

    // Increase the time since last heard clients first, then process packets.
    for(u32 i = 1; i < host->client_count; ++i){
      if(!host->clients[i].is_connected){
        continue;
      }

      host->clients[i].time_since_last_message += dt_for_tick;
      // If we haven't heard from them in 10 seconds, disconnect.
      if(host->clients[i].time_since_last_message > 10.0f){
        syslog(LOG_NOTICE, "Connection timed out.\n");
        --host->connected_client_count;
        host->clients[i] = {};
        host->clients[i].host = host;
        squares[i] = {};
      }
    }
    
    int32 wait_time = 10;
    // If no one is connected, we will wait infinitely until we get a new connection.
    // As there is nothing to do anyway.
    if(!host->connected_client_count) {
      wait_time = -1;
      resuming_from_sleep = 1;
    }
    int is_incoming_packet = socket_wait(host->socket, wait_time);

    // TODO(Trystan): The way this is set up will only get us one packet per tick.
    // We need to keep handling packets until there are none left.
    while(is_incoming_packet){
      handle_incoming_packets(host);
      is_incoming_packet = socket_wait(host->socket, 0);
    }

    if(time_since_last_packet_sent > 0.167f){
      time_since_last_packet_sent = 0.0f;
      if(!host->connected_client_count){
        
      }
      packet_number += 1;
      // construct a packet to send out to all clients.
      network_buffer raw_packet = {};
      pack_uint32(&raw_packet, (u32)network_packet_type_normal);
      pack_uint64(&raw_packet, packet_number);
      pack_uint32(&raw_packet, host->connected_client_count);
      for(u32 i = 1; i < host->client_count; ++i){
        if(!host->clients[i].is_connected){
          continue;
        }

        // The player_id
        pack_uint32(&raw_packet, i);
        uint32 encoded_x = (u32)net_encode_32(squares[i].x);
        pack_uint32(&raw_packet, encoded_x);
        uint32 encoded_y = (u32)net_encode_32(squares[i].y);
        pack_uint32(&raw_packet, encoded_y);
      }
      // Now the packet has been fully constructed
      // Send out to all connected clients.
      for(u32 i = 1; i < host->client_count; ++i){
        if(!host->clients[i].is_connected){
          continue;
        }

        send_packet(&host->clients[i], &raw_packet);
      }
    }

    struct timespec end_counter = linux_get_wall_clock();
    f32 measured_seconds_per_tick = linux_get_seconds_elapsed(last_counter, end_counter);

    target_seconds_per_tick = measured_seconds_per_tick;

    if(resuming_from_sleep){
      // When we wake after sleep, we will want to reset the delta time.
      // Otherwise it seems like the new connection hasn't sent a message since before sleep.
      // We wouldn't want to receive a normal packet using the sleep delta time either.
      // Bad all around. Best to get rid of it.
      target_seconds_per_tick = (f32)expected_frames_per_update / (f32)tick_rate;
    }

    last_counter = end_counter;
  }

  return 0;
}
