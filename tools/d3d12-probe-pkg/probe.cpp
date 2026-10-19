// D3D12 gates: hardware device, copy/fence, shader readback and optional presentation.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

static void check(HRESULT result, const char* step) {
    if (FAILED(result)) {
        std::fprintf(stderr,"D3D12_FAIL %s hr=0x%08lx\n",step,(unsigned long)result);
        std::exit(1);
    }
}

static unsigned keys, resizes, width=640, height=360;
static bool pendingResize;
static LRESULT CALLBACK windowProc(HWND window,UINT message,WPARAM w,LPARAM l) {
    if(message==WM_DESTROY){PostQuitMessage(0);return 0;}
    if(message==WM_KEYDOWN){
        ++keys;
        if(w==VK_ESCAPE)DestroyWindow(window);
        if(w=='R'){
            RECT rect{0,0,1280,720};AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
            if(!SetWindowPos(window,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER))std::exit(6);
        }
        return 0;
    }
    if(message==WM_SIZE && w!=SIZE_MINIMIZED && LOWORD(l) && HIWORD(l)){
        width=LOWORD(l);height=HIWORD(l);pendingResize=true;++resizes;
    }
    return DefWindowProcW(window,message,w,l);
}
static ID3DBlob* compile(const char* source,const char* target) {
    ID3DBlob *blob=nullptr,*error=nullptr;
    HRESULT result=D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,"main",target,0,0,&blob,&error);
    if(error){if(FAILED(result))std::fwrite(error->GetBufferPointer(),1,error->GetBufferSize(),stderr);error->Release();}
    check(result,target);return blob;
}
static void transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource=resource;barrier.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore=before;barrier.Transition.StateAfter=after;list->ResourceBarrier(1,&barrier);
}
static unsigned render(ID3D12Device* device,ID3D12CommandQueue* queue,IDXGIFactory4* factory,bool windowed,unsigned frames) {
    HWND window=nullptr;IDXGISwapChain3* swap=nullptr;
    if(windowed){
        WNDCLASSW wc{};wc.lpfnWndProc=windowProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"BrookD3D12";
        if(!RegisterClassW(&wc))std::exit(6);
        RECT rect{0,0,640,360};AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
        window=CreateWindowW(wc.lpszClassName,L"Brook D3D12 via VKD3D",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
            CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,wc.hInstance,nullptr);
        if(!window)std::exit(6);
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=width;desc.Height=height;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        IDXGISwapChain1* chain=nullptr;check(factory->CreateSwapChainForHwnd(queue,window,&desc,nullptr,nullptr,&chain),"swapchain");
        check(chain->QueryInterface(IID_PPV_ARGS(&swap)),"swapchain3");chain->Release();pendingResize=false;
    }
    D3D12_ROOT_SIGNATURE_DESC rootDesc{};rootDesc.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ID3DBlob *serialized=nullptr,*errors=nullptr;
    HRESULT result=D3D12SerializeRootSignature(&rootDesc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,&errors);
    if(errors)errors->Release();
    check(result,"serialize root signature");
    ID3D12RootSignature* root=nullptr;check(device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root)),"root signature");serialized->Release();
    ID3DBlob* vs=compile("float4 main(uint id:SV_VertexID):SV_Position {float2 p[3]={float2(-.8,-.8),float2(0,.8),float2(.8,-.8)};return float4(p[id],0,1);}","vs_5_0");
    ID3DBlob* ps=compile("float4 main():SV_Target{return float4(1,0,0,1);}","ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root;
    pso.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pso.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
    pso.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pso.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pso.RasterizerState.DepthClipEnable=TRUE;
    auto& blend=pso.BlendState.RenderTarget[0];blend.SrcBlend=blend.SrcBlendAlpha=D3D12_BLEND_ONE;
    blend.DestBlend=blend.DestBlendAlpha=D3D12_BLEND_ZERO;blend.BlendOp=blend.BlendOpAlpha=D3D12_BLEND_OP_ADD;
    blend.LogicOp=D3D12_LOGIC_OP_NOOP;blend.RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;pso.SampleMask=UINT_MAX;
    pso.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pso.NumRenderTargets=1;
    pso.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;pso.SampleDesc.Count=1;
    ID3D12PipelineState* pipeline=nullptr;check(device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&pipeline)),"graphics pipeline");vs->Release();ps->Release();
    D3D12_DESCRIPTOR_HEAP_DESC descriptor{};descriptor.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;descriptor.NumDescriptors=1;
    ID3D12DescriptorHeap* rtv=nullptr;check(device->CreateDescriptorHeap(&descriptor,IID_PPV_ARGS(&rtv)),"RTV heap");
    ID3D12CommandAllocator* allocator=nullptr;check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"draw allocator");
    ID3D12GraphicsCommandList* list=nullptr;check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator,pipeline,IID_PPV_ARGS(&list)),"draw list");check(list->Close(),"initial draw close");
    ID3D12Fence* fence=nullptr;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"draw fence");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)std::exit(3);
    unsigned rendered=0;bool verify=true,quit=false;UINT64 sequence=0;
    while(rendered<(windowed?frames:1) && !quit){
        MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT)quit=true;TranslateMessage(&msg);DispatchMessageW(&msg);}
        if(quit)break;
        if(windowed && IsIconic(window)){Sleep(10);continue;}
        if(swap && pendingResize){check(swap->ResizeBuffers(2,width,height,DXGI_FORMAT_UNKNOWN,0),"resize");pendingResize=false;verify=true;std::printf("D3D12_RESIZE %ux%u\n",width,height);}
        ID3D12Resource *target=nullptr,*readback=nullptr;
        D3D12_RESOURCE_DESC texture{};texture.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width=width;texture.Height=height;texture.DepthOrArraySize=texture.MipLevels=1;
        texture.Format=DXGI_FORMAT_R8G8B8A8_UNORM;texture.SampleDesc.Count=1;texture.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if(swap)check(swap->GetBuffer(swap->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&target)),"backbuffer");
        else {D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&texture,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target)),"render target");}
        auto handle=rtv->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(target,nullptr,handle);
        check(allocator->Reset(),"reset allocator");check(list->Reset(allocator,pipeline),"reset draw list");
        if(swap)transition(list,target,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->SetGraphicsRootSignature(root);D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};D3D12_RECT scissor{0,0,LONG(width),LONG(height)};
        list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);list->OMSetRenderTargets(1,&handle,FALSE,nullptr);
        const float blue[]={0,0,1,1};list->ClearRenderTargetView(handle,blue,0,nullptr);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes=0;
        if(verify){
            device->GetCopyableFootprints(&texture,0,1,0,&footprint,nullptr,nullptr,&bytes);
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;
            buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"pixel readback buffer");
            transition(list,target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src{},dst{};src.pResource=target;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.pResource=readback;dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;
            list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            if(swap)transition(list,target,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
        }else if(swap)transition(list,target,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
        check(list->Close(),"close draw list");ID3D12CommandList* commands[]={list};queue->ExecuteCommandLists(1,commands);
        if(swap)check(swap->Present(1,0),"present");
        check(fence->SetEventOnCompletion(++sequence,event),"draw event");check(queue->Signal(fence,sequence),"draw signal");
        if(WaitForSingleObject(event,10000)!=WAIT_OBJECT_0 || fence->GetCompletedValue()<sequence)std::exit(4);
        if(readback){
            unsigned char* pixels=nullptr;D3D12_RANGE range{0,SIZE_T(bytes)};
            check(readback->Map(0,&range,reinterpret_cast<void**>(&pixels)),"pixel map");
            auto center=pixels+footprint.Offset+(height/2)*footprint.Footprint.RowPitch+(width/2)*4;
            auto corner=pixels+footprint.Offset+4;
            bool correct=center[0]>253&&center[1]<2&&center[2]<2&&center[3]>253&&corner[0]<2&&corner[1]<2&&corner[2]>253&&corner[3]>253;
            std::printf("D3D12_READBACK %ux%u center=%u,%u,%u,%u corner=%u,%u,%u,%u\n",width,height,center[0],center[1],center[2],center[3],corner[0],corner[1],corner[2],corner[3]);
            D3D12_RANGE empty{0,0};readback->Unmap(0,&empty);readback->Release();if(!correct)std::exit(7);verify=false;
        }
        target->Release();++rendered;
    }
    CloseHandle(event);fence->Release();list->Release();allocator->Release();rtv->Release();pipeline->Release();root->Release();
    if(swap)swap->Release();
    if(window && IsWindow(window))DestroyWindow(window);
    std::printf("D3D12_RENDER_COMPLETE frames=%u keys=%u resizes=%u\n",rendered,keys,resizes);return rendered;
}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);
    bool deviceOnly=false,windowed=false;unsigned frames=180;
    for(int i=1;i<argc;++i){
        if(!std::strcmp(argv[i],"--device-only"))deviceOnly=true;
        else if(!std::strcmp(argv[i],"--window"))windowed=true;
        else if(!std::strcmp(argv[i],"--frames") && i+1<argc)frames=std::strtoul(argv[++i],nullptr,10);
        else return 64;
    }
    if(!frames || frames>6000)return 64;
    std::puts("D3D12_START hardware feature_level=11_0");
    IDXGIFactory4* factory=nullptr;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");
    IDXGIAdapter1* adapter=nullptr;
    check(factory->EnumAdapters1(0,&adapter),"adapter");
    DXGI_ADAPTER_DESC1 desc{};check(adapter->GetDesc1(&desc),"adapter description");
    char name[256]{};
    WideCharToMultiByte(CP_UTF8,0,desc.Description,-1,name,sizeof(name),nullptr,nullptr);
    std::printf("D3D12_ADAPTER %s vendor=%x device=%x software=%u\n",
                name,desc.VendorId,desc.DeviceId,unsigned(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE));
    if (desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) return 2;
    ID3D12Device* device=nullptr;
    check(D3D12CreateDevice(adapter,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"device");
    std::puts("D3D12_DEVICE_CREATED");
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    check(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,&options,sizeof(options)),"options");
    std::printf("D3D12_OPTIONS binding_tier=%u heap_tier=%u\n",
                unsigned(options.ResourceBindingTier),unsigned(options.ResourceHeapTier));
    ID3D12CommandQueue* queue=nullptr;D3D12_COMMAND_QUEUE_DESC q{};
    q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"queue");
    ID3D12CommandAllocator* allocator=nullptr;
    check(device->CreateCommandAllocator(q.Type,IID_PPV_ARGS(&allocator)),"allocator");
    ID3D12GraphicsCommandList* list=nullptr;
    check(device->CreateCommandList(0,q.Type,allocator,nullptr,IID_PPV_ARGS(&list)),"list");
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width=65536;buffer.Height=1;buffer.DepthOrArraySize=1;buffer.MipLevels=1;
    buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    ID3D12Resource *upload=nullptr,*readback=nullptr;
    check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,
        D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),"upload buffer");
    heap.Type=D3D12_HEAP_TYPE_READBACK;
    check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,
        D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"readback buffer");
    uint32_t* data=nullptr;D3D12_RANGE empty{0,0};
    check(upload->Map(0,&empty,reinterpret_cast<void**>(&data)),"upload map");
    for(unsigned i=0;i<16384;++i)data[i]=0x5a17c300u^(i*2654435761u);
    D3D12_RANGE written{0,65536};upload->Unmap(0,&written);
    list->CopyBufferRegion(readback,0,upload,0,65536);check(list->Close(),"close list");
    ID3D12CommandList* lists[]={list};queue->ExecuteCommandLists(1,lists);
    ID3D12Fence* fence=nullptr;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"fence");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return 3;
    check(fence->SetEventOnCompletion(1,event),"fence event");check(queue->Signal(fence,1),"signal");
    DWORD waited=WaitForSingleObject(event,10000);
    if(waited!=WAIT_OBJECT_0 || fence->GetCompletedValue()<1) {
        std::fprintf(stderr,"D3D12_FAIL fence wait=%lu value=%llu\n",waited,fence->GetCompletedValue());return 4;
    }
    D3D12_RANGE range{0,65536};check(readback->Map(0,&range,reinterpret_cast<void**>(&data)),"readback map");
    for(unsigned i=0;i<16384;++i)if(data[i]!=(0x5a17c300u^(i*2654435761u))) {
        std::fprintf(stderr,"D3D12_FAIL copy word=%u value=%x\n",i,data[i]);return 5;
    }
    readback->Unmap(0,&empty);
    unsigned rendered=deviceOnly?0:render(device,queue,factory,windowed,frames);
    if(!deviceOnly && !rendered)return 8;
    CloseHandle(event);fence->Release();list->Release();allocator->Release();
    readback->Release();upload->Release();queue->Release();device->Release();
    adapter->Release();factory->Release();
    std::printf("{\"passed\":true,\"copied_words\":16384,\"event_fence\":true,\"rendering_tested\":%s,\"frames\":%u,\"windowed\":%s}\n",deviceOnly?"false":"true",rendered,windowed?"true":"false");
    return 0;
}
