#include "game_platform.h"
#include "game_math.h"
#include "game_intrinsics.h"
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
#define FRAME_COUNT 2

#include "win32_main.h"

global int global_running;
global s64 global_performance_count_frequency;

// D3D12 related variables
global ID3D12Device *d3d12_device;
global ID3D12CommandQueue *d3d12_command_queue;
global IDXGISwapChain3 *d3d12_swap_chain;
global ID3D12DescriptorHeap *d3d12_rtv_heap;
global ID3D12DescriptorHeap *d3d12_cbv_heap;
global ID3D12Resource *d3d12_render_targets[FRAME_COUNT];
global ID3D12CommandAllocator *d3d12_command_allocator;
global ID3D12RootSignature *d3d12_signature;
global ID3D12PipelineState *d3d12_pipeline_state;
global ID3D12GraphicsCommandList *d3d12_command_list;

global ID3D12Resource *d3d12_vertex_buffer;
global ID3D12Resource *d3d12_constant_buffer;
global D3D12_VERTEX_BUFFER_VIEW d3d12_vertex_buffer_view;



struct d3d12_vertex {
  v3 position;
  v4 color;
};

struct temp_constant_buffer{
  v4 offset;
  float padding[60]; // NOTE(Trystan): constant buffer must be 256-byte aligned.
};

global UINT8 *d3d12_cbv_data_begin = NULL;
global temp_constant_buffer d3d12_constant_buffer_data = {};

global UINT d3d12_frame_index;
global UINT d3d12_rtv_descriptor_size;
global HANDLE d3d12_fence_event;
global ID3D12Fence *d3d12_fence;
global UINT64 d3d12_fence_value;

global D3D12_VIEWPORT d3d12_viewport;
global D3D12_RECT d3d12_scissor_rect;

global char *d3d12_shader_code = R"FOO(
// I'm thinking this should be temporary.
// We either want to embed this in our code files
// Or I think end game would be pre-compiling the shader.

cbuffer temp_constant_buffer : register(b0)
{
   float4 offset;
   float4 padding[15];
};

struct PSInput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

PSInput VSMain(float4 position : POSITION, float4 color : COLOR)
{
    PSInput result;

    result.position = position + offset;
    result.color = color;

    return result;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    return input.color;
}
)FOO";

