//
// NOTE(casey): Compilers
//

#if !defined(COMPILER_MSVC)
#define COMPILER_MSVC 0
#endif

#if !defined(COMPILER_LLVM)
#define COMPILER_LLVM 0
#endif

#if !COMPILER_MSVC && !COMPILER_LLVM
#if _MSC_VER
#undef COMPILER_MSVC
#define COMPILER_MSVC 1
#else
// TODO(casey): Moar compilerz!!!
#undef COMPILER_LLVM
#define COMPILER_LLVM 1
#endif
#endif

#if COMPILER_MSVC
#include <intrin.h>
#elif COMPILER_LLVM
#include <x86intrin.h>
#else
#error SEE/NEON optimizations are not available for this compiler yet!!!!
#endif

#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <float.h>

// NOTE(Trystan): Casey has this in it's own file with memset. Which so far hasn't come up for us.
// But since we aren't using the C run-time library, we have to define that for ourselves.
// From Day 435
// https://hero.handmade.network/forums/code-discussion/t/94-guide_-_how_to_avoid_c_c++_runtime_on_windows
int _fltused = 0x9875;

typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;
typedef int32 bool32;

typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;

typedef float real32;
typedef double real64;

typedef int8 s8;
typedef int8 s08;
typedef int16 s16;
typedef int32 s32;
typedef int64 s64;
typedef bool32 b32;

typedef uint8 u8;
typedef uint8 u08;
typedef uint16 u16;
typedef uint32 u32;
typedef uint64 u64;

typedef real32 r32;
typedef real64 r64;
typedef real32 f32;
typedef real64 f64;

union v2
{
  struct
  {
    real32 x, y;
  };
  struct
  {
    real32 u, v;
  };
  struct
  {
    real32 width, height;
  };
  real32 E[2];
};

union v3
{
  struct
  {
    real32 x, y, z;
  };
  struct
  {
    real32 u, v, __;
  };
  struct
  {
    real32 r, g, b;
  };
  struct
  {
    v2 xy;
    real32 Ignored0_;
  };
  struct
  {
    real32 Ignored1_;
    v2 yz;
  };
  struct
  {
    v2 uv;
    real32 Ignored2_;
  };
  struct
  {
    real32 Ignored3_;
    v2 v__;
  };
  f32 E[3];
};

union v4
{
  struct
  {
    union
    {
      v3 xyz;
      struct
      {
	real32 x, y, z;
      };
    };
        
    real32 w;
  };
  struct
  {
    union
    {
      v3 rgb;
      struct
      {
	real32 r, g, b;
      };
    };
        
    real32 a;
  };
  struct
  {
    v2 xy;
    real32 Ignored0_;
    real32 Ignored1_;
  };
  struct
  {
    real32 Ignored2_;
    v2 yz;
    real32 Ignored3_;
  };
  struct
  {
    real32 Ignored4_;
    real32 Ignored5_;
    v2 zw;
  };
  f32 E[4];
};

#if !defined(internal)
#define internal static
#endif
#define local_persist static
#define global static

#define assert(expression) if(!(expression)) {*(volatile int *)0 = 0;}

#define kilobytes(value) ((value)*1024LL)
#define megabytes(value) (kilobytes(value)*1024LL)
#define gigabytes(value) (megabytes(value)*1024LL)
#define terabytes(value) (gigabytes(value)*1024LL)

#define array_count(array) (sizeof(array) / sizeof((array)[0]))


inline u32 string_length(char *string){
  u32 count = 0;
  if(string){
    while(*string++){
      ++count;
    }
  }
  
  return count;
}
