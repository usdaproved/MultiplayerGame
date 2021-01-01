#include <windows.h>

#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <float.h>

// D3D12 related includes
#include <d3d12.h>
#include <dxgi1_6.h>
#include <D3DCompiler.h>
#define DEBUG 1
#define FRAME_COUNT 2


// I was getting an error with _fltused not being defined.
// I should investigate what is causing this to be invoked in the first place.
// Apparently it has something to do with floats or doubles.
#ifdef __cplusplus
extern "C" {
#endif
  int _fltused=0; // it should be a single underscore since the double one is the mangled name
#ifdef __cplusplus
}
#endif


// Extract these typedefs out.
typedef int32_t int32;
typedef int32 s32;
typedef uint32_t uint32;
typedef uint32 u32;
typedef float real32;
typedef double real64;
typedef real32 f32;

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

#define array_count(array) (sizeof(array) / sizeof((array)[0]))
#define assert(expression) if(!(expression)) {*(volatile int *)0 = 0;}

static int global_running;

// D3D12 related variables
static ID3D12Device *d3d12_device;
static ID3D12CommandQueue *d3d12_command_queue;
static IDXGISwapChain3 *d3d12_swap_chain;
static ID3D12DescriptorHeap *d3d12_rtv_heap;
static ID3D12Resource *d3d12_render_targets[FRAME_COUNT];
static ID3D12CommandAllocator *d3d12_command_allocator;
static ID3D12RootSignature *d3d12_signature;
static ID3D12PipelineState *d3d12_pipeline_state;
static ID3D12GraphicsCommandList *d3d12_command_list;

static ID3D12Resource *d3d12_vertex_buffer;
static D3D12_VERTEX_BUFFER_VIEW d3d12_vertex_buffer_view;

struct d3d12_vertex {
  v3 position;
  v4 color;
};

static UINT d3d12_frame_index;
static UINT d3d12_rtv_descriptor_size;
static HANDLE d3d12_fence_event;
static ID3D12Fence *d3d12_fence;
static UINT64 d3d12_fence_value;

static D3D12_VIEWPORT d3d12_viewport;
static D3D12_RECT d3d12_scissor_rect;

static char *d3d12_shader_code = R"FOO(
// I'm thinking this should be temporary.
// We either want to embed this in our code files
// Or I think end game would be pre-compiling the shader.

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

PSInput VSMain(float4 position : POSITION, float4 color : COLOR)
{
    PSInput result;

    result.position = position;
    result.color = color;

    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return input.color;
}
)FOO";

inline u32 string_length(char *string){
  u32 count = 0;
  if(string){
    while(*string++){
      ++count;
    }
  }
  
  return count;
}

static void d3d12_wait_for_previous_frame(){
  // WAITING FOR THE FRAME TO COMPLETE BEFORE CONTINUING IS NOT BEST PRACTICE.
  // This is code implemented as such for simplicity. The D3D12HelloFrameBuffering
  // sample illustrates how to use fences for efficient resource usage and to
  // maximize GPU utilization.

  UINT64 fence = d3d12_fence_value;
  assert(d3d12_command_queue->Signal(d3d12_fence, fence) == 0);
  d3d12_fence_value += 1;

  // Wait until previous frame is finished.
  if(d3d12_fence->GetCompletedValue() < fence){
    assert(d3d12_fence->SetEventOnCompletion(fence, d3d12_fence_event) == 0);
    WaitForSingleObject(d3d12_fence_event, INFINITE);
  }

  d3d12_frame_index = d3d12_swap_chain->GetCurrentBackBufferIndex();
}

static void d3d12_init(HWND *window) {
  // Some top level variable setup.
  // 1280 720 width and height
  d3d12_viewport = {0.0f, 0.0f, 1280.0f, 720.0f};
  d3d12_scissor_rect = {0, 0, 1280, 720};
  
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

  DXGI_SWAP_CHAIN_DESC1 swap_chain_description = {};
  swap_chain_description.BufferCount = FRAME_COUNT;
  // Example shows this height and width
  swap_chain_description.Width = 1280;
  swap_chain_description.Height = 720;
  swap_chain_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  swap_chain_description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap_chain_description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD; // Check if we want this.
  swap_chain_description.SampleDesc.Count = 1; // Wut?

  IDXGISwapChain1 *swap_chain1;
  assert(dxgi_factory->CreateSwapChainForHwnd(d3d12_command_queue, *window, &swap_chain_description, NULL, NULL, &swap_chain1) == 0);

  // That example doesn't support fullscreen, circle back around to that.
  assert(dxgi_factory->MakeWindowAssociation(*window, DXGI_MWA_NO_ALT_ENTER) == 0);

  d3d12_swap_chain = (IDXGISwapChain3 *)swap_chain1;

  d3d12_frame_index = d3d12_swap_chain->GetCurrentBackBufferIndex();

  {
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_description = {};
    rtv_heap_description.NumDescriptors = FRAME_COUNT;
    rtv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    
    assert(d3d12_device->CreateDescriptorHeap(&rtv_heap_description, IID_PPV_ARGS(&d3d12_rtv_heap)) == 0);

    d3d12_rtv_descriptor_size = d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  }
    
  {
    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = {};
    rtv_handle.ptr = d3d12_rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr;
    
    for(UINT n = 0; n < FRAME_COUNT; ++n){
      assert(d3d12_swap_chain->GetBuffer(n, IID_PPV_ARGS(&d3d12_render_targets[n])) == 0);
      d3d12_device->CreateRenderTargetView(d3d12_render_targets[n], NULL, rtv_handle);
      rtv_handle.ptr += d3d12_rtv_descriptor_size;
    }
  }
    
  assert(d3d12_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&d3d12_command_allocator)) == 0);
}

