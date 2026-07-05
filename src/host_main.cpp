#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <windows.h>
#include <shlobj.h>
#include <ws2tcpip.h>
#include <exdisp.h>
#include <shldisp.h>
#include <wrl/client.h>

#include "llk_protocol.h"
#include "llk_transfer.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace {

using Microsoft::WRL::ComPtr;

struct Args final {
  std::wstring viewer_host = L"127.0.0.1";
  std::uint16_t video_port = 52334;
  std::uint16_t control_port = 52333;
  std::uint16_t transfer_port = 52335;
  std::uint32_t fps = 30;
  std::uint32_t bitrate_mbps = 8;
  std::uint32_t output_index = 0;
  std::wstring ffmpeg_path;
};

struct RuntimeState final {
  std::mutex mutex;
  std::condition_variable cv;
  Args current{};
  bool stop = false;
  bool sender_requested = false;
  bool sender_running = false;
  std::uint64_t lease_deadline_ms = 0;
};

struct SenderProcess final {
  HANDLE process = nullptr;
  HANDLE thread = nullptr;
};

std::atomic<bool> g_stop = false;
std::mutex g_transfer_root_mutex;
std::unordered_map<std::uint64_t, std::wstring> g_transfer_roots;
RuntimeState g_runtime;

std::uint64_t NowMs() {
  return static_cast<std::uint64_t>(GetTickCount64());
}

std::wstring GetExecutableDirectory();

bool IsAbsoluteWindowsPath(const std::wstring& path) {
  return path.size() >= 2 && path[1] == L':' ||
         path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\';
}

std::wstring ResolveFfmpegCandidate(const std::wstring& candidate) {
  if (candidate.empty()) {
    return L"";
  }
  if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return candidate;
  }
  if (IsAbsoluteWindowsPath(candidate)) {
    return L"";
  }
  const std::wstring sibling = GetExecutableDirectory() + L"\\" + candidate;
  if (GetFileAttributesW(sibling.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return sibling;
  }
  return L"";
}

std::wstring DetectFfmpegPath(const std::wstring& preferred) {
  std::wstring resolved = ResolveFfmpegCandidate(preferred);
  if (!resolved.empty()) {
    return resolved;
  }
  return ResolveFfmpegCandidate(L"ffmpeg.exe");
}

std::wstring GetExecutableDirectory() {
  std::wstring path(MAX_PATH, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  if (length == 0) {
    return L".";
  }
  path.resize(length);
  const std::size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return L".";
  }
  return path.substr(0, slash);
}

std::string WideToUtf8(const std::wstring& value) {
  if (value.empty()) {
    return "";
  }
  const int required =
      WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (required <= 0) {
    return "";
  }
  std::string result(required, '\0');
  WideCharToMultiByte(
      CP_UTF8,
      0,
      value.data(),
      static_cast<int>(value.size()),
      result.data(),
      required,
      nullptr,
      nullptr);
  return result;
}

std::wstring Utf8ToWide(const std::string& value) {
  if (value.empty()) {
    return L"";
  }
  const int required = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (required <= 0) {
    return L"";
  }
  std::wstring result(required, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required);
  return result;
}

std::string TrimNullTerminated(const char* value, const std::size_t size) {
  std::size_t length = 0;
  while (length < size && value[length] != '\0') {
    ++length;
  }
  return std::string(value, value + length);
}

void AppendLog(const std::wstring& message, const bool error = false) {
  const std::wstring path = GetExecutableDirectory() + (error ? L"\\host_stderr.txt" : L"\\host_stdout.txt");
  WIN32_FILE_ATTRIBUTE_DATA file_data{};
  const bool truncate =
      GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &file_data) != 0 &&
      (((static_cast<std::uint64_t>(file_data.nFileSizeHigh) << 32u) | file_data.nFileSizeLow) >= (1ull << 20u));
  HANDLE file = CreateFileW(
      path.c_str(),
      truncate ? GENERIC_WRITE : FILE_APPEND_DATA,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      nullptr,
      truncate ? CREATE_ALWAYS : OPEN_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  const std::string utf8 = WideToUtf8(message + L"\r\n");
  DWORD written = 0;
  WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
  CloseHandle(file);
}

