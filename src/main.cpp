#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using Microsoft::WRL::ComPtr;

namespace {
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kCaptureSelection = WM_APP + 2;
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kPreviewTimer = 1;
constexpr int kIconId = 1;
constexpr int kHotkeyMonitor = 3;
constexpr int kHotkeySelection = 4;
constexpr int kHotkeyPanel = 5;
constexpr UINT kMenuMonitor = 101;
constexpr UINT kMenuSelection = 102;
constexpr UINT kMenuRecordMonitor = 103;
constexpr UINT kMenuRecordSelection = 104;
constexpr UINT kMenuExit = 105;

struct Config {
    std::wstring save_dir;
    std::wstring filename;
    std::wstring format;
    int jpeg_quality = 0;
    int fps = 0;
    int bitrate = 0;
};

Config g_config;
HINSTANCE g_instance = nullptr;
HWND g_main = nullptr;
HWND g_overlay = nullptr;
HWND g_preview = nullptr;
HBITMAP g_preview_bitmap = nullptr;
int g_preview_width = 0;
int g_preview_height = 0;
std::wstring g_preview_path;
UINT g_taskbar_created = 0;
RECT g_selected = {};
POINT g_drag_start = {};
POINT g_drag_end = {};
bool g_dragging = false;

void Error(const std::wstring& message) {
    MessageBoxW(g_main, message.c_str(), L"Win Capture", MB_OK | MB_ICONERROR);
}

std::wstring ExeDirectory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length == path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path().wstring();
}

bool CreateInitialIni(const std::wstring& path, std::wstring& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"capture.ini を作成できません。EXEのあるフォルダーの書き込み権限を確認してください。";
        return false;
    }
    constexpr wchar_t contents[] =
        L"\xFEFF[General]\r\nsave_dir=%USERPROFILE%\\capture\r\n"
        L"filename=capture-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}\r\n\r\n"
        L"[Image]\r\nformat=png\r\njpeg_quality=90\r\n\r\n"
        L"[Video]\r\nfps=30\r\nbitrate=8000000\r\n";
    DWORD written = 0;
    const DWORD size = static_cast<DWORD>((std::size(contents) - 1) * sizeof(wchar_t));
    const bool ok = WriteFile(file, contents, size, &written, nullptr) && written == size;
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(path.c_str());
        error = L"capture.ini の初期設定を書き込めません。";
    }
    return ok;
}

bool ReadRequired(const std::wstring& path, const wchar_t* section, const wchar_t* key,
                  std::wstring& value, std::wstring& error) {
    constexpr wchar_t missing[] = L"__WIN_CAPTURE_MISSING_7E36A7__";
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetPrivateProfileStringW(section, key, missing, buffer.data(),
                                                   static_cast<DWORD>(buffer.size()), path.c_str());
    buffer.resize(length);
    if (buffer.empty() || buffer == missing || length >= 32766) {
        error = std::format(L"capture.ini の [{}] {} が欠落または空です。", section, key);
        return false;
    }
    value = std::move(buffer);
    return true;
}

bool ReadNumber(const std::wstring& path, const wchar_t* section, const wchar_t* key,
                int minimum, int maximum, int& value, std::wstring& error) {
    std::wstring raw;
    if (!ReadRequired(path, section, key, raw, error)) return false;
    if (raw.empty() || !std::all_of(raw.begin(), raw.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; })) {
        error = std::format(L"capture.ini の [{}] {} は数値で指定してください。", section, key);
        return false;
    }
    try {
        const long parsed = std::stol(raw);
        if (parsed < minimum || parsed > maximum) throw std::out_of_range("range");
        value = static_cast<int>(parsed);
    } catch (...) {
        error = std::format(L"capture.ini の [{}] {} は {}～{} の範囲で指定してください。",
                            section, key, minimum, maximum);
        return false;
    }
    return true;
}

bool ValidFilenameTemplate(const std::wstring& name) {
    if (name.empty() || name.back() == L'.' || name.back() == L' ') return false;
    constexpr std::wstring_view forbidden = L"<>:\"/\\|?*";
    for (wchar_t c : name) {
        if (c < 32 || forbidden.find(c) != std::wstring_view::npos) return false;
    }
    std::wstring test = name;
    for (const wchar_t* token : {L"{yyyy}", L"{MM}", L"{dd}", L"{HH}", L"{mm}", L"{ss}"}) {
        size_t pos = 0;
        while ((pos = test.find(token, pos)) != std::wstring::npos) test.replace(pos, wcslen(token), L"0");
    }
    return test.find_first_of(L"{}") == std::wstring::npos;
}

