#include <Windows.h>

#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include "d3dx12.h"

#include <wrl.h>
#include <process.h>
#include <stdexcept>
#include <iostream>

#define InterlockedGetValue(object) InterlockedCompareExchange(object, 0, 0)

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// -----------------------------------------------------------------------------------------------------

#define SINGLETHREADED TRUE

const UINT FrameCount = 3;

const UINT NumContexts = 1;


const int CommandListCount = 3;
const int CommandListPre = 0;
const int CommandListMid = 1;
const int CommandListPost = 2;

// -----------------------------------------------------------------------------------------------------

inline std::string HrToString(HRESULT hr)
{
    char str[64] = {};
    sprintf_s(str, "HRESULT: 0x%08X", static_cast<UINT>(hr));
    return std::string(str);
}

class HrException : public std::runtime_error
{
public:
    HrException(HRESULT hr) : std::runtime_error(HrToString(hr)), hr(hr) {}
    HRESULT Error() const { return hr; }
private:
    const HRESULT hr;
};

inline void ThrowIfFailed(HRESULT hr)
{
    if (FAILED(hr))
    {
        throw HrException(hr);
    }
}

// -----------------------------------------------------------------------------------------------------

struct Vertex
{
    XMFLOAT3 position;
    XMFLOAT4 color;
};

const UINT MaxVertexCount = 10000000;
const UINT MaxModelCount = 10000;

// -----------------------------------------------------------------------------------------------------

Vertex verticesList[] =
{
    // Cubo: face frontal
    { { -0.5f, 0.5f, -0.5f }, {1.0f, 0.0f, 0.0f, 1.0f} },
    { { 0.5f, 0.5f, -0.5f }, {0.0f, 1.0f, 0.0f, 1.0f} },
    { { -0.5f, -0.5f, -0.5f }, {0.0f, 0.0f, 1.0f, 1.0f} },
    { { 0.5f, -0.5f, -0.5f }, {1.0f, 0.0f, 0.0f, 1.0f} },

    // Cubo: face traseira
    { { 0.5f, 0.5f, 0.5f }, {1.0f, 0.0f, 0.0f, 1.0f} },
    { { -0.5f, 0.5f, 0.5f }, {0.0f, 1.0f, 0.0f, 1.0f} },
    { { 0.5f, -0.5f, 0.5f }, {0.0f, 0.0f, 1.0f, 1.0f} },
    { { -0.5f, -0.5f, 0.5f }, {1.0f, 0.0f, 0.0f, 1.0f} },

    // Pirâmide
    { { 0.0f, 1.0f, 0.0f }, {1.0f, 0.0f, 0.0f, 1.0f} },
    { { 0.0f, -0.5f, -0.5f }, {0.0f, 1.0f, 0.0f, 1.0f} },
    { { 0.8f, -0.5f, 0.7f }, {0.0f, 0.0f, 1.0f, 1.0f} },
    { { -0.8f, -0.5f, 0.7f }, {1.0f, 0.0f, 0.0f, 1.0f} }
};
const UINT vertexBufferSize = sizeof(verticesList);

UINT indicesList[] = { 
    // Cubo
    0,1,2, 2,1,3,  // face frontal 
    3,1,4, 3,4,6,  // face direita
    0,4,1, 0,5,4,  // face superior
    0,2,5, 2,7,5,  // face esquerda
    4,5,6, 5,7,6,  // face traseira
    2,3,6, 2,6,7,  // face inferior

    // Pirâmide
    0,2,1,
    0,1,3,
    0,3,2,
    1,2,3,
};
const UINT indexBufferSize = sizeof(indicesList);

struct SceneConstantBuffer
{
    XMFLOAT4X4 model;
    XMFLOAT4X4 view;
    XMFLOAT4X4 projection;
    XMFLOAT4 padding[4]; // Alinhamento 256-byte.
};

// -----------------------------------------------------------------------------------------------------

struct WindowInfo
{
    UINT width;
    UINT height;
    float aspectRatio;

    HWND hwnd;
    std::wstring title;
};

struct Camera
{
    struct KeysPressed
    {
        bool w;
        bool a;
        bool s;
        bool d;

        bool rightButton;
    };

    bool mouseMoved;
    //POINT cursorPos;
    XMFLOAT2 lookCurrentPoint;
    XMFLOAT2 lookLastPoint;
    float rotationGain;

    XMFLOAT3 position;
    float pitch;
    float yaw;
    float roll;
    float fov;
    float moveSpeed;

    KeysPressed keysPressed;
};

struct Timer
{
    static const UINT64 TicksPerSecond = 10000000;

    LARGE_INTEGER qpcFrequency;
    LARGE_INTEGER qpcLastTime;
    UINT64 qpcMaxDelta;

    UINT64 elapsedTicks;
    UINT64 totalTicks;
    UINT64 leftOverTicks;

    UINT32 frameCount;
    UINT32 framesPerSecond;
    UINT32 framesThisSecond;
    UINT64 qpcSecondCounter;

    bool isFixedTimeStep;
    UINT64 targetElapsedTicks;
};

struct FrameResource
{
    ID3D12CommandList* batchSubmit[NumContexts * 1 + CommandListCount];

    ComPtr<ID3D12CommandAllocator> commandAllocators[CommandListCount];
    ComPtr<ID3D12GraphicsCommandList> commandLists[CommandListCount];

    ComPtr<ID3D12CommandAllocator> sceneCommandAllocators[NumContexts];
    ComPtr<ID3D12GraphicsCommandList> sceneCommandLists[NumContexts];

    UINT64 fenceValue;


    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12PipelineState> pipelineStateShadowMap;
    ComPtr<ID3D12Resource> sceneConstantBuffer;
    SceneConstantBuffer* sceneConstantBufferWO;
};

struct D3D12Core
{
    WindowInfo windowInfo;

    
    CD3DX12_VIEWPORT viewport;
    CD3DX12_RECT scissorRect;
    ComPtr<IDXGISwapChain3> swapChain;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Resource> renderTargets[FrameCount];
    ComPtr<ID3D12Resource> depthStencil;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12CommandSignature> commandSignature;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    ComPtr<ID3D12DescriptorHeap> cbvSrvHeap;
    ComPtr<ID3D12PipelineState> pipelineState;