Args ParseArgs(const int argc, wchar_t** argv) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::wstring_view key = argv[i];
    if (i + 1 >= argc) {
      break;
    }
    const std::wstring_view value = argv[i + 1];
    if (key == L"--viewer-host") {
      args.viewer_host = std::wstring(value);
      ++i;
    } else if (key == L"--video-port") {
      args.video_port = static_cast<std::uint16_t>(std::stoi(std::wstring(value)));
      ++i;
    } else if (key == L"--control-port") {
      args.control_port = static_cast<std::uint16_t>(std::stoi(std::wstring(value)));
      ++i;
    } else if (key == L"--transfer-port") {
      args.transfer_port = static_cast<std::uint16_t>(std::stoi(std::wstring(value)));
      ++i;
    } else if (key == L"--fps") {
      args.fps = static_cast<std::uint32_t>(std::stoul(std::wstring(value)));
      ++i;
    } else if (key == L"--bitrate-mbps") {
      args.bitrate_mbps = static_cast<std::uint32_t>(std::stoul(std::wstring(value)));
      ++i;
    } else if (key == L"--output-idx") {
      args.output_index = static_cast<std::uint32_t>(std::stoul(std::wstring(value)));
      ++i;
    } else if (key == L"--ffmpeg") {
      args.ffmpeg_path = std::wstring(value);
      ++i;
    }
  }
  return args;
}

bool RecvAll(SOCKET sock, void* buffer, std::size_t total_size) {
  auto* current = static_cast<std::uint8_t*>(buffer);
  std::size_t received_total = 0;
  while (received_total < total_size) {
    const int received = recv(
        sock,
        reinterpret_cast<char*>(current + received_total),
        static_cast<int>(std::min<std::size_t>(total_size - received_total, 64 * 1024)),
        0);
    if (received <= 0) {
      return false;
    }
    received_total += static_cast<std::size_t>(received);
  }
  return true;
}

std::wstring JoinPathParts(const std::wstring& left, const std::wstring& right) {
  if (left.empty()) {
    return right;
  }
  if (right.empty()) {
    return left;
  }
  return left + L"\\" + right;
}

std::wstring SanitizePathComponent(const std::wstring& original) {
  std::wstring value = original;
  if (value.empty() || value == L"." || value == L"..") {
    return L"_";
  }
  for (auto& ch : value) {
    switch (ch) {
      case L'<':
      case L'>':
      case L':':
      case L'"':
      case L'/':
      case L'\\':
      case L'|':
      case L'?':
      case L'*':
        ch = L'_';
        break;
      default:
        break;
    }
  }
  return value;
}

bool GetDesktopPath(std::wstring& path) {
  path.clear();
  PWSTR raw = nullptr;
  const HRESULT hr = SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &raw);
  if (FAILED(hr) || raw == nullptr) {
    return false;
  }
  path.assign(raw);
  CoTaskMemFree(raw);
  return !path.empty();
}

bool TryGetForegroundExplorerPath(std::wstring& path) {
  path.clear();
  const HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
  if (foreground == nullptr) {
    return false;
  }

  const HRESULT init_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool need_uninit = SUCCEEDED(init_hr);
  if (FAILED(init_hr) && init_hr != RPC_E_CHANGED_MODE) {
    return false;
  }

  ComPtr<IShellWindows> shell_windows;
  const HRESULT create_hr = CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&shell_windows));
  if (FAILED(create_hr) || shell_windows == nullptr) {
    if (need_uninit) {
      CoUninitialize();
    }
    return false;
  }

  long count = 0;
  shell_windows->get_Count(&count);
  for (long index = 0; index < count; ++index) {
    VARIANT item_index{};
    VariantInit(&item_index);
    item_index.vt = VT_I4;
    item_index.lVal = index;

    ComPtr<IDispatch> dispatch;
    const HRESULT item_hr = shell_windows->Item(item_index, &dispatch);
    VariantClear(&item_index);
    if (FAILED(item_hr) || dispatch == nullptr) {
      continue;
    }

    ComPtr<IWebBrowserApp> browser;
    if (FAILED(dispatch.As(&browser)) || browser == nullptr) {
      continue;
    }

    SHANDLE_PTR shell_hwnd = 0;
    if (FAILED(browser->get_HWND(&shell_hwnd))) {
      continue;
    }
    if (reinterpret_cast<HWND>(static_cast<LONG_PTR>(shell_hwnd)) != foreground) {
      continue;
    }

    ComPtr<IDispatch> document;
    if (FAILED(browser->get_Document(&document)) || document == nullptr) {
      continue;
    }

    ComPtr<IShellFolderViewDual> view;
    if (FAILED(document.As(&view)) || view == nullptr) {
      continue;
    }

    Folder* folder = nullptr;
    if (FAILED(view->get_Folder(&folder)) || folder == nullptr) {
      continue;
    }

    Folder2* folder2 = nullptr;
    const HRESULT folder2_hr = folder->QueryInterface(__uuidof(Folder2), reinterpret_cast<void**>(&folder2));
    folder->Release();
    if (FAILED(folder2_hr) || folder2 == nullptr) {
      continue;
    }

    FolderItem* self = nullptr;
    const HRESULT self_hr = folder2->get_Self(&self);
    folder2->Release();
    if (FAILED(self_hr) || self == nullptr) {
      continue;
    }

    BSTR raw_path = nullptr;
    const HRESULT path_hr = self->get_Path(&raw_path);
    self->Release();
    if (FAILED(path_hr) || raw_path == nullptr) {
      continue;
    }

    path.assign(raw_path, SysStringLen(raw_path));
    SysFreeString(raw_path);
    if (need_uninit) {
      CoUninitialize();
    }
    return !path.empty();
  }

  if (need_uninit) {
    CoUninitialize();
  }
  return false;
}

