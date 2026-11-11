#include "recording.hpp"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#include <vector>

int wmain() {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / L"win-capture-recording-smoke.mp4";
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop) return 1;
    std::thread stopper([stop] {
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        SetEvent(stop);
    });
    const RECT area{0, 0, 320, 240};
    const HRESULT hr = RecordScreen(area, path.wstring(), 10, 500000, stop);
    stopper.join();
    CloseHandle(stop);
    if (FAILED(hr)) {
        std::wcerr << L"RecordScreen failed: 0x" << std::hex << static_cast<unsigned>(hr) << L'\n';
        return 2;
    }
    std::ifstream input(path, std::ios::binary);
    const std::vector<char> bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (bytes.size() < 1000 || bytes[4] != 'f' || bytes[5] != 't' || bytes[6] != 'y' || bytes[7] != 'p' ||
        std::search(bytes.begin(), bytes.end(), "moov", "moov" + 4) == bytes.end()) {
        std::wcerr << L"MP4 structure is incomplete\n";
        return 3;
    }
    std::wcout << L"Recorded and finalized " << bytes.size() << L" bytes\n";
    return 0;
}
