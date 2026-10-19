// Deterministic Windows D3D11 workload. No fallback to a software D3D device.
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static unsigned keys, resizes, clientWidth=640, clientHeight=360;
static bool resizePending;
static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (message==WM_KEYDOWN) {
        ++keys;
        if(w==VK_ESCAPE)DestroyWindow(window);
        if(w=='R') {
            RECT rect{0,0,1280,720};
            if(!AdjustWindowRectEx(&rect,GetWindowLongW(window,GWL_STYLE),FALSE,GetWindowLongW(window,GWL_EXSTYLE)) ||
               !SetWindowPos(window,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER)) {
                std::fprintf(stderr,"D3D11_FAIL programmatic resize error=%lu\n",GetLastError());std::exit(3);
            }
        }
        return 0;
    }
    if (message==WM_SIZE) {
        ++resizes;
        if(w!=SIZE_MINIMIZED && LOWORD(l) && HIWORD(l)) {
            clientWidth=LOWORD(l);clientHeight=HIWORD(l);resizePending=true;
        }
    }
    if (message==WM_DESTROY) {PostQuitMessage(0);return 0;}
    return DefWindowProcW(window,message,w,l);
}
static void check(HRESULT result,const char* step) {
    if(FAILED(result)) {std::fprintf(stderr,"D3D11_FAIL %s hr=0x%08lx\n",step,(unsigned long)result);std::exit(1);}
}
static ID3DBlob* compile(const char* source,const char* target) {
    ID3DBlob *code=nullptr,*errors=nullptr;
    HRESULT result=D3DCompile(source,std::strlen(source),nullptr,nullptr,nullptr,"main",target,0,0,&code,&errors);
    if(errors) {if(FAILED(result))std::fwrite(errors->GetBufferPointer(),1,errors->GetBufferSize(),stderr);errors->Release();}
    check(result,target);return code;
}
static void readback(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* target) {
    D3D11_TEXTURE2D_DESC desc{};target->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    ID3D11Texture2D* staging=nullptr;check(device->CreateTexture2D(&desc,nullptr,&staging),"staging");
    context->CopyResource(staging,target);
    D3D11_MAPPED_SUBRESOURCE map{};check(context->Map(staging,0,D3D11_MAP_READ,0,&map),"readback map");
    const unsigned char* center=static_cast<unsigned char*>(map.pData)+(desc.Height/2)*map.RowPitch+(desc.Width/2)*4;
    const unsigned char* corner=static_cast<unsigned char*>(map.pData)+4;
    // BGRA target: triangle center red, untouched corner blue. Excludes a
    // clear-only false positive; Map must wait for the real shader draw/copy.
    bool correct=center[0]<2 && center[1]<2 && center[2]>253 && center[3]>253 &&
                 corner[0]>253 && corner[1]<2 && corner[2]<2 && corner[3]>253;
    std::printf("D3D11_READBACK %ux%u center=%u,%u,%u,%u corner=%u,%u,%u,%u\n",
        desc.Width,desc.Height,center[0],center[1],center[2],center[3],corner[0],corner[1],corner[2],corner[3]);
    context->Unmap(staging,0);staging->Release();
    if(!correct) {std::fprintf(stderr,"D3D11_FAIL incorrect triangle pixels\n");std::exit(2);}
}
int main(int argc,char** argv) {
    std::setvbuf(stdout,nullptr,_IONBF,0);bool offscreen=false;unsigned frames=180;
    for(int i=1;i<argc;++i) {
        if(!std::strcmp(argv[i],"--offscreen"))offscreen=true;
        else if(!std::strcmp(argv[i],"--frames") && i+1<argc)frames=std::strtoul(argv[++i],nullptr,10);
        else {std::fprintf(stderr,"usage: brook-d3d11.exe [--offscreen] [--frames N]\n");return 64;}
    }
    if(!frames || frames>6000)return 64;
    ULONGLONG start=GetTickCount64();
    ID3D11Device* device=nullptr;ID3D11DeviceContext* context=nullptr;D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL requested[]={D3D_FEATURE_LEVEL_11_0};
    std::puts("D3D11_START hardware feature_level=11_0");
    check(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,requested,1,D3D11_SDK_VERSION,&device,&level,&context),"device");
    std::printf("D3D11_DEVICE level=0x%x startup_ms=%llu\n",unsigned(level),GetTickCount64()-start);
    IDXGIDevice* dxgi=nullptr;IDXGIAdapter* adapter=nullptr;IDXGIFactory* factory=nullptr;
    check(device->QueryInterface(IID_IDXGIDevice,reinterpret_cast<void**>(&dxgi)),"DXGI device");
    check(dxgi->GetAdapter(&adapter),"adapter");DXGI_ADAPTER_DESC adapterDesc{};check(adapter->GetDesc(&adapterDesc),"adapter description");
    char name[256]{};WideCharToMultiByte(CP_UTF8,0,adapterDesc.Description,-1,name,sizeof(name),nullptr,nullptr);
    std::printf("D3D11_ADAPTER %s vendor=%x device=%x\n",name,adapterDesc.VendorId,adapterDesc.DeviceId);
    check(adapter->GetParent(IID_IDXGIFactory,reinterpret_cast<void**>(&factory)),"factory");
    HWND window=nullptr;IDXGISwapChain* swapchain=nullptr;ID3D11Texture2D* target=nullptr;
    if(!offscreen) {
        WNDCLASSW wc{};wc.lpfnWndProc=WindowProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"BrookD3D11Probe";
        if(!RegisterClassW(&wc))return 3;
        RECT rect{0,0,640,360};AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);
        window=CreateWindowW(wc.lpszClassName,L"Brook D3D11 via DXVK",WS_OVERLAPPEDWINDOW|WS_VISIBLE,
             CW_USEDEFAULT,CW_USEDEFAULT,rect.right-rect.left,rect.bottom-rect.top,nullptr,nullptr,wc.hInstance,nullptr);
        if(!window)return 3;
        DXGI_SWAP_CHAIN_DESC swap{};swap.BufferDesc.Width=640;swap.BufferDesc.Height=360;
        swap.BufferDesc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;swap.SampleDesc.Count=1;
        swap.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;swap.BufferCount=2;swap.OutputWindow=window;swap.Windowed=TRUE;
        swap.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
        check(factory->CreateSwapChain(device,&swap,&swapchain),"swapchain");
        check(swapchain->GetBuffer(0,IID_ID3D11Texture2D,reinterpret_cast<void**>(&target)),"backbuffer");
    } else {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=640;desc.Height=360;desc.MipLevels=1;desc.ArraySize=1;
        desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
        check(device->CreateTexture2D(&desc,nullptr,&target),"offscreen target");
    }
    ID3D11RenderTargetView* view=nullptr;check(device->CreateRenderTargetView(target,nullptr,&view),"target view");
    const char* vs="float4 main(uint id:SV_VertexID):SV_Position {float2 p[3]={float2(-0.8,-0.8),float2(0,0.8),float2(0.8,-0.8)};return float4(p[id],0,1);}";
    ID3DBlob *vertex=compile(vs,"vs_4_0"),*pixel=compile("float4 main():SV_Target{return float4(1,0,0,1);}","ps_4_0");
    ID3D11VertexShader* vertexShader=nullptr;ID3D11PixelShader* pixelShader=nullptr;ID3D11RasterizerState* raster=nullptr;
    check(device->CreateVertexShader(vertex->GetBufferPointer(),vertex->GetBufferSize(),nullptr,&vertexShader),"vertex shader");
    check(device->CreatePixelShader(pixel->GetBufferPointer(),pixel->GetBufferSize(),nullptr,&pixelShader),"pixel shader");vertex->Release();pixel->Release();
    D3D11_RASTERIZER_DESC rasterDesc{};rasterDesc.FillMode=D3D11_FILL_SOLID;rasterDesc.CullMode=D3D11_CULL_NONE;rasterDesc.DepthClipEnable=TRUE;
    check(device->CreateRasterizerState(&rasterDesc,&raster),"rasterizer");context->RSSetState(raster);
    D3D11_VIEWPORT viewport{0,0,640,360,0,1};context->RSSetViewports(1,&viewport);
    context->VSSetShader(vertexShader,nullptr,0);context->PSSetShader(pixelShader,nullptr,0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float blue[4]={0,0,1,1};unsigned rendered=0;bool quit=false,verifyFrame=true;ULONGLONG renderStart=GetTickCount64();
    for(;rendered<(offscreen?1:frames) && !quit;) {
        MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {if(msg.message==WM_QUIT)quit=true;TranslateMessage(&msg);DispatchMessageW(&msg);}
        if(quit)break;
        if(swapchain && resizePending) {
            context->OMSetRenderTargets(0,nullptr,nullptr);view->Release();target->Release();
            check(swapchain->ResizeBuffers(0,clientWidth,clientHeight,DXGI_FORMAT_UNKNOWN,0),"resize buffers");
            check(swapchain->GetBuffer(0,IID_ID3D11Texture2D,reinterpret_cast<void**>(&target)),"resized backbuffer");
            check(device->CreateRenderTargetView(target,nullptr,&view),"resized view");
            viewport.Width=float(clientWidth);viewport.Height=float(clientHeight);context->RSSetViewports(1,&viewport);
            resizePending=false;verifyFrame=true;std::printf("D3D11_RESIZE %ux%u\n",clientWidth,clientHeight);
        }
        if(window && IsIconic(window)) {Sleep(10);continue;}
        context->OMSetRenderTargets(1,&view,nullptr);context->ClearRenderTargetView(view,blue);context->Draw(3,0);
        if(verifyFrame) {readback(device,context,target);verifyFrame=false;}
        if(swapchain)check(swapchain->Present(1,0),"present");
        ++rendered;
    }
    std::puts("D3D11_CLEANUP clear-state");context->ClearState();context->Flush();
    std::puts("D3D11_CLEANUP target");view->Release();target->Release();
    std::puts("D3D11_CLEANUP shaders");raster->Release();pixelShader->Release();vertexShader->Release();
    if(swapchain)swapchain->Release();
    if(window && IsWindow(window))DestroyWindow(window);
    std::puts("D3D11_CLEANUP factory");factory->Release();adapter->Release();dxgi->Release();
    std::puts("D3D11_CLEANUP context");context->Release();
    std::puts("D3D11_CLEANUP device");device->Release();
    std::puts("D3D11_CLEANUP complete");
    std::printf("{\"passed\":true,\"frames\":%u,\"offscreen\":%s,\"keys\":%u,\"size_events\":%u,\"render_ms\":%llu,\"total_ms\":%llu}\n",rendered,offscreen?"true":"false",keys,resizes,GetTickCount64()-renderStart,GetTickCount64()-start);
}
