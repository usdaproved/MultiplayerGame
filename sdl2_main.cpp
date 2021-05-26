#include "game_platform.h"
#include "game_intrinsics.h"
#include "game_math.h"
#include "game_network.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <SDL.h>

// Socket programming on windows.
#include <winsock2.h>
#include <ws2tcpip.h>

// D3D12 related includes
#include <d3d12.h>
#include <dxgi1_6.h>
#include <D3DCompiler.h>
#define DEBUG 1

#include "win32_main.h"

#define SQUARE_LENGTH 300
#define SCREEN_WIDTH 1920
#define SCREEN_HEIGHT 1080


global int global_running;
global s64 global_performance_count_frequency;

internal LRESULT CALLBACK
Win32MainWindowCallback(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam) {
  LRESULT result = 0;
    
  switch(Message){
  case WM_CLOSE:
    {
      global_running = false;
    } break;
        
  case WM_DESTROY:
    {
      global_running = false;
    } break;
        
  default:
    {
      result = DefWindowProcA(Window, Message, WParam, LParam);
    } break;
  }
    
  return result;
}

inline LARGE_INTEGER win32_get_wall_clock(){
  LARGE_INTEGER result;
  QueryPerformanceCounter(&result);
  return result;
}

inline real32 win32_get_seconds_elapsed(LARGE_INTEGER start, LARGE_INTEGER end){
  real32 result = ((real32)(end.QuadPart - start.QuadPart) /
                   (real32)global_performance_count_frequency);
  return result;
}

internal void win32_process_keyboard_message(game_button_state *new_state, b32 is_down){
  if(new_state->ended_down != is_down){
    new_state->ended_down = is_down;
    ++new_state->half_transition_count;
  }
}

internal void sdl2_process_event(game_controller_input *controller){
  SDL_Event event;
  // I know we're doing switch inside switch
  // but I want all event handling to be in one place.
  // And it's not my fault SDL sets up their events this way.
  while(SDL_PollEvent(&event)){
    switch(event.type){
    case SDL_WINDOWEVENT:
      {
        switch(event.window.event){
          case SDL_WINDOWEVENT_CLOSE:
            {
              global_running = false;
            } break;
        }
      } break;

    case SDL_KEYDOWN:
    case SDL_KEYUP:
      {
        b32 was_down = event.key.state == SDL_RELEASED;
        b32 is_down = event.key.state == SDL_PRESSED;
        if(was_down != is_down){
          if(event.key.keysym.sym == SDLK_w){
            win32_process_keyboard_message(&controller->move_up, is_down);
          }
          else if(event.key.keysym.sym == SDLK_a){
            win32_process_keyboard_message(&controller->move_left, is_down);
          }
          else if(event.key.keysym.sym == SDLK_s){
            win32_process_keyboard_message(&controller->move_down, is_down);
          }
          else if(event.key.keysym.sym == SDLK_d){
            win32_process_keyboard_message(&controller->move_right, is_down);
          }
          else if(event.key.keysym.sym == SDLK_ESCAPE){
            win32_process_keyboard_message(&controller->back, is_down);
          }
        }
      } break;
    }
  }
}

#define PORT "28976"

struct win32_network_connection{
  SOCKET socket;
  struct sockaddr address; // This is the address of the server.
  u32 address_length; // This should just be the same value every time.
};

// NOTE(Trystan): This gets us a connectionless UDP socket, set to our server.
internal win32_network_connection win32_socket_init(){
  struct addrinfo hints = {};
  // NOTE(Trystan): AF_INET6 if we want to try IPv6
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;

  struct addrinfo *server_info = NULL;
  getaddrinfo("134.122.112.231", PORT, &hints, &server_info);

  win32_network_connection connection = {};
  
  for(struct addrinfo *p = server_info; p != NULL; p = p->ai_next){
    connection.socket = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if(connection.socket != -1){
      // We got the socket we wanted.
      connection.address = *p->ai_addr;
      connection.address_length = (u32)p->ai_addrlen;
      break;
    }
  }

  assert(connection.socket != 0);

  freeaddrinfo(server_info);

  int yes = 1;
  ioctlsocket(connection.socket, FIONBIO, (u_long *)&yes);
  setsockopt(connection.socket, SOL_SOCKET, SO_BROADCAST, (char *)&yes, sizeof(int));

  return connection;
}

internal void win32_send_packet(win32_network_connection *connection, network_buffer *buffer){
  sendto(connection->socket, (char *)buffer->data, buffer->data_length, 0, &connection->address, connection->address_length);
};

internal int win32_socket_wait(win32_network_connection *connection, u32 timeout){
  WSAPOLLFD poll_socket = {};

  poll_socket.fd = connection->socket;
  poll_socket.events = POLLIN;

  int poll_count = WSAPoll(&poll_socket, 1, timeout);

  if(poll_count == 0){
    return 0;
  }

  if(poll_socket.revents & POLLIN){
    return poll_count;
  }

  return 0;
}

