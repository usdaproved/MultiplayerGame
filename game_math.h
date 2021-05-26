inline v2
V2(real32 x, real32 y)
{
  v2 result;
    
  result.x = x;
  result.y = y;
    
  return result;
}

inline v3
V3(real32 x, real32 y, real32 z)
{
  v3 result;
    
  result.x = x;
  result.y = y;
  result.z = z;
    
  return result;
}

inline v4
V4(real32 x, real32 y, real32 z, real32 w)
{
  v4 result;
    
  result.x = x;
  result.y = y;
  result.z = z;
  result.w = w;
    
  return result;
}

inline v4
V4(v3 XYZ, real32 W)
{
    v4 Result;
    
    Result.xyz = XYZ;
    Result.w = W;
    
    return(Result);
}

inline v3
operator*(real32 A, v3 B)
{
    v3 Result;
    
    Result.x = A*B.x;
    Result.y = A*B.y;
    Result.z = A*B.z;
    
    return(Result);
}

inline v3
operator*(v3 B, real32 A)
{
    v3 Result = A*B;
    
    return(Result);
}

inline v3
operator-(v3 A, v3 B)
{
    v3 Result;
    
    Result.x = A.x - B.x;
    Result.y = A.y - B.y;
    Result.z = A.z - B.z;
    
    return(Result);
}

inline real32
Inner(v3 A, v3 B)
{
    real32 Result = A.x*B.x + A.y*B.y + A.z*B.z;
    
    return(Result);
}

inline v3
Cross(v3 A, v3 B)
{
    v3 Result;
    
    Result.x = A.y*B.z - A.z*B.y;
    Result.y = A.z*B.x - A.x*B.z;
    Result.z = A.x*B.y - A.y*B.x;
    
    return(Result);
}

inline real32
LengthSq(v3 A)
{
    real32 Result = Inner(A, A);
    
    return(Result);
}

inline real32
Length(v3 A)
{
  real32 Result = 0;//SquareRoot(LengthSq(A));
  return(Result);
}

inline v3
Normalize(v3 A)
{
    v3 Result = A * (1.0f / Length(A));
    
    return(Result);
}

internal m4x4
operator*(m4x4 A, m4x4 B)
{
    // NOTE(casey): This is written to be instructive, not optimal!
    
    m4x4 R = {};
    
    for(int r = 0; r <= 3; ++r) // NOTE(casey): Rows (of A)
    {
        for(int c = 0; c <= 3; ++c) // NOTE(casey): Column (of B)
        {
            for(int i = 0; i <= 3; ++i) // NOTE(casey): Columns of A, rows of B!
            {
                R.E[r][c] += A.E[r][i]*B.E[i][c];
            }
        }
    }
    
    return(R);
}

internal v4
Transform(m4x4 A, v4 P)
{
    // NOTE(casey): This is written to be instructive, not optimal!
    
    v4 R;
    
    R.x = P.x*A.E[0][0] + P.y*A.E[0][1] + P.z*A.E[0][2] + P.w*A.E[0][3];
    R.y = P.x*A.E[1][0] + P.y*A.E[1][1] + P.z*A.E[1][2] + P.w*A.E[1][3];
    R.z = P.x*A.E[2][0] + P.y*A.E[2][1] + P.z*A.E[2][2] + P.w*A.E[2][3];
    R.w = P.x*A.E[3][0] + P.y*A.E[3][1] + P.z*A.E[3][2] + P.w*A.E[3][3];
    
    return(R);
}

internal m4x4
Translate(m4x4 A, v3 T)
{
    m4x4 R = A;
    
    R.E[0][3] += T.x;
    R.E[1][3] += T.y;
    R.E[2][3] += T.z;
    
    return(R);
}

inline m4x4
Transpose(m4x4 A)
{
    m4x4 R;
    
    for(int j = 0; j <= 3; ++j)
    {
        for(int i = 0; i <= 3; ++i)
        {
            R.E[j][i] = A.E[i][j];
        }
    }
    
    return(R);
}

inline v3
operator*(m4x4 A, v3 P)
{
    v3 R = Transform(A, V4(P, 1.0f)).xyz;
    return(R);
}

inline v4
operator*(m4x4 A, v4 P)
{
    v4 R = Transform(A, P);
    return(R);
}

inline m4x4
Identity(void)
{
    m4x4 R =
    {
        {{1, 0, 0, 0},
            {0, 1, 0, 0},
            {0, 0, 1, 0},
            {0, 0, 0, 1}},
    };
    
    return(R);
}

inline real32
Lerp(real32 A, real32 t, real32 B)
{
    real32 Result = (1.0f - t)*A + t*B;

    return(Result);
}
