#pragma once

#ifdef _WIN32

#include <d3d11_1.h>
#include <dxgi.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

class D3D11TextureReader {
public:
    D3D11TextureReader() = default;

    ~D3D11TextureReader() {
        Release();
        staging_.Reset();
        context_.Reset();
        device_.Reset();
    }

    // Open the shared texture handle from CEF, copy to staging, map for CPU read.
    // Returns true on success. out_data points to BGRA pixels, out_stride is row pitch.
    // Call Release() after you're done copying pixels.
    bool ReadTexture(HANDLE shared_handle, int width, int height,
                     const void** out_data, int* out_stride) {
        if (!EnsureDevice()) return false;
        if (!EnsureStaging(width, height, DXGI_FORMAT_B8G8R8A8_UNORM)) return false;

        // Open shared texture
        ComPtr<ID3D11Texture2D> shared_tex;
        HRESULT hr = E_FAIL;

        if (device1_) {
            hr = device1_->OpenSharedResource1(
                shared_handle, IID_PPV_ARGS(&shared_tex));
        }

        if (FAILED(hr)) {
            // Fallback: try OpenSharedResource (older API)
            hr = device_->OpenSharedResource(
                shared_handle, IID_PPV_ARGS(&shared_tex));
            if (FAILED(hr)) return false;
        }

        // Validate shared texture format/dimensions before CopyResource
        D3D11_TEXTURE2D_DESC shared_desc = {};
        shared_tex->GetDesc(&shared_desc);
        if (shared_desc.Width != (UINT)width ||
            shared_desc.Height != (UINT)height ||
            (shared_desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
             shared_desc.Format != DXGI_FORMAT_B8G8R8X8_UNORM)) {
            // Format/size mismatch — recreate staging to match actual texture
            if (!EnsureStaging(shared_desc.Width, shared_desc.Height, shared_desc.Format))
                return false;
        }

        // Copy shared texture to staging texture
        context_->CopyResource(staging_.Get(), shared_tex.Get());

        // Map staging texture for CPU read
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        hr = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
        if (FAILED(hr)) return false;

        *out_data = mapped.pData;
        *out_stride = static_cast<int>(mapped.RowPitch);
        mapped_ = true;
        return true;
    }

    // Unmap the staging texture. Must call after ReadTexture() and before next call.
    void Release() {
        if (mapped_ && staging_ && context_) {
            context_->Unmap(staging_.Get(), 0);
            mapped_ = false;
        }
    }

private:
    ComPtr<ID3D11Device>        device_;
    ComPtr<ID3D11Device1>       device1_;
    ComPtr<ID3D11DeviceContext>  context_;
    ComPtr<ID3D11Texture2D>     staging_;
    int staging_width_ = 0;
    int staging_height_ = 0;
    DXGI_FORMAT staging_format_ = DXGI_FORMAT_UNKNOWN;
    bool mapped_ = false;

    bool EnsureDevice() {
        if (device_) return true;

        D3D_FEATURE_LEVEL feature_levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
        };

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        HRESULT hr = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            feature_levels,
            ARRAYSIZE(feature_levels),
            D3D11_SDK_VERSION,
            &device,
            nullptr,
            &context
        );
        if (FAILED(hr)) return false;

        device_ = device;
        context_ = context;

        // Get ID3D11Device1 for OpenSharedResource1
        hr = device_.As(&device1_);
        // device1_ may be null on older drivers - we fall back to OpenSharedResource

        return true;
    }

    bool EnsureStaging(int width, int height, DXGI_FORMAT format) {
        if (staging_ && staging_width_ == width && staging_height_ == height
            && staging_format_ == format)
            return true;

        staging_.Reset();

        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &staging_);
        if (FAILED(hr)) return false;

        staging_width_ = width;
        staging_height_ = height;
        staging_format_ = format;
        return true;
    }
};

#endif // _WIN32

