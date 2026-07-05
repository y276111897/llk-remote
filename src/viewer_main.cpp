#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <winsock2.h>
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <ws2tcpip.h>
#include <wrl/client.h>

#include "llk_protocol.h"
#include "llk_transfer.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClassName[] = L"LLK_REBUILD_VIEWER";
constexpr UINT kFrameReadyMessage = WM_APP + 1;

struct Args final {
  std::wstring agent_host = L"127.0.0.1";
  std::uint16_t video_port = 52334;
  std::uint16_t control_port = 52333;
  std::uint16_t transfer_port = 52335;
  std::uint32_t width = 1920;
  std::uint32_t height = 1080;
  std::uint32_t fps = 30;
  std::wstring ffmpeg_path;
};

struct AppState final {
  std::atomic<std::uint64_t> frames{0};
  std::atomic<bool> redraw_posted{false};
  std::mutex frame_mutex;
  std::vector<std::uint8_t> frame_bgra;
  std::uint32_t frame_width = 0;
  std::uint32_t frame_height = 0;
};

struct UploadItem final {
  std::wstring local_path;
  std::wstring remote_name;
  bool is_directory = false;
};


struct FrameLayout final {
  int dest_x = 0;
  int dest_y = 0;
  int dest_width = 0;
  int dest_height = 0;
};

struct D3DVertex final {
  float position[2];
  float texcoord[2];
};

struct Renderer final {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<IDXGISwapChain> swap_chain;
  ComPtr<ID3D11RenderTargetView> render_target_view;
  ComPtr<ID3D11VertexShader> vertex_shader;
  ComPtr<ID3D11PixelShader> pixel_shader;
  ComPtr<ID3D11InputLayout> input_layout;
  ComPtr<ID3D11Buffer> vertex_buffer;
  ComPtr<ID3D11SamplerState> sampler_state;
  ComPtr<ID3D11Texture2D> frame_texture;
  ComPtr<ID3D11ShaderResourceView> frame_srv;
  std::uint32_t frame_texture_width = 0;
  std::uint32_t frame_texture_height = 0;
  std::uint64_t uploaded_frame_count = 0;
};

struct ControlClient final {
  SOCKET sock = INVALID_SOCKET;
  sockaddr_in remote{};
  bool remote_ready = false;
  std::uint32_t button_mask = 0;
  std::mutex mutex;
};

constexpr char kVertexShaderSource[] = R"(
struct VSInput {
  float2 position : POSITION;
  float2 texcoord : TEXCOORD0;
};

struct PSInput {
  float4 position : SV_POSITION;
  float2 texcoord : TEXCOORD0;
};

PSInput main(VSInput input) {
  PSInput output;
  output.position = float4(input.position, 0.0f, 1.0f);
  output.texcoord = input.texcoord;
  return output;
}
)";

constexpr char kPixelShaderSource[] = R"(
Texture2D frame_texture : register(t0);
SamplerState frame_sampler : register(s0);

float4 main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) : SV_TARGET {
  return frame_texture.Sample(frame_sampler, texcoord);
}
)";

Args g_args;
AppState g_state;
Renderer g_renderer;
ControlClient g_control;
HWND g_hwnd = nullptr;
std::atomic<bool> g_stop = false;
std::atomic<bool> g_file_transfer_active = false;
std::atomic<bool> g_swallow_next_v_keyup = false;
std::atomic<std::uint64_t> g_file_transfer_sent_bytes = 0;
std::atomic<std::uint64_t> g_file_transfer_total_bytes = 0;

void ReleaseModifierKeys();
void SendKeyControl(const WPARAM wparam, const bool down);