    D3D12_VERTEX_BUFFER_VIEW vertexBufferView;
    D3D12_INDEX_BUFFER_VIEW indexBufferView;
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    UINT rtvDescriptorSize;
    Timer timer;
    Camera camera;

    
    HANDLE swapChainEvent;
    HANDLE workerBeginRenderFrame[NumContexts];
    HANDLE workerFinishShadowPass[NumContexts];
    HANDLE workerFinishedRenderFrame[NumContexts];
    HANDLE threadHandles[NumContexts];
    UINT frameIndex;
    UINT frameCounter;
    HANDLE fenceEvent;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue;


    
    static D3D12Core* app;

    
    FrameResource* frameResources[FrameCount];
    FrameResource* currentFrameResource;
    int currentFrameResourceIndex;

    struct ThreadParameter
    {
        int threadIndex;
    };
    ThreadParameter threadParameters[NumContexts];
};

// -----------------------------------------------------------------------------------------------------

void InitWindowInfo(UINT width, UINT height, std::wstring title, WindowInfo* windowInfo)
{
    windowInfo->width = width;
    windowInfo->height = height;
    windowInfo->title = title;
    windowInfo->aspectRatio = static_cast<float>(width) / static_cast<float>(height);
}

void InitCamera(Camera* camera)
{
    camera->position = XMFLOAT3(0, 0, -3.0f);
    camera->pitch = 0.0f;
    camera->yaw = 0.0f;
    camera->roll = 0.0f;
    camera->fov = 60.0f;
    camera->moveSpeed = 2.0f;
    camera->keysPressed = {};
    camera->mouseMoved = false;
    camera->rotationGain = 0.4f;
}

void InitTimer(Timer* timer)
{
    timer->elapsedTicks = 0;
    timer->totalTicks = 0;
    timer->leftOverTicks = 0;
    timer->frameCount = 0;
    timer->framesPerSecond = 0;
    timer->framesThisSecond = 0;
    timer->qpcSecondCounter = 0;
    timer->isFixedTimeStep = false;
    timer->targetElapsedTicks = timer->TicksPerSecond / 60;

    QueryPerformanceFrequency(&timer->qpcFrequency);
    QueryPerformanceCounter(&timer->qpcLastTime);

    timer->qpcMaxDelta = timer->qpcFrequency.QuadPart / 10;
}

void InitFrameResource(D3D12Core* d3d12Core, UINT frameResourceIndex, FrameResource* frameResource)
{
    frameResource->fenceValue = 0;
    frameResource->pipelineState = d3d12Core->pipelineState;

    for (UINT i = 0; i < CommandListCount; i++)
    {
        ThrowIfFailed(d3d12Core->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frameResource->commandAllocators[i])));
        ThrowIfFailed(d3d12Core->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frameResource->commandAllocators[i].Get(), frameResource->pipelineState.Get(), IID_PPV_ARGS(&frameResource->commandLists[i])));

        
        ThrowIfFailed(frameResource->commandLists[i]->Close());
    }

    for (UINT i = 0; i < NumContexts; i++)
    {
        ThrowIfFailed(d3d12Core->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frameResource->sceneCommandAllocators[i])));


        ThrowIfFailed(d3d12Core->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frameResource->sceneCommandAllocators[i].Get(), frameResource->pipelineState.Get(), IID_PPV_ARGS(&frameResource->sceneCommandLists[i])));
        

        ThrowIfFailed(frameResource->sceneCommandLists[i]->Close());
    }

    const UINT cbvSrvDescriptorSize = d3d12Core->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    CD3DX12_CPU_DESCRIPTOR_HANDLE cbvSrvCpuHandle(d3d12Core->cbvSrvHeap->GetCPUDescriptorHandleForHeapStart());
    cbvSrvCpuHandle.Offset(0 + 0 + frameResourceIndex * MaxModelCount, cbvSrvDescriptorSize);

    ThrowIfFailed(d3d12Core->device->CreateCommittedResource(
        &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
        D3D12_HEAP_FLAG_NONE,
        &CD3DX12_RESOURCE_DESC::Buffer(sizeof(SceneConstantBuffer) * MaxModelCount),
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&frameResource->sceneConstantBuffer)));

    UINT64 cbOffset = 0;
    for(UINT i = 0; i < MaxModelCount; i++)
    {
        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.SizeInBytes = sizeof(SceneConstantBuffer);
        cbvDesc.BufferLocation = frameResource->sceneConstantBuffer->GetGPUVirtualAddress() + cbOffset;
        cbOffset += cbvDesc.SizeInBytes;

        d3d12Core->device->CreateConstantBufferView(&cbvDesc, cbvSrvCpuHandle);
        cbvSrvCpuHandle.Offset(cbvSrvDescriptorSize);
    }

    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(frameResource->sceneConstantBuffer->Map(0, &readRange, reinterpret_cast<void**>(&frameResource->sceneConstantBufferWO)));


    const UINT batchSize = _countof(frameResource->sceneCommandLists) + 3;
    frameResource->batchSubmit[0] = frameResource->commandLists[CommandListPre].Get();
    frameResource->batchSubmit[1] = frameResource->commandLists[CommandListMid].Get();
    memcpy(frameResource->batchSubmit + 2, frameResource->sceneCommandLists, _countof(frameResource->sceneCommandLists) * sizeof(ID3D12CommandList*));
    frameResource->batchSubmit[batchSize - 1] = frameResource->commandLists[CommandListPost].Get();
}

D3D12Core* D3D12Core::app = nullptr;
void InitD3D12Core(UINT width, UINT height, std::wstring title, D3D12Core* d3d12Core)
{
    InitWindowInfo(width, height, title, &d3d12Core->windowInfo);
    InitCamera(&d3d12Core->camera);
    InitTimer(&d3d12Core->timer);

    d3d12Core->frameIndex = 0;
    d3d12Core->viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    d3d12Core->scissorRect = CD3DX12_RECT(0, 0, static_cast<LONG>(width), static_cast<LONG>(height));
    d3d12Core->fenceValue = 0;
    d3d12Core->frameCounter = 0;
    d3d12Core->rtvDescriptorSize = 0;
    d3d12Core->currentFrameResourceIndex = 0;
    d3d12Core->currentFrameResource = nullptr;
    d3d12Core->app = d3d12Core;
}

