// This is for stuff that is platform agnostic.
// This sits between win32 and the game.
// So the game talks to this instead of directly with any OS.

#ifdef __cplusplus
extern "C" {
#endif

#include "game_types.h"

  struct platform_work_queue;
#define PLATFORM_WORK_QUEUE_CALLBACK(name) void name(platform_work_queue *queue, void *data)
  typedef PLATFORM_WORK_QUEUE_CALLBACK(platform_work_queue_callback);

  struct game_button_state{
    uint32 half_transition_count;
    b32 ended_down;
  };
  
  struct game_controller_input {
    b32 is_connected;
    
    union {
      game_button_state buttons[5];
      struct{
	game_button_state move_up;
	game_button_state move_down;
	game_button_state move_left;
	game_button_state move_right;
	game_button_state back;

	// NOTE(Trystan): This is to help itterate over the list of buttons.
	game_button_state terminator;
      };
    };
  };

  struct game_input{
    r32 dt_for_frame;

    // At the moment I'm only concerned with getting keyboard.
    // Casey has an array of 5 controllers here.
    game_controller_input controller;

    // NOTE(casey): Signals back to the platform layer
    b32 quit_requested;
    
    // I don't think I care about tracking alt, shift, or control at the moment.
  };

  inline b32 was_pressed(game_button_state state){
    b32 result = ((state.half_transition_count > 1) ||
		  ((state.half_transition_count == 1) && (state.ended_down)));
    return result;
  }

  inline b32 is_down(game_button_state state){
    b32 result = (state.ended_down);
    return result;
  }
    
  // Casey has this portion inside of debug_interface.h
#ifdef __cplusplus
}
#endif