void UpdateWindowTitle(HWND hwnd) {
  if (hwnd == nullptr) {
    return;
  }

  wchar_t title[256]{};
  if (g_file_transfer_active.load(std::memory_order_acquire)) {
    const auto sent = g_file_transfer_sent_bytes.load(std::memory_order_acquire);
    const auto total = g_file_transfer_total_bytes.load(std::memory_order_acquire);
    if (total > 0) {
      const auto percent = static_cast<unsigned>(std::min<std::uint64_t>(100, (sent * 100) / total));
      swprintf_s(
          title,
          L"LLK Rebuild Viewer - %ufps - file %u%% (%llu/%llu MB)",
          g_args.fps,
          percent,
          sent / (1024ull * 1024ull),
          total / (1024ull * 1024ull));
    } else {
      swprintf_s(title, L"LLK Rebuild Viewer - %ufps - file preparing", g_args.fps);
    }
  } else {
    swprintf_s(title, L"LLK Rebuild Viewer - %ufps", g_args.fps);
  }
  SetWindowTextW(hwnd, title);
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

bool SendAll(SOCKET sock, const void* data, const std::size_t total_size) {
  const auto* current = static_cast<const std::uint8_t*>(data);
  std::size_t sent_total = 0;
  while (sent_total < total_size) {
    const int sent = send(
        sock,
        reinterpret_cast<const char*>(current + sent_total),
        static_cast<int>(std::min<std::size_t>(total_size - sent_total, 64 * 1024)),
        0);
    if (sent <= 0) {
      return false;
    }
    sent_total += static_cast<std::size_t>(sent);
  }
  return true;
}

bool RecvAll(SOCKET sock, void* data, const std::size_t total_size) {
  auto* current = static_cast<std::uint8_t*>(data);
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

bool ConnectTransferSocket(SOCKET& sock) {
  sock = INVALID_SOCKET;

  addrinfoW hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfoW* result = nullptr;
  const std::wstring port = std::to_wstring(g_args.transfer_port);
  if (GetAddrInfoW(g_args.agent_host.c_str(), port.c_str(), &hints, &result) != 0) {
    return false;
  }

  for (auto* current = result; current != nullptr; current = current->ai_next) {
    SOCKET candidate = socket(current->ai_family, current->ai_socktype, current->ai_protocol);
    if (candidate == INVALID_SOCKET) {
      continue;
    }
    if (connect(candidate, current->ai_addr, static_cast<int>(current->ai_addrlen)) == 0) {
      DWORD timeout_ms = 120000;
      setsockopt(candidate, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
      setsockopt(candidate, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
      FreeAddrInfoW(result);
      sock = candidate;
      return true;
    }
    closesocket(candidate);
  }

  FreeAddrInfoW(result);
  return false;
}

bool GetClipboardTextUtf8(std::string& utf8) {
  utf8.clear();
  if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) {
    return false;
  }
  if (!OpenClipboard(nullptr)) {
    return false;
  }

  HANDLE data = GetClipboardData(CF_UNICODETEXT);
  if (data == nullptr) {
    CloseClipboard();
    return false;
  }

  const auto* text = static_cast<const wchar_t*>(GlobalLock(data));
  if (text == nullptr) {
    CloseClipboard();
    return false;
  }

  std::wstring value(text);
  GlobalUnlock(data);
  CloseClipboard();

  utf8 = WideToUtf8(value);
  return !utf8.empty();
}

std::vector<std::wstring> GetClipboardFiles() {
  std::vector<std::wstring> files;
  if (!IsClipboardFormatAvailable(CF_HDROP)) {
    return files;
  }
  if (!OpenClipboard(nullptr)) {
    return files;
  }

  HANDLE data = GetClipboardData(CF_HDROP);
  if (data == nullptr) {
    CloseClipboard();
    return files;
  }

  const HDROP drop = static_cast<HDROP>(data);
  const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  for (UINT index = 0; index < count; ++index) {
    const UINT length = DragQueryFileW(drop, index, nullptr, 0);
    std::wstring path(length + 1, L'\0');
    DragQueryFileW(drop, index, path.data(), length + 1);
    if (!path.empty() && path.back() == L'\0') {
      path.pop_back();
    }
    if (!path.empty()) {
      files.push_back(path);
    }
  }

  CloseClipboard();
  return files;
}

std::wstring BaseName(const std::wstring& path) {
  const std::size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) {
    return path;
  }
  return path.substr(slash + 1);
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

bool CollectUploadItems(const std::wstring& source_path, const std::wstring& remote_name, std::vector<UploadItem>& items) {
  const DWORD attributes = GetFileAttributesW(source_path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return false;
  }

  const std::wstring effective_remote_name = remote_name.empty() ? BaseName(source_path) : remote_name;
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    items.push_back({source_path, effective_remote_name, false});
    return true;
  }

  items.push_back({source_path, effective_remote_name, true});

  const std::wstring pattern = source_path + L"\\*";
  WIN32_FIND_DATAW find_data{};
  HANDLE find = FindFirstFileW(pattern.c_str(), &find_data);
  if (find == INVALID_HANDLE_VALUE) {
    return true;
  }

  bool ok = true;
  do {
    const wchar_t* name = find_data.cFileName;
    if (std::wcscmp(name, L".") == 0 || std::wcscmp(name, L"..") == 0) {
      continue;
    }
    const std::wstring child_local = JoinPathParts(source_path, name);
    const std::wstring child_remote = JoinPathParts(effective_remote_name, name);
    if (!CollectUploadItems(child_local, child_remote, items)) {
      ok = false;
      break;
    }
  } while (FindNextFileW(find, &find_data));

  FindClose(find);
  return ok;
}

bool SendTransferRequestOnSocket(
    SOCKET sock,
    const llk::transfer::Kind kind,
    const std::uint64_t transfer_id,
    const std::wstring& remote_name,
    const std::uint64_t offset_bytes,
    const void* payload,
    const std::size_t payload_bytes) {
  const std::string utf8_name = WideToUtf8(remote_name);
  if (utf8_name.size() > llk::transfer::kMaxNameBytes) {
    return false;
  }

  llk::transfer::Header header{};
  header.kind = static_cast<std::uint16_t>(kind);
  header.name_bytes = static_cast<std::uint32_t>(utf8_name.size());
  header.payload_bytes = payload_bytes;
  header.transfer_id = transfer_id;
  header.offset_bytes = offset_bytes;

  llk::transfer::Response response{};
  const bool ok = SendAll(sock, &header, sizeof(header)) &&
                  (utf8_name.empty() || SendAll(sock, utf8_name.data(), utf8_name.size())) &&
                  (payload_bytes == 0 || SendAll(sock, payload, payload_bytes)) &&
                  RecvAll(sock, &response, sizeof(response)) &&
                  response.status == llk::transfer::kStatusOk;
  return ok;
}

bool SendClipboardFileToRemote(
    SOCKET sock,
    const std::uint64_t transfer_id,
    const std::wstring& local_path,
    const std::wstring& remote_name,
    HWND hwnd,
    std::uint64_t& sent_bytes,
    const std::uint64_t total_bytes) {
  HANDLE file = CreateFileW(
      local_path.c_str(),
      GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      nullptr,
      OPEN_EXISTING,
      FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
      nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }

  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
    CloseHandle(file);
    return false;
  }

  const std::string utf8_name = WideToUtf8(remote_name);
  if (utf8_name.empty() || utf8_name.size() > llk::transfer::kMaxNameBytes) {
    CloseHandle(file);
    return false;
  }

  constexpr std::size_t kChunkBytes = 8 * 1024 * 1024;
  std::vector<std::uint8_t> buffer(kChunkBytes);
  std::uint64_t offset = 0;
  bool ok = true;

  if (size.QuadPart == 0) {
    ok = SendTransferRequestOnSocket(sock, llk::transfer::Kind::ClipboardFile, transfer_id, remote_name, 0, nullptr, 0);
  }

  while (ok) {
    DWORD read = 0;
    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
      ok = false;
      break;
    }
    if (read == 0) {
      break;
    }
    ok = SendTransferRequestOnSocket(
        sock,
        llk::transfer::Kind::ClipboardFile,
        transfer_id,
        remote_name,
        offset,
        buffer.data(),
        read);
    if (ok) {
      sent_bytes += read;
      g_file_transfer_sent_bytes.store(sent_bytes, std::memory_order_release);
      UpdateWindowTitle(hwnd);
    }
    offset += read;
  }

  CloseHandle(file);
  return ok;
}