// -----------------------------------------------------------------------------------------------------

void DestroyFrameResource(FrameResource* frameResource)
{
    for (int i = 0; i < CommandListCount; i++)
    {
        frameResource->commandAllocators[i] = nullptr;
        frameResource->commandLists[i] = nullptr;
    }

    for (int i = 0; i < NumContexts; i++)
    {
        frameResource->sceneCommandLists[i] = nullptr;
        frameResource->sceneCommandAllocators[i] = nullptr;
    }
}

// -----------------------------------------------------------------------------------------------------

typedef void(*LPUPDATEFUNC) (void);

void Tick(Timer* timer, LPUPDATEFUNC update = nullptr)
{
    LARGE_INTEGER currentTime;

    QueryPerformanceCounter(&currentTime);

    UINT64 timeDelta = currentTime.QuadPart - timer->qpcLastTime.QuadPart;

    timer->qpcLastTime = currentTime;
    timer->qpcSecondCounter += timeDelta;

    if (timeDelta > timer->qpcMaxDelta)
    {
        timeDelta = timer->qpcMaxDelta;
    }

    timeDelta *= timer->TicksPerSecond;
    timeDelta /= timer->qpcFrequency.QuadPart;

    UINT32 lastFrameCount = timer->frameCount;

    if (timer->isFixedTimeStep)
    {
        if (abs(static_cast<int>(timeDelta - timer->targetElapsedTicks)) < timer->TicksPerSecond / 4000)
        {
            timeDelta = timer->targetElapsedTicks;
        }

        timer->leftOverTicks += timeDelta;

        while (timer->leftOverTicks >= timer->targetElapsedTicks)
        {
            timer->elapsedTicks = timer->targetElapsedTicks;
            timer->totalTicks += timer->targetElapsedTicks;
            timer->leftOverTicks -= timer->targetElapsedTicks;
            timer->frameCount++;

            if (update)
            {
                update();
            }
        }
    }
    else
    {
        timer->elapsedTicks = timeDelta;
        timer->totalTicks += timeDelta;
        timer->leftOverTicks = 0;
        timer->frameCount++;

        if (update)
        {
            update();
        }
    }

    if (timer->frameCount != lastFrameCount)
    {
        timer->framesThisSecond++;
    }

    if (timer->qpcSecondCounter >= static_cast<UINT64>(timer->qpcFrequency.QuadPart))
    {
        timer->framesPerSecond = timer->framesThisSecond;
        timer->framesThisSecond = 0;
        timer->qpcSecondCounter %= timer->qpcFrequency.QuadPart;
    }
}

double TicksToSeconds(Timer* timer, UINT64 ticks) { return static_cast<double>(ticks) / timer->TicksPerSecond; }

// -----------------------------------------------------------------------------------------------------

void GetHardwareAdapter(IDXGIFactory2* pFactory, IDXGIAdapter1** ppAdapter)
{
    ComPtr<IDXGIAdapter1> adapter;
    *ppAdapter = nullptr;

    for (UINT adapterIndex = 0; DXGI_ERROR_NOT_FOUND != pFactory->EnumAdapters1(adapterIndex, &adapter); ++adapterIndex)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            continue;

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, _uuidof(ID3D12Device), nullptr)))
            break;
    }

    *ppAdapter = adapter.Detach();
}

// ----------------------------------------------------------
void UpdateCamera(Camera* camera, float elapsedSeconds)
{
    XMFLOAT3 move(0, 0, 0);

    if (camera->keysPressed.a)
        move.x -= 1.0f;
    if (camera->keysPressed.d)
        move.x += 1.0f;
    if (camera->keysPressed.w)
        move.z += 1.0f;
    if (camera->keysPressed.s)
        move.z -= 1.0f;

    XMVECTOR vector = XMVector3Normalize(XMLoadFloat3(&move));
    move.x = XMVectorGetX(vector);
    move.z = XMVectorGetZ(vector);

    float moveInterval = camera->moveSpeed * elapsedSeconds;

    XMMATRIX cameraRotation = XMMatrixRotationRollPitchYaw((camera->pitch * XM_PI) / 180.0f, (camera->yaw * XM_PI) / 180.0f, (camera->roll * XM_PI) / 180.0f);

    float x = move.x * (cameraRotation.r[0]).m128_f32[0] + move.z * (cameraRotation.r[2]).m128_f32[0];
    float y = move.x * (cameraRotation.r[0]).m128_f32[1] + move.z * (cameraRotation.r[2]).m128_f32[1];
    float z = move.x * (cameraRotation.r[0]).m128_f32[2] + move.z * (cameraRotation.r[2]).m128_f32[2];

    camera->position.x += x * moveInterval;
    camera->position.y += y * moveInterval;
    camera->position.z += z * moveInterval;

    if (camera->mouseMoved)
    {
        camera->mouseMoved = false;

        if (camera->keysPressed.rightButton)
        {
            XMFLOAT2 pointerDelta;
            pointerDelta.x = camera->lookCurrentPoint.x - camera->lookLastPoint.x;
            pointerDelta.y = camera->lookCurrentPoint.y - camera->lookLastPoint.y;

            //pointerDelta.x = camera->cursorPos.x - windowInfo.windowCenter.x;
            //pointerDelta.y = camera->cursorPos.y - windowInfo.windowCenter.y;

            XMFLOAT2 rotationDelta;
            rotationDelta.x = pointerDelta.x * camera->rotationGain;
            rotationDelta.y = pointerDelta.y * camera->rotationGain;

            {
                if (camera->yaw + rotationDelta.x >= 360.0f)
                    camera->yaw += rotationDelta.x - 360.0f;
                else if (camera->yaw + rotationDelta.x < 0.0f)
                    camera->yaw += rotationDelta.x + 360.0f;
                else
                    camera->yaw += rotationDelta.x;
            }

            {
                if (camera->pitch + rotationDelta.y > 90.0f)
                    camera->pitch = 90.0f;
                else if (camera->pitch + rotationDelta.y < -90.0f)
                    camera->pitch = -90.0f;
                else
                    camera->pitch += rotationDelta.y;
            }

            //ClientToScreen(windowInfo.hWnd, &windowInfo.windowCenter);
            //SetCursorPos(windowInfo.windowCenter.x, windowInfo.windowCenter.y);
        }

        camera->lookLastPoint.x = camera->lookCurrentPoint.x;
        camera->lookLastPoint.y = camera->lookCurrentPoint.y;
    }
}
XMFLOAT3 GetLookDirection(float pitch_deg, float yaw_deg)
{
    XMFLOAT3 lookDirection;
    float pitch_rad, yaw_rad;

    pitch_rad = XM_PI * pitch_deg / 180.0f;
    yaw_rad = XM_PI * yaw_deg / 180.0f;

    lookDirection.x = cosf(pitch_rad) * sinf(yaw_rad);
    lookDirection.y = -sinf(pitch_rad);
    lookDirection.z = cosf(pitch_rad) * cosf(yaw_rad);

    return lookDirection;
}