bool ResolveTransferRoot(std::wstring& path) {
  path.clear();
  HWND foreground = GetAncestor(GetForegroundWindow(), GA_ROOT);
  if (foreground != nullptr) {
    wchar_t class_name[64]{};
    GetClassNameW(foreground, class_name, static_cast<int>(std::size(class_name)));
    if (std::wcscmp(class_name, L"Progman") == 0 || std::wcscmp(class_name, L"WorkerW") == 0) {
      return GetDesktopPath(path);
    }
  }
  if (TryGetForegroundExplorerPath(path)) {
    return true;
  }
  return GetDesktopPath(path);
}

void SetTransferRoot(const std::uint64_t transfer_id, const std::wstring& root) {
  std::scoped_lock lock(g_transfer_root_mutex);
  g_transfer_roots[transfer_id] = root;
}

bool LookupTransferRoot(const std::uint64_t transfer_id, std::wstring& root) {
  std::scoped_lock lock(g_transfer_root_mutex);
  const auto it = g_transfer_roots.find(transfer_id);
  if (it == g_transfer_roots.end()) {
    return false;
  }
  root = it->second;
  return true;
}

void ClearTransferRoot(const std::uint64_t transfer_id) {
  std::scoped_lock lock(g_transfer_root_mutex);
  g_transfer_roots.erase(transfer_id);
}

bool GetTransferPathForRequest(
    const std::uint64_t transfer_id,
    const std::wstring& relative_name,
    std::wstring& result_path) {
  std::wstring current;
  if (!LookupTransferRoot(transfer_id, current) || current.empty()) {
    return false;
  }
  std::wstring component;
  std::vector<std::wstring> components;
  for (wchar_t ch : relative_name) {
    if (ch == L'\\' || ch == L'/') {
      if (!component.empty()) {
        components.push_back(SanitizePathComponent(component));
        component.clear();
      }
    } else {
      component.push_back(ch);
    }
  }
  if (!component.empty()) {
    components.push_back(SanitizePathComponent(component));
  }
  for (const auto& part : components) {
    current = JoinPathParts(current, part);
  }
  result_path = current;
  return true;
}

bool CreateParentDirectories(const std::wstring& file_path) {
  const std::size_t slash = file_path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return true;
  }
  std::wstring current;
  const std::wstring parent = file_path.substr(0, slash);
  std::size_t index = 0;
  if (parent.size() >= 2 && parent[1] == L':') {
    current = parent.substr(0, 2);
    index = 2;
  }
  while (index < parent.size()) {
    while (index < parent.size() && (parent[index] == L'\\' || parent[index] == L'/')) {
      current.push_back(L'\\');
      ++index;
    }
    std::size_t next = index;
    while (next < parent.size() && parent[next] != L'\\' && parent[next] != L'/') {
      ++next;
    }
    if (next > index) {
      current.append(parent, index, next - index);
      CreateDirectoryW(current.c_str(), nullptr);
    }
    index = next;
  }
  return true;
}