bool ComputeUploadTotalBytes(
    const std::vector<UploadItem>& items,
    std::uint64_t& total_bytes,
    std::wstring& failing_path) {
  total_bytes = 0;
  failing_path.clear();

  for (const auto& item : items) {
    if (item.is_directory) {
      continue;
    }

    HANDLE file = CreateFileW(
        item.local_path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      failing_path = item.local_path;
      return false;
    }

    LARGE_INTEGER size{};
    const bool ok = GetFileSizeEx(file, &size) && size.QuadPart >= 0;
    CloseHandle(file);
    if (!ok) {
      failing_path = item.local_path;
      return false;
    }
    total_bytes += static_cast<std::uint64_t>(size.QuadPart);
  }

  return true;
}

bool SendClipboardFilesToRemote(HWND hwnd) {
  const auto files = GetClipboardFiles();
  if (files.empty()) {
    return false;
  }

  std::vector<UploadItem> items;
  for (const auto& file : files) {
    const std::wstring root_name = BaseName(file);
    if (!CollectUploadItems(file, root_name, items)) {
      ReleaseModifierKeys();
      std::wstring message = L"File paste failed:\n" + file;
      MessageBoxW(hwnd, message.c_str(), L"LLK Rebuild", MB_ICONWARNING | MB_OK);
      return false;
    }
  }

  std::uint64_t total_bytes = 0;
  std::wstring failing_path;
  if (!ComputeUploadTotalBytes(items, total_bytes, failing_path)) {
    ReleaseModifierKeys();
    std::wstring message = L"File paste failed:\n" + failing_path;
    MessageBoxW(hwnd, message.c_str(), L"LLK Rebuild", MB_ICONWARNING | MB_OK);
    return false;
  }
  g_file_transfer_total_bytes.store(total_bytes, std::memory_order_release);
  g_file_transfer_sent_bytes.store(0, std::memory_order_release);
  UpdateWindowTitle(hwnd);

  const std::uint64_t transfer_id = GetTickCount64();
  SOCKET sock = INVALID_SOCKET;
  if (!ConnectTransferSocket(sock)) {
    ReleaseModifierKeys();
    MessageBoxW(hwnd, L"Cannot connect to remote file clipboard.", L"LLK Rebuild", MB_ICONWARNING | MB_OK);
    return false;
  }

  if (!SendTransferRequestOnSocket(sock, llk::transfer::Kind::ClipboardReset, transfer_id, L"", 0, nullptr, 0)) {
    shutdown(sock, SD_BOTH);
    closesocket(sock);
    ReleaseModifierKeys();
    MessageBoxW(hwnd, L"Cannot prepare remote file clipboard.", L"LLK Rebuild", MB_ICONWARNING | MB_OK);
    return false;
  }

  std::uint64_t sent_bytes = 0;
  for (const auto& item : items) {
    bool ok = false;
    if (item.is_directory) {
      ok = SendTransferRequestOnSocket(sock, llk::transfer::Kind::ClipboardDirectory, transfer_id, item.remote_name, 0, nullptr, 0);
    } else {
      ok = SendClipboardFileToRemote(sock, transfer_id, item.local_path, item.remote_name, hwnd, sent_bytes, total_bytes);
    }

    if (!ok) {
      shutdown(sock, SD_BOTH);
      closesocket(sock);
      ReleaseModifierKeys();
      std::wstring message = L"File paste failed:\n" + item.local_path;
      MessageBoxW(hwnd, message.c_str(), L"LLK Rebuild", MB_ICONWARNING | MB_OK);
      return false;
    }
  }

  const bool committed = SendTransferRequestOnSocket(
          sock,
          llk::transfer::Kind::ClipboardCommit,
          transfer_id,
          L"",
          0,
          nullptr,
          0);
  shutdown(sock, SD_BOTH);
  closesocket(sock);
  if (!committed) {
    ReleaseModifierKeys();
    MessageBoxW(hwnd, L"Cannot finalize remote file clipboard.", L"LLK Rebuild", MB_ICONWARNING | MB_OK);
    return false;
  }

  return true;
}