static void d3d12_load_assets() {
  // Create empty root signature.
  {
    D3D12_ROOT_SIGNATURE_DESC signature_description = {};

    signature_description.NumParameters = 0;
    signature_description.pParameters = NULL;
    signature_description.NumStaticSamplers = 0;
    signature_description.pStaticSamplers = NULL;
    signature_description.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob *signature;
    ID3DBlob *error;
    
    assert(D3D12SerializeRootSignature(&signature_description, D3D_ROOT_SIGNATURE_VERSION_1, &signature, &error) == 0);
    assert(d3d12_device->CreateRootSignature(0, signature->GetBufferPointer(),
					     signature->GetBufferSize(), IID_PPV_ARGS(&d3d12_signature)) == 0);
  }

  // Create the pipeline state
  {
    ID3DBlob *vertex_shader;
    ID3DBlob *pixel_shader;
    
#if defined(DEBUG)
    UINT compile_flags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    UINT compile_flags = 0;
#endif

    
    assert(D3DCompile(d3d12_shader_code, string_length(d3d12_shader_code),
		      NULL, NULL, NULL, "VSMain", "vs_5_0", compile_flags, 0, &vertex_shader, NULL) == 0);
    assert(D3DCompile(d3d12_shader_code, string_length(d3d12_shader_code),
		      NULL, NULL, NULL, "PSMain", "ps_5_0", compile_flags, 0, &pixel_shader, NULL) == 0);

    // Define the vertex input layout.
    D3D12_INPUT_ELEMENT_DESC input_element_descriptions[] =
      {
       { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
       { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
      };

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
    
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_description = {};
    pso_description.InputLayout = { input_element_descriptions, array_count(input_element_descriptions) };
    pso_description.pRootSignature = d3d12_signature;
    pso_description.VS = vs_bytecode;
    pso_description.PS = ps_bytecode;
    pso_description.RasterizerState = rasterizer_description;
    pso_description.BlendState = blend_description;
    pso_description.DepthStencilState.DepthEnable = FALSE;
    pso_description.DepthStencilState.StencilEnable = FALSE;
    pso_description.SampleMask = UINT_MAX;
    pso_description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_description.NumRenderTargets = 1;
    pso_description.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_description.SampleDesc.Count = 1;
    
    assert(d3d12_device->CreateGraphicsPipelineState(&pso_description, IID_PPV_ARGS(&d3d12_pipeline_state)) == 0);
  }
  
  assert(d3d12_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, d3d12_command_allocator,
					 d3d12_pipeline_state, IID_PPV_ARGS(&d3d12_command_list)) == 0);

  // Command lists are created in the recording state, but there is nothing to record yet.
  assert(d3d12_command_list->Close() == 0);

  // Create vertex buffer
  {
    float aspect_ratio = 1280 / 720;
    d3d12_vertex triangle_vertices[] = {{ V3(0.0f, 0.25f * aspect_ratio, 0.0f), V4(1.0f, 0.0f, 0.0f, 1.0f) },
					{ V3(0.25f, -0.25f * aspect_ratio, 0.0f), V4(0.0f, 1.0f, 0.0f, 1.0f) },
					{ V3(-0.25f, -0.25f * aspect_ratio, 0.0f), V4(0.0f, 0.0f, 1.0f, 1.0f) }
    };
      
    UINT vertex_buffer_size = sizeof(triangle_vertices);

    D3D12_HEAP_PROPERTIES heap_properties = {};
    heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap_properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_properties.CreationNodeMask = 1;
    heap_properties.VisibleNodeMask = 1;

    D3D12_RESOURCE_DESC resource_description = {};
    resource_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_description.Alignment = 0;
    resource_description.Width = vertex_buffer_size;
    resource_description.Height = 1;
    resource_description.DepthOrArraySize = 1;
    resource_description.MipLevels = 1;
    resource_description.Format = DXGI_FORMAT_UNKNOWN;
    resource_description.SampleDesc.Count = 1;
    resource_description.SampleDesc.Quality = 0;
    resource_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource_description.Flags = D3D12_RESOURCE_FLAG_NONE;
      
    // Note: using upload heaps to transfer static data like vert buffers is not 
    // recommended. Every time the GPU needs it, the upload heap will be marshalled 
    // over. Please read up on Default Heap usage. An upload heap is used here for 
    // code simplicity and because there are very few verts to actually transfer.
    assert(d3d12_device->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE,
						 &resource_description, D3D12_RESOURCE_STATE_GENERIC_READ,
						 NULL, IID_PPV_ARGS(&d3d12_vertex_buffer)) == 0);

    // Copy the triangle data to the vertex buffer.
    UINT8 *vertex_data_begin;
    D3D12_RANGE read_range = {}; // Sets range.Begin and range.End to zero.
    assert(d3d12_vertex_buffer->Map(0, &read_range, (void **)&vertex_data_begin) == 0);
    memcpy(vertex_data_begin, triangle_vertices, sizeof(triangle_vertices));
    d3d12_vertex_buffer->Unmap(0, NULL);

    // Initialize the vertex buffer view.
    d3d12_vertex_buffer_view.BufferLocation = d3d12_vertex_buffer->GetGPUVirtualAddress();
    d3d12_vertex_buffer_view.StrideInBytes = sizeof(d3d12_vertex);
    d3d12_vertex_buffer_view.SizeInBytes = vertex_buffer_size;
  }

  // Create synchronization objects and wait until assets are uploaded to the GPU.
  {
    assert(d3d12_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d3d12_fence)) == 0);
    d3d12_fence_value = 1;

    // Create an event handle to use for frame synchronization.
    d3d12_fence_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if(!d3d12_fence_event){
      assert(HRESULT_FROM_WIN32(GetLastError()) == 0);
    }

    // Wait for the command list to execute; we are reusing the same command 
    // list in our main loop but for now, we just want to wait for setup to 
    // complete before continuing.
    d3d12_wait_for_previous_frame();
  }
}

