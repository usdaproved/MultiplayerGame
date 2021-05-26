#include "game_platform.h"
#include "game_intrinsics.h"
#include "game_math.h"
#include "game_network.h"

// This is required becuase we are doing socket programming while also including windows.h
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

// Socket programming on windows.
#include <winsock2.h>
#include <ws2tcpip.h>

// D3D12 related includes
#include <d3d12.h>
#include <dxgi1_6.h>
#include <D3DCompiler.h>
#define DEBUG 1

#include "win32_main.h"







//
//
// ==================================================
//
//
//
//
//
//
//  Okay I'm taking a break from this for a bit.
//  I think I've narrowed down the problem
//  The constant buffers aren't making it into the shader
//  So something is going wrong with the upload I think
//  Or the memcpy.
//
//
//  But we finally got the vertex and index data going in
//  Yet we still do not have anything on the screen
//
//  One thing I wanted to test was to have a simple shader program
//  That requires no inputs. See if that puts anything on the screen
//  Like the program we had when it was just one square.
//
//  I am leaving now to work on the silly mapping tool.
//  I hope to be back soon.
//  03/10/2021
//
//
//
//
//
//
//
//
//
// ===================================================
//
//



















#define MAX_SQUARES 8
#define FRAME_RESOURCE_COUNT 3
#define BACK_BUFFER_COUNT 2

global int global_running;
global s64 global_performance_count_frequency;

// D3D12 related variables
global ID3D12Device *d3d12_device;
global ID3D12CommandQueue *d3d12_command_queue;
global IDXGISwapChain *d3d12_swap_chain;
global ID3D12DescriptorHeap *d3d12_rtv_heap;
global ID3D12DescriptorHeap *d3d12_dsv_heap;
global ID3D12DescriptorHeap *d3d12_cbv_heap;
global ID3D12Resource *d3d12_swap_chain_buffer[BACK_BUFFER_COUNT];
global u32 d3d12_current_back_buffer = 0;
global ID3D12Resource *d3d12_depth_stencil_buffer;
global ID3D12CommandAllocator *d3d12_command_allocator;
global ID3D12RootSignature *d3d12_signature;
global ID3D12PipelineState *d3d12_pipeline_state;
global ID3D12GraphicsCommandList *d3d12_command_list;

global UINT d3d12_rtv_descriptor_size;
global ID3D12Fence *d3d12_fence;
global UINT64 d3d12_fence_value;

global u32 d3d12_pass_cbv_offset = 0;

global D3D12_VIEWPORT d3d12_viewport;
global D3D12_RECT d3d12_scissor_rect;

global char *d3d12_shader_code = R"FOO(
// I'm thinking this should be temporary.
// We either want to embed this in our code files
// Or I think end game would be pre-compiling the shader.

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
};

cbuffer cbPass : register(b1)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float3 gEyePosW;
    float cbPerObjectPad1;
    float2 gRenderTargetSize;
    float2 gInvRenderTargetSize;
    float gNearZ;
    float gFarZ;
    float gTotalTime;
    float gDeltaTime;
};

struct VSInput
{
	float3 position  : POSITION;
    float4 color : COLOR;
};

struct PSInput
{
	float4 position  : SV_POSITION;
    float4 color : COLOR;
};

PSInput VSMain(VSInput input)
{
	PSInput result;
	
	// Transform to homogeneous clip space.
    //float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    //vout.PosH = mul(posW, gViewProj);
	
	// Just pass vertex color into the pixel shader.
    result.color = input.color;
    result.position = float4(input.position, 1.0f);
    
    return result;
}

float4 PSMain(PSInput input) : SV_Target
{
    return input.color;
}
)FOO";

// object and pass constants get used in the shader program.
struct d3d12_object_constants{
  m4x4 world;
};

struct d3d12_pass_constants{
  m4x4 view;
  m4x4 inv_view;
  m4x4 projection;
  m4x4 inv_projection;
  m4x4 view_projection;
  m4x4 inv_view_projection;
  v3 eye_pos_w;
  f32 cb_per_object_pad1;
  v2 render_target_size;
  v2 inv_render_target_size;
  f32 near_z;
  f32 far_z;
  f32 total_time;
  f32 delta_time;
};

struct d3d12_vertex {
  v3 position;
  v4 color;
};

struct d3d12_mesh_geometry{
  // System memory copies. Blobs are used as format can be generic.
  // Cast when using the code.
  ID3DBlob *vertex_buffer_CPU;
  ID3DBlob *index_buffer_CPU;

  ID3D12Resource *vertex_buffer_GPU;
  ID3D12Resource *index_buffer_GPU;

  ID3D12Resource *vertex_buffer_uploader;
  ID3D12Resource *index_buffer_uploader;

  u32 vertex_byte_stride;
  u32 vertex_buffer_byte_size;
  DXGI_FORMAT index_format;  // 16 bit unsigned. We may need to change the u32 *indices
  u32 index_buffer_byte_size;
  u32 index_count;

  
  d3d12_vertex *vertices;
  u16 *indices;
};

// TODO(Trystan): When we organize the code, put this somewhere better.
global d3d12_mesh_geometry mesh_geometry;

struct d3d12_render_item{
  m4x4 world;
  s32 constant_buffer_index;
};

struct d3d12_upload_buffer {
  ID3D12Resource *upload_buffer;
  u8 *mapped_data;

  u32 element_byte_size;
};

struct d3d12_frame_resource {
  ID3D12CommandAllocator *command_list_allocator;

  // We cannot update a cbuffer until the GPU is done processing the commands
  // that reference it. So each frame needs their own cbuffers.
  d3d12_upload_buffer pass_constant_buffer;
  d3d12_upload_buffer object_constant_buffer;

  u64 fence;
};

global d3d12_render_item d3d12_squares[MAX_SQUARES] = {};
global d3d12_pass_constants d3d12_pass_constant;
global m4x4 view_matrix;
global m4x4 projection_matrix;
global v3 eye_pos;
global d3d12_frame_resource d3d12_frame_resources[FRAME_RESOURCE_COUNT];

global u32 d3d12_current_frame_resource_index = 0;
global d3d12_frame_resource *d3d12_current_frame_resource = NULL;

internal D3D12_RESOURCE_DESC d3d12_create_default_description_buffer(u64 byte_size){
  D3D12_RESOURCE_DESC description;
  description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  description.Alignment = 0;
  description.Width = byte_size;
  description.Height = 1;
  description.DepthOrArraySize = 1;
  description.MipLevels = 1;
  description.Format = DXGI_FORMAT_UNKNOWN;
  description.SampleDesc.Count = 1;
  description.SampleDesc.Quality = 0;
  description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  description.Flags = D3D12_RESOURCE_FLAG_NONE;

  return description;
}