void StartClipboardFileTransfer(HWND hwnd) {
  if (g_file_transfer_active.exchange(true, std::memory_order_acq_rel)) {
    return;
  }

  g_swallow_next_v_keyup.store(true, std::memory_order_release);
  g_file_transfer_sent_bytes.store(0, std::memory_order_release);
  g_file_transfer_total_bytes.store(0, std::memory_order_release);
  UpdateWindowTitle(hwnd);
  ReleaseModifierKeys();

  std::thread([hwnd]() {
    const bool ok = SendClipboardFilesToRemote(hwnd);
    g_file_transfer_active.store(false, std::memory_order_release);
    g_file_transfer_sent_bytes.store(0, std::memory_order_release);
    g_file_transfer_total_bytes.store(0, std::memory_order_release);
    if (IsWindow(hwnd)) {
      UpdateWindowTitle(hwnd);
    }
    if (!ok) {
      ReleaseModifierKeys();
    }
  }).detach();
}


bool SendClipboardTextToRemote(HWND hwnd) {
  std::string utf8;
  if (!GetClipboardTextUtf8(utf8)) {
    return false;
  }
  if (utf8.size() > llk::transfer::kMaxClipboardBytes) {
    MessageBoxW(hwnd, L"Clipboard text is too large.", L"LLK Rebuild", MB_ICONWARNING | MB_OK);
    return false;
  }

  SOCKET sock = INVALID_SOCKET;
  if (!ConnectTransferSocket(sock)) {
    MessageBoxW(hwnd, L"Cannot connect to remote transfer port.", L"LLK Rebuild", MB_ICONWARNING | MB_OK);
    return false;
  }

  llk::transfer::Header header{};
  header.kind = static_cast<std::uint16_t>(llk::transfer::Kind::ClipboardText);
  header.payload_bytes = utf8.size();

  llk::transfer::Response response{};
  const bool ok = SendAll(sock, &header, sizeof(header)) &&
                  (utf8.empty() || SendAll(sock, utf8.data(), utf8.size())) &&
                  shutdown(sock, SD_SEND) == 0 &&
                  RecvAll(sock, &response, sizeof(response)) &&
                  response.status == llk::transfer::kStatusOk;
  shutdown(sock, SD_BOTH);
  closesocket(sock);
  return ok;
}


Args ParseArgs() {
  Args args;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) {
    return args;
  }
  for (int i = 1; i + 1 < argc; ++i) {
    const std::wstring_view key = argv[i];
    const std::wstring_view value = argv[i + 1];
    if (key == L"--agent-host") {
      args.agent_host = std::wstring(value);
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
      } else if (key == L"--width") {
        args.width = static_cast<std::uint32_t>(std::stoul(std::wstring(value)));
        ++i;
    } else if (key == L"--height") {
      args.height = static_cast<std::uint32_t>(std::stoul(std::wstring(value)));
      ++i;
    } else if (key == L"--fps") {
      args.fps = static_cast<std::uint32_t>(std::stoul(std::wstring(value)));
      ++i;
    } else if (key == L"--ffmpeg") {
      args.ffmpeg_path = std::wstring(value);
      ++i;
    }
  }
  LocalFree(argv);
  return args;
}

bool ComputeFrameLayout(
    const int client_width,
    const int client_height,
    const int frame_width,
    const int frame_height,
    FrameLayout& layout) {
  if (client_width <= 0 || client_height <= 0 || frame_width <= 0 || frame_height <= 0) {
    return false;
  }

  const double scale_x = static_cast<double>(client_width) / frame_width;
  const double scale_y = static_cast<double>(client_height) / frame_height;
  const double scale = std::min(scale_x, scale_y);
  layout.dest_width = std::max(1, static_cast<int>(frame_width * scale));
  layout.dest_height = std::max(1, static_cast<int>(frame_height * scale));
  layout.dest_x = (client_width - layout.dest_width) / 2;
  layout.dest_y = (client_height - layout.dest_height) / 2;
  return true;
}

void RequestRepaint() {
  if (g_hwnd != nullptr && !g_state.redraw_posted.exchange(true, std::memory_order_acq_rel)) {
    PostMessageW(g_hwnd, kFrameReadyMessage, 0, 0);
  }
}

bool MapClientPointToRemote(HWND hwnd, POINT point, int& remote_x, int& remote_y) {
  RECT rect{};
  GetClientRect(hwnd, &rect);
  const int client_width = rect.right - rect.left;
  const int client_height = rect.bottom - rect.top;
  FrameLayout layout{};
  if (!ComputeFrameLayout(client_width, client_height, static_cast<int>(g_args.width), static_cast<int>(g_args.height), layout)) {
    return false;
  }
  const int local_x = std::clamp<int>(point.x - layout.dest_x, 0, layout.dest_width - 1);
  const int local_y = std::clamp<int>(point.y - layout.dest_y, 0, layout.dest_height - 1);
  remote_x = std::clamp<int>(MulDiv(local_x, static_cast<int>(g_args.width), layout.dest_width), 0, static_cast<int>(g_args.width) - 1);
  remote_y = std::clamp<int>(MulDiv(local_y, static_cast<int>(g_args.height), layout.dest_height), 0, static_cast<int>(g_args.height) - 1);
  return true;
}