static void d3d12_populate_command_list() {
  // Command list allocators can only be reset when the associated 
  // command lists have finished execution on the GPU; apps should use 
  // fences to determine GPU execution progress.
  assert(d3d12_command_allocator->Reset() == 0);

  // However, when ExecuteCommandList() is called on a particular command 
  // list, that command list can then be reset at any time and must be before 
  // re-recording.
  assert(d3d12_command_list->Reset(d3d12_command_allocator, d3d12_pipeline_state) == 0);

  // Set necessary state.
  d3d12_command_list->SetGraphicsRootSignature(d3d12_signature);
  d3d12_command_list->RSSetViewports(1, &d3d12_viewport);
  d3d12_command_list->RSSetScissorRects(1, &d3d12_scissor_rect);

  // We are going to have two of these, where the before and after states get flipped.
  // NOTE(Trystan): See if we can consolidate this into one barrier, by flipping instead of creating two
  D3D12_RESOURCE_BARRIER resource_barrier_render_target = {};
  resource_barrier_render_target.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  resource_barrier_render_target.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  resource_barrier_render_target.Transition.pResource = d3d12_render_targets[d3d12_frame_index];
  resource_barrier_render_target.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  resource_barrier_render_target.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  resource_barrier_render_target.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

  // Indicate that the back buffer will be used as a render target.
  d3d12_command_list->ResourceBarrier(1, &resource_barrier_render_target);
  
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = {};
  rtv_handle.ptr = d3d12_rtv_heap->GetCPUDescriptorHandleForHeapStart().ptr + (d3d12_frame_index * d3d12_rtv_descriptor_size);
  d3d12_command_list->OMSetRenderTargets(1, &rtv_handle, FALSE, NULL);

  // Record commands
  v4 clear_color = V4(0.0f, 0.2f, 0.4f, 1.0f);
  d3d12_command_list->ClearRenderTargetView(rtv_handle, clear_color.E, 0, NULL);
  d3d12_command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  d3d12_command_list->IASetVertexBuffers(0, 1, &d3d12_vertex_buffer_view);
  d3d12_command_list->DrawInstanced(3, 1, 0, 0);

  // Indicate that the back buffer will now be used to present.
  D3D12_RESOURCE_BARRIER resource_barrier_present = {};
  resource_barrier_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  resource_barrier_present.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  resource_barrier_present.Transition.pResource = d3d12_render_targets[d3d12_frame_index];
  resource_barrier_present.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
  resource_barrier_present.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  resource_barrier_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

  d3d12_command_list->ResourceBarrier(1, &resource_barrier_present);

  assert(d3d12_command_list->Close() == 0);
}

static void d3d12_on_render() {
  // Record all the commands needed to render the scene.
  d3d12_populate_command_list();
    
  // Execute the command list.
  ID3D12CommandList *command_lists[] = { d3d12_command_list };
  d3d12_command_queue->ExecuteCommandLists(array_count(command_lists), command_lists);

  // Present the frame.
  assert(d3d12_swap_chain->Present(0, 0) == 0);

  d3d12_wait_for_previous_frame();
}

static LRESULT CALLBACK
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

static void win32_process_pending_messages() {
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
      d3d12_init(&window);
      d3d12_load_assets();
      
      global_running = 1;

      while(global_running){
	win32_process_pending_messages();
	d3d12_on_render();
      }
      
    }
  }

  ExitProcess(0);
}