internal void win32_get_raw_packet_data(win32_network_connection *connection, network_buffer *buffer){
  buffer->data_length = recvfrom(connection->socket, (char *)buffer->data, MAX_DATA_LENGTH, 0, 0, 0);
}

// Add color parameter?
internal void sdl_draw_square(SDL_Renderer *renderer, s32 x, s32 y){
  SDL_Rect rect;
  rect.x = x;
  rect.y = y;
  rect.w = SQUARE_LENGTH;
  rect.h = SQUARE_LENGTH;

  SDL_SetRenderDrawColor(renderer, 251, 72, 196, 255);
  SDL_RenderFillRect(renderer, &rect);
}


extern "C" int __stdcall WinMainCRTStartup() {
  // Get timer info
  LARGE_INTEGER performance_count_frequency_result;
  QueryPerformanceFrequency(&performance_count_frequency_result);
  global_performance_count_frequency = performance_count_frequency_result.QuadPart;

  SDL_Window* window = NULL;
  SDL_Surface* screenSurface = NULL;
  SDL_Init(SDL_INIT_VIDEO);
  //SDL_SetWindowsMessageHook(Win32MainWindowCallback, NULL);
  
  window = SDL_CreateWindow(
			    "Multiplayer Game",
			    SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
			    SCREEN_WIDTH, SCREEN_HEIGHT,
			    SDL_WINDOW_SHOWN);
  
  SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);

  WSADATA wsa_data;
  // MAKEWORD(2,0) denotes we want to use winsock 2.0
  assert(WSAStartup(MAKEWORD(2, 0), &wsa_data) == 0);

  win32_network_connection connection = win32_socket_init();
  network_buffer connecting_message = {};
  pack_uint32(&connecting_message, (u32)network_packet_type_connect);
  win32_send_packet(&connection, &connecting_message);
  // The server then sends back a packet containing the packet type header + the assigned ID.

  v2 player_position = {};
  
  global_running = 1;

  game_input input[2] = {};
  game_input *new_input = &input[0];
  game_input *old_input = &input[1];

  // TODO(Trystan): Get this programmatically. I know my monitor is 144hz.
  uint32 monitor_refresh_rate = 144;

  LARGE_INTEGER last_counter = win32_get_wall_clock();
  u32 expected_frames_per_update = 1;
  f32 target_seconds_per_frame = (f32)expected_frames_per_update / (f32)monitor_refresh_rate;

  f32 time_since_last_sent_packet = 0.0f;
  f32 time_since_last_received_packet = 0.0f;

  u64 last_received_packet_number = 0;

  network_client_normal_packet normal_outgoing_packet = {};

  // Contains position data about all players.
  network_server_normal_packet new_server_data = {};
  // We want to lerp between the old and the new.
  network_server_normal_packet old_server_data = {};
  f32 lerp_percent = 0.0f;

  while(global_running){
    new_input->dt_for_frame = target_seconds_per_frame;
    time_since_last_sent_packet += new_input->dt_for_frame;
    time_since_last_received_packet += new_input->dt_for_frame;

    // We got disconnected, attempt to reconnect.
    if(time_since_last_received_packet > 10.0f){
      time_since_last_received_packet = 0.0f;
      normal_outgoing_packet.player_id = 0;
      
      closesocket(connection.socket);
      connection = win32_socket_init();
      win32_send_packet(&connection, &connecting_message);
    }

    game_controller_input *old_controller = &old_input->controller;
    game_controller_input *new_controller = &new_input->controller;
    *new_controller = {};
    new_controller->is_connected = true;
    // We need to carry over the button state every time to ensure half_transition_count updates properly.
    for(uint32 button_index = 0; button_index < array_count(new_controller->buttons); ++button_index){
      new_controller->buttons[button_index].ended_down = old_controller->buttons[button_index].ended_down;
    }
    
    sdl2_process_event(&new_input->controller);

    int wait_condition = win32_socket_wait(&connection, 0);
    if(wait_condition){
      time_since_last_received_packet = 0.0f;
      network_buffer buffer = {};
      win32_get_raw_packet_data(&connection, &buffer);
      if(buffer.data_length){
        uint32 index = 0;
        uint32 packet_type = unpack_uint32(&buffer);
        switch(packet_type){
        case network_packet_type_connect:
          {
            // Connecting packet contains our new ID that we use to refer to ourselves.
            normal_outgoing_packet.player_id = unpack_uint32(&buffer);
          } break;
        case network_packet_type_normal:
          {
            // Clear the packet every time.
            // This old/new system this way will lead to an error.
            // especially when the last packet number is greater than received.
            // What we really want is a persistent player data.
            old_server_data = new_server_data;
            new_server_data = {};
            new_server_data.packet_number = unpack_uint64(&buffer);
            new_server_data.player_count = unpack_uint32(&buffer);
            if(last_received_packet_number > new_server_data.packet_number){
              break;
            }
                
            for(u32 i = 0; i < new_server_data.player_count; ++i){
              new_server_data.player_info[i].player_id = unpack_uint32(&buffer);
              u32 encoded_x = unpack_uint32(&buffer);
              f32 x = (f32)net_decode_32(encoded_x);
              new_server_data.player_info[i].x = x;
              u32 encoded_y = unpack_uint32(&buffer);
              f32 y = (f32)net_decode_32(encoded_y);
              new_server_data.player_info[i].y = y;
            }
            
            last_received_packet_number = new_server_data.packet_number;
            lerp_percent = 0.0f;
          } break;
        default:
          // Invalid packet type.
          break;
        }
      }
    }

    // TODO(Trystan): Yeet all button processing out of here into the game layer.
    f32 square_speed = 1000.0f;
    if(is_down(new_input->controller.move_left)){
      player_position.x -= square_speed * new_input->dt_for_frame;
      normal_outgoing_packet.button_durations[network_button_type_left] += new_input->dt_for_frame;
    }
    if(is_down(new_input->controller.move_right)){
      player_position.x += square_speed * new_input->dt_for_frame;
      normal_outgoing_packet.button_durations[network_button_type_right] += new_input->dt_for_frame;
    }
    if(is_down(new_input->controller.move_up)){
      player_position.y -= (square_speed * new_input->dt_for_frame);
      normal_outgoing_packet.button_durations[network_button_type_up] += new_input->dt_for_frame;
    }
    if(is_down(new_input->controller.move_down)){
      player_position.y += square_speed * new_input->dt_for_frame;
      normal_outgoing_packet.button_durations[network_button_type_down] += new_input->dt_for_frame;
    }
    if(was_pressed(new_input->controller.back)){
      global_running = false;
    }

    if(player_position.x > SCREEN_WIDTH - SQUARE_LENGTH) player_position.x = 1920 - SQUARE_LENGTH;
    if(player_position.x < 0) player_position.x = 0;
    if(player_position.y > SCREEN_HEIGHT - SQUARE_LENGTH) player_position.y = 1080 - SQUARE_LENGTH;
    if(player_position.y < 0) player_position.y = 0;

    
    SDL_RenderClear(renderer);
    
    for(u32 i = 0; i < new_server_data.player_count; ++i){
      // At the moment, ignore our own packet. Do some type of correction if the player is too out of sync.
      if(new_server_data.player_info[i].player_id == normal_outgoing_packet.player_id){
        continue;
      }

      
      f32 peer_x = new_server_data.player_info[i].x;
      f32 peer_y = new_server_data.player_info[i].y;
      
      if(lerp_percent < 1.0f){
        peer_x = Lerp(old_server_data.player_info[i].x, lerp_percent, new_server_data.player_info[i].x);
        peer_y = Lerp(old_server_data.player_info[i].y, lerp_percent, new_server_data.player_info[i].y);
        lerp_percent += (new_input->dt_for_frame / 0.167f);
        if(lerp_percent > 1.0f) lerp_percent = 1.0f;
      }
      
      // Draw square at position.
      sdl_draw_square(renderer, (s32)peer_x, (s32)peer_y);
    }

    sdl_draw_square(renderer, (s32)player_position.x, (s32)player_position.y);

    SDL_SetRenderDrawColor(renderer, 100, 149, 237, 255);

    SDL_RenderPresent(renderer);


    // NOTE(Trystan): We want to send updates to the server 60 times a second.
    if(time_since_last_sent_packet > 0.167f && normal_outgoing_packet.player_id != 0){
      time_since_last_sent_packet = 0.0f;
      network_buffer raw_packet = {};
      pack_uint32(&raw_packet, (u32)network_packet_type_normal);
      pack_uint32(&raw_packet, normal_outgoing_packet.player_id);
      for(int i = 0; i < network_button_type_count; ++i){
        uint32 encoded_duration = (u32)net_encode_32(normal_outgoing_packet.button_durations[i]);
        pack_uint32(&raw_packet, encoded_duration);

        // reset for the next packet. Keeping player_id
        normal_outgoing_packet.button_durations[i] = 0.0f;
      }
          
      win32_send_packet(&connection, &raw_packet);
    }


    game_input *temp = new_input;
    new_input = old_input;
    old_input = temp;

    LARGE_INTEGER end_counter = win32_get_wall_clock();
    f32 measured_seconds_per_frame = win32_get_seconds_elapsed(last_counter, end_counter);
    f32 exact_target_frames_per_update = measured_seconds_per_frame * (f32)monitor_refresh_rate;
    u32 new_expected_frames_per_update = round_real32_to_int32(exact_target_frames_per_update);
    expected_frames_per_update = new_expected_frames_per_update;

    target_seconds_per_frame = measured_seconds_per_frame;

    last_counter = end_counter;
  }

  // Exiting cleanup.
  network_buffer disconnect_message = {};
  pack_uint32(&disconnect_message, (u32)network_packet_type_disconnect);
  pack_uint32(&disconnect_message, normal_outgoing_packet.player_id);
  win32_send_packet(&connection, &disconnect_message);

  SDL_DestroyRenderer(renderer);
  SDL_DestroyWindow(window);
  SDL_Quit();
  
  ExitProcess(0);
}
