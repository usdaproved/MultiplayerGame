inline s32 round_real32_to_int32(f32 real32){
  s32 result = _mm_cvtss_si32(_mm_set_ss(real32));
  return result;
}

inline f32
SquareRoot(f32 Real32)
{
    f32 Result = _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(Real32)));
    return(Result);
}