// We could just directly store the vertices and indices directly, since we're only doing squares anyway.
// However this allows us to be a little more flexible.
internal void generate_box_geometry(d3d12_mesh_geometry *geometry, f32 width, f32 height, f32 depth){
  geometry->vertex_buffer_byte_size = 24 * sizeof(d3d12_vertex);
  geometry->vertices = (d3d12_vertex *)VirtualAlloc(0, geometry->vertex_buffer_byte_size,
                                                    MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);

  geometry->index_buffer_byte_size = 36 * sizeof(u16);
  geometry->index_count = 36;
  geometry->indices = (u16 *)VirtualAlloc(0, geometry->index_buffer_byte_size,
                                          MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);

  f32 aspect_ratio = 1920.0f / 1080.0f;
  
  f32 w = 0.5f * width;
  f32 h = (0.5f * height) * aspect_ratio;
  f32 d = 0.5f * depth;
  
  d3d12_vertex *v = geometry->vertices;

  // Front face
  v[0]  = { V3(-w, -h, -d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[1]  = { V3(-w, +h, -d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[2]  = { V3(+w, +h, -d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[3]  = { V3(+w, -h, -d), V4(0.9f, 0.2f, 0.9f, 1.0f) };

  // Back face
  v[4]  = { V3(-w, -h, +d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[5]  = { V3(+w, -h, +d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[6]  = { V3(+w, +h, +d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[7]  = { V3(-w, +h, +d), V4(0.9f, 0.2f, 0.9f, 1.0f) };

  // Top face
  v[8]  = { V3(-w, +h, -d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[9]  = { V3(-w, +h, +d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[10] = { V3(+w, +h, +d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[11] = { V3(+w, +h, -d), V4(0.9f, 0.2f, 0.9f, 1.0f) };

  // Bottom face
  v[12] = { V3(-w, -h, -d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[13] = { V3(+w, -h, -d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[14] = { V3(+w, -h, +d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[15] = { V3(-w, -h, +d), V4(0.9f, 0.2f, 0.9f, 1.0f) };

  // Left face
  v[16] = { V3(-w, -h, +d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[17] = { V3(-w, +h, +d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[18] = { V3(-w, +h, -d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[19] = { V3(-w, -h, -d), V4(0.9f, 0.2f, 0.9f, 1.0f) };

  // Right face
  v[20] = { V3(+w, -h, -d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[21] = { V3(+w, +h, -d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[22] = { V3(+w, +h, +d), V4(0.9f, 0.2f, 0.9f, 1.0f) };
  v[23] = { V3(+w, -h, +d), V4(0.9f, 0.2f, 0.9f, 1.0f) };

  u16 *i = geometry->indices;

  // Front face
  i[0] = 0; i[1] = 1; i[2] = 2;
  i[3] = 0; i[4] = 2; i[5] = 3;

  // Back face
  i[6] = 4; i[7]  = 5; i[8]  = 6;
  i[9] = 4; i[10] = 6; i[11] = 7;

  // Top face
  i[12] = 8; i[13] =  9; i[14] = 10;
  i[15] = 8; i[16] = 10; i[17] = 11;

  // Bottom face
  i[18] = 12; i[19] = 13; i[20] = 14;
  i[21] = 12; i[22] = 14; i[23] = 15;

  // Left face
  i[24] = 16; i[25] = 17; i[26] = 18;
  i[27] = 16; i[28] = 18; i[29] = 19;

  // Right face
  i[30] = 20; i[31] = 21; i[32] = 22;
  i[33] = 20; i[34] = 22; i[35] = 23;

  geometry->vertex_byte_stride = sizeof(d3d12_vertex);
  geometry->index_format = DXGI_FORMAT_R16_UINT;
}

internal ID3D12Resource * d3d12_create_default_buffer(ID3D12Device *device, ID3D12GraphicsCommandList *command_list,
                                                      void *init_data, u64 byte_size, ID3D12Resource *upload_buffer){
  ID3D12Resource *default_buffer;
  
  D3D12_HEAP_PROPERTIES default_heap_properties = {};
  default_heap_properties.Type = D3D12_HEAP_TYPE_DEFAULT;
  default_heap_properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  default_heap_properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  default_heap_properties.CreationNodeMask = 1;
  default_heap_properties.VisibleNodeMask = 1;
        
  D3D12_RESOURCE_DESC default_resource_description = {};
  default_resource_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  default_resource_description.Alignment = 0;
  default_resource_description.Width = byte_size;
  default_resource_description.Height = 1;
  default_resource_description.DepthOrArraySize = 1;
  default_resource_description.MipLevels = 1;
  default_resource_description.Format = DXGI_FORMAT_UNKNOWN;
  default_resource_description.SampleDesc.Count = 1;
  default_resource_description.SampleDesc.Quality = 0;
  default_resource_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  default_resource_description.Flags = D3D12_RESOURCE_FLAG_NONE;

  assert(d3d12_device->CreateCommittedResource(&default_heap_properties, D3D12_HEAP_FLAG_NONE,
                                               &default_resource_description, D3D12_RESOURCE_STATE_COMMON,
                                               0, IID_PPV_ARGS(&default_buffer)) == 0);

  D3D12_HEAP_PROPERTIES upload_heap_properties = {};
  upload_heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
  upload_heap_properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
  upload_heap_properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
  upload_heap_properties.CreationNodeMask = 1;
  upload_heap_properties.VisibleNodeMask = 1;

  // TODO(Trystan): Can I just re-use the description from up above? I assume I can.
  D3D12_RESOURCE_DESC upload_resource_description = {};
  upload_resource_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  upload_resource_description.Alignment = 0;
  upload_resource_description.Width = byte_size;
  upload_resource_description.Height = 1;
  upload_resource_description.DepthOrArraySize = 1;
  upload_resource_description.MipLevels = 1;
  upload_resource_description.Format = DXGI_FORMAT_UNKNOWN;
  upload_resource_description.SampleDesc.Count = 1;
  upload_resource_description.SampleDesc.Quality = 0;
  upload_resource_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  upload_resource_description.Flags = D3D12_RESOURCE_FLAG_NONE;

  // Leaving this note here from microsoft. May not refer to exactly what I'm doing here.
  // But still something to keep in mind.
  // Note: using upload heaps to transfer static data like vert buffers is not 
  // recommended. Every time the GPU needs it, the upload heap will be marshalled 
  // over. Please read up on Default Heap usage. An upload heap is used here for 
  // code simplicity and because there are very few verts to actually transfer.
  assert(d3d12_device->CreateCommittedResource(&upload_heap_properties, D3D12_HEAP_FLAG_NONE,
                                               &upload_resource_description, D3D12_RESOURCE_STATE_GENERIC_READ,
                                               NULL, IID_PPV_ARGS(&upload_buffer)) == 0);

  // Describe the data we want to copy into the default buffer.
  D3D12_SUBRESOURCE_DATA sub_resource_data = {};
  sub_resource_data.pData = init_data;
  sub_resource_data.RowPitch = byte_size;
  sub_resource_data.SlicePitch = sub_resource_data.RowPitch;

  // Schedule to copy the data to the default buffer resource.  At a high level, the helper function UpdateSubresources
  // will copy the CPU memory into the intermediate upload heap.  Then, using ID3D12CommandList::CopySubresourceRegion,
  // the intermediate upload heap data will be copied to mBuffer.
  D3D12_RESOURCE_BARRIER default_barrier;
  memset(&default_barrier, 0, sizeof(default_barrier));
  default_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  default_barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  default_barrier.Transition.pResource = default_buffer;
  default_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
  default_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
  default_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

  command_list->ResourceBarrier(1, &default_barrier);

  u64 required_size = 0;
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT layouts[1];
  u32 row_count[1];
  u64 row_sizes_in_bytes[1];

  D3D12_RESOURCE_DESC resource_description = default_buffer->GetDesc();
  ID3D12Device *default_buffer_device;
  default_buffer->GetDevice(IID_PPV_ARGS(&default_buffer_device));
  default_buffer_device->GetCopyableFootprints(&resource_description, 0, 1, 0, layouts, row_count, row_sizes_in_bytes, &required_size);
  default_buffer_device->Release();

  u8 *data;
  
  upload_buffer->Map(0, NULL, (void **)&data);

  // TODO(Trystan): I am absolutely certain this is overkill.
  // I'm just following the way the book shows. But I think we should be able to do it
  // the same way we did square_vertices.
  D3D12_MEMCPY_DEST dest_data = { data + layouts[0].Offset, layouts[0].Footprint.RowPitch, layouts[0].Footprint.RowPitch * row_count[0] };
  for(u32 z = 0; z < layouts[0].Footprint.Depth; ++z){
    u8 *dest_slice = (u8 *)(dest_data.pData) + (dest_data.SlicePitch * z);
    u8 *src_slice = (u8 *)(sub_resource_data.pData) + (sub_resource_data.SlicePitch * z);
    for (u32 y = 0; y < row_count[0]; ++y){
      memcpy(dest_slice + dest_data.RowPitch * y, src_slice + sub_resource_data.RowPitch * y, (SIZE_T)row_sizes_in_bytes[0]);
    }
  }

  //memcpy(dest_data.pData, init_data, byte_size);

  upload_buffer->Unmap(0, NULL);

  // NOTE(Trystan): This code is in d3dx12.h and doesn't even do anything???
  //CD3DX12_BOX SrcBox( UINT( layouts[0].Offset ), UINT( layouts[0].Offset + layouts[0].Footprint.Width ) );
  //D3D12_BOX src_box = {};
  //src_box.left = layouts[0].Offset;
  //src_box.top = 0;
  //src_box.front = 0;
  //src_box.right = layouts[0].Offset + layouts[0].Footprint.Width;
  //src_box.bottom = 1;
  //src_box.back = 1;
  command_list->CopyBufferRegion(default_buffer, 0, upload_buffer, layouts[0].Offset, layouts[0].Footprint.Width);

  default_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
  default_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
  command_list->ResourceBarrier(1, &default_barrier);

  return default_buffer;
}

internal void d3d12_update_object_constant_buffer(){
  d3d12_upload_buffer *object_buffer = &d3d12_current_frame_resource->object_constant_buffer;
  for(u32 i = 0; i < MAX_SQUARES; ++i){
    d3d12_object_constants object_constant;
    object_constant.world = Transpose(d3d12_squares[i].world);

    u32 object_buffer_size = (sizeof(d3d12_object_constants) + 255) & ~255;
    memcpy(&object_buffer->mapped_data[object_buffer_size * d3d12_squares[i].constant_buffer_index],
           &object_constant, sizeof(d3d12_object_constants));
  }
}

internal void d3d12_update_pass_constant_buffer(){
  m4x4 view_projection = view_matrix * projection_matrix;


  d3d12_pass_constant.view = Transpose(view_matrix);
  d3d12_pass_constant.projection = Transpose(projection_matrix);
  d3d12_pass_constant.view_projection = Transpose(view_projection);
  d3d12_pass_constant.eye_pos_w = eye_pos;
  d3d12_pass_constant.render_target_size = V2(1920.0f, 1080.0f);
  d3d12_pass_constant.inv_render_target_size = V2(1.0f / 1920.0f, 1.0f / 1080.0f);
  d3d12_pass_constant.near_z = 1.0f;
  d3d12_pass_constant.far_z = 1000.0f;
  d3d12_pass_constant.total_time = 0.0f; // Time seems to be unused.
  d3d12_pass_constant.delta_time = 0.0f;
  
  memcpy(d3d12_current_frame_resource->pass_constant_buffer.mapped_data, &d3d12_pass_constant, sizeof(d3d12_pass_constants));
}

internal void d3d12_update(){
  f32 theta = 1.5f * Pi32;
  f32 phi = 0.2f * Pi32;
  f32 radius = 15.0f;

  // Convert Spherical to Cartesian coordinates.
  eye_pos.x = radius * -1 * 0; //radius * Sin(theta) * Cos(theta);
  eye_pos.z = radius * 0.809017f * -1; //radius * Cos(phi) * Sin(theta);
  eye_pos.y = radius * 0.809017f;

    

  // Build the view matrix.
  v3 pos = V3(eye_pos.x, eye_pos.y, eye_pos.z);
  v3 target = V3(0.0f, 0.0f, 0.0f);
  v3 up = V3(0.0f, 1.0f, 0.0f);

  v3 z_axis = Normalize(target) - Normalize(pos);
  v3 x_axis = Normalize(Cross(up, z_axis));
  v3 y_axis = Cross(z_axis, x_axis);

  m4x4 orientation = {
    {{x_axis.x, y_axis.x, z_axis.x, 0.0f},
     {x_axis.y, y_axis.y, z_axis.y, 0.0f},
     {x_axis.z, y_axis.z, z_axis.z, 0.0f},
     {0.0f,     0.0f,     0.0f,     1.0f}},
  };

  m4x4 translation = {
    {{ 1.0f,   0.0f,   0.0f,  0.0f},
     { 0.0f,   1.0f,   0.0f,  0.0f},
     { 0.0f,   0.0f,   1.0f,  0.0f},
     {-pos.x, -pos.y, -pos.z, 1.0f}},
  };

  m4x4 view = orientation * translation;

  view_matrix = view;

  
  // WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
  // This is code implemented as such for simplicity. The D3D12HelloFrameBuffering
  // sample illustrates how to use fences for efficient resource usage and to
  // maximize GPU utilization.
  
  d3d12_current_frame_resource_index = (d3d12_current_frame_resource_index + 1) % FRAME_RESOURCE_COUNT;
  d3d12_current_frame_resource = &d3d12_frame_resources[d3d12_current_frame_resource_index];

  // Wait until previous frame is finished.
  if(d3d12_current_frame_resource->fence != 0 && d3d12_fence->GetCompletedValue() < d3d12_current_frame_resource->fence){
    HANDLE event_handle = CreateEventEx(0, 0, 0, EVENT_ALL_ACCESS);
    assert(d3d12_fence->SetEventOnCompletion(d3d12_current_frame_resource->fence, event_handle) == 0);
    WaitForSingleObject(event_handle, INFINITE);
    CloseHandle(event_handle);
  }

  d3d12_update_object_constant_buffer();
  d3d12_update_pass_constant_buffer();
}

internal void d3d12_init(HWND *window) {
  d3d12_viewport.TopLeftX = 0;
  d3d12_viewport.TopLeftY = 0;
  d3d12_viewport.Width    = 1920.0f;
  d3d12_viewport.Height   = 1080.0f;
  d3d12_viewport.MinDepth = 0.0f;
  d3d12_viewport.MaxDepth = 1.0f;

  d3d12_pass_constant = {};
  view_matrix = Identity();
  projection_matrix = Identity();
  eye_pos = V3(0.0f, 0.0f, 0.0f);
  
  d3d12_scissor_rect = {0, 0, 1920, 1080};
    
  UINT dxgi_factory_flags = 0;
    
#if DEBUG
  {
    ID3D12Debug *debug_controller;
    if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug_controller)))){
      debug_controller->EnableDebugLayer();
            
      dxgi_factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    }
  }
#endif
    
  IDXGIFactory4 *dxgi_factory;
  // It seems like we create a lower, safe version of the factory to query if we can create a higher one.
  assert(CreateDXGIFactory2(dxgi_factory_flags, IID_PPV_ARGS(&dxgi_factory)) == 0);
    
  IDXGIAdapter1 *hardware_adapter = NULL;
    
  // I'm thinking the factory6 is the one we are wanting to use, and checking if possible to use. The latest version perhaps?
  IDXGIFactory6 *dxgi_factory6;
  if(SUCCEEDED(dxgi_factory->QueryInterface(IID_PPV_ARGS(&dxgi_factory6)))){
    // This loop setup looks so ugly, but it's how Microsoft shows the example. RIP
    for(UINT adapter_index = 0;
        DXGI_ERROR_NOT_FOUND != dxgi_factory6->EnumAdapterByGpuPreference(adapter_index,
                                                                          DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                          IID_PPV_ARGS(&hardware_adapter));
        ++adapter_index){
            
      DXGI_ADAPTER_DESC1 description;
      hardware_adapter->GetDesc1(&description);
            
      if(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE){
        // We don't want the Basic Render Driver adapter.
        // I also don't think I want to fall back to a software adapter.
        // That sounds gross.
        continue;
      }
            
      if(SUCCEEDED(D3D12CreateDevice(hardware_adapter, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), NULL))){
        break;
      }
    }
  } else {
    // We weren't able to use the factory6, we have to fall back to the factory4.
    // This doesn't preclude us from being able to use D3D12.
    for(UINT adapter_index = 0; DXGI_ERROR_NOT_FOUND != dxgi_factory->EnumAdapters1(adapter_index, &hardware_adapter); ++adapter_index){
      DXGI_ADAPTER_DESC1 description;
      hardware_adapter->GetDesc1(&description);
            
      if(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE){
        // Again, this is the software renderer, we don't want it.
        continue;
      }
            
      if(SUCCEEDED(D3D12CreateDevice(hardware_adapter, D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), NULL))){
        break;
      }
    }
  }
    
  // So now hardware_adapter should be the GPU we want to use.
  assert(D3D12CreateDevice(hardware_adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&d3d12_device)) == 0);
    
  D3D12_COMMAND_QUEUE_DESC queue_description = {};
  queue_description.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
  queue_description.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    
  assert(d3d12_device->CreateCommandQueue(&queue_description, IID_PPV_ARGS(&d3d12_command_queue)) == 0);
    
  DXGI_SWAP_CHAIN_DESC swap_chain_description = {};
  swap_chain_description.BufferCount = BACK_BUFFER_COUNT;
  // Example shows this height and width
  swap_chain_description.BufferDesc.Width = 1920;
  swap_chain_description.BufferDesc.Height = 1080;
  swap_chain_description.BufferDesc.RefreshRate.Numerator = 144;
  swap_chain_description.BufferDesc.RefreshRate.Denominator = 1;
  swap_chain_description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swap_chain_description.BufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
  swap_chain_description.BufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;
  swap_chain_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_description.BufferCount = BACK_BUFFER_COUNT;
  swap_chain_description.SampleDesc.Count = 1;
  swap_chain_description.SampleDesc.Quality = 0;
  swap_chain_description.OutputWindow = *window;
  swap_chain_description.Windowed = true;
  swap_chain_description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  swap_chain_description.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
  
  assert(dxgi_factory->CreateSwapChain(d3d12_command_queue, &swap_chain_description, &d3d12_swap_chain) == 0);
  
  {
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_description = {};
    rtv_heap_description.NumDescriptors = BACK_BUFFER_COUNT;
    rtv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        
    assert(d3d12_device->CreateDescriptorHeap(&rtv_heap_description, IID_PPV_ARGS(&d3d12_rtv_heap)) == 0);
    
        
    d3d12_rtv_descriptor_size = d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // Depth stencil view
    D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_description = {};
    dsv_heap_description.NumDescriptors = 1;
    dsv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    
    assert(d3d12_device->CreateDescriptorHeap(&dsv_heap_description, IID_PPV_ARGS(&d3d12_dsv_heap)) == 0);
  }

  {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = {};
    rtv_handle.ptr = d3d12_rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
        
    for(UINT n = 0; n < BACK_BUFFER_COUNT; ++n){
      assert(d3d12_swap_chain->GetBuffer(n, IID_PPV_ARGS(&d3d12_swap_chain_buffer[n])) == 0);
      d3d12_device->CreateRenderTargetView(d3d12_swap_chain_buffer[n], NULL, rtv_handle);
      rtv_handle.ptr += d3d12_rtv_descriptor_size;
    }

    D3D12_RESOURCE_DESC stencil_description;
    stencil_description.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    stencil_description.Alignment = 0;
    stencil_description.Width = 1920;
    stencil_description.Height = 1080;
    stencil_description.DepthOrArraySize = 1;
    stencil_description.MipLevels = 1;
    stencil_description.Format = DXGI_FORMAT_R24G8_TYPELESS;

    stencil_description.SampleDesc.Count = 1;
    stencil_description.SampleDesc.Quality = 0;
    stencil_description.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    stencil_description.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE opt_clear;
    opt_clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    opt_clear.DepthStencil.Depth = 1.0f;
    opt_clear.DepthStencil.Stencil = 0;

    D3D12_HEAP_PROPERTIES properties;
    properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    
    assert(d3d12_device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &stencil_description,
                                                 D3D12_RESOURCE_STATE_COMMON, &opt_clear,
                                                 IID_PPV_ARGS(&d3d12_depth_stencil_buffer)) == 0);

    D3D12_DEPTH_STENCIL_VIEW_DESC dsv_desc;
	dsv_desc.Flags = D3D12_DSV_FLAG_NONE;
	dsv_desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
	dsv_desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	dsv_desc.Texture2D.MipSlice = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = {};
    dsv_handle.ptr = d3d12_dsv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    
    d3d12_device->CreateDepthStencilView(d3d12_depth_stencil_buffer, &dsv_desc, dsv_handle);
  }
    
  assert(d3d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d3d12_command_allocator)) == 0);
}

internal void d3d12_flush_command_queue(){
  ++d3d12_fence_value;

  assert(d3d12_command_queue->Signal(d3d12_fence, d3d12_fence_value) == 0);

  if(d3d12_fence->GetCompletedValue() < d3d12_fence_value){
    HANDLE event_handle = CreateEventEx(0, 0, 0, EVENT_ALL_ACCESS);

    assert(d3d12_fence->SetEventOnCompletion(d3d12_fence_value, event_handle) == 0);

    WaitForSingleObject(event_handle, INFINITE);
    CloseHandle(event_handle);
  }
}

internal void d3d12_load_assets() {
  // Create a root signature consisting of a descriptor table with a single CBV
  {
    D3D12_FEATURE_DATA_ROOT_SIGNATURE feature_data = {};
    // The sample I followed only allowed this as the highest version.
    feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

    if(FAILED(d3d12_device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &feature_data, sizeof(feature_data)))){
      feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }

    D3D12_DESCRIPTOR_RANGE1 range0 = {};
    range0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    range0.NumDescriptors = 1;
    range0.BaseShaderRegister = 0;
    range0.RegisterSpace = 0;
    range0.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_DESCRIPTOR_RANGE1 range1 = {};
    range1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    range1.NumDescriptors = 1;
    range1.BaseShaderRegister = 1;
    range1.RegisterSpace = 0;
    range1.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    
    D3D12_ROOT_PARAMETER1 root_parameters[2] = {};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_DESCRIPTOR_TABLE1 root_descriptor_table0 = {};
    root_descriptor_table0.NumDescriptorRanges = 1;
    root_descriptor_table0.pDescriptorRanges = &range0;

    root_parameters[0].DescriptorTable = root_descriptor_table0;

    root_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_DESCRIPTOR_TABLE1 root_descriptor_table1 = {};
    root_descriptor_table1.NumDescriptorRanges = 1;
    root_descriptor_table1.pDescriptorRanges = &range1;

    root_parameters[1].DescriptorTable = root_descriptor_table1;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC signature_description = {};
    signature_description.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    signature_description.Desc_1_1.NumParameters = array_count(root_parameters);
    signature_description.Desc_1_1.pParameters = root_parameters;
    signature_description.Desc_1_1.NumStaticSamplers = 0;
    signature_description.Desc_1_1.pStaticSamplers = NULL;
    signature_description.Desc_1_1.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    
    ID3DBlob *signature;
    ID3DBlob *error;

    assert(D3D12SerializeVersionedRootSignature(&signature_description, &signature, &error) == 0);

    assert(d3d12_device->CreateRootSignature(0, signature->GetBufferPointer(),
                                             signature->GetBufferSize(), IID_PPV_ARGS(&d3d12_signature)) == 0);
  }
    
  // Create the pipeline state
  {
    ID3DBlob *vertex_shader;
    ID3DBlob *pixel_shader;
        
#if DEBUG
    UINT compile_flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    UINT compile_flags = 0;
#endif
        
    assert(D3DCompile(d3d12_shader_code, string_length(d3d12_shader_code),
                      NULL, NULL, NULL, "VSMain", "vs_5_1", compile_flags, 0, &vertex_shader, NULL) == 0);
    assert(D3DCompile(d3d12_shader_code, string_length(d3d12_shader_code),
                      NULL, NULL, NULL, "PSMain", "ps_5_1", compile_flags, 0, &pixel_shader, NULL) == 0);
        
    // Define the vertex input layout.
    D3D12_INPUT_ELEMENT_DESC input_element_descriptions[2] = {};
    input_element_descriptions[0] = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
    input_element_descriptions[1] = { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 };
        
    // Describe and create thet graphics pipeline state object (PSO).
    D3D12_SHADER_BYTECODE vs_bytecode = {};
    vs_bytecode.pShaderBytecode = vertex_shader->GetBufferPointer();
    vs_bytecode.BytecodeLength = vertex_shader->GetBufferSize();
    D3D12_SHADER_BYTECODE ps_bytecode = {};
    ps_bytecode.pShaderBytecode = pixel_shader->GetBufferPointer();
    ps_bytecode.BytecodeLength = pixel_shader->GetBufferSize();
        
    // This is the default RASTERIZER_DESC
    D3D12_RASTERIZER_DESC rasterizer_description = {};
    rasterizer_description.FillMode = D3D12_FILL_MODE_SOLID;
    rasterizer_description.CullMode = D3D12_CULL_MODE_BACK;
    rasterizer_description.FrontCounterClockwise = FALSE;
    rasterizer_description.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
    rasterizer_description.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
    rasterizer_description.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
    rasterizer_description.DepthClipEnable = TRUE;
    rasterizer_description.MultisampleEnable = FALSE;
    rasterizer_description.AntialiasedLineEnable = FALSE;
    rasterizer_description.ForcedSampleCount = 0;
    rasterizer_description.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        
    // This is the default BLEND_DESC
    D3D12_BLEND_DESC blend_description = {};
    blend_description.AlphaToCoverageEnable = FALSE;
    blend_description.IndependentBlendEnable = FALSE;
    D3D12_RENDER_TARGET_BLEND_DESC default_render_target_blend_description =
      {
        FALSE,FALSE,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_BLEND_ONE, D3D12_BLEND_ZERO, D3D12_BLEND_OP_ADD,
        D3D12_LOGIC_OP_NOOP,
        D3D12_COLOR_WRITE_ENABLE_ALL,
      };
    for (UINT i = 0; i < D3D12_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
      blend_description.RenderTarget[i] = default_render_target_blend_description;
    }

    // This is the default DEPTH_STENCIL_DESC
    D3D12_DEPTH_STENCIL_DESC depth_description = {};
    depth_description.DepthEnable = TRUE;
    depth_description.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    depth_description.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    depth_description.StencilEnable = FALSE;
    depth_description.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
    depth_description.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
    const D3D12_DEPTH_STENCILOP_DESC default_stencil_op =
      { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
    depth_description.FrontFace = default_stencil_op;
    depth_description.BackFace = default_stencil_op;
        
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_description = {};
    pso_description.InputLayout = { input_element_descriptions, array_count(input_element_descriptions) };
    pso_description.pRootSignature = d3d12_signature;
    pso_description.VS = vs_bytecode;
    pso_description.PS = ps_bytecode;
    pso_description.RasterizerState = rasterizer_description;
    pso_description.BlendState = blend_description;
    pso_description.DepthStencilState = depth_description;
    pso_description.SampleMask = UINT_MAX;
    pso_description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_description.NumRenderTargets = 1;
    pso_description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_description.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    pso_description.SampleDesc.Count = 1;
        
    assert(d3d12_device->CreateGraphicsPipelineState(&pso_description, IID_PPV_ARGS(&d3d12_pipeline_state)) == 0);
  }
    
  assert(d3d12_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d3d12_command_allocator,
                                         d3d12_pipeline_state, IID_PPV_ARGS(&d3d12_command_list)) == 0);
    
  // Create vertex and index buffer
  {
    generate_box_geometry(&mesh_geometry, 100.0f, 100.0f, 100.0f);
    
    assert(D3DCreateBlob(mesh_geometry.vertex_buffer_byte_size, &mesh_geometry.vertex_buffer_CPU) == 0);
    memcpy(mesh_geometry.vertex_buffer_CPU->GetBufferPointer(), mesh_geometry.vertices, mesh_geometry.vertex_buffer_byte_size);
    
    assert(D3DCreateBlob(mesh_geometry.index_buffer_byte_size, &mesh_geometry.index_buffer_CPU) == 0);
    memcpy(mesh_geometry.index_buffer_CPU->GetBufferPointer(), mesh_geometry.indices, mesh_geometry.index_buffer_byte_size);
    
    mesh_geometry.vertex_buffer_GPU = d3d12_create_default_buffer(d3d12_device,
                                                                  d3d12_command_list, &mesh_geometry.vertices[0],
                                                                  mesh_geometry.vertex_buffer_byte_size,
                                                                  mesh_geometry.vertex_buffer_uploader);

    mesh_geometry.index_buffer_GPU = d3d12_create_default_buffer(d3d12_device,
                                                                 d3d12_command_list, &mesh_geometry.indices[0],
                                                                 mesh_geometry.index_buffer_byte_size,
                                                                 mesh_geometry.index_buffer_uploader);
  }

  // Setup the 8 render_items. All using the cube mesh, but individual constant buffers.
  {
    for(s32 i = 0; i < MAX_SQUARES; ++i){
      d3d12_squares[i].constant_buffer_index = i;
      d3d12_squares[i].world = Identity();
      // NOTE(Trystan): The default state of squares is to be off screen.
      // And drawn on screen whenever we get data on them, then back off screen if we don't.
      // Obviously not the best way of handling this, but just getting us going quickly.
      Translate(d3d12_squares[i].world, V3(2.0f * i, 2.0f * i, -5.0f * i));
    }
  }

  // Build the frame resources
  {
    for(u32 i = 0; i < FRAME_RESOURCE_COUNT; ++i){
      assert(d3d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  IID_PPV_ARGS(&d3d12_frame_resources[i].command_list_allocator)) == 0);
      // Constant buffers must be a multiple of 256 bytes. This rounds us up to the nearest 256.
      u32 pass_buffer_size = (sizeof(d3d12_pass_constants) + 255) & ~255;
      u32 object_buffer_size = (sizeof(d3d12_object_constants) + 255) & ~255;

      d3d12_frame_resources[i].fence = 0;
      d3d12_frame_resources[i].pass_constant_buffer.element_byte_size = pass_buffer_size;
      d3d12_frame_resources[i].object_constant_buffer.element_byte_size = object_buffer_size;

      D3D12_RESOURCE_DESC pass_buffer_description = d3d12_create_default_description_buffer(pass_buffer_size);
      D3D12_RESOURCE_DESC object_buffer_description = d3d12_create_default_description_buffer(object_buffer_size * MAX_SQUARES);

      // The same for both constant buffers.
      D3D12_HEAP_PROPERTIES properties;
      properties.Type = D3D12_HEAP_TYPE_UPLOAD;
      properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
      properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
      properties.CreationNodeMask = 1;
      properties.VisibleNodeMask = 1;

      // Just to make the below a little more readable.
      d3d12_upload_buffer *pass_buffer = &d3d12_frame_resources[i].pass_constant_buffer;
      d3d12_upload_buffer *object_buffer = &d3d12_frame_resources[i].object_constant_buffer;

      assert(d3d12_device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &pass_buffer_description,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, 0,
                                                   IID_PPV_ARGS(&pass_buffer->upload_buffer)) == 0);

      assert(d3d12_device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &object_buffer_description,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, 0,
                                                   IID_PPV_ARGS(&object_buffer->upload_buffer)) == 0);

      assert(pass_buffer->upload_buffer->Map(0, 0, (void **)&pass_buffer->mapped_data) == 0);
      assert(object_buffer->upload_buffer->Map(0, 0, (void **)&object_buffer->mapped_data) == 0);
    }
  }

  // Create constant buffer view descriptor heap
  {
    // The one is for the per pass CBV for each frame resource.
    uint32 descriptor_count = (MAX_SQUARES + 1) * FRAME_RESOURCE_COUNT;
    d3d12_pass_cbv_offset = MAX_SQUARES * FRAME_RESOURCE_COUNT;

    // Describe and create a constant buffer view (CBV) descriptor heap.
    // Flags indicate that this descriptor heap can be bound to the pipeline
    // and that descriptors contained in it can be referenced by a root table.
    D3D12_DESCRIPTOR_HEAP_DESC cbv_heap_description = {};
    cbv_heap_description.NumDescriptors = descriptor_count;
    cbv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    cbv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbv_heap_description.NodeMask = 0;
    assert(d3d12_device->CreateDescriptorHeap(&cbv_heap_description, IID_PPV_ARGS(&d3d12_cbv_heap)) == 0);
  }

  // Now we actually create the constant buffer view
  {
    u32 object_buffer_size = (sizeof(d3d12_object_constants) + 255) & ~255;

    for(u32 frame_index = 0; frame_index < FRAME_RESOURCE_COUNT; ++frame_index){
      ID3D12Resource *upload_buffer = d3d12_frame_resources[frame_index].object_constant_buffer.upload_buffer;
      for(u32 i = 0; i < MAX_SQUARES; ++i){
        D3D12_GPU_VIRTUAL_ADDRESS constant_buffer_address = upload_buffer->GetGPUVirtualAddress();

        // Offset to the ith object constant buffer.
        constant_buffer_address += i * object_buffer_size;

        // Offset to the object cbv in the descriptor heap.
        u32 heap_index = frame_index * MAX_SQUARES + i;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
        handle.ptr = d3d12_cbv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
        handle.ptr += heap_index * d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_description;
        cbv_description.BufferLocation = constant_buffer_address;
        cbv_description.SizeInBytes = object_buffer_size;

        d3d12_device->CreateConstantBufferView(&cbv_description, handle);
      }
    }
    
    u32 pass_buffer_size = (sizeof(d3d12_pass_constants) + 255) & ~255;

    for(u32 frame_index = 0; frame_index < FRAME_RESOURCE_COUNT; ++frame_index){
      ID3D12Resource *upload_buffer = d3d12_frame_resources[frame_index].pass_constant_buffer.upload_buffer;
      D3D12_GPU_VIRTUAL_ADDRESS constant_buffer_address = upload_buffer->GetGPUVirtualAddress();

      // Offset to the pass cbv in the descriptor heap.
      u32 heap_index = d3d12_pass_cbv_offset + frame_index;
      D3D12_CPU_DESCRIPTOR_HANDLE handle = {};
      handle.ptr = d3d12_cbv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
      handle.ptr += heap_index * d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

      D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_description;
      cbv_description.BufferLocation = constant_buffer_address;
      cbv_description.SizeInBytes = pass_buffer_size;

      d3d12_device->CreateConstantBufferView(&cbv_description, handle);
    }
  }
    
  // Create synchronization objects and wait until assets are uploaded to the GPU.
  {
    assert(d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d3d12_fence)) == 0);
    d3d12_fence_value = 1;
        
    // Execute the initialization commands.
    D3D12_RESOURCE_BARRIER resource_barrier_stencil = {};
    resource_barrier_stencil.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    resource_barrier_stencil.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    resource_barrier_stencil.Transition.pResource = d3d12_depth_stencil_buffer;
    resource_barrier_stencil.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    resource_barrier_stencil.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    resource_barrier_stencil.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    
    d3d12_command_list->ResourceBarrier(1, &resource_barrier_stencil);
    
    assert(d3d12_command_list->Close() == 0);
    ID3D12CommandList *command_lists[] = { d3d12_command_list };
    d3d12_command_queue->ExecuteCommandLists(array_count(command_lists), command_lists);

    // Wait until initialization is complete.
    d3d12_flush_command_queue();
  }
}

internal void d3d12_on_render() {
  // Command list allocators can only be reset when the associated 
  // command lists have finished execution on the GPU; apps should use 
  // fences to determine GPU execution progress.
  ID3D12CommandAllocator *command_allocator = d3d12_current_frame_resource->command_list_allocator;
  assert(command_allocator->Reset() == 0);
    
  // However, when ExecuteCommandList() is called on a particular command 
  // list, that command list can then be reset at any time and must be before 
  // re-recording.
  assert(d3d12_command_list->Reset(command_allocator, d3d12_pipeline_state) == 0);

  d3d12_command_list->RSSetViewports(1, &d3d12_viewport);
  d3d12_command_list->RSSetScissorRects(1, &d3d12_scissor_rect);
    
  // We are going to have two of these, where the before and after states get flipped.
  // NOTE(Trystan): See if we can consolidate this into one barrier, by flipping instead of creating two
  D3D12_RESOURCE_BARRIER resource_barrier_render_target = {};
  resource_barrier_render_target.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  resource_barrier_render_target.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  resource_barrier_render_target.Transition.pResource = d3d12_swap_chain_buffer[d3d12_current_back_buffer];
  resource_barrier_render_target.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  resource_barrier_render_target.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  resource_barrier_render_target.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

  // Indicate that the back buffer will be used as a render target.
  d3d12_command_list->ResourceBarrier(1, &resource_barrier_render_target);

  D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = {};
  rtv_handle.ptr = d3d12_rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + (d3d12_current_back_buffer * d3d12_rtv_descriptor_size);
  v4 clear_color = V4(0.0f, 0.2f, 0.4f, 1.0f);
  d3d12_command_list->ClearRenderTargetView(rtv_handle, clear_color.E, 0, NULL);

  D3D12_CPU_DESCRIPTOR_HANDLE dsv_handle = {};
  dsv_handle.ptr = d3d12_dsv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
  d3d12_command_list->ClearDepthStencilView(dsv_handle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

  d3d12_command_list->OMSetRenderTargets(1, &rtv_handle, TRUE, &dsv_handle);

  ID3D12DescriptorHeap *heaps[] = { d3d12_cbv_heap };
  d3d12_command_list->SetDescriptorHeaps(array_count(heaps), heaps);

  d3d12_command_list->SetGraphicsRootSignature(d3d12_signature);
  
  u32 pass_cbv_index = d3d12_pass_cbv_offset + d3d12_current_frame_resource_index;

  D3D12_GPU_DESCRIPTOR_HANDLE pass_cbv_handle = {};
  pass_cbv_handle.ptr = d3d12_cbv_heap->GetGPUDescriptorHandleForHeapStart().ptr;
  pass_cbv_handle.ptr += pass_cbv_index * d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  d3d12_command_list->SetGraphicsRootDescriptorTable(1, pass_cbv_handle);
  
  

  u32 object_buffer_size = (sizeof(d3d12_object_constants) + 255) & ~255;
  ID3D12Resource* object_upload_buffer = d3d12_current_frame_resource->object_constant_buffer.upload_buffer;

  D3D12_VERTEX_BUFFER_VIEW square_vbv;
  square_vbv.BufferLocation = mesh_geometry.vertex_buffer_GPU->GetGPUVirtualAddress();
  square_vbv.StrideInBytes = mesh_geometry.vertex_byte_stride;
  square_vbv.SizeInBytes = mesh_geometry.vertex_buffer_byte_size;

  D3D12_INDEX_BUFFER_VIEW square_ibv;
  square_ibv.BufferLocation = mesh_geometry.index_buffer_GPU->GetGPUVirtualAddress();
  square_ibv.Format = mesh_geometry.index_format;
  square_ibv.SizeInBytes = mesh_geometry.index_buffer_byte_size;
  
  for(u32 i = 0; i < MAX_SQUARES; ++i){
    d3d12_render_item *square = &d3d12_squares[i];

    

    d3d12_command_list->IASetVertexBuffers(0, 1, &square_vbv);
    d3d12_command_list->IASetIndexBuffer(&square_ibv);
    d3d12_command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Offset to the constant buffer view in the descriptor heap for this object and frame resource.
    u32 object_cbv_index = d3d12_current_frame_resource_index * MAX_SQUARES + square->constant_buffer_index;
    D3D12_GPU_DESCRIPTOR_HANDLE object_cbv_handle = {};
    object_cbv_handle.ptr = d3d12_cbv_heap->GetGPUDescriptorHandleForHeapStart().ptr;
    object_cbv_handle.ptr += object_cbv_index * d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    d3d12_command_list->SetGraphicsRootDescriptorTable(0, object_cbv_handle);

    d3d12_command_list->DrawIndexedInstanced(mesh_geometry.index_count, 1, 0, 0, 0);
  }

  // Indicate that the back buffer will now be used to present.
  D3D12_RESOURCE_BARRIER resource_barrier_present = {};
  resource_barrier_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  resource_barrier_present.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  resource_barrier_present.Transition.pResource = d3d12_swap_chain_buffer[d3d12_current_back_buffer];
  resource_barrier_present.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  resource_barrier_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  resource_barrier_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    
  d3d12_command_list->ResourceBarrier(1, &resource_barrier_present);
    
  assert(d3d12_command_list->Close() == 0);

  // Execute the command list.
  ID3D12CommandList *command_lists[] = { d3d12_command_list };
  d3d12_command_queue->ExecuteCommandLists(array_count(command_lists), command_lists);
    
  // Present the frame.
  assert(d3d12_swap_chain->Present(0, 0) == 0);
  d3d12_current_back_buffer = (d3d12_current_back_buffer + 1) % BACK_BUFFER_COUNT;

  d3d12_current_frame_resource->fence = ++d3d12_fence_value;

  d3d12_command_queue->Signal(d3d12_fence, d3d12_fence_value);
}

internal void d3d12_update_square_position(v3 position) {
  //d3d12_constant_buffer_data.offset.x = position.x;
  //d3d12_constant_buffer_data.offset.y = position.y;
  //d3d12_constant_buffer_data.offset.z = position.z;
  //temp_constant_buffer *test = (temp_constant_buffer *)d3d12_cbv_data_begin;
  //memcpy(d3d12_cbv_data_begin, &d3d12_constant_buffer_data, sizeof(d3d12_constant_buffer_data));
}

internal void win32_process_keyboard_message(game_button_state *new_state, b32 is_down){
  if(new_state->ended_down != is_down){
    new_state->ended_down = is_down;
    ++new_state->half_transition_count;
  }
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

// NOTE(Trystan): Either make a separate function for adding network entries,
// Or add a parameter to denote that it goes in the network specific queue.
internal void win32_add_queue_entry(platform_work_queue *queue, platform_work_queue_callback *callback, void *data){
  uint32 new_next_entry_to_write = (queue->next_entry_to_write + 1) % array_count(queue->entries);
  assert(new_next_entry_to_write != queue->next_entry_to_read);
  platform_work_queue_entry *entry = queue->entries + queue->next_entry_to_write;
  entry->callback = callback;
  entry->data = data;
  ++queue->completion_goal;
  _WriteBarrier();
  queue->next_entry_to_write = new_next_entry_to_write;
  ReleaseSemaphore(queue->semaphore_handle, 1, 0);
}

// NOTE(Trystan): I'm thinking I want a system where
// we check if there's a task in the networking queue first no matter what,
// then move on to a normal queue after that.
// I want networking to always take priority, but I don't want to have a dedicated thread for it.
// As I feel like that thread will be wasted, sitting idle most of the time.
internal bool32 win32_do_next_work_queue_entry(platform_work_queue *queue){
  bool32 we_should_sleep = false;
    
  uint32 original_next_entry_to_read = queue->next_entry_to_read;
  uint32 new_next_entry_to_read = (original_next_entry_to_read + 1) % array_count(queue->entries);
    
  if(original_next_entry_to_read != queue->next_entry_to_read){
    uint32 index = InterlockedCompareExchange((LONG volatile *)&queue->next_entry_to_read,
                                              new_next_entry_to_read,
                                              original_next_entry_to_read);
        
    if(index == original_next_entry_to_read){
      platform_work_queue_entry entry = queue->entries[index];
      // NOTE(Trystan): Casey has a todo about a thread-specific struct
      // that contains a scratch arena that the thread can use for temprorary work.
      entry.callback(queue, entry.data);
      InterlockedIncrement((LONG volatile *)&queue->completion_count);
    }
  } else {
    we_should_sleep = true;
  }
  
  
  return we_should_sleep;
}

internal void win32_complete_all_work(platform_work_queue *queue){
  while(queue->completion_goal != queue->completion_count){
    win32_do_next_work_queue_entry(queue);
  }
  
  queue->completion_goal = 0;
  queue->completion_count = 0;
}

DWORD WINAPI ThreadProc(LPVOID lpParameter){
  win32_thread_startup *thread = (win32_thread_startup *)lpParameter;
  platform_work_queue *queue = thread->queue;
    
  // uint32 test_thread_id = GetThreadID();
  // assert(test_thread_id == GetCurrentThreadId());
    
  for(;;){
    if(win32_do_next_work_queue_entry(queue)){
      WaitForSingleObjectEx(queue->semaphore_handle, INFINITE, FALSE);
    }
  }
}

internal void win32_make_queue(platform_work_queue *queue, uint32 thread_count, win32_thread_startup *startups){
  queue->completion_goal = 0;
  queue->completion_count = 0;
    
  queue->next_entry_to_read = 0;
  queue->next_entry_to_write = 0;
    
  uint32 initial_count = 0;
  queue->semaphore_handle = CreateSemaphoreEx(0, initial_count, thread_count, 0, 0, SEMAPHORE_ALL_ACCESS);
    
  for(uint32 thread_index = 0; thread_index < thread_count; ++thread_index){
    win32_thread_startup *startup = startups + thread_index;
    startup->queue = queue;
        
    DWORD thread_id;
    HANDLE thread_handle = CreateThread(0, megabytes(1), ThreadProc, startup, 0, &thread_id);
    CloseHandle(thread_handle);
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
  WSADATA wsa_data;

  // MAKEWORD(2,0) denotes we want to use winsock 2.0
  assert(WSAStartup(MAKEWORD(2, 0), &wsa_data) == 0);
  
  struct addrinfo hints = {};
  // NOTE(Trystan): AF_INET6 if we want to try IPv6
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;

  struct addrinfo *server_info = NULL;
  // 192.168.2.146 My desktop
  // 192.168.20.140 wsl2 ubuntu // This changes at startup RIP
  // 192.168.2.98 8GB pi
  getaddrinfo("172.17.132.247", PORT, &hints, &server_info);

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

internal void win32_process_pending_messages(game_controller_input *controller) {
  MSG message = {};
  for(;;){
    BOOL got_message = FALSE;
        
    {
      // This part is just from handmade hero,
      // refer to Casey's note for more information about skipped messages.
      DWORD skip_messages[] = { 0x738, 0xFFFFFFFF };
            
      DWORD last_message = 0;
      for(u32 skip_index = 0; skip_index < array_count(skip_messages); ++skip_index){
        DWORD skip = skip_messages[skip_index];
        got_message = PeekMessage(&message, 0, last_message, skip - 1, PM_REMOVE);
        if(got_message){
          break;
        }
                
        last_message = skip + 1;
      }
    }
        
    if(!got_message){
      break;
    }
        
    switch(message.message) {
    case WM_QUIT:
      {
        global_running = false;
      } break;
      
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
    case WM_KEYDOWN:
    case WM_KEYUP:
      {
        uint32 vk_code = (uint32)message.wParam;

        // TODO(Trystan): If we care about alt or shift, we have to check the lParam.

        b32 was_down = ((message.lParam & (1 << 30)) != 0);
        b32 is_down = ((message.lParam & (1UL << 31)) == 0);
        if(was_down != is_down){
          if(vk_code == 'W'){
            win32_process_keyboard_message(&controller->move_up, is_down);
          }
          else if(vk_code == 'A'){
            win32_process_keyboard_message(&controller->move_left, is_down);
          }
          else if(vk_code == 'S'){
            win32_process_keyboard_message(&controller->move_down, is_down);
          }
          else if(vk_code == 'D'){
            win32_process_keyboard_message(&controller->move_right, is_down);
          }
          else if(vk_code == VK_ESCAPE){
            win32_process_keyboard_message(&controller->back, is_down);
          }
        }
      } break;
            
    default:
      {
        TranslateMessage(&message);
        DispatchMessage(&message);
      } break;
    }
  }
}

extern "C" int __stdcall WinMainCRTStartup() {
  // This could be useful later. This is how you load the full file path including the exe name.
  // So this is how you would get wherever the exe is stored.
  //char exe_file_name[MAX_PATH];
  //GetModuleFileNameA(0, exe_file_name, sizeof(exe_file_name));
    
  HINSTANCE instance = GetModuleHandle(0);

  LARGE_INTEGER performance_count_frequency_result;
  QueryPerformanceFrequency(&performance_count_frequency_result);
  global_performance_count_frequency = performance_count_frequency_result.QuadPart;
  
  WNDCLASSA window_class = {};
  window_class.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
  window_class.lpfnWndProc = Win32MainWindowCallback;
  window_class.hInstance = instance;
  window_class.hCursor = LoadCursor(0, IDC_ARROW);
  window_class.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
  window_class.lpszClassName = "TrystanWindowClass";
    
  if(RegisterClassA(&window_class)){
    HWND window = CreateWindowExA(0, window_class.lpszClassName, "The Game", WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                                  CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                  0, 0, instance, 0);
        
    if(window){
      win32_network_connection connection = win32_socket_init();

      network_buffer connecting_message = {};
      pack_uint32(&connecting_message, (u32)network_packet_type_connect);
      win32_send_packet(&connection, &connecting_message);
      // The server then sends back a packet containing the packet type header + the assigned ID.
      
      d3d12_init(&window);
      d3d12_load_assets();

      SYSTEM_INFO system_info = {};
      GetSystemInfo(&system_info);

      uint32 logical_processor_count = system_info.dwNumberOfProcessors;
      uint32 thread_count = logical_processor_count - 1;
            
      // TODO(Trystan): Again we want to take this out of a larger startup memory buffer.
      win32_thread_startup *thread_startups = (win32_thread_startup *)VirtualAlloc(0, thread_count * sizeof(win32_thread_startup),
                                                                                   MEM_RESERVE|MEM_COMMIT, PAGE_READWRITE);
            
      platform_work_queue thread_queue = {};
      win32_make_queue(&thread_queue, thread_count, thread_startups);

      // TODO(Trystan): Temporary
      float aspect_ratio = 1920.0f / 1080.0f;
      v3 square_position = {};
      float half_square_width = 0.5f / 2.0f;
      float half_square_height = (0.5f * aspect_ratio) / 2.0f;
      v2 square_bounding_box = V2(1 - half_square_width, 1 - half_square_height);
      

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

      u64 last_received_packet_number = 0;

      network_client_normal_packet normal_outgoing_packet = {};

      while(global_running){
        new_input->dt_for_frame = target_seconds_per_frame;
        time_since_last_sent_packet += new_input->dt_for_frame;

        game_controller_input *old_controller = &old_input->controller;
        game_controller_input *new_controller = &new_input->controller;
        *new_controller = {};
        new_controller->is_connected = true;
        // We need to carry over the button state every time to ensure half_transition_count updates properly.
        for(uint32 button_index = 0; button_index < array_count(new_controller->buttons); ++button_index){
          new_controller->buttons[button_index].ended_down = old_controller->buttons[button_index].ended_down;
        }

        // Get input, update game simulation, render.
        win32_process_pending_messages(&new_input->controller);

        int wait_condition = win32_socket_wait(&connection, 0);
        if(wait_condition){
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
                // Contains position data about all players.
                network_server_normal_packet server_data = {};
                server_data.packet_number = unpack_uint64(&buffer);
                server_data.player_count = unpack_uint32(&buffer);
                if(last_received_packet_number > server_data.packet_number){
                  break;
                }
                
                for(u32 i = 0; i < server_data.player_count; ++i){
                  server_data.player_info[i].player_id = unpack_uint32(&buffer);
                  u32 encoded_x = unpack_uint32(&buffer);
                  f32 x = (f32)net_decode_32(encoded_x);
                  server_data.player_info[i].x = x;
                  u32 encoded_y = unpack_uint32(&buffer);
                  f32 y = (f32)net_decode_32(encoded_y);
                  server_data.player_info[i].y = y;
                }

                for(u32 i = 0; i < server_data.player_count; ++i){
                  // At the moment, ignore our own packet. Do some type of correction if the player is too out of sync.
                  if(server_data.player_info[i].player_id == normal_outgoing_packet.player_id){
                    continue;
                  }

                  // Draw square at position.
                  
                }
                last_received_packet_number = server_data.packet_number;
              } break;
            default:
              // Invalid packet type.
              break;
            }
          }
        }

        // TODO(Trystan): Yeet all button processing out of here into the game layer.
        f32 square_speed = 1.0f;
        if(is_down(new_input->controller.move_left)){
          square_position.x -= square_speed * new_input->dt_for_frame;
          normal_outgoing_packet.button_durations[network_button_type_left] += new_input->dt_for_frame;
        }
        if(is_down(new_input->controller.move_right)){
          square_position.x += square_speed * new_input->dt_for_frame;
          normal_outgoing_packet.button_durations[network_button_type_right] += new_input->dt_for_frame;
        }
        if(is_down(new_input->controller.move_up)){
          square_position.y += (square_speed * new_input->dt_for_frame) * aspect_ratio;
          normal_outgoing_packet.button_durations[network_button_type_up] += new_input->dt_for_frame;
        }
        if(is_down(new_input->controller.move_down)){
          square_position.y -= square_speed * new_input->dt_for_frame * aspect_ratio;
          normal_outgoing_packet.button_durations[network_button_type_down] += new_input->dt_for_frame;
        }
        if(was_pressed(new_input->controller.back)){
          global_running = false;
        }

        if(square_position.x > square_bounding_box.x) square_position.x = square_bounding_box.x;
        if(square_position.x < -square_bounding_box.x) square_position.x = -square_bounding_box.x;
        if(square_position.y > square_bounding_box.y) square_position.y = square_bounding_box.y;
        if(square_position.y < -square_bounding_box.y) square_position.y = -square_bounding_box.y;

        d3d12_update_square_position(square_position);
        
        d3d12_update();
        d3d12_on_render();

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
    }
  }
  
  ExitProcess(0);
}