bool SetClipboardText(const std::wstring& text) {
  for (int attempt = 0; attempt < 10; ++attempt) {
    if (!OpenClipboard(nullptr)) {
      Sleep(20);
      continue;
    }
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
      CloseClipboard();
      return false;
    }
    void* target = GlobalLock(memory);
    if (target == nullptr) {
      GlobalFree(memory);
      CloseClipboard();
      return false;
    }
    std::memcpy(target, text.c_str(), bytes);
    GlobalUnlock(memory);
    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
      GlobalFree(memory);
      CloseClipboard();
      return false;
    }
    CloseClipboard();
    return true;
  }
  return false;
}

bool ReceiveClipboardTransfer(SOCKET client, const llk::transfer::Header& header) {
  if (header.payload_bytes > llk::transfer::kMaxClipboardBytes) {
    return false;
  }
  std::string utf8(static_cast<std::size_t>(header.payload_bytes), '\0');
  if (!utf8.empty() && !RecvAll(client, utf8.data(), utf8.size())) {
    return false;
  }
  return SetClipboardText(Utf8ToWide(utf8));
}

bool ReceiveTransferName(SOCKET client, const llk::transfer::Header& header, std::wstring& name) {
  name.clear();
  if (header.name_bytes > llk::transfer::kMaxNameBytes) {
    return false;
  }
  std::string utf8_name(static_cast<std::size_t>(header.name_bytes), '\0');
  if (!utf8_name.empty() && !RecvAll(client, utf8_name.data(), utf8_name.size())) {
    return false;
  }
  name = Utf8ToWide(utf8_name);
  return true;
}

bool ReceiveClipboardReset(const llk::transfer::Header& header) {
  std::wstring root;
  if (!ResolveTransferRoot(root) || root.empty()) {
    return false;
  }
  SetTransferRoot(header.transfer_id, root);
  return true;
}

bool ReceiveClipboardDirectory(SOCKET client, const llk::transfer::Header& header) {
  std::wstring name;
  if (!ReceiveTransferName(client, header, name)) {
    return false;
  }
  std::wstring path;
  if (!GetTransferPathForRequest(header.transfer_id, name, path)) {
    return false;
  }
  CreateDirectoryW(path.c_str(), nullptr);
  return true;
}

bool ReceiveClipboardFile(SOCKET client, const llk::transfer::Header& header) {
  std::wstring name;
  if (!ReceiveTransferName(client, header, name)) {
    return false;
  }
  std::wstring file_path;
  if (!GetTransferPathForRequest(header.transfer_id, name, file_path)) {
    return false;
  }
  CreateParentDirectories(file_path);
  const DWORD creation = (header.offset_bytes == 0) ? CREATE_ALWAYS : OPEN_ALWAYS;
  HANDLE file = CreateFileW(
      file_path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ,
      nullptr,
      creation,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(header.offset_bytes);
  if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
    CloseHandle(file);
    return false;
  }
  std::vector<std::uint8_t> buffer(64 * 1024);
  std::uint64_t remaining = header.payload_bytes;
  while (remaining > 0) {
    const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size()));
    if (!RecvAll(client, buffer.data(), chunk)) {
      CloseHandle(file);
      return false;
    }
    DWORD written = 0;
    if (!WriteFile(file, buffer.data(), static_cast<DWORD>(chunk), &written, nullptr) ||
        written != static_cast<DWORD>(chunk)) {
      CloseHandle(file);
      return false;
    }
    remaining -= chunk;
  }
  CloseHandle(file);
  return true;
}

bool ReceiveClipboardCommit(SOCKET client, const llk::transfer::Header& header) {
  if (header.payload_bytes > 0) {
    std::vector<std::uint8_t> discard(64 * 1024);
    std::uint64_t remaining = header.payload_bytes;
    while (remaining > 0) {
      const std::size_t chunk = static_cast<std::size_t>(std::min<std::uint64_t>(remaining, discard.size()));
      if (!RecvAll(client, discard.data(), chunk)) {
        ClearTransferRoot(header.transfer_id);
        return false;
      }
      remaining -= chunk;
    }
  }
  ClearTransferRoot(header.transfer_id);
  return true;
}