XMFLOAT3 GetUpDirection(float pitch_deg, float yaw_deg, float roll_deg)
{
    XMFLOAT3 upDirection;
    float pitch_rad, yaw_rad, roll_rad;

    pitch_rad = XM_PI * pitch_deg / 180.0f;
    yaw_rad = XM_PI * yaw_deg / 180.0f;
    roll_rad = XM_PI * roll_deg / 180.0f;

    upDirection.x = cosf(roll_rad) * sinf(pitch_rad) * sinf(yaw_rad) - sinf(roll_rad) * cosf(yaw_rad);
    upDirection.y = cosf(roll_rad) * cosf(pitch_rad);
    upDirection.z = sinf(roll_rad) * sinf(yaw_rad) + cosf(roll_rad) * sinf(pitch_rad) * cosf(yaw_rad);

    return upDirection;
}

XMMATRIX GetViewMatrix(XMFLOAT3 position, float pitch_deg, float yaw_deg, float roll_deg)
{
    XMFLOAT3 lookDirection, upDirection;

    lookDirection = GetLookDirection(pitch_deg, yaw_deg);
    upDirection = GetUpDirection(pitch_deg, yaw_deg, roll_deg);

    return XMMatrixLookToLH(XMLoadFloat3(&position), XMLoadFloat3(&lookDirection), XMLoadFloat3(&upDirection));
}

XMMATRIX GetPerspectiveProjectionMatrix(float fov_deg, float aspectRatio, float nearPlane = 1.0f, float farPlane = 1000.0f)
{
    return XMMatrixPerspectiveFovLH(fov_deg * XM_PI / 180.0f, aspectRatio, nearPlane, farPlane);
}

XMMATRIX GetOrthographicProjectionMatrix(float screenWidth, float screenHeight, float nearPlane = 0.0f, float farPlane = 1000.0f)
{
    return XMMatrixOrthographicLH(screenWidth, screenHeight, nearPlane, farPlane);
}

void WriteConstantBuffers(FrameResource* frameResource, Camera* camera, D3D12_VIEWPORT* viewport)
{
    //if (frameResource->modelCount == 0)
        //return;

    XMMATRIX model, view, projection;
    SceneConstantBuffer* sceneConsts = new SceneConstantBuffer[2];

    view = GetViewMatrix(camera->position, camera->pitch, camera->yaw, camera->roll);
    projection = GetPerspectiveProjectionMatrix(camera->fov, viewport->Width / viewport->Height);
    //projection = GetOrthographicProjectionMatrix(viewport->Width, viewport->Height);


    for (UINT i = 0; i < 2; i++)
    {
        XMStoreFloat4x4(&sceneConsts[i].view, view);
        XMStoreFloat4x4(&sceneConsts[i].projection, projection);
    }

    model = XMMatrixTranslation(-1.5f, 0.0f, 0.0f);
    XMStoreFloat4x4(&sceneConsts[0].model, model);

    model = XMMatrixTranslation(1.5f, 0.0f, 0.0f); //XMMatrixRotationRollPitchYaw(0.0f * XM_PI / 180.0f, 0.0f * XM_PI / 180.0f, 0.0f) * XMMatrixTranslation(0.4f, 0.0f, 1.0f);
    XMStoreFloat4x4(&sceneConsts[1].model, model);

    memcpy(frameResource->sceneConstantBufferWO, sceneConsts, sizeof(SceneConstantBuffer) * 2);

    delete[] sceneConsts;
}

void OnKeyDown(Camera* camera, WPARAM key)
{
    switch (key)
    {
    case 'W':
        camera->keysPressed.w = true;
        break;
    case 'A':
        camera->keysPressed.a = true;
        break;
    case 'S':
        camera->keysPressed.s = true;
        break;
    case 'D':
        camera->keysPressed.d = true;
        break;
    }
}

void OnKeyUp(Camera* camera, WPARAM key)
{
    switch (key)
    {
    case 'W':
        camera->keysPressed.w = false;
        break;
    case 'A':
        camera->keysPressed.a = false;
        break;
    case 'S':
        camera->keysPressed.s = false;
        break;
    case 'D':
        camera->keysPressed.d = false;
        break;
    }
}

void OnMouseMove(Camera* camera, UINT x, UINT y)
{
    camera->mouseMoved = true;
    camera->lookCurrentPoint.x = x;
    camera->lookCurrentPoint.y = y;
}

void OnRightButtonDown(Camera* camera)
{
    camera->keysPressed.rightButton = true;
}

void OnRightButtonUp(Camera* camera)
{
    camera->keysPressed.rightButton = false;
}

// -----------------------------------------------------------------------------------------------------
void SetCommonPipelineState(D3D12Core* d3d12Core, ID3D12GraphicsCommandList* commandList)
{
    commandList->SetGraphicsRootSignature(d3d12Core->rootSignature.Get());


    commandList->RSSetViewports(1, &d3d12Core->viewport);
    commandList->RSSetScissorRects(1, &d3d12Core->scissorRect);

    commandList->OMSetStencilRef(0);
}

void Bind(FrameResource* frameResource, ID3D12GraphicsCommandList* commandList, BOOL scenePass, D3D12_CPU_DESCRIPTOR_HANDLE* rtvHandle, D3D12_CPU_DESCRIPTOR_HANDLE* dsvHandle)
{
    if (scenePass)
    {
        commandList->OMSetRenderTargets(1, rtvHandle, FALSE, dsvHandle);
    }
    else {  }
}

