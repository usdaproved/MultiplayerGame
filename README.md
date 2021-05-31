## Multiplayer Game
A player opens the program, gets a square that they can control, and sees other player's squares move in real time.
That's it.

Read more about it [here](https://trystanbrock.dev/multiplayer-game) and download the game [here](https://usdaproved.itch.io/multiplayer-game).


Created with the Windows API and SDL2. The server code runs on Linux.
The code for data serialization and sending packets was created from scratch.
I wanted the rendering code to be from scratch using D3D12. But all I really wanted to do was draw a few squares on the screen, which D3D12 turned out to be massive overkill for. You can see the remnants of that code in the abandoned win32_main.cpp. The main entry point for the finished version is sdl2_main.cpp.

The rendering is using SDL2's built in hardware accelerated renderer, which I think is just D3D9 in immediate mode.

At any rate, the server can handle up to 8 simultaneous connections, which is an arbitrary cap I put in place. Didn't want to overload my $5 DigitalOcean droplet. The server can handle disconnects from the client exiting either on purpose or not sending a message for 10 seconds.