void HandleTransferClient(SOCKET client) {
  while (!g_stop.load()) {
    llk::transfer::Header header{};
    if (!RecvAll(client, &header, sizeof(header))) {
      return;
    }
    if (header.magic != llk::transfer::kMagic || header.version != llk::transfer::kVersion) {
      return;
    }
    bool ok = false;
    switch (static_cast<llk::transfer::Kind>(header.kind)) {
      case llk::transfer::Kind::ClipboardText:
        ok = ReceiveClipboardTransfer(client, header);
        break;
      case llk::transfer::Kind::ClipboardReset:
        ok = ReceiveClipboardReset(header);
        break;
      case llk::transfer::Kind::ClipboardDirectory:
        ok = ReceiveClipboardDirectory(client, header);
        break;
      case llk::transfer::Kind::ClipboardFile:
        ok = ReceiveClipboardFile(client, header);
        break;
      case llk::transfer::Kind::ClipboardCommit:
        ok = ReceiveClipboardCommit(client, header);
        break;
      default:
        break;
    }
    llk::transfer::Response response{};
    response.status = ok ? llk::transfer::kStatusOk : llk::transfer::kStatusError;
    if (send(client, reinterpret_cast<const char*>(&response), sizeof(response), 0) != sizeof(response)) {
      return;
    }
    if (!ok) {
      return;
    }
  }
}

void TransferLoop(const std::uint16_t port) {
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    return;
  }
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listener == INVALID_SOCKET) {
    WSACleanup();
    return;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(listener, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR ||
      listen(listener, 4) == SOCKET_ERROR) {
    closesocket(listener);
    WSACleanup();
    return;
  }
  u_long nonblocking = 1;
  ioctlsocket(listener, FIONBIO, &nonblocking);
  while (!g_stop.load()) {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(listener, &read_set);
    timeval timeout{};
    timeout.tv_usec = 200000;
    const int ready = select(0, &read_set, nullptr, nullptr, &timeout);
    if (ready <= 0) {
      continue;
    }
    SOCKET client = accept(listener, nullptr, nullptr);
    if (client == INVALID_SOCKET) {
      continue;
    }
    u_long blocking = 0;
    ioctlsocket(client, FIONBIO, &blocking);
    HandleTransferClient(client);
    shutdown(client, SD_BOTH);
    closesocket(client);
  }
  closesocket(listener);
  WSACleanup();
}

bool QueryDesktopSize(std::uint32_t& width, std::uint32_t& height) {
  DEVMODEW mode{};
  mode.dmSize = sizeof(mode);
  if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
    width = std::max<DWORD>(1, mode.dmPelsWidth);
    height = std::max<DWORD>(1, mode.dmPelsHeight);
    return true;
  }
  width = static_cast<std::uint32_t>(std::max(1, GetSystemMetrics(SM_CXSCREEN)));
  height = static_cast<std::uint32_t>(std::max(1, GetSystemMetrics(SM_CYSCREEN)));
  return true;
}

bool IsLeaseActive(const RuntimeState& state, const std::uint64_t now_ms) {
  return state.sender_requested && state.lease_deadline_ms > now_ms;
}

void FillSyncStatePayload(llk::proto::SyncStatePayload& payload) {
  std::scoped_lock lock(g_runtime.mutex);
  const auto now_ms = NowMs();
  if (g_runtime.sender_requested) {
    payload.status_flags |= llk::proto::kSyncStateFlagSenderRequested;
  }
  if (g_runtime.sender_running) {
    payload.status_flags |= llk::proto::kSyncStateFlagSenderRunning;
  }
  if (IsLeaseActive(g_runtime, now_ms)) {
    payload.status_flags |= llk::proto::kSyncStateFlagLeaseActive;
    payload.keepalive_remaining_ms =
        static_cast<std::uint32_t>(std::min<std::uint64_t>(g_runtime.lease_deadline_ms - now_ms, 0xFFFFFFFFull));
  }
  if (!g_runtime.current.viewer_host.empty()) {
    payload.status_flags |= llk::proto::kSyncStateFlagViewerHostReady;
  }
  payload.video_port = g_runtime.current.video_port;
  payload.control_port = g_runtime.current.control_port;
  payload.transfer_port = g_runtime.current.transfer_port;
  payload.fps = g_runtime.current.fps;
  payload.bitrate_mbps = g_runtime.current.bitrate_mbps;
  payload.output_index = g_runtime.current.output_index;
  const std::string viewer_host = WideToUtf8(g_runtime.current.viewer_host);
  std::memcpy(payload.viewer_host, viewer_host.data(), std::min<std::size_t>(viewer_host.size(), llk::proto::kViewerHostBytes - 1));
}