void BindVertexBuffer(ID3D12GraphicsCommandList* commandList, ID3D12Resource* vertexBuffer, D3D12_VERTEX_BUFFER_VIEW vertexBufferView, BYTE* verticesList, UINT verticesListSize)
{
    UINT8* vertexDataBegin;
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(vertexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&vertexDataBegin)));
    memcpy(vertexDataBegin, verticesList, verticesListSize);
    vertexBuffer->Unmap(0, nullptr);

    commandList->IASetVertexBuffers(0, 1, &vertexBufferView);
}

void BindIndexBuffer(ID3D12GraphicsCommandList* commandList, ID3D12Resource* indexBuffer, D3D12_INDEX_BUFFER_VIEW indexBufferView, BYTE* indicesList, UINT indexListSize)
{
    UINT8* indexDataBegin;
    CD3DX12_RANGE readRange(0, 0);
    ThrowIfFailed(indexBuffer->Map(0, &readRange, reinterpret_cast<void**>(&indexDataBegin)));
    memcpy(indexDataBegin, indicesList, indexListSize);
    indexBuffer->Unmap(0, nullptr);

    commandList->IASetIndexBuffer(&indexBufferView);
}

void WorkerThread(D3D12Core* d3d12Core, int threadIndex)
{
#if !SINGLETHREADED

    while (threadIndex >= 0 && threadIndex < NumContexts)
    {

        WaitForSingleObject(d3d12Core->workerBeginRenderFrame[threadIndex], INFINITE);

#endif
        ID3D12GraphicsCommandList* sceneCommandList = d3d12Core->currentFrameResource->sceneCommandLists[threadIndex].Get();

#if !SINGLETHREADED
        
        SetEvent(d3d12Core->workerFinishShadowPass[threadIndex]);
#endif
        
        SetCommonPipelineState(d3d12Core, sceneCommandList);

        ID3D12DescriptorHeap* ppHeaps[] = { d3d12Core->cbvSrvHeap.Get() };
        sceneCommandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

        const UINT cbvSrvDescriptorSize = d3d12Core->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        

        CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(d3d12Core->rtvHeap->GetCPUDescriptorHandleForHeapStart(), d3d12Core->frameIndex, d3d12Core->rtvDescriptorSize);
        CD3DX12_CPU_DESCRIPTOR_HANDLE dsvHandle(d3d12Core->dsvHeap->GetCPUDescriptorHandleForHeapStart());
        Bind(d3d12Core->currentFrameResource, sceneCommandList, TRUE, &rtvHandle, &dsvHandle);
        BindVertexBuffer(sceneCommandList, d3d12Core->vertexBuffer.Get(), d3d12Core->vertexBufferView, (BYTE*)verticesList, vertexBufferSize);
        BindIndexBuffer(sceneCommandList, d3d12Core->indexBuffer.Get(), d3d12Core->indexBufferView, (BYTE*)indicesList, indexBufferSize);
        sceneCommandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        
        
        CD3DX12_GPU_DESCRIPTOR_HANDLE cbvSrvGpuHandle(d3d12Core->cbvSrvHeap->GetGPUDescriptorHandleForHeapStart(), d3d12Core->currentFrameResourceIndex * MaxModelCount, cbvSrvDescriptorSize);
        sceneCommandList->SetGraphicsRootDescriptorTable(0, cbvSrvGpuHandle);
        sceneCommandList->DrawIndexedInstanced(36, 1, 0, 0, 0);

        cbvSrvGpuHandle.Offset(cbvSrvDescriptorSize);
        sceneCommandList->SetGraphicsRootDescriptorTable(0, cbvSrvGpuHandle);
        sceneCommandList->DrawIndexedInstanced(12, 1, 36, 8, 0);
        


        ThrowIfFailed(sceneCommandList->Close());

#if !SINGLETHREADED
        
        SetEvent(d3d12Core->workerFinishedRenderFrame[threadIndex]);
    }
#endif
}

// -----------------------------------------------------------------------------------------------------

void LoadPipeline(D3D12Core* d3d12Core)
{
    UINT dxgiFactoryFlags = 0;

#if defined(_DEBUG)
    
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();

            
            dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
        }
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)));

    ComPtr<IDXGIAdapter1> hardwareAdapter;
    GetHardwareAdapter(factory.Get(), &hardwareAdapter);

    ThrowIfFailed(D3D12CreateDevice(
        hardwareAdapter.Get(),
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&d3d12Core->device)
    ));


    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;

    ThrowIfFailed(d3d12Core->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&d3d12Core->commandQueue)));


    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FrameCount;
    swapChainDesc.Width = d3d12Core->windowInfo.width;
    swapChainDesc.Height = d3d12Core->windowInfo.height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(
        d3d12Core->commandQueue.Get(),
        d3d12Core->windowInfo.hwnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain
    ));


    ThrowIfFailed(factory->MakeWindowAssociation(d3d12Core->windowInfo.hwnd, DXGI_MWA_NO_ALT_ENTER));

    ThrowIfFailed(swapChain.As(&d3d12Core->swapChain));
    d3d12Core->frameIndex = d3d12Core->swapChain->GetCurrentBackBufferIndex();
    d3d12Core->swapChainEvent = d3d12Core->swapChain->GetFrameLatencyWaitableObject();

    // Cria os heaps de descritores.
    {

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = FrameCount;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(d3d12Core->device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&d3d12Core->rtvHeap)));


        D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
        dsvHeapDesc.NumDescriptors = 1 + FrameCount * 1;
        dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ThrowIfFailed(d3d12Core->device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&d3d12Core->dsvHeap)));

        D3D12_DESCRIPTOR_HEAP_DESC cbvHeapDesc = {};
        UINT cbvCount = MaxModelCount * FrameCount;
        cbvHeapDesc.NumDescriptors = cbvCount;
        cbvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        cbvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        ThrowIfFailed(d3d12Core->device->CreateDescriptorHeap(&cbvHeapDesc, IID_PPV_ARGS(&d3d12Core->cbvSrvHeap)));

        d3d12Core->rtvDescriptorSize = d3d12Core->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    }
}