bool LoadConfig(Config& config, std::wstring& error) {
    const std::wstring dir = ExeDirectory();
    if (dir.empty()) { error = L"EXEの場所を取得できません。"; return false; }
    const std::wstring path = (std::filesystem::path(dir) / L"capture.ini").wstring();
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND || !CreateInitialIni(path, error)) {
            if (error.empty()) error = L"capture.ini を読み込めません。";
            return false;
        }
    } else if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        error = L"capture.ini はファイルである必要があります。";
        return false;
    }
    std::wstring save_dir;
    if (!ReadRequired(path, L"General", L"save_dir", save_dir, error) ||
        !ReadRequired(path, L"General", L"filename", config.filename, error) ||
        !ReadRequired(path, L"Image", L"format", config.format, error) ||
        !ReadNumber(path, L"Image", L"jpeg_quality", 1, 100, config.jpeg_quality, error) ||
        !ReadNumber(path, L"Video", L"fps", 1, 120, config.fps, error) ||
        !ReadNumber(path, L"Video", L"bitrate", 100000, 100000000, config.bitrate, error)) return false;
    if (config.format != L"png" && config.format != L"jpeg" && config.format != L"jpg") {
        error = L"capture.ini の [Image] format は png または jpeg を指定してください。";
        return false;
    }
    if (!ValidFilenameTemplate(config.filename)) {
        error = L"capture.ini の [General] filename に無効な文字またはトークンがあります。";
        return false;
    }
    const DWORD needed = ExpandEnvironmentStringsW(save_dir.c_str(), nullptr, 0);
    if (!needed || needed > 32768) { error = L"保存先の環境変数を展開できません。"; return false; }
    std::wstring expanded(needed, L'\0');
    if (!ExpandEnvironmentStringsW(save_dir.c_str(), expanded.data(), needed)) {
        error = L"保存先の環境変数を展開できません。"; return false;
    }
    expanded.resize(needed - 1);
    if (expanded.find(L'%') != std::wstring::npos || !std::filesystem::path(expanded).is_absolute()) {
        error = L"保存先は展開可能な環境変数を含む絶対パスで指定してください。";
        return false;
    }
    try {
        std::filesystem::create_directories(expanded);
        if (!std::filesystem::is_directory(expanded)) throw std::filesystem::filesystem_error(
            "not directory", std::filesystem::path(expanded), std::make_error_code(std::errc::not_a_directory));
    } catch (...) {
        error = L"保存先ディレクトリを作成または使用できません。";
        return false;
    }
    config.save_dir = std::move(expanded);
    return true;
}

void ReplaceAll(std::wstring& value, std::wstring_view token, const std::wstring& replacement) {
    size_t pos = 0;
    while ((pos = value.find(token, pos)) != std::wstring::npos) {
        value.replace(pos, token.size(), replacement);
        pos += replacement.size();
    }
}

std::wstring OutputName() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    std::wstring name = g_config.filename;
    ReplaceAll(name, L"{yyyy}", std::format(L"{:04}", now.wYear));
    ReplaceAll(name, L"{MM}", std::format(L"{:02}", now.wMonth));
    ReplaceAll(name, L"{dd}", std::format(L"{:02}", now.wDay));
    ReplaceAll(name, L"{HH}", std::format(L"{:02}", now.wHour));
    ReplaceAll(name, L"{mm}", std::format(L"{:02}", now.wMinute));
    ReplaceAll(name, L"{ss}", std::format(L"{:02}", now.wSecond));
    return name;
}

std::wstring ReserveOutput(std::wstring& error) {
    const std::wstring base = OutputName();
    const std::wstring extension = g_config.format == L"png" ? L".png" : L".jpg";
    for (int suffix = 0; suffix < 10000; ++suffix) {
        const std::wstring filename = base + (suffix ? std::format(L"-{}", suffix) : L"") + extension;
        const std::wstring path = (std::filesystem::path(g_config.save_dir) / filename).wstring();
        HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); return path; }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
            error = L"保存ファイルを作成できません。保存先の権限と空き容量を確認してください。";
            return {};
        }
    }
    error = L"同じ名前のファイルが多すぎます。";
    return {};
}

