#pragma once

// Frame grabs for the dev-only automation plugin: the image the device draws to at a frame stage
// (asi_automation_frames.h), read back to the CPU as BGRA and written as PNG (WIC) or raw BGRA by a worker thread.
// Header-only, x86 and x64.
//
// - Direct3D 10/11: IDXGISwapChain::GetBuffer(0) -> (ResolveSubresource when multisampled) -> CopyResource into a
//   staging texture -> Map. Stalls the render thread until the GPU is done, which is fine: the world is frozen while
//   capturing.
// - Direct3D 9: GetRenderTarget(0) -> (StretchRect when multisampled) -> GetRenderTargetData into a system memory
//   surface -> LockRect. Works on the Windows runtime and on DXVK.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <wincodec.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace asi_automation
{
    // One grabbed frame, BGRA, alpha 255
    struct Image
    {
        uint32_t width = 0, height = 0;
        std::vector<uint8_t> bgra;
    };

    namespace capture_detail
    {
        inline float HalfToFloat(uint16_t h)
        {
            uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
            uint32_t bits;
            if (exponent == 0)
            {
                if (mantissa == 0)
                    bits = sign;
                else
                {
                    exponent = 127 - 15 + 1;
                    while (!(mantissa & 0x400))
                    {
                        mantissa <<= 1;
                        --exponent;
                    }
                    bits = sign | (exponent << 23) | ((mantissa & 0x3FF) << 13);
                }
            }
            else if (exponent == 31)
                bits = sign | 0x7F800000u | (mantissa << 13);
            else
                bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
            float f;
            std::memcpy(&f, &bits, 4);
            return f;
        }

        inline uint8_t Unorm(float v)
        {
            if (!(v > 0.0f))
                return 0;
            return v >= 1.0f ? 255 : uint8_t(v * 255.0f + 0.5f);
        }

        // Rows of the source format into BGRA. Kinds: 0 BGRA8, 1 RGBA8, 2 RGB10A2 (DXGI order), 3 A2R10G10B10 (D3D9
        // order), 4 RGBA16F; false for anything else
        inline bool Convert(int kind, const uint8_t* src, uint32_t pitch, uint32_t width, uint32_t height, Image& out)
        {
            out.width = width;
            out.height = height;
            out.bgra.resize(size_t(width) * height * 4);
            for (uint32_t y = 0; y < height; ++y)
            {
                const uint8_t* row = src + size_t(y) * pitch;
                uint8_t* dst = out.bgra.data() + size_t(y) * width * 4;
                for (uint32_t x = 0; x < width; ++x, dst += 4)
                {
                    switch (kind)
                    {
                        case 0:
                            dst[0] = row[x * 4 + 0];
                            dst[1] = row[x * 4 + 1];
                            dst[2] = row[x * 4 + 2];
                            break;
                        case 1:
                            dst[0] = row[x * 4 + 2];
                            dst[1] = row[x * 4 + 1];
                            dst[2] = row[x * 4 + 0];
                            break;
                        case 2:
                        case 3:
                        {
                            uint32_t p;
                            std::memcpy(&p, row + x * 4, 4);
                            uint32_t a = p & 0x3FF, b = (p >> 10) & 0x3FF, c = (p >> 20) & 0x3FF;
                            // DXGI R10G10B10A2: R low bits; D3D9 A2R10G10B10: B low bits
                            uint32_t r = kind == 2 ? a : c, bl = kind == 2 ? c : a;
                            dst[0] = uint8_t(bl >> 2);
                            dst[1] = uint8_t(b >> 2);
                            dst[2] = uint8_t(r >> 2);
                            break;
                        }
                        case 4:
                        {
                            uint16_t h[4];
                            std::memcpy(h, row + x * 8, 8);
                            dst[0] = Unorm(HalfToFloat(h[2]));
                            dst[1] = Unorm(HalfToFloat(h[1]));
                            dst[2] = Unorm(HalfToFloat(h[0]));
                            break;
                        }
                        default:
                            return false;
                    }
                    dst[3] = 255;
                }
            }
            return true;
        }

        inline int KindOf(DXGI_FORMAT format)
        {
            switch (format)
            {
                case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
                    return 0;
                case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                    return 1;
                case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
                    return 2;
                case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS:
                    return 4;
                default:
                    return -1;
            }
        }

        inline int KindOf(D3DFORMAT format)
        {
            switch (format)
            {
                case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: return 0;
                case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8: return 1;
                case D3DFMT_A2R10G10B10: return 3;
                case D3DFMT_A2B10G10R10: return 2;
                case D3DFMT_A16B16G16R16F: return 4;
                default: return -1;
            }
        }

        // A resolve format for a typeless multisampled texture
        inline DXGI_FORMAT Typed(DXGI_FORMAT format)
        {
            switch (format)
            {
                case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
                case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
                case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
                case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
                case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
                default: return format;
            }
        }
    }

    // Direct3D 10/11: the swap chain's back buffer as it is now. error: why not
    inline bool GrabD3D11(IDXGISwapChain* swapChain, Image& out, std::string& error)
    {
        ID3D11Texture2D* backBuffer = nullptr;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) || !backBuffer)
        {
            error = "no Direct3D 11 back buffer";
            return false;
        }
        ID3D11Device* device = nullptr;
        backBuffer->GetDevice(&device);
        ID3D11DeviceContext* context = nullptr;
        device->GetImmediateContext(&context);
        D3D11_TEXTURE2D_DESC desc{};
        backBuffer->GetDesc(&desc);

        bool ok = false;
        int kind = capture_detail::KindOf(desc.Format);
        ID3D11Texture2D* resolved = nullptr;
        ID3D11Texture2D* staging = nullptr;
        ID3D11Texture2D* source = backBuffer;
        if (kind < 0)
            error = "back buffer format " + std::to_string(int(desc.Format)) + " isn't supported";
        else
        {
            if (desc.SampleDesc.Count > 1)
            {
                D3D11_TEXTURE2D_DESC rd = desc;
                rd.SampleDesc = { 1, 0 };
                rd.Usage = D3D11_USAGE_DEFAULT;
                rd.BindFlags = 0;
                rd.CPUAccessFlags = 0;
                rd.MiscFlags = 0;
                rd.ArraySize = 1;
                rd.MipLevels = 1;
                if (SUCCEEDED(device->CreateTexture2D(&rd, nullptr, &resolved)))
                {
                    context->ResolveSubresource(resolved, 0, backBuffer, 0, capture_detail::Typed(desc.Format));
                    source = resolved;
                }
            }
            D3D11_TEXTURE2D_DESC sd = desc;
            sd.SampleDesc = { 1, 0 };
            sd.Usage = D3D11_USAGE_STAGING;
            sd.BindFlags = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags = 0;
            sd.ArraySize = 1;
            sd.MipLevels = 1;
            if (desc.SampleDesc.Count > 1 && !resolved)
                error = "couldn't resolve the multisampled back buffer";
            else if (FAILED(device->CreateTexture2D(&sd, nullptr, &staging)))
                error = "couldn't create a staging texture";
            else
            {
                context->CopyResource(staging, source);
                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
                {
                    ok = capture_detail::Convert(kind, static_cast<const uint8_t*>(mapped.pData), mapped.RowPitch, desc.Width, desc.Height, out);
                    context->Unmap(staging, 0);
                }
                else
                    error = "Map failed";
            }
        }
        if (staging) staging->Release();
        if (resolved) resolved->Release();
        context->Release();
        device->Release();
        backBuffer->Release();
        return ok;
    }

    // Direct3D 9: render target 0 as it is now
    inline bool GrabD3D9(IDirect3DDevice9* device, Image& out, std::string& error)
    {
        IDirect3DSurface9* target = nullptr;
        if (FAILED(device->GetRenderTarget(0, &target)) || !target)
        {
            error = "no Direct3D 9 render target";
            return false;
        }
        D3DSURFACE_DESC desc{};
        target->GetDesc(&desc);
        bool ok = false;
        int kind = capture_detail::KindOf(desc.Format);
        IDirect3DSurface9* plain = nullptr;
        IDirect3DSurface9* system = nullptr;
        IDirect3DSurface9* source = target;
        if (kind < 0)
            error = "render target format " + std::to_string(int(desc.Format)) + " isn't supported";
        else
        {
            if (desc.MultiSampleType != D3DMULTISAMPLE_NONE)
            {
                if (SUCCEEDED(device->CreateRenderTarget(desc.Width, desc.Height, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &plain, nullptr)) &&
                    SUCCEEDED(device->StretchRect(target, nullptr, plain, nullptr, D3DTEXF_NONE)))
                    source = plain;
            }
            if (desc.MultiSampleType != D3DMULTISAMPLE_NONE && source == target)
                error = "couldn't resolve the multisampled render target";
            else if (FAILED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &system, nullptr)))
                error = "couldn't create a system memory surface";
            else if (FAILED(device->GetRenderTargetData(source, system)))
                error = "GetRenderTargetData failed";
            else
            {
                D3DLOCKED_RECT locked{};
                if (SUCCEEDED(system->LockRect(&locked, nullptr, D3DLOCK_READONLY)))
                {
                    ok = capture_detail::Convert(kind, static_cast<const uint8_t*>(locked.pBits), uint32_t(locked.Pitch), desc.Width, desc.Height, out);
                    system->UnlockRect();
                }
                else
                    error = "LockRect failed";
            }
        }
        if (system) system->Release();
        if (plain) plain->Release();
        target->Release();
        return ok;
    }

    // Whatever the stage drew with: a swap chain (Direct3D 10/11) or a Direct3D 9 device
    inline bool Grab(void* object, Image& out, std::string& error)
    {
        if (!object)
        {
            error = "the stage gave no device";
            return false;
        }
        auto unknown = static_cast<IUnknown*>(object);
        IDXGISwapChain* swapChain = nullptr;
        if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&swapChain))) && swapChain)
        {
            bool ok = GrabD3D11(swapChain, out, error);
            swapChain->Release();
            return ok;
        }
        IDirect3DDevice9* device9 = nullptr;
        if (SUCCEEDED(unknown->QueryInterface(__uuidof(IDirect3DDevice9), reinterpret_cast<void**>(&device9))) && device9)
        {
            bool ok = GrabD3D9(device9, out, error);
            device9->Release();
            return ok;
        }
        error = "the stage's device is neither a swap chain nor a Direct3D 9 device";
        return false;
    }

    // PNG through WIC (24-bit, the alpha is dropped). Call with COM initialised on the thread.
    inline bool WritePng(const Image& image, const std::filesystem::path& path, std::string& error)
    {
        IWICImagingFactory* factory = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
        {
            error = "no WIC";
            return false;
        }
        std::vector<uint8_t> bgr(size_t(image.width) * image.height * 3);
        for (size_t i = 0, n = size_t(image.width) * image.height; i < n; ++i)
        {
            bgr[i * 3 + 0] = image.bgra[i * 4 + 0];
            bgr[i * 3 + 1] = image.bgra[i * 4 + 1];
            bgr[i * 3 + 2] = image.bgra[i * 4 + 2];
        }
        bool ok = false;
        IWICStream* stream = nullptr;
        IWICBitmapEncoder* encoder = nullptr;
        IWICBitmapFrameEncode* frame = nullptr;
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
            SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(stream, WICBitmapEncoderNoCache)) && SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) &&
            SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(image.width, image.height)))
        {
            WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
            if (SUCCEEDED(frame->SetPixelFormat(&format)) && format == GUID_WICPixelFormat24bppBGR &&
                SUCCEEDED(frame->WritePixels(image.height, image.width * 3, UINT(bgr.size()), bgr.data())) &&
                SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit()))
                ok = true;
        }
        if (!ok)
            error = "couldn't write " + path.string();
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        factory->Release();
        return ok;
    }

    // Raw BGRA, width * height * 4 bytes, no header (the reply names the size)
    inline bool WriteRaw(const Image& image, const std::filesystem::path& path, std::string& error)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f)
        {
            error = "couldn't write " + path.string();
            return false;
        }
        bool ok = fwrite(image.bgra.data(), 1, image.bgra.size(), f) == image.bgra.size();
        fclose(f);
        if (!ok)
            error = "couldn't write " + path.string();
        return ok;
    }

    // Writes images on its own thread, so the render thread only copies
    class ImageWriter
    {
    public:
        struct Done
        {
            std::filesystem::path path;
            bool ok = false;
            std::string error;
            uint32_t width = 0, height = 0;
        };

        ~ImageWriter()
        {
            {
                std::lock_guard guard(lock);
                stop = true;
            }
            wake.notify_all();
            if (worker.joinable())
                worker.join();
        }

        void Write(Image image, std::filesystem::path path, std::function<void(const Done&)> finished)
        {
            {
                std::lock_guard guard(lock);
                if (!worker.joinable())
                    worker = std::thread([this] { Run(); });
                queue.push_back({ std::move(image), std::move(path), std::move(finished) });
            }
            wake.notify_one();
        }

    private:
        struct Item
        {
            Image image;
            std::filesystem::path path;
            std::function<void(const Done&)> finished;
        };

        void Run()
        {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            for (;;)
            {
                Item item;
                {
                    std::unique_lock guard(lock);
                    wake.wait(guard, [&] { return stop || !queue.empty(); });
                    if (queue.empty())
                        break;
                    item = std::move(queue.front());
                    queue.pop_front();
                }
                Done done;
                done.path = item.path;
                done.width = item.image.width;
                done.height = item.image.height;
                auto ext = item.path.extension().wstring();
                bool raw = _wcsicmp(ext.c_str(), L".bgra") == 0 || _wcsicmp(ext.c_str(), L".raw") == 0;
                done.ok = raw ? WriteRaw(item.image, item.path, done.error) : WritePng(item.image, item.path, done.error);
                if (item.finished)
                    item.finished(done);
            }
            CoUninitialize();
        }

        std::mutex lock;
        std::condition_variable wake;
        std::deque<Item> queue;
        std::thread worker;
        bool stop = false;
    };
}