void LoadAssets(D3D12Core* d3d12Core)
{
    D3D12_FEATURE_DATA_ROOT_SIGNATURE featureData = {};

    
    featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;

    if (FAILED(d3d12Core->device->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &featureData, sizeof(featureData))))
    {
        featureData.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
    }

    
    {
        CD3DX12_DESCRIPTOR_RANGE1 ranges[1];
        ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC);
       
        CD3DX12_ROOT_PARAMETER1 rootParameters[1];
        rootParameters[0].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_ALL);

        CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
        rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        ComPtr<ID3DBlob> signature;
        ComPtr<ID3DBlob> error;
        ThrowIfFailed(D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, featureData.HighestVersion, &signature, &error));
        ThrowIfFailed(d3d12Core->device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(&d3d12Core->rootSignature)));
    }

    
    {
        ComPtr<ID3DBlob> vertexShader;
        ComPtr<ID3DBlob> pixelShader;

#if defined(_DEBUG)
        UINT compileFlags = D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        UINT compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ThrowIfFailed(D3DCompileFromFile(L"shaders.hlsl", nullptr, nullptr, "VSMain", "vs_5_0", compileFlags, 0, &vertexShader, nullptr));
        ThrowIfFailed(D3DCompileFromFile(L"shaders.hlsl", nullptr, nullptr, "PSMain", "ps_5_0", compileFlags, 0, &pixelShader, nullptr));

        const D3D12_INPUT_ELEMENT_DESC StandardVertexDescription[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        CD3DX12_DEPTH_STENCIL_DESC depthStencilDesc(D3D12_DEFAULT);
        depthStencilDesc.DepthEnable = true;
        depthStencilDesc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        depthStencilDesc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        depthStencilDesc.StencilEnable = FALSE;

        
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { StandardVertexDescription,_countof(StandardVertexDescription) };
        psoDesc.pRootSignature = d3d12Core->rootSignature.Get();
        psoDesc.VS = CD3DX12_SHADER_BYTECODE(vertexShader.Get());
        psoDesc.PS = CD3DX12_SHADER_BYTECODE(pixelShader.Get());
        psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
        psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
        psoDesc.DepthStencilState = depthStencilDesc;
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc.Count = 1;

        ThrowIfFailed(d3d12Core->device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&d3d12Core->pipelineState)));
    }

    
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ThrowIfFailed(d3d12Core->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocator)));

    
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ThrowIfFailed(d3d12Core->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator.Get(), d3d12Core->pipelineState.Get(), IID_PPV_ARGS(&commandList)));


    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(d3d12Core->rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT i = 0; i < FrameCount; i++)
    {
        ThrowIfFailed(d3d12Core->swapChain->GetBuffer(i, IID_PPV_ARGS(&d3d12Core->renderTargets[i])));
        d3d12Core->device->CreateRenderTargetView(d3d12Core->renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.Offset(1, d3d12Core->rtvDescriptorSize);
    }


    {
        CD3DX12_RESOURCE_DESC shadowTextureDesc(
            D3D12_RESOURCE_DIMENSION_TEXTURE2D,
            0,
            static_cast<UINT>(d3d12Core->viewport.Width),
            static_cast<UINT>(d3d12Core->viewport.Height),
            1,
            1,
            DXGI_FORMAT_D32_FLOAT,
            1,
            0,
            D3D12_TEXTURE_LAYOUT_UNKNOWN,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE);

        D3D12_CLEAR_VALUE clearValue;
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 1.0f;
        clearValue.DepthStencil.Stencil = 0;

        ThrowIfFailed(d3d12Core->device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_NONE,
            &shadowTextureDesc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE,
            &clearValue,
            IID_PPV_ARGS(&d3d12Core->depthStencil)));

        
        d3d12Core->device->CreateDepthStencilView(d3d12Core->depthStencil.Get(), nullptr, d3d12Core->dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    // Cria o vertex buffer.
    {
        
        ThrowIfFailed(d3d12Core->device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(sizeof(Vertex) * MaxVertexCount),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&d3d12Core->vertexBuffer)));

        d3d12Core->vertexBufferView.BufferLocation = d3d12Core->vertexBuffer->GetGPUVirtualAddress();
        d3d12Core->vertexBufferView.StrideInBytes = sizeof(Vertex);
        d3d12Core->vertexBufferView.SizeInBytes = sizeof(Vertex) * MaxVertexCount;
        
    }

    // Cria o index buffer.
    {

        ThrowIfFailed(d3d12Core->device->CreateCommittedResource(
            &CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD),
            D3D12_HEAP_FLAG_NONE,
            &CD3DX12_RESOURCE_DESC::Buffer(sizeof(Vertex) * MaxVertexCount),
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&d3d12Core->indexBuffer)));

        d3d12Core->indexBufferView.BufferLocation = d3d12Core->indexBuffer->GetGPUVirtualAddress();
        d3d12Core->indexBufferView.Format = DXGI_FORMAT_R32_UINT;
        d3d12Core->indexBufferView.SizeInBytes = sizeof(Vertex) * MaxVertexCount;
    }

    
    ThrowIfFailed(commandList->Close());
    ID3D12CommandList* ppCommandLists[] = { commandList.Get() };
    d3d12Core->commandQueue->ExecuteCommandLists(_countof(ppCommandLists), ppCommandLists);

    
    for (int i = 0; i < FrameCount; i++)
    {
        d3d12Core->frameResources[i] = new FrameResource;
        InitFrameResource(d3d12Core, i, d3d12Core->frameResources[i]);
        //d3d12Core->frameResources[i]->WriteConstantBuffers(&m_viewport, &m_camera, m_lightCameras, m_lights);
    }


    {
        ThrowIfFailed(d3d12Core->device->CreateFence(d3d12Core->fenceValue, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&d3d12Core->fence)));
        d3d12Core->fenceValue++;

        d3d12Core->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (d3d12Core->fenceEvent == nullptr)
        {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }

        const UINT64 fenceToWaitFor = d3d12Core->fenceValue;
        ThrowIfFailed(d3d12Core->commandQueue->Signal(d3d12Core->fence.Get(), fenceToWaitFor));
        d3d12Core->fenceValue++;


        ThrowIfFailed(d3d12Core->fence->SetEventOnCompletion(fenceToWaitFor, d3d12Core->fenceEvent));
        WaitForSingleObject(d3d12Core->fenceEvent, INFINITE);
    }
}