bool SaveBitmap(HBITMAP bitmap, UINT width, UINT height, const std::wstring& path, std::wstring& error) {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmap> source;
    ComPtr<IWICFormatConverter> converter;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source);
    if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(hr)) hr = converter->Initialize(source.Get(), GUID_WICPixelFormat24bppBGR,
                                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                                   WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(g_config.format == L"png" ? GUID_ContainerFormatPng : GUID_ContainerFormatJpeg,
                                                   nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &options);
    if (SUCCEEDED(hr) && g_config.format != L"png") {
        PROPBAG2 property{};
        property.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT quality{};
        quality.vt = VT_R4;
        quality.fltVal = static_cast<float>(g_config.jpeg_quality) / 100.0f;
        hr = options->Write(1, &property, &quality);
    }
    if (SUCCEEDED(hr)) hr = frame->Initialize(options.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(width, height);
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&pixel_format);
    if (SUCCEEDED(hr)) hr = frame->WriteSource(converter.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    if (FAILED(hr)) {
        error = std::format(L"画像の保存に失敗しました (0x{:08X})。", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

void HidePreview() {
    if (g_preview) DestroyWindow(g_preview);
}

LRESULT CALLBACK PreviewProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
    case WM_LBUTTONUP: {
        const std::wstring path = g_preview_path;
        HidePreview();
        const HINSTANCE result = ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(result) <= 32) Error(L"保存した画像を開けませんでした。");
        return 0;
    }
    case WM_TIMER:
        if (wparam == kPreviewTimer) { HidePreview(); return 0; }
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        HDC image_dc = CreateCompatibleDC(dc);
        if (image_dc && g_preview_bitmap) {
            HGDIOBJ old = SelectObject(image_dc, g_preview_bitmap);
            BitBlt(dc, 6, 6, g_preview_width, g_preview_height, image_dc, 0, 0, SRCCOPY);
            SelectObject(image_dc, old);
        }
        if (image_dc) DeleteDC(image_dc);
        FrameRect(dc, &client, static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kPreviewTimer);
        if (g_preview_bitmap) DeleteObject(g_preview_bitmap);
        g_preview_bitmap = nullptr;
        g_preview = nullptr;
        g_preview_path.clear();
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

HBITMAP MakeThumbnail(HBITMAP source, int source_width, int source_height, int width, int height) {
    HDC screen = GetDC(nullptr);
    if (!screen) return nullptr;
    HDC input = CreateCompatibleDC(screen);
    HDC output = CreateCompatibleDC(screen);
    HBITMAP thumbnail = input && output ? CreateCompatibleBitmap(screen, width, height) : nullptr;
    if (thumbnail) {
        HGDIOBJ old_input = SelectObject(input, source);
        HGDIOBJ old_output = SelectObject(output, thumbnail);
        SetStretchBltMode(output, HALFTONE);
        SetBrushOrgEx(output, 0, 0, nullptr);
        const bool ok = StretchBlt(output, 0, 0, width, height, input, 0, 0,
                                   source_width, source_height, SRCCOPY) != 0;
        SelectObject(output, old_output);
        SelectObject(input, old_input);
        if (!ok) { DeleteObject(thumbnail); thumbnail = nullptr; }
    }
    if (output) DeleteDC(output);
    if (input) DeleteDC(input);
    ReleaseDC(nullptr, screen);
    return thumbnail;
}

void ShowPreview(HBITMAP bitmap, RECT captured, const std::wstring& path) {
    HidePreview();
    const int source_width = captured.right - captured.left;
    const int source_height = captured.bottom - captured.top;
    const double scale = std::min({1.0, 240.0 / source_width, 135.0 / source_height});
    g_preview_width = std::max(1, static_cast<int>(std::lround(source_width * scale)));
    g_preview_height = std::max(1, static_cast<int>(std::lround(source_height * scale)));
    g_preview_bitmap = MakeThumbnail(bitmap, source_width, source_height,
                                     g_preview_width, g_preview_height);
    if (!g_preview_bitmap) return;
    const POINT center{captured.left + source_width / 2, captured.top + source_height / 2};
    const HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) {
        DeleteObject(g_preview_bitmap);
        g_preview_bitmap = nullptr;
        return;
    }
    const int window_width = g_preview_width + 12;
    const int window_height = g_preview_height + 12;
    const int x = info.rcWork.right - window_width - 16;
    const int y = info.rcWork.bottom - window_height - 16;
    g_preview_path = path;
    g_preview = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                L"WinCapturePreview", L"Saved capture", WS_POPUP,
                                x, y, window_width, window_height,
                                nullptr, nullptr, g_instance, nullptr);
    if (!g_preview) {
        DeleteObject(g_preview_bitmap);
        g_preview_bitmap = nullptr;
        g_preview_path.clear();
        return;
    }
    SetWindowDisplayAffinity(g_preview, WDA_EXCLUDEFROMCAPTURE);
    ShowWindow(g_preview, SW_SHOWNOACTIVATE);
    SetTimer(g_preview, kPreviewTimer, 5000, nullptr);
}

void CaptureRect(RECT rect) {
    HidePreview();
    DwmFlush();
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;
    HDC screen = GetDC(nullptr);
    if (!screen) { Error(L"画面を取得できません。"); return; }
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = memory ? CreateCompatibleBitmap(screen, width, height) : nullptr;
    HGDIOBJ old = bitmap ? SelectObject(memory, bitmap) : nullptr;
    const bool copied = bitmap && old && BitBlt(memory, 0, 0, width, height, screen,
                                                  rect.left, rect.top, SRCCOPY | CAPTUREBLT);
    if (old) SelectObject(memory, old);
    if (memory) DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (!copied) {
        if (bitmap) DeleteObject(bitmap);
        Error(L"画面の撮影に失敗しました。");
        return;
    }
    std::wstring error;
    const std::wstring path = ReserveOutput(error);
    if (!path.empty()) {
        if (SaveBitmap(bitmap, static_cast<UINT>(width), static_cast<UINT>(height), path, error))
            ShowPreview(bitmap, rect, path);
        else
            DeleteFileW(path.c_str());
    }
    DeleteObject(bitmap);
    if (!error.empty()) Error(error);
}

void CaptureCursorMonitor() {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) { Error(L"カーソル位置を取得できません。"); return; }
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    if (!GetMonitorInfoW(monitor, &info)) { Error(L"モニター情報を取得できません。"); return; }
    CaptureRect(info.rcMonitor);
}

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_LBUTTONDOWN:
        g_dragging = true;
        g_drag_start = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        g_drag_end = g_drag_start;
        SetCapture(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_MOUSEMOVE:
        if (g_dragging) {
            g_drag_end = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_dragging) {
            g_drag_end = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ReleaseCapture();
            g_dragging = false;
            POINT a = g_drag_start;
            POINT b = g_drag_end;
            ClientToScreen(hwnd, &a);
            ClientToScreen(hwnd, &b);
            g_selected = {std::min(a.x, b.x), std::min(a.y, b.y),
                          std::max(a.x, b.x), std::max(a.y, b.y)};
            DestroyWindow(hwnd);
            g_overlay = nullptr;
            PostMessageW(g_main, kCaptureSelection, 0, 0);
        }
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) { DestroyWindow(hwnd); g_overlay = nullptr; return 0; }
        break;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        RECT client{};
        GetClientRect(hwnd, &client);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        if (g_dragging) {
            RECT selection{std::min(g_drag_start.x, g_drag_end.x), std::min(g_drag_start.y, g_drag_end.y),
                           std::max(g_drag_start.x, g_drag_end.x), std::max(g_drag_start.y, g_drag_end.y)};
            HPEN pen = CreatePen(PS_SOLID, 3, RGB(255, 255, 255));
            HGDIOBJ old_pen = SelectObject(dc, pen);
            HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Rectangle(dc, selection.left, selection.top, selection.right, selection.bottom);
            SelectObject(dc, old_brush);
            SelectObject(dc, old_pen);
            DeleteObject(pen);
        }
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_DESTROY:
        if (GetCapture() == hwnd) ReleaseCapture();
        g_dragging = false;
        g_overlay = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void StartSelection() {
    HidePreview();
    if (g_overlay) { SetForegroundWindow(g_overlay); return; }
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    g_overlay = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
                                 L"WinCaptureOverlay", L"Select capture area", WS_POPUP,
                                 x, y, width, height, nullptr, nullptr, g_instance, nullptr);
    if (!g_overlay) { Error(L"範囲選択画面を表示できません。"); return; }
    SetLayeredWindowAttributes(g_overlay, 0, 80, LWA_ALPHA);
    ShowWindow(g_overlay, SW_SHOW);
    SetForegroundWindow(g_overlay);
    SetFocus(g_overlay);
}

