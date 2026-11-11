#pragma once

#include <windows.h>
#include <string>

// Runs on a worker thread. Returns only after MP4 finalization completes.
HRESULT RecordScreen(RECT area, const std::wstring& path, int fps, int bitrate, HANDLE stop_event);