template <typename T>
void SendControlPacket(const llk::proto::ControlKind kind, const T& payload) {
  std::scoped_lock lock(g_control.mutex);
  if (g_control.sock == INVALID_SOCKET || !g_control.remote_ready) {
    return;
  }

  std::uint8_t buffer[sizeof(llk::proto::ControlHeader) + sizeof(T)]{};
  llk::proto::ControlHeader header{};
  header.kind = static_cast<std::uint16_t>(kind);
  std::memcpy(buffer, &header, sizeof(header));
  std::memcpy(buffer + sizeof(header), &payload, sizeof(payload));
  sendto(
      g_control.sock,
      reinterpret_cast<const char*>(buffer),
      sizeof(buffer),
      0,
      reinterpret_cast<const sockaddr*>(&g_control.remote),
      sizeof(g_control.remote));
}

void SendPointerControl(HWND hwnd, POINT point, const int wheel_delta = 0) {
  int remote_x = 0;
  int remote_y = 0;
  if (!MapClientPointToRemote(hwnd, point, remote_x, remote_y)) {
    return;
  }

  llk::proto::PointerPayload payload{};
  payload.x = remote_x;
  payload.y = remote_y;
  payload.wheel_delta = wheel_delta;
  {
    std::scoped_lock lock(g_control.mutex);
    payload.button_mask = g_control.button_mask;
  }
  SendControlPacket(llk::proto::ControlKind::Pointer, payload);
}

void UpdateButtonMask(const std::uint32_t mask, const bool down) {
  std::scoped_lock lock(g_control.mutex);
  if (down) {
    g_control.button_mask |= mask;
  } else {
    g_control.button_mask &= ~mask;
  }
}

void SendKeyControl(const WPARAM wparam, const bool down) {
  llk::proto::KeyPayload payload{};
  payload.virtual_key = static_cast<std::uint32_t>(wparam);
  payload.down = down ? 1u : 0u;
  SendControlPacket(llk::proto::ControlKind::Key, payload);
}

void ReleaseModifierKeys() {
  SendKeyControl(VK_CONTROL, false);
  SendKeyControl(VK_SHIFT, false);
  SendKeyControl(VK_MENU, false);
  SendKeyControl(VK_LWIN, false);
  SendKeyControl(VK_RWIN, false);
}

bool CreateSwapChainRenderTarget() {
  if (!g_renderer.swap_chain || !g_renderer.device) {
    return false;
  }
  ComPtr<ID3D11Texture2D> back_buffer;
  if (FAILED(g_renderer.swap_chain->GetBuffer(0, IID_PPV_ARGS(back_buffer.ReleaseAndGetAddressOf())))) {
    return false;
  }
  return SUCCEEDED(g_renderer.device->CreateRenderTargetView(
      back_buffer.Get(),
      nullptr,
      g_renderer.render_target_view.ReleaseAndGetAddressOf()));
}

void ShutdownRenderer() {
  g_renderer.frame_srv.Reset();
  g_renderer.frame_texture.Reset();
  g_renderer.sampler_state.Reset();
  g_renderer.vertex_buffer.Reset();
  g_renderer.input_layout.Reset();
  g_renderer.pixel_shader.Reset();
  g_renderer.vertex_shader.Reset();
  g_renderer.render_target_view.Reset();
  g_renderer.swap_chain.Reset();
  g_renderer.context.Reset();
  g_renderer.device.Reset();
  g_renderer.frame_texture_width = 0;
  g_renderer.frame_texture_height = 0;
  g_renderer.uploaded_frame_count = 0;
}

bool CreateRendererPipeline() {
  ComPtr<ID3DBlob> vertex_blob;
  ComPtr<ID3DBlob> pixel_blob;
  ComPtr<ID3DBlob> error_blob;

  if (FAILED(D3DCompile(
          kVertexShaderSource,
          sizeof(kVertexShaderSource) - 1,
          nullptr,
          nullptr,
          nullptr,
          "main",
          "vs_4_0",
          0,
          0,
          vertex_blob.ReleaseAndGetAddressOf(),
          error_blob.ReleaseAndGetAddressOf()))) {
    return false;
  }

  if (FAILED(D3DCompile(
          kPixelShaderSource,
          sizeof(kPixelShaderSource) - 1,
          nullptr,
          nullptr,
          nullptr,
          "main",
          "ps_4_0",
          0,
          0,
          pixel_blob.ReleaseAndGetAddressOf(),
          error_blob.ReleaseAndGetAddressOf()))) {
    return false;
  }

  if (FAILED(g_renderer.device->CreateVertexShader(
          vertex_blob->GetBufferPointer(),
          vertex_blob->GetBufferSize(),
          nullptr,
          g_renderer.vertex_shader.ReleaseAndGetAddressOf())) ||
      FAILED(g_renderer.device->CreatePixelShader(
          pixel_blob->GetBufferPointer(),
          pixel_blob->GetBufferSize(),
          nullptr,
          g_renderer.pixel_shader.ReleaseAndGetAddressOf()))) {
    return false;
  }

  const D3D11_INPUT_ELEMENT_DESC layout_desc[] = {
      {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(D3DVertex, position), D3D11_INPUT_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(D3DVertex, texcoord), D3D11_INPUT_PER_VERTEX_DATA, 0},
  };
  if (FAILED(g_renderer.device->CreateInputLayout(
          layout_desc,
          static_cast<UINT>(std::size(layout_desc)),
          vertex_blob->GetBufferPointer(),
          vertex_blob->GetBufferSize(),
          g_renderer.input_layout.ReleaseAndGetAddressOf()))) {
    return false;
  }

  D3D11_BUFFER_DESC buffer_desc{};
  buffer_desc.ByteWidth = sizeof(D3DVertex) * 6;
  buffer_desc.Usage = D3D11_USAGE_DYNAMIC;
  buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
  buffer_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(g_renderer.device->CreateBuffer(
          &buffer_desc,
          nullptr,
          g_renderer.vertex_buffer.ReleaseAndGetAddressOf()))) {
    return false;
  }

  D3D11_SAMPLER_DESC sampler_desc{};
  sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler_desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
  sampler_desc.MinLOD = 0;
  sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
  if (FAILED(g_renderer.device->CreateSamplerState(
          &sampler_desc,
          g_renderer.sampler_state.ReleaseAndGetAddressOf()))) {
    return false;
  }

  return true;
}