void LoadContexts(D3D12Core* d3d12Core)
{
#if !SINGLETHREADED
    struct threadwrapper
    {
        static unsigned int WINAPI thunk(LPVOID lpParameter)
        {
            D3D12Core::ThreadParameter* parameter = reinterpret_cast<D3D12Core::ThreadParameter*>(lpParameter);
            WorkerThread(D3D12Core::app, parameter->threadIndex);
            return 0;
        }
    };

    for (int i = 0; i < NumContexts; i++)
    {
        d3d12Core->workerBeginRenderFrame[i] = CreateEvent(
            NULL,
            FALSE,
            FALSE,
            NULL);

        d3d12Core->workerFinishedRenderFrame[i] = CreateEvent(
            NULL,
            FALSE,
            FALSE,
            NULL);

        d3d12Core->workerFinishShadowPass[i] = CreateEvent(
            NULL,
            FALSE,
            FALSE,
            NULL);

        d3d12Core->threadParameters[i].threadIndex = i;

        d3d12Core->threadHandles[i] = reinterpret_cast<HANDLE>(_beginthreadex(
            nullptr,
            0,
            threadwrapper::thunk,
            reinterpret_cast<LPVOID>(&d3d12Core->threadParameters[i]),
            0,
            nullptr));

    }
#endif
}

// -----------------------------------------------------------------------------------------------------

void ResetFrameResource(FrameResource* frameResource)
{
    for (int i = 0; i < CommandListCount; i++)
    {
        ThrowIfFailed(frameResource->commandAllocators[i]->Reset());
        ThrowIfFailed(frameResource->commandLists[i]->Reset(frameResource->commandAllocators[i].Get(), frameResource->pipelineState.Get()));
    }


    for (int i = 0; i < NumContexts; i++)
    {
        ThrowIfFailed(frameResource->sceneCommandAllocators[i]->Reset());
        ThrowIfFailed(frameResource->sceneCommandLists[i]->Reset(frameResource->sceneCommandAllocators[i].Get(), frameResource->pipelineState.Get()));
    }
}

// -----------------------------------------------------------------------------------------------------

void BeginFrame(D3D12Core* d3d12Core)
{
    ResetFrameResource(d3d12Core->currentFrameResource);

    d3d12Core->currentFrameResource->commandLists[CommandListPre]->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(d3d12Core->renderTargets[d3d12Core->frameIndex].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET));


    const float clearColor[] = { 0.2f, 0.2f, 0.2f, 1.0f };
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(d3d12Core->rtvHeap->GetCPUDescriptorHandleForHeapStart(), d3d12Core->frameIndex, d3d12Core->rtvDescriptorSize);
    d3d12Core->currentFrameResource->commandLists[CommandListPre]->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    d3d12Core->currentFrameResource->commandLists[CommandListPre]->ClearDepthStencilView(d3d12Core->dsvHeap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    ThrowIfFailed(d3d12Core->currentFrameResource->commandLists[CommandListPre]->Close());
}

void MidFrame(D3D12Core* d3d12Core)
{
    ThrowIfFailed(d3d12Core->currentFrameResource->commandLists[CommandListMid]->Close());
}

void EndFrame(D3D12Core* d3d12Core)
{
    d3d12Core->currentFrameResource->commandLists[CommandListPost]->ResourceBarrier(1, &CD3DX12_RESOURCE_BARRIER::Transition(d3d12Core->renderTargets[d3d12Core->frameIndex].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT));

    ThrowIfFailed(d3d12Core->currentFrameResource->commandLists[CommandListPost]->Close());
}

// -----------------------------------------------------------------------------------------------------

void OnInit(D3D12Core* d3d12Core)
{
    LoadPipeline(d3d12Core);
    LoadAssets(d3d12Core);
    LoadContexts(d3d12Core);
}

void OnUpdate(D3D12Core* d3d12Core)
{
    WaitForSingleObjectEx(d3d12Core->swapChainEvent, 100, FALSE);

    Tick(&d3d12Core->timer, NULL);

    if (d3d12Core->frameCounter == 100)
    {
        std::cout << "FPS: " << d3d12Core->timer.framesPerSecond << std::endl;
        d3d12Core->frameCounter = 0;
    }

    d3d12Core->frameCounter++;

   
    const UINT64 lastCompletedFence = d3d12Core->fence->GetCompletedValue();

    
    d3d12Core->currentFrameResourceIndex = (d3d12Core->currentFrameResourceIndex + 1) % FrameCount;
    d3d12Core->currentFrameResource = d3d12Core->frameResources[d3d12Core->currentFrameResourceIndex];

  
    if (d3d12Core->currentFrameResource->fenceValue > lastCompletedFence)
    {
        HANDLE eventHandle = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (eventHandle == nullptr)
        {
            ThrowIfFailed(HRESULT_FROM_WIN32(GetLastError()));
        }
        ThrowIfFailed(d3d12Core->fence->SetEventOnCompletion(d3d12Core->currentFrameResource->fenceValue, eventHandle));
        WaitForSingleObject(eventHandle, INFINITE);
        CloseHandle(eventHandle);
    }

    UpdateCamera(&d3d12Core->camera, TicksToSeconds(&d3d12Core->timer, d3d12Core->timer.elapsedTicks));
    WriteConstantBuffers(d3d12Core->currentFrameResource, &d3d12Core->camera, &d3d12Core->viewport);
}