void SendSyncStateResponse(SOCKET sock, const sockaddr_storage& remote, const int remote_len) {
  llk::proto::ControlHeader header{};
  header.kind = static_cast<std::uint16_t>(llk::proto::ControlKind::SyncState);
  llk::proto::SyncStatePayload payload{};
  FillSyncStatePayload(payload);
  QueryDesktopSize(payload.desktop_width, payload.desktop_height);
  std::array<std::uint8_t, sizeof(header) + sizeof(payload)> buffer{};
  std::memcpy(buffer.data(), &header, sizeof(header));
  std::memcpy(buffer.data() + sizeof(header), &payload, sizeof(payload));
  sendto(sock, reinterpret_cast<const char*>(buffer.data()), static_cast<int>(buffer.size()), 0, reinterpret_cast<const sockaddr*>(&remote), remote_len);
}

std::wstring RemoteAddressToWide(const sockaddr_storage& remote) {
  wchar_t host[INET6_ADDRSTRLEN]{};
  if (remote.ss_family == AF_INET) {
    const auto* addr = reinterpret_cast<const sockaddr_in*>(&remote);
    InetNtopW(AF_INET, const_cast<IN_ADDR*>(&addr->sin_addr), host, static_cast<DWORD>(std::size(host)));
  } else if (remote.ss_family == AF_INET6) {
    const auto* addr = reinterpret_cast<const sockaddr_in6*>(&remote);
    InetNtopW(AF_INET6, const_cast<IN6_ADDR*>(&addr->sin6_addr), host, static_cast<DWORD>(std::size(host)));
  }
  return std::wstring(host);
}

void ApplyRequestSync(const llk::proto::RequestSyncPayload& payload, const sockaddr_storage& remote) {
  std::scoped_lock lock(g_runtime.mutex);
  if ((payload.flags & llk::proto::kRequestSyncFlagUpdateViewerHost) != 0) {
    g_runtime.current.viewer_host = Utf8ToWide(TrimNullTerminated(payload.viewer_host, llk::proto::kViewerHostBytes));
  }
  if ((payload.flags & llk::proto::kRequestSyncFlagUseSourceHost) != 0) {
    const std::wstring source_host = RemoteAddressToWide(remote);
    if (!source_host.empty()) {
      g_runtime.current.viewer_host = source_host;
    }
  }
  if ((payload.flags & llk::proto::kRequestSyncFlagUpdateVideoPort) != 0 && payload.video_port != 0) {
    g_runtime.current.video_port = payload.video_port;
  }
  if ((payload.flags & llk::proto::kRequestSyncFlagUpdateFps) != 0 && payload.fps > 0) {
    g_runtime.current.fps = std::min(payload.fps, llk::proto::kRequestSyncMaxFps);
  }
  if ((payload.flags & llk::proto::kRequestSyncFlagUpdateBitrate) != 0 && payload.bitrate_mbps > 0) {
    g_runtime.current.bitrate_mbps = std::min(payload.bitrate_mbps, llk::proto::kRequestSyncMaxBitrateMbps);
  }
  if ((payload.flags & llk::proto::kRequestSyncFlagUpdateOutputIndex) != 0) {
    g_runtime.current.output_index = payload.output_index;
  }
  if ((payload.flags & llk::proto::kRequestSyncFlagWakeSender) != 0) {
    const auto keepalive_ms =
        payload.keepalive_ms == 0 ? llk::proto::kRequestSyncDefaultKeepaliveMs
                                  : std::min(payload.keepalive_ms, llk::proto::kRequestSyncMaxKeepaliveMs);
    g_runtime.sender_requested = true;
    g_runtime.lease_deadline_ms = NowMs() + keepalive_ms;
  }
  if ((payload.flags & llk::proto::kRequestSyncFlagStopSender) != 0) {
    g_runtime.sender_requested = false;
    g_runtime.lease_deadline_ms = 0;
  }
  g_runtime.cv.notify_all();
}

