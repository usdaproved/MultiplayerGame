inline s32 round_real32_to_int32(f32 real32){
  s32 result = _mm_cvtss_si32(_mm_set_ss(real32));
  return result;
}