bool InitializeRenderer(HWND hwnd) {
  const D3D_FEATURE_LEVEL levels[] = {
      D3D_FEATURE_LEVEL_11_0,
      D3D_FEATURE_LEVEL_10_1,
      D3D_FEATURE_LEVEL_10_0,
  };

  DXGI_SWAP_CHAIN_DESC desc{};
  desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.OutputWindow = hwnd;
  desc.Windowed = TRUE;
  desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

  D3D_FEATURE_LEVEL created = D3D_FEATURE_LEVEL_10_0;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(
      nullptr,
      D3D_DRIVER_TYPE_HARDWARE,
      nullptr,
      D3D11_CREATE_DEVICE_BGRA_SUPPORT,
      levels,
      static_cast<UINT>(std::size(levels)),
      D3D11_SDK_VERSION,
      &desc,
      g_renderer.swap_chain.ReleaseAndGetAddressOf(),
      g_renderer.device.ReleaseAndGetAddressOf(),
      &created,
      g_renderer.context.ReleaseAndGetAddressOf());
  if (FAILED(hr)) {
    hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_WARP,
        nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels,
        static_cast<UINT>(std::size(levels)),
        D3D11_SDK_VERSION,
        &desc,
        g_renderer.swap_chain.ReleaseAndGetAddressOf(),
        g_renderer.device.ReleaseAndGetAddressOf(),
        &created,
        g_renderer.context.ReleaseAndGetAddressOf());
  }
  if (FAILED(hr)) {
    ShutdownRenderer();
    return false;
  }
  if (!CreateSwapChainRenderTarget() || !CreateRendererPipeline()) {
    ShutdownRenderer();
    return false;
  }
  return true;
}

bool ResizeRenderer(const UINT width, const UINT height) {
  if (!g_renderer.swap_chain || !g_renderer.context || width == 0 || height == 0) {
    return true;
  }
  g_renderer.context->OMSetRenderTargets(0, nullptr, nullptr);
  g_renderer.context->ClearState();
  g_renderer.render_target_view.Reset();
  if (FAILED(g_renderer.swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0))) {
    return false;
  }
  return CreateSwapChainRenderTarget();
}

bool EnsureFrameTexture(const std::uint32_t width, const std::uint32_t height) {
  if (!g_renderer.device || !g_renderer.context || width == 0 || height == 0) {
    return false;
  }
  if (g_renderer.frame_texture &&
      g_renderer.frame_srv &&
      g_renderer.frame_texture_width == width &&
      g_renderer.frame_texture_height == height) {
    return true;
  }

  g_renderer.frame_srv.Reset();
  g_renderer.frame_texture.Reset();

  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(g_renderer.device->CreateTexture2D(
          &desc,
          nullptr,
          g_renderer.frame_texture.ReleaseAndGetAddressOf()))) {
    return false;
  }

  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
  srv_desc.Format = desc.Format;
  srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  srv_desc.Texture2D.MostDetailedMip = 0;
  srv_desc.Texture2D.MipLevels = 1;
  if (FAILED(g_renderer.device->CreateShaderResourceView(
          g_renderer.frame_texture.Get(),
          &srv_desc,
          g_renderer.frame_srv.ReleaseAndGetAddressOf()))) {
    g_renderer.frame_texture.Reset();
    return false;
  }

  g_renderer.frame_texture_width = width;
  g_renderer.frame_texture_height = height;
  return true;
}

bool UpdateFrameVertices(const int client_width, const int client_height) {
  FrameLayout layout{};
  if (!ComputeFrameLayout(
          client_width,
          client_height,
          static_cast<int>(g_args.width),
          static_cast<int>(g_args.height),
          layout)) {
    return false;
  }

  const float left = (static_cast<float>(layout.dest_x) / client_width) * 2.0f - 1.0f;
  const float right = (static_cast<float>(layout.dest_x + layout.dest_width) / client_width) * 2.0f - 1.0f;
  const float top = 1.0f - (static_cast<float>(layout.dest_y) / client_height) * 2.0f;
  const float bottom = 1.0f - (static_cast<float>(layout.dest_y + layout.dest_height) / client_height) * 2.0f;

  const D3DVertex vertices[6] = {
      {{left, top}, {0.0f, 0.0f}},
      {{right, top}, {1.0f, 0.0f}},
      {{left, bottom}, {0.0f, 1.0f}},
      {{left, bottom}, {0.0f, 1.0f}},
      {{right, top}, {1.0f, 0.0f}},
      {{right, bottom}, {1.0f, 1.0f}},
  };

  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(g_renderer.context->Map(
          g_renderer.vertex_buffer.Get(),
          0,
          D3D11_MAP_WRITE_DISCARD,
          0,
          &mapped))) {
    return false;
  }
  std::memcpy(mapped.pData, vertices, sizeof(vertices));
  g_renderer.context->Unmap(g_renderer.vertex_buffer.Get(), 0);
  return true;
}