std::wstring BuildSenderCommand(const Args& args) {
  const std::wstring ffmpeg = DetectFfmpegPath(args.ffmpeg_path);
  const std::wstring filter =
      L"ddagrab=output_idx=" + std::to_wstring(args.output_index) +
      L":framerate=" + std::to_wstring(args.fps) + L":draw_mouse=1";
  const std::wstring bitrate = std::to_wstring(std::max<std::uint32_t>(1u, args.bitrate_mbps)) + L"M";
  const std::wstring gop = std::to_wstring(std::max<std::uint32_t>(1u, args.fps));
  const std::wstring sink =
      L"udp://" + args.viewer_host + L":" + std::to_wstring(args.video_port) +
      L"?pkt_size=1316&buffer_size=65535";
  return
      L"\"" + ffmpeg + L"\" -hide_banner -loglevel error -fflags nobuffer -flags low_delay"
      L" -f lavfi -i \"" + filter +
      L"\" -an -c:v h264_nvenc -preset p1 -tune ull -zerolatency 1 -delay 0 -rc cbr_ld_hq"
      L" -rc-lookahead 0 -surfaces 2 -bf 0 -strict_gop 1 -g " + gop + L" -forced-idr 1"
      L" -bsf:v dump_extra -b:v " + bitrate + L" -maxrate " + bitrate + L" -bufsize " + bitrate +
      L" -flush_packets 1 -muxdelay 0 -muxpreload 0 -mpegts_flags resend_headers"
      L" -f mpegts \"" + sink + L"\"";
}