void OnRender(D3D12Core* d3d12Core)
{
    BeginFrame(d3d12Core);

#if SINGLETHREADED
    for (int i = 0; i < NumContexts; i++)
    {
        WorkerThread(d3d12Core, i);
    }
    MidFrame(d3d12Core);
    EndFrame(d3d12Core);

    d3d12Core->commandQueue->ExecuteCommandLists(_countof(d3d12Core->currentFrameResource->batchSubmit), d3d12Core->currentFrameResource->batchSubmit);
#else
    for (int i = 0; i < NumContexts; i++)
    {
        SetEvent(d3d12Core->workerBeginRenderFrame[i]);
    }

    MidFrame(d3d12Core);
    EndFrame(d3d12Core);

    WaitForMultipleObjects(NumContexts, d3d12Core->workerFinishShadowPass, TRUE, INFINITE);

    
    d3d12Core->commandQueue->ExecuteCommandLists(2, d3d12Core->currentFrameResource->batchSubmit); // Submit PRE, MID and shadows.

    WaitForMultipleObjects(NumContexts, d3d12Core->workerFinishedRenderFrame, TRUE, INFINITE);

    
    d3d12Core->commandQueue->ExecuteCommandLists(_countof(d3d12Core->currentFrameResource->batchSubmit) - 2, d3d12Core->currentFrameResource->batchSubmit + 2);

#endif

   
    ThrowIfFailed(d3d12Core->swapChain->Present(1, 0));
    d3d12Core->frameIndex = d3d12Core->swapChain->GetCurrentBackBufferIndex();


    d3d12Core->currentFrameResource->fenceValue = d3d12Core->fenceValue;
    ThrowIfFailed(d3d12Core->commandQueue->Signal(d3d12Core->fence.Get(), d3d12Core->fenceValue));
    d3d12Core->fenceValue++;
}

void OnDestroy(D3D12Core* d3d12Core)
{
    {
        const UINT64 fence = d3d12Core->fenceValue;
        const UINT64 lastCompletedFence = d3d12Core->fence->GetCompletedValue();


        ThrowIfFailed(d3d12Core->commandQueue->Signal(d3d12Core->fence.Get(), d3d12Core->fenceValue));
        d3d12Core->fenceValue++;


        if (lastCompletedFence < fence)
        {
            ThrowIfFailed(d3d12Core->fence->SetEventOnCompletion(fence, d3d12Core->fenceEvent));
            WaitForSingleObject(d3d12Core->fenceEvent, INFINITE);
        }
        CloseHandle(d3d12Core->fenceEvent);
    }

#if !SINGLETHREADED

    for (int i = 0; i < NumContexts; i++)
    {
        CloseHandle(d3d12Core->workerBeginRenderFrame[i]);
        CloseHandle(d3d12Core->workerFinishShadowPass[i]);
        CloseHandle(d3d12Core->workerFinishedRenderFrame[i]);
        CloseHandle(d3d12Core->threadHandles[i]);
    }
#endif

    for (int i = 0; i < _countof(d3d12Core->frameResources); i++)
    {
        DestroyFrameResource(d3d12Core->frameResources[i]);
        delete d3d12Core->frameResources[i];
    }
}

// -----------------------------------------------------------------------------------------------------

LRESULT CALLBACK WindowProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    D3D12Core* d3d12Core = reinterpret_cast<D3D12Core*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));

    switch (message)
    {
    case WM_CREATE:
    {
        // Save the DXSample* passed in to CreateWindow.
        LPCREATESTRUCT pCreateStruct = reinterpret_cast<LPCREATESTRUCT>(lParam);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pCreateStruct->lpCreateParams));
    }
    return 0;

    case WM_KEYDOWN:
        if (d3d12Core)
        {
            OnKeyDown(&d3d12Core->camera, static_cast<UINT8>(wParam));
        }
        return 0;

    case WM_KEYUP:
        if (d3d12Core)
        {
            OnKeyUp(&d3d12Core->camera, static_cast<UINT8>(wParam));
        }
        return 0;

    case WM_PAINT:
        if (d3d12Core)
        {
            OnUpdate(d3d12Core);
            OnRender(d3d12Core);
        }
        return 0;

    case WM_MOUSEMOVE:
        if (d3d12Core /*&& static_cast<UINT8>(wParam) == MK_RBUTTON*/)
        {
            UINT x = LOWORD(lParam);
            UINT y = HIWORD(lParam);
            OnMouseMove(&d3d12Core->camera, x, y);
        }
        return 0;

    case WM_RBUTTONDOWN:
    {
        UINT x = LOWORD(lParam);
        UINT y = HIWORD(lParam);
        OnRightButtonDown(&d3d12Core->camera);
    }
    return 0;

    case WM_RBUTTONUP:
    {
        UINT x = LOWORD(lParam);
        UINT y = HIWORD(lParam);
        OnRightButtonUp(&d3d12Core->camera);
    }
    return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    
    return DefWindowProc(hWnd, message, wParam, lParam);
}

int RunWin32App(D3D12Core* d3d12Core, HINSTANCE hInstance, int nCmdShow)
{
    WNDCLASSEX windowClass = { 0 };
    windowClass.cbSize = sizeof(WNDCLASSEX);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = hInstance;
    windowClass.hCursor = LoadCursor(NULL, IDC_ARROW);
    windowClass.lpszClassName = L"Infinity";
    RegisterClassEx(&windowClass);

    RECT windowRect = { 0, 0, static_cast<LONG>(d3d12Core->windowInfo.width), static_cast<LONG>(d3d12Core->windowInfo.height) };
    AdjustWindowRect(&windowRect, WS_OVERLAPPEDWINDOW, FALSE);

    
    d3d12Core->windowInfo.hwnd = CreateWindow(
        windowClass.lpszClassName,
        d3d12Core->windowInfo.title.c_str(),
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        windowRect.right - windowRect.left,
        windowRect.bottom - windowRect.top,
        nullptr,
        nullptr,
        hInstance,
        d3d12Core);

    
    OnInit(d3d12Core);

    ShowWindow(d3d12Core->windowInfo.hwnd, nCmdShow);

    MSG msg = {};
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    OnDestroy(d3d12Core);

    return static_cast<char>(msg.wParam);
}

// -----------------------------------------------------------------------------------------------------

//_Use_decl_annotations_
//int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
int main()
{
    D3D12Core d3d12Core;
    InitD3D12Core(1280, 720, L"Infinity Engine [DX12]", &d3d12Core);
    //D3D12Multithreading sample(1280, 720, L"D3D12 Multithreading Sample");
    //return Win32Application::Run(&sample, hInstance, nCmdShow);
    return RunWin32App(&d3d12Core, 0, 1);;
}