void RenderViewer(HWND hwnd) {
  if (!g_renderer.context || !g_renderer.swap_chain || !g_renderer.render_target_view) {
    return;
  }

  RECT rect{};
  GetClientRect(hwnd, &rect);
  const int client_width = rect.right - rect.left;
  const int client_height = rect.bottom - rect.top;
  if (client_width <= 0 || client_height <= 0) {
    return;
  }

  const float clear_color[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  ID3D11RenderTargetView* targets[] = {g_renderer.render_target_view.Get()};
  g_renderer.context->OMSetRenderTargets(1, targets, nullptr);

  D3D11_VIEWPORT viewport{};
  viewport.Width = static_cast<FLOAT>(client_width);
  viewport.Height = static_cast<FLOAT>(client_height);
  viewport.MinDepth = 0.0f;
  viewport.MaxDepth = 1.0f;
  g_renderer.context->RSSetViewports(1, &viewport);
  g_renderer.context->ClearRenderTargetView(g_renderer.render_target_view.Get(), clear_color);

  const auto latest_frame_count = g_state.frames.load();
  if (latest_frame_count != 0 && latest_frame_count != g_renderer.uploaded_frame_count) {
    std::scoped_lock lock(g_state.frame_mutex);
    if (!g_state.frame_bgra.empty() &&
        EnsureFrameTexture(g_args.width, g_args.height)) {
      g_renderer.context->UpdateSubresource(
          g_renderer.frame_texture.Get(),
          0,
          nullptr,
          g_state.frame_bgra.data(),
          g_args.width * 4,
          0);
      g_renderer.uploaded_frame_count = latest_frame_count;
    }
  }

  if (g_renderer.frame_srv &&
      g_renderer.vertex_shader &&
      g_renderer.pixel_shader &&
      g_renderer.input_layout &&
      g_renderer.sampler_state &&
      UpdateFrameVertices(client_width, client_height)) {
    const UINT stride = sizeof(D3DVertex);
    const UINT offset = 0;
    ID3D11Buffer* vertex_buffers[] = {g_renderer.vertex_buffer.Get()};
    ID3D11ShaderResourceView* shader_resources[] = {g_renderer.frame_srv.Get()};
    ID3D11SamplerState* samplers[] = {g_renderer.sampler_state.Get()};

    g_renderer.context->IASetInputLayout(g_renderer.input_layout.Get());
    g_renderer.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_renderer.context->IASetVertexBuffers(0, 1, vertex_buffers, &stride, &offset);
    g_renderer.context->VSSetShader(g_renderer.vertex_shader.Get(), nullptr, 0);
    g_renderer.context->PSSetShader(g_renderer.pixel_shader.Get(), nullptr, 0);
    g_renderer.context->PSSetShaderResources(0, 1, shader_resources);
    g_renderer.context->PSSetSamplers(0, 1, samplers);
    g_renderer.context->Draw(6, 0);
    ID3D11ShaderResourceView* null_srvs[] = {nullptr};
    g_renderer.context->PSSetShaderResources(0, 1, null_srvs);
  }

  g_renderer.swap_chain->Present(0, 0);
}

bool ReadExact(HANDLE pipe, std::uint8_t* buffer, const std::size_t total_bytes) {
  std::size_t offset = 0;
  while (offset < total_bytes && !g_stop.load()) {
    DWORD read = 0;
    if (!ReadFile(pipe, buffer + offset, static_cast<DWORD>(total_bytes - offset), &read, nullptr) || read == 0) {
      return false;
    }
    offset += read;
  }
  return offset == total_bytes;
}

void VideoThreadProc() {
  const std::wstring ffmpeg = DetectFfmpegPath(g_args.ffmpeg_path);
  if (ffmpeg.empty()) {
    return;
  }

  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE stdout_read = nullptr;
  HANDLE stdout_write = nullptr;
  if (!CreatePipe(&stdout_read, &stdout_write, &sa, 0)) {
    return;
  }
  SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);

  const std::wstring stderr_path = GetExecutableDirectory() + L"\\viewer_ffmpeg_stderr.txt";
  SECURITY_ATTRIBUTES file_sa{};
  file_sa.nLength = sizeof(file_sa);
  file_sa.bInheritHandle = TRUE;
  HANDLE stderr_file = CreateFileW(
      stderr_path.c_str(),
      GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE,
      &file_sa,
      CREATE_ALWAYS,
      FILE_ATTRIBUTE_NORMAL,
      nullptr);
  if (stderr_file == INVALID_HANDLE_VALUE) {
    CloseHandle(stdout_read);
    CloseHandle(stdout_write);
    return;
  }

  const std::wstring input =
      L"udp://0.0.0.0:" + std::to_wstring(g_args.video_port) +
      L"?fifo_size=1000000&overrun_nonfatal=1&buffer_size=65535";
  const std::wstring vf =
      L"scale=" + std::to_wstring(g_args.width) + L":" + std::to_wstring(g_args.height) + L":flags=fast_bilinear";
  std::wstring command =
      L"\"" + ffmpeg + L"\" -hide_banner -loglevel info -fflags nobuffer -flags low_delay"
      L" -analyzeduration 2000000 -probesize 5000000 -i \"" + input +
      L"\" -an -vf \"" + vf + L"\" -pix_fmt bgra -f rawvideo pipe:1";

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = stdout_write;
  si.hStdError = stderr_file;

  PROCESS_INFORMATION pi{};
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

  CloseHandle(stdout_write);
  CloseHandle(stderr_file);
  if (!ok) {
    CloseHandle(stdout_read);
    return;
  }

  const std::size_t frame_bytes = static_cast<std::size_t>(g_args.width) * g_args.height * 4u;
  std::vector<std::uint8_t> frame(frame_bytes);
  while (!g_stop.load()) {
    if (!ReadExact(stdout_read, frame.data(), frame_bytes)) {
      break;
    }
    {
      std::scoped_lock lock(g_state.frame_mutex);
      g_state.frame_bgra = frame;
      g_state.frame_width = g_args.width;
      g_state.frame_height = g_args.height;
    }
    const auto frame_count = g_state.frames.fetch_add(1) + 1;
    if (g_hwnd != nullptr && frame_count == 1) {
      wchar_t title[128]{};
      swprintf_s(title, L"LLK Rebuild Viewer - %ufps", g_args.fps);
      SetWindowTextW(g_hwnd, title);
    }
    RequestRepaint();
  }

  TerminateProcess(pi.hProcess, 0);
  WaitForSingleObject(pi.hProcess, 2000);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(stdout_read);
}