bool StartSenderProcess(const Args& args, SenderProcess& sender) {
  const std::wstring ffmpeg = DetectFfmpegPath(args.ffmpeg_path);
  if (ffmpeg.empty()) {
    AppendLog(L"ffmpeg not found", true);
    return false;
  }
  const std::wstring ffmpeg_log_path = GetExecutableDirectory() + L"\\host_ffmpeg_stderr.txt";
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE ffmpeg_log = CreateFileW(
      ffmpeg_log_path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      &sa,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (ffmpeg_log == INVALID_HANDLE_VALUE) {
    return false;
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  si.hStdOutput = ffmpeg_log;
  si.hStdError = ffmpeg_log;
  PROCESS_INFORMATION pi{};
  std::wstring command = BuildSenderCommand(args);
  std::vector<wchar_t> mutable_command(command.begin(), command.end());
  mutable_command.push_back(L'\0');
  const BOOL ok = CreateProcessW(
      nullptr,
      mutable_command.data(),
      nullptr,
      nullptr,
      TRUE,
      CREATE_NO_WINDOW,
      nullptr,
      nullptr,
      &si,
      &pi);
  CloseHandle(ffmpeg_log);
  if (!ok) {
    AppendLog(L"CreateProcess failed for ffmpeg", true);
    return false;
  }
  sender.process = pi.hProcess;
  sender.thread = pi.hThread;
  AppendLog(L"Started ffmpeg sender");
  return true;
}

void StopSenderProcess(SenderProcess& sender) {
  if (sender.process != nullptr) {
    TerminateProcess(sender.process, 0);
    WaitForSingleObject(sender.process, 1500);
    CloseHandle(sender.process);
    sender.process = nullptr;
  }
  if (sender.thread != nullptr) {
    CloseHandle(sender.thread);
    sender.thread = nullptr;
  }
}

bool ProcessAlive(HANDLE process) {
  if (process == nullptr) {
    return false;
  }
  DWORD exit_code = 0;
  if (!GetExitCodeProcess(process, &exit_code)) {
    return false;
  }
  return exit_code == STILL_ACTIVE;
}

void SenderManagerLoop() {
  SenderProcess sender{};
  Args active_args{};
  while (!g_stop.load()) {
    Args desired{};
    bool should_run = false;
    {
      std::unique_lock lock(g_runtime.mutex);
      g_runtime.cv.wait_for(lock, std::chrono::milliseconds(250));
      const auto now_ms = NowMs();
      if (!IsLeaseActive(g_runtime, now_ms)) {
        g_runtime.sender_requested = false;
      }
      desired = g_runtime.current;
      should_run = g_runtime.sender_requested &&
                   !g_runtime.current.viewer_host.empty() &&
                   g_runtime.current.video_port != 0;
    }

    const bool running = ProcessAlive(sender.process);
    {
      std::scoped_lock lock(g_runtime.mutex);
      g_runtime.sender_running = running;
    }

    if (!should_run) {
      if (running || sender.process != nullptr) {
        StopSenderProcess(sender);
        std::scoped_lock lock(g_runtime.mutex);
        g_runtime.sender_running = false;
      }
      continue;
    }

    const bool config_changed =
        !running ||
        active_args.viewer_host != desired.viewer_host ||
        active_args.video_port != desired.video_port ||
        active_args.fps != desired.fps ||
        active_args.bitrate_mbps != desired.bitrate_mbps ||
        active_args.output_index != desired.output_index ||
        active_args.ffmpeg_path != desired.ffmpeg_path;
    if (!config_changed) {
      continue;
    }
    StopSenderProcess(sender);
    if (StartSenderProcess(desired, sender)) {
      active_args = desired;
      std::scoped_lock lock(g_runtime.mutex);
      g_runtime.sender_running = true;
    } else {
      std::scoped_lock lock(g_runtime.mutex);
      g_runtime.sender_running = false;
    }
  }
  StopSenderProcess(sender);
}

void ControlLoop(const std::uint16_t port) {
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    return;
  }
  SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock == INVALID_SOCKET) {
    WSACleanup();
    return;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
    closesocket(sock);
    WSACleanup();
    return;
  }
  std::array<std::uint8_t, 512> buffer{};
  while (!g_stop.load()) {
    sockaddr_storage remote{};
    int remote_len = sizeof(remote);
    const int received = recvfrom(
        sock,
        reinterpret_cast<char*>(buffer.data()),
        static_cast<int>(buffer.size()),
        0,
        reinterpret_cast<sockaddr*>(&remote),
        &remote_len);
    if (received < static_cast<int>(sizeof(llk::proto::ControlHeader))) {
      continue;
    }
    llk::proto::ControlHeader header{};
    std::memcpy(&header, buffer.data(), sizeof(header));
    if (header.magic != llk::proto::kMagic || header.version != llk::proto::kVersion) {
      continue;
    }
    switch (static_cast<llk::proto::ControlKind>(header.kind)) {
      case llk::proto::ControlKind::RequestSync: {
        llk::proto::RequestSyncPayload payload{};
        if (received >= static_cast<int>(sizeof(header) + sizeof(payload))) {
          std::memcpy(&payload, buffer.data() + sizeof(header), sizeof(payload));
        } else {
          payload.flags = llk::proto::kRequestSyncFlagWakeSender | llk::proto::kRequestSyncFlagUseSourceHost;
          payload.keepalive_ms = llk::proto::kRequestSyncDefaultKeepaliveMs;
        }
        ApplyRequestSync(payload, remote);
        SendSyncStateResponse(sock, remote, remote_len);
        break;
      }
      default:
        break;
    }
  }
  closesocket(sock);
  WSACleanup();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  const Args args = ParseArgs(argc, argv);
  DeleteFileW((GetExecutableDirectory() + L"\\host_ffmpeg_stderr.txt").c_str());
  {
    std::scoped_lock lock(g_runtime.mutex);
    g_runtime.current = args;
    g_runtime.sender_requested = false;
    g_runtime.sender_running = false;
    g_runtime.lease_deadline_ms = 0;
  }

  std::wcout << L"llk_host rebuild path starting" << std::endl;
  std::wcout << L"control_port=" << args.control_port
             << L" transfer_port=" << args.transfer_port
             << L" default_video_port=" << args.video_port
             << L" fps=" << args.fps
             << L" bitrate=" << args.bitrate_mbps << L"Mbps" << std::endl;

  std::thread control_thread(ControlLoop, args.control_port);
  std::thread transfer_thread(TransferLoop, args.transfer_port);
  std::thread sender_thread(SenderManagerLoop);

  control_thread.join();
  g_stop.store(true);
  {
    std::scoped_lock lock(g_runtime.mutex);
    g_runtime.stop = true;
    g_runtime.cv.notify_all();
  }
  if (transfer_thread.joinable()) {
    transfer_thread.join();
  }
  if (sender_thread.joinable()) {
    sender_thread.join();
  }
  return 0;
}