internal void d3d12_wait_for_previous_frame(){
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

internal void d3d12_init(HWND *window) {
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
    
  // TODO(Trystan): That example doesn't support fullscreen, circle back around to that.
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

    // Describe and create a constant buffer view (CBV) descriptor heap.
    // Flags indicate that this descriptor heap can be bound to the pipeline
    // and that descriptors contained in it can be referenced by a root table.
    D3D12_DESCRIPTOR_HEAP_DESC cbv_heap_description = {};
    cbv_heap_description.NumDescriptors = 1;
    cbv_heap_description.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    cbv_heap_description.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    assert(d3d12_device->CreateDescriptorHeap(&cbv_heap_description, IID_PPV_ARGS(&d3d12_cbv_heap)) == 0);
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

internal void d3d12_load_assets() {
  // Create a root signature consisting of a descriptor table with a single CBV
  {
    D3D12_FEATURE_DATA_ROOT_SIGNATURE feature_data = {};
    // The sample I followed only allowed this as the highest version.
    feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

    if(FAILED(d3d12_device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &feature_data, sizeof(feature_data)))){
      feature_data.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }

    D3D12_DESCRIPTOR_RANGE1 ranges[1] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
    ranges[0].NumDescriptors = 1;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    
    D3D12_ROOT_PARAMETER1 root_parameters[1] = {};
    root_parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

    D3D12_ROOT_DESCRIPTOR_TABLE1 root_descriptor_table = {};
    root_descriptor_table.NumDescriptorRanges = 1;
    root_descriptor_table.pDescriptorRanges = &ranges[0];

    root_parameters[0].DescriptorTable = root_descriptor_table;

    // Allow input layout and deny uneccessary access to certain pipeline stages.
    D3D12_ROOT_SIGNATURE_FLAGS signature_flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
      D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
      D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
      D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS |
      D3D12_ROOT_SIGNATURE_FLAG_DENY_PIXEL_SHADER_ROOT_ACCESS;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC signature_description = {};
    signature_description.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    signature_description.Desc_1_1.NumParameters = array_count(root_parameters);
    signature_description.Desc_1_1.pParameters = root_parameters;
    signature_description.Desc_1_1.NumStaticSamplers = 0;
    signature_description.Desc_1_1.pStaticSamplers = NULL;
    signature_description.Desc_1_1.Flags = signature_flags;
    
    ID3DBlob *signature;
    ID3DBlob *error;

    // This call assumes that we actually got version 1_1.
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
                      NULL, NULL, NULL, "VSMain", "vs_5_0", compile_flags, 0, &vertex_shader, NULL) == 0);
    assert(D3DCompile(d3d12_shader_code, string_length(d3d12_shader_code),
                      NULL, NULL, NULL, "PSMain", "ps_5_0", compile_flags, 0, &pixel_shader, NULL) == 0);
        
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
    float aspect_ratio = 1920.0f / 1080.0f;
    d3d12_vertex square_vertices[] = {{ V3(0.25f,  0.25f * aspect_ratio, 0.0f),  V4(0.9f, 0.2f, 0.9f, 1.0f) },
                                      { V3(0.25f, -0.25f * aspect_ratio, 0.0f),  V4(0.9f, 0.2f, 0.9f, 1.0f) },
                                      { V3(-0.25f,  0.25f * aspect_ratio, 0.0f), V4(0.9f, 0.2f, 0.9f, 1.0f) },
                                      { V3(0.25f, -0.25f * aspect_ratio, 0.0f),  V4(0.9f, 0.2f, 0.9f, 1.0f) },
                                      { V3(-0.25f, -0.25f * aspect_ratio, 0.0f), V4(0.9f, 0.2f, 0.9f, 1.0f) },
                                      { V3(-0.25f,  0.25f * aspect_ratio, 0.0f), V4(0.9f, 0.2f, 0.9f, 1.0f) }};
        
    UINT vertex_buffer_size = sizeof(square_vertices);
        
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
        
    // Copy the square data to the vertex buffer.
    UINT8 *vertex_data_begin;
    D3D12_RANGE read_range = {}; // Sets range.Begin and range.End to zero.
    assert(d3d12_vertex_buffer->Map(0, &read_range, (void **)&vertex_data_begin) == 0);
    memcpy(vertex_data_begin, square_vertices, sizeof(square_vertices));
    d3d12_vertex_buffer->Unmap(0, NULL);
        
    // Initialize the vertex buffer view.
    d3d12_vertex_buffer_view.BufferLocation = d3d12_vertex_buffer->GetGPUVirtualAddress();
    d3d12_vertex_buffer_view.StrideInBytes = sizeof(d3d12_vertex);
    d3d12_vertex_buffer_view.SizeInBytes = vertex_buffer_size;
  }

  // Create the constant buffer.
  {
    // This one is exactly like the square one.
    D3D12_HEAP_PROPERTIES heap_properties = {};
    heap_properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap_properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heap_properties.CreationNodeMask = 1;
    heap_properties.VisibleNodeMask = 1;

    // temp_constant_buffer is the name of the struct, yes bad. yes temp.
    UINT constant_buffer_size = sizeof(temp_constant_buffer);
    
    // This one only differs in size.
    D3D12_RESOURCE_DESC resource_description = {};
    resource_description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_description.Alignment = 0;
    resource_description.Width = constant_buffer_size;
    resource_description.Height = 1;
    resource_description.DepthOrArraySize = 1;
    resource_description.MipLevels = 1;
    resource_description.Format = DXGI_FORMAT_UNKNOWN;
    resource_description.SampleDesc.Count = 1;
    resource_description.SampleDesc.Quality = 0;
    resource_description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    resource_description.Flags = D3D12_RESOURCE_FLAG_NONE;
    
    assert(d3d12_device->CreateCommittedResource(&heap_properties, D3D12_HEAP_FLAG_NONE,
                                                 &resource_description, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 NULL, IID_PPV_ARGS(&d3d12_constant_buffer)) == 0);

    // Describe and create a constant buffer view.
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv_description = {};
    cbv_description.BufferLocation = d3d12_constant_buffer->GetGPUVirtualAddress();
    cbv_description.SizeInBytes = constant_buffer_size;
    d3d12_device->CreateConstantBufferView(&cbv_description, d3d12_cbv_heap->GetCPUDescriptorHandleForHeapStart());

    // Map and initialize the constant buffer. We don't unmap this until the app closes.
    // Keeping things mapped for the lifetime of the resource is okay.
    D3D12_RANGE read_range = {}; // again set Begin and End to zero.
    assert(d3d12_constant_buffer->Map(0, &read_range, (void **)&d3d12_cbv_data_begin) == 0);
    memcpy(d3d12_cbv_data_begin, &d3d12_constant_buffer_data, sizeof(d3d12_constant_buffer_data));
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

internal void d3d12_populate_command_list() {
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

  ID3D12DescriptorHeap *heaps[] = { d3d12_cbv_heap };
  d3d12_command_list->SetDescriptorHeaps(array_count(heaps), heaps);
  
  d3d12_command_list->SetGraphicsRootDescriptorTable(0, d3d12_cbv_heap->GetGPUDescriptorHandleForHeapStart());
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
  d3d12_command_list->DrawInstanced(6, 1, 0, 0);
    
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

internal void d3d12_on_render() {
  // Record all the commands needed to render the scene.
  d3d12_populate_command_list();
    
  // Execute the command list.
  ID3D12CommandList *command_lists[] = { d3d12_command_list };
  d3d12_command_queue->ExecuteCommandLists(array_count(command_lists), command_lists);
    
  // Present the frame.
  assert(d3d12_swap_chain->Present(0, 0) == 0);
    
  d3d12_wait_for_previous_frame();
}

internal void d3d12_update_square_position(v3 position) {
  d3d12_constant_buffer_data.offset.x = position.x;
  d3d12_constant_buffer_data.offset.y = position.y;
  d3d12_constant_buffer_data.offset.z = position.z;
  temp_constant_buffer *test = (temp_constant_buffer *)d3d12_cbv_data_begin;
  memcpy(d3d12_cbv_data_begin, &d3d12_constant_buffer_data, sizeof(d3d12_constant_buffer_data));
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
// Time in ms, 60 updates per second.
#define PACKET_SEND_RATE 16

struct win32_network_connection{
  SOCKET socket;
  struct sockaddr address; // This is the address of the server.
  u32 address_length; // This should just be the same value every time.
};

// We need some sort of win32_packet.
// Have our own header set up, first X bytes say what type of message it is.
// CONNECTING,
// GAME

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
  getaddrinfo("192.168.101.247", PORT, &hints, &server_info);

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
