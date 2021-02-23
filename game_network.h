enum network_packet_type{
  network_packet_type_connect    = 0x01,
  network_packet_type_normal     = 0x02,
  network_packet_type_disconnect = 0x03,
};



// NOTE(Trystan): Just getting network_buffer to a round 256 bits, not thought out at all.
// NOTE(Trystan): Stores raw incoming packet data.
#define MAX_DATA_LENGTH 224
struct network_buffer{
  // TODO(Trystan): Perhaps change this to something that points to a re-usable pool of memory.
  u8 data[MAX_DATA_LENGTH];
  uint32 data_length;
  uint32 current_index; // The point at which we are currently reading or writing.
};

enum network_button_type{
  network_button_type_up    = 0x00,
  network_button_type_down  = 0x01,
  network_button_type_left  = 0x02,
  network_button_type_right = 0x03,

  network_button_type_count = 0x04
};

// NOTE(Trystan): This struct shows how normal client outgoing packets are structured.
// When connecting, just send a message containing only the network_packet_type_connect value.
// When disconnecting, send the packet_type header + the player id.
struct network_client_normal_packet{
  u32 player_id;
  f32 button_durations[4];
};

struct network_player_info{
  u32 player_id;
  f32 x;
  f32 y;
};

struct network_server_normal_packet{
  u64 packet_number;
  u32 player_count;
  network_player_info player_info[8]; // 8 max number of players.
};

#define net_encode_32(f) (_net_encode((f), 32, 8))
#define net_encode_64(f) (_net_encode((f), 64, 11))
#define net_decode_32(i) (_net_decode((i), 32, 8))
#define net_decode_64(i) (_net_decode((i), 64, 11))

inline uint64 _net_encode(f64 f, uint32 bits, uint32 expbits)
{
    f64 fnorm;
    int32 shift;
    int64 sign, exp, significand;
    uint32 significandbits = bits - expbits - 1; // -1 for sign bit

    if (f == 0.0) return 0; // get this special case out of the way

    // check sign and begin normalization
    if (f < 0) { sign = 1; fnorm = -f; }
    else { sign = 0; fnorm = f; }

    // get the normalized form of f and track the exponent
    shift = 0;
    while(fnorm >= 2.0) { fnorm /= 2.0; shift++; }
    while(fnorm < 1.0) { fnorm *= 2.0; shift--; }
    fnorm = fnorm - 1.0;

    // calculate the binary form (non-float) of the significand data
    significand = (uint32)(fnorm * ((1LL<<significandbits) + 0.5f));

    // get the biased exponent
    exp = shift + ((1<<(expbits-1)) - 1); // shift + bias

    // return the final answer
    return (sign<<(bits-1)) | (exp<<(bits-expbits-1)) | significand;
}

inline f64 _net_decode(uint64 i, uint32 bits, uint32 expbits)
{
    f64 result;
    int64 shift;
    uint32 bias;
    uint32 significandbits = bits - expbits - 1; // -1 for sign bit

    if (i == 0) return 0.0;

    // pull the significand
    result = (f64)(i&((1LL<<significandbits)-1)); // mask
    result /= (1LL<<significandbits); // convert back to float
    result += 1.0f; // add the one back on

    // deal with the exponent
    bias = (1<<(expbits-1)) - 1;
    shift = ((i>>significandbits)&((1LL<<expbits)-1)) - bias;
    while(shift > 0) { result *= 2.0; shift--; }
    while(shift < 0) { result /= 2.0; shift++; }

    // sign it
    result *= (i>>(bits-1))&1? -1.0: 1.0;

    return result;
}

inline void pack_uint32(network_buffer *buffer, uint32 i){
  u32 index = buffer->current_index;
  buffer->data[index] = (uint8)(i>>24);
  buffer->data[index + 1] = (uint8)(i>>16);
  buffer->data[index + 2] = (uint8)(i>>8);
  buffer->data[index + 3] = (uint8)i;

  buffer->current_index += 4;
  buffer->data_length += 4;
}

inline void pack_uint64(network_buffer *buffer, uint64 i){
  u32 index = buffer->current_index;
  buffer->data[index] = (uint8)(i>>56);
  buffer->data[index + 1] = (uint8)(i>>48);
  buffer->data[index + 2] = (uint8)(i>>40);
  buffer->data[index + 3] = (uint8)(i>>32);
  buffer->data[index + 4] = (uint8)(i>>24);
  buffer->data[index + 5] = (uint8)(i>>16);
  buffer->data[index + 6] = (uint8)(i>>8);
  buffer->data[index + 7] = (uint8)i;

  buffer->current_index += 8;
  buffer->data_length += 8;
}

inline uint32 unpack_uint32(network_buffer *buffer){
  u32 index = buffer->current_index;
  uint32 result = (((u32)buffer->data[index]<<24) |
                   ((u32)buffer->data[index + 1]<<16) |
                   ((u32)buffer->data[index + 2]<<8) |
                   buffer->data[index + 3]);
  
  buffer->current_index += 4;
  return result;
}

inline uint64 unpack_uint64(network_buffer *buffer){
  u32 index = buffer->current_index;
  uint64 result = (((u64)buffer->data[index]<<56) |
                   ((u64)buffer->data[index + 1]<<48) |
                   ((u64)buffer->data[index + 2]<<40) |
                   ((u64)buffer->data[index + 3]<<32) |
                   ((u64)buffer->data[index + 4]<<24) |
                   ((u64)buffer->data[index + 5]<<16) |
                   ((u64)buffer->data[index + 6]<<8) |
                   buffer->data[index + 7]);
  
  buffer->current_index += 8;
  return result;
}