void InitializeControlClient() {
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
    return;
  }

  g_control.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (g_control.sock == INVALID_SOCKET) {
    return;
  }

  g_control.remote.sin_family = AF_INET;
  g_control.remote.sin_port = htons(g_args.control_port);
  if (InetPtonW(AF_INET, g_args.agent_host.c_str(), &g_control.remote.sin_addr) == 1) {
    g_control.remote_ready = true;
  }
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      if (wparam != SIZE_MINIMIZED) {
        ResizeRenderer(static_cast<UINT>(LOWORD(lparam)), static_cast<UINT>(HIWORD(lparam)));
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    case WM_MOUSEMOVE: {
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      SendPointerControl(hwnd, point);
      return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN: {
      const std::uint32_t mask =
          message == WM_LBUTTONDOWN ? 1u : (message == WM_MBUTTONDOWN ? 2u : 4u);
      UpdateButtonMask(mask, true);
      SetCapture(hwnd);
      SetFocus(hwnd);
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      SendPointerControl(hwnd, point);
      return 0;
    }
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP: {
      const std::uint32_t mask =
          message == WM_LBUTTONUP ? 1u : (message == WM_MBUTTONUP ? 2u : 4u);
      UpdateButtonMask(mask, false);
      if ((wparam & (MK_LBUTTON | MK_MBUTTON | MK_RBUTTON)) == 0) {
        ReleaseCapture();
      }
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      SendPointerControl(hwnd, point);
      return 0;
    }
    case WM_MOUSEWHEEL: {
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(hwnd, &point);
      SendPointerControl(hwnd, point, GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA);
      return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
      if ((wparam == 'V' || wparam == 'v') && (GetKeyState(VK_CONTROL) & 0x8000)) {
        if (IsClipboardFormatAvailable(CF_HDROP)) {
          StartClipboardFileTransfer(hwnd);
          return 0;
        } else {
          std::string clipboard_text_utf8;
          if (GetClipboardTextUtf8(clipboard_text_utf8)) {
          SendClipboardTextToRemote(hwnd);
          }
        }
      }
      SendKeyControl(wparam, true);
      return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
      if ((wparam == 'V' || wparam == 'v') && g_swallow_next_v_keyup.exchange(false, std::memory_order_acq_rel)) {
        return 0;
      }
      SendKeyControl(wparam, false);
      return 0;
    case WM_KILLFOCUS:
      ReleaseModifierKeys();
      return 0;
    case WM_ACTIVATEAPP:
      if (wparam == FALSE) {
        ReleaseModifierKeys();
      }
      return 0;
    case kFrameReadyMessage:
      g_state.redraw_posted.store(false, std::memory_order_release);
      InvalidateRect(hwnd, nullptr, FALSE);
      return 0;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd, &ps);
      RenderViewer(hwnd);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_DESTROY:
      ReleaseModifierKeys();
      g_stop.store(true);
      g_hwnd = nullptr;
      ShutdownRenderer();
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wparam, lparam);
  }
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show_command) {
  g_args = ParseArgs();
  InitializeControlClient();

  WNDCLASSW window_class{};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kWindowClassName;
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  RegisterClassW(&window_class);

  HWND hwnd = CreateWindowExW(
      0,
      kWindowClassName,
      L"LLK Rebuild Viewer",
      WS_OVERLAPPEDWINDOW | WS_VISIBLE,
      CW_USEDEFAULT,
      CW_USEDEFAULT,
      1280,
      720,
      nullptr,
      nullptr,
      instance,
      nullptr);
  if (hwnd == nullptr) {
    return 1;
  }
  g_hwnd = hwnd;
  if (!InitializeRenderer(hwnd)) {
    return 1;
  }

  ShowWindow(hwnd, show_command);
  UpdateWindow(hwnd);

  std::thread video_thread(VideoThreadProc);

  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }

  g_stop.store(true);
  if (video_thread.joinable()) {
    video_thread.join();
  }
  if (g_control.sock != INVALID_SOCKET) {
    closesocket(g_control.sock);
    g_control.sock = INVALID_SOCKET;
  }
  WSACleanup();
  return 0;
}
