#include "recording.hpp"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace {
BYTE Clamp(int value) {
    return static_cast<BYTE>(std::clamp(value, 0, 255));
}

void ConvertBgraToNv12(const BYTE* source, BYTE* target, int width, int height) {
    BYTE* y_plane = target;
    BYTE* uv_plane = target + width * height;
    for (int y = 0; y < height; y += 2) {
        for (int x = 0; x < width; x += 2) {
            int u_total = 0;
            int v_total = 0;
            for (int dy = 0; dy < 2; ++dy) {
                for (int dx = 0; dx < 2; ++dx) {
                    const BYTE* pixel = source + ((y + dy) * width + x + dx) * 4;
                    const int blue = pixel[0];
                    const int green = pixel[1];
                    const int red = pixel[2];
                    y_plane[(y + dy) * width + x + dx] = Clamp(((66 * red + 129 * green + 25 * blue + 128) >> 8) + 16);
                    u_total += ((-38 * red - 74 * green + 112 * blue + 128) >> 8) + 128;
                    v_total += ((112 * red - 94 * green - 18 * blue + 128) >> 8) + 128;
                }
            }
            const int uv = (y / 2) * width + x;
            uv_plane[uv] = Clamp((u_total + 2) / 4);
            uv_plane[uv + 1] = Clamp((v_total + 2) / 4);
        }
    }
}

HRESULT SetVideoType(IMFMediaType* type, const GUID& subtype, int width, int height, int fps, int bitrate) {
    HRESULT hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) hr = type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(type, MF_MT_FRAME_SIZE, width, height);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_FRAME_RATE, fps, 1);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (SUCCEEDED(hr) && bitrate > 0) hr = type->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (SUCCEEDED(hr) && IsEqualGUID(subtype, MFVideoFormat_NV12))
        hr = type->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width));
    return hr;
}

HRESULT CreateWriter(const std::wstring& path, int width, int height, int fps, int bitrate,
                     ComPtr<IMFSinkWriter>& writer, DWORD& stream_index) {
    ComPtr<IMFByteStream> stream;
    ComPtr<IMFAttributes> attributes;
    ComPtr<IMFMediaType> output;
    ComPtr<IMFMediaType> input;
    HRESULT hr = MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST,
                              MF_FILEFLAGS_NONE, path.c_str(), &stream);
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attributes, 1);
    if (SUCCEEDED(hr)) hr = attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(path.c_str(), stream.Get(), attributes.Get(), &writer);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&output);
    if (SUCCEEDED(hr)) hr = SetVideoType(output.Get(), MFVideoFormat_H264, width, height, fps, bitrate);
    if (SUCCEEDED(hr)) hr = writer->AddStream(output.Get(), &stream_index);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&input);
    if (SUCCEEDED(hr)) hr = SetVideoType(input.Get(), MFVideoFormat_NV12, width, height, fps, 0);
    if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(stream_index, input.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = writer->BeginWriting();
    return hr;
}

HRESULT WriteFrame(IMFSinkWriter* writer, DWORD stream_index, const BYTE* bgra,
                   int width, int height, LONGLONG timestamp, LONGLONG duration) {
    const DWORD size = static_cast<DWORD>(static_cast<uint64_t>(width) * height * 3 / 2);
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    HRESULT hr = MFCreateMemoryBuffer(size, &buffer);
    BYTE* data = nullptr;
    if (SUCCEEDED(hr)) hr = buffer->Lock(&data, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        ConvertBgraToNv12(bgra, data, width, height);
        const HRESULT unlock = buffer->Unlock();
        if (FAILED(unlock)) hr = unlock;
    }
    if (SUCCEEDED(hr)) hr = buffer->SetCurrentLength(size);
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(timestamp);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(duration);
    if (SUCCEEDED(hr)) hr = writer->WriteSample(stream_index, sample.Get());
    return hr;
}
} // namespace

HRESULT RecordScreen(RECT area, const std::wstring& path, int fps, int bitrate, HANDLE stop_event) {
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    if (width < 2 || height < 2 || width > 16384 || height > 16384 ||
        (width & 1) || (height & 1) || fps < 1 || bitrate < 1 || !stop_event) return E_INVALIDARG;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return hr;
    hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) { CoUninitialize(); return hr; }

    ComPtr<IMFSinkWriter> writer;
    DWORD stream_index = 0;
    hr = CreateWriter(path, width, height, fps, bitrate, writer, stream_index);
    if (SUCCEEDED(hr)) {
        HDC screen = GetDC(nullptr);
        HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* pixels = nullptr;
        HBITMAP bitmap = memory ? CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
        HGDIOBJ old = bitmap ? SelectObject(memory, bitmap) : nullptr;
        if (!screen || !memory || !bitmap || !old || !pixels) hr = E_FAIL;

        const LONGLONG frame_duration = 10000000LL / fps;
        const auto interval = std::chrono::duration<double>(1.0 / fps);
        const auto start = std::chrono::steady_clock::now();
        auto next = start;
        LONGLONG last_time = -1;
        while (SUCCEEDED(hr)) {
            if (!BitBlt(memory, 0, 0, width, height, screen, area.left, area.top,
                        SRCCOPY | CAPTUREBLT)) { hr = E_FAIL; break; }
            GdiFlush();
            const auto now = std::chrono::steady_clock::now();
            LONGLONG timestamp = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count() / 100;
            if (timestamp <= last_time) timestamp = last_time + 1;
            hr = WriteFrame(writer.Get(), stream_index, static_cast<const BYTE*>(pixels),
                            width, height, timestamp, frame_duration);
            last_time = timestamp;
            if (WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0) break;
            next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(interval);
            const auto remaining = next - std::chrono::steady_clock::now();
            if (remaining > std::chrono::steady_clock::duration::zero()) {
                const auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count();
                if (WaitForSingleObject(stop_event, static_cast<DWORD>(std::max<int64_t>(1, wait_ms))) == WAIT_OBJECT_0) break;
            } else {
                next = std::chrono::steady_clock::now();
            }
        }
        if (old) SelectObject(memory, old);
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        const HRESULT finalize = writer->Finalize();
        if (SUCCEEDED(hr)) hr = finalize;
    }
    writer.Reset();
    MFShutdown();
    CoUninitialize();
    return hr;
}