void AddTrayIcon() {
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = g_main;
    icon.uID = kTrayId;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = kTrayMessage;
    icon.hIcon = LoadIconW(g_instance, MAKEINTRESOURCEW(kIconId));
    wcscpy_s(icon.szTip, L"Win Capture");
    if (!Shell_NotifyIconW(NIM_ADD, &icon)) Error(L"タスクトレイアイコンを登録できません。");
}

void RemoveTrayIcon() {
    NOTIFYICONDATAW icon{};
    icon.cbSize = sizeof(icon);
    icon.hWnd = g_main;
    icon.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &icon);
}

void ShowPanel() {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, kMenuMonitor, L"モニターを撮影");
    AppendMenuW(menu, MF_STRING, kMenuSelection, L"範囲を撮影");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kMenuRecordMonitor, L"モニターを録画（未実装）");
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kMenuRecordSelection, L"範囲を録画（未実装）");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kMenuExit, L"終了");
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(g_main);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, g_main, nullptr);
    DestroyMenu(menu);
    PostMessageW(g_main, WM_NULL, 0, 0);
}

LRESULT CALLBACK MainProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == g_taskbar_created && g_taskbar_created != 0) { AddTrayIcon(); return 0; }
    switch (message) {
    case WM_HOTKEY:
        if (wparam == kHotkeyMonitor) CaptureCursorMonitor();
        else if (wparam == kHotkeySelection) StartSelection();
        else if (wparam == kHotkeyPanel) ShowPanel();
        return 0;
    case kCaptureSelection:
        DwmFlush();
        CaptureRect(g_selected);
        return 0;
    case kTrayMessage:
        if (lparam == WM_RBUTTONUP || lparam == WM_LBUTTONDBLCLK) ShowPanel();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kMenuMonitor: CaptureCursorMonitor(); break;
        case kMenuSelection: StartSelection(); break;
        case kMenuExit: DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_DESTROY:
        HidePreview();
        if (g_overlay) DestroyWindow(g_overlay);
        UnregisterHotKey(hwnd, kHotkeyMonitor);
        UnregisterHotKey(hwnd, kHotkeySelection);
        UnregisterHotKey(hwnd, kHotkeyPanel);
        RemoveTrayIcon();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\WinCaptureSingleInstance");
    if (!mutex) { Error(L"多重起動の確認に失敗しました。"); return 1; }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        Error(L"Win Capture は既に起動しています。");
        return 1;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        Error(L"COMを初期化できません。");
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }
    std::wstring error;
    if (!LoadConfig(g_config, error)) {
        Error(error);
        CoUninitialize();
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }
    WNDCLASSEXW main_class{sizeof(WNDCLASSEXW)};
    main_class.hInstance = instance;
    main_class.lpfnWndProc = MainProc;
    main_class.lpszClassName = L"WinCaptureMain";
    WNDCLASSEXW overlay_class{sizeof(WNDCLASSEXW)};
    overlay_class.hInstance = instance;
    overlay_class.lpfnWndProc = OverlayProc;
    overlay_class.lpszClassName = L"WinCaptureOverlay";
    overlay_class.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    WNDCLASSEXW preview_class{sizeof(WNDCLASSEXW)};
    preview_class.hInstance = instance;
    preview_class.lpfnWndProc = PreviewProc;
    preview_class.lpszClassName = L"WinCapturePreview";
    preview_class.style = CS_DROPSHADOW;
    if (!RegisterClassExW(&main_class) || !RegisterClassExW(&overlay_class) ||
        !RegisterClassExW(&preview_class)) {
        Error(L"ウィンドウクラスを登録できません。");
        CoUninitialize();
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }
    g_main = CreateWindowExW(0, main_class.lpszClassName, L"Win Capture", WS_OVERLAPPED,
                             0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    if (!g_main) {
        Error(L"メッセージウィンドウを作成できません。");
        CoUninitialize();
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    AddTrayIcon();
    const UINT modifiers = MOD_ALT | MOD_SHIFT | MOD_NOREPEAT;
    std::wstring failed;
    for (const auto [id, key] : {std::pair{kHotkeyMonitor, '3'},
                                  std::pair{kHotkeySelection, '4'},
                                  std::pair{kHotkeyPanel, '5'}}) {
        if (!RegisterHotKey(g_main, id, modifiers, key)) failed += std::format(L"Alt+Shift+{} ", id);
    }
    if (!failed.empty()) Error(L"ホットキーを登録できません: " + failed + L"\n他のアプリや入力言語設定との競合を確認してください。");
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return static_cast<int>(message.wParam);
}
