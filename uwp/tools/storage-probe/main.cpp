#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>
#include <atomic>
#include <array>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

using namespace winrt;
using namespace Windows::ApplicationModel::Core;
using namespace Windows::UI::Core;
namespace {
std::atomic<int> status{0}; // amber: running; green: all succeeded; red: inspect log
constexpr uint64_t ballastSize = 512ull * 1024 * 1024;
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(Handle const&) = delete;
    Handle& operator=(Handle const&) = delete;
    explicit operator bool() const { return value != INVALID_HANDLE_VALUE; }
};
struct Probe {
    std::ostringstream log;
    bool ok = true;
    void error(char const* operation, DWORD code) {
        ok = false;
        log << operation << " failed: GetLastError=" << code << " (0x"
            << std::hex << code << std::dec << ")\n";
    }
    void exception(char const* operation, hresult_error const& e) {
        ok = false;
        log << operation << " failed: HRESULT=0x" << std::hex
            << static_cast<uint32_t>(e.code().value) << std::dec
            << " " << to_string(e.message()) << '\n';
    }
    static HANDLE open(std::wstring const& path, DWORD access, DWORD disposition, DWORD flags = FILE_ATTRIBUTE_NORMAL) {
        CREATEFILE2_EXTENDED_PARAMETERS params{};
        params.dwSize = sizeof(params);
        params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        params.dwFileFlags = flags & ~FILE_ATTRIBUTE_NORMAL;
        return CreateFile2(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            disposition, &params);
    }
    bool write(HANDLE file, void const* bytes, DWORD count) {
        auto p = static_cast<char const*>(bytes);
        while (count) {
            DWORD written = 0;
            if (!WriteFile(file, p, count, &written, nullptr)) { error("WriteFile", GetLastError()); return false; }
            if (!written) { error("WriteFile (zero progress)", ERROR_WRITE_FAULT); return false; }
            p += written;
            count -= written;
        }
        return true;
    }
    void finalPath(HANDLE directory, DWORD mode, char const* label) {
        std::vector<wchar_t> buffer(32768);
        DWORD n = GetFinalPathNameByHandleW(directory, buffer.data(), static_cast<DWORD>(buffer.size()), mode);
        if (!n) error(label, GetLastError());
        else if (n >= buffer.size()) error(label, ERROR_INSUFFICIENT_BUFFER);
        else log << label << "=" << to_string(std::wstring_view(buffer.data(), n)) << '\n';
    }
    void measure(char const* label, std::wstring const& path) {
        log << "\n[" << label << "] path=" << to_string(path) << '\n';
        ULARGE_INTEGER caller{}, total{}, free{};
        if (GetDiskFreeSpaceExW(path.c_str(), &caller, &total, &free)) {
            log << "bytes: caller_free=" << caller.QuadPart << " total=" << total.QuadPart << " total_free=" << free.QuadPart
                << "\nMiB: caller_free=" << caller.QuadPart / 1048576 << " total=" << total.QuadPart / 1048576
                << " total_free=" << free.QuadPart / 1048576 << '\n';
        } else error("GetDiskFreeSpaceExW", GetLastError());
        Handle directory(open(path, FILE_READ_ATTRIBUTES, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS));
        if (!directory) { error("CreateFile2(directory)", GetLastError()); return; }
        FILE_ID_INFO id{};
        if (GetFileInformationByHandleEx(directory.value, FileIdInfo, &id, sizeof(id)))
            log << "VolumeSerialNumber=0x" << std::hex << id.VolumeSerialNumber << std::dec << '\n';
        else error("GetFileInformationByHandleEx(FileIdInfo)", GetLastError());
        finalPath(directory.value, VOLUME_NAME_NT, "GetFinalPathNameByHandleW(NT)");
        finalPath(directory.value, VOLUME_NAME_GUID, "GetFinalPathNameByHandleW(GUID)");
    }
    void ballast(std::wstring const& path) {
        // OPEN_EXISTING distinguishes absence from access errors. A partial file is deleted next launch too.
        bool exists = false;
        {
            Handle file(open(path, FILE_READ_ATTRIBUTES, OPEN_EXISTING));
            if (file) exists = true;
            else {
                DWORD code = GetLastError();
                if (code != ERROR_FILE_NOT_FOUND) { error("CreateFile2(ballast existence)", code); return; }
            }
        }
        if (exists) {
            if (DeleteFileW(path.c_str())) log << "\nAction: deleted ballast.bin\n";
            else error("DeleteFileW(ballast)", GetLastError());
            return;
        }
        log << "\nAction: creating 512 MiB ballast.bin (real writes, no sparse allocation)\n";
        Handle file(open(path, GENERIC_WRITE, CREATE_NEW));
        if (!file) { error("CreateFile2(ballast)", GetLastError()); return; }
        std::vector<unsigned char> chunk(1024 * 1024);
        // Nonzero, deterministic contents avoid relying on zero-filled extension or compression.
        uint32_t random = 0x12345678;
        for (auto& byte : chunk) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; byte = static_cast<unsigned char>(random); }
        uint64_t written = 0;
        while (written < ballastSize) {
            if (!write(file.value, chunk.data(), static_cast<DWORD>(chunk.size()))) break;
            written += chunk.size();
        }
        if (!FlushFileBuffers(file.value)) error("FlushFileBuffers(ballast)", GetLastError());
        log << "Completed ballast chunks: " << written << " bytes; target=" << ballastSize
            << ". Any partial file is removed on next launch.\n";
    }
    bool append(std::wstring const& path, std::string const& text) {
        Handle file(open(path, FILE_APPEND_DATA, OPEN_ALWAYS));
        if (!file) { error("CreateFile2(log)", GetLastError()); return false; }
        if (!write(file.value, text.data(), static_cast<DWORD>(text.size()))) return false;
        if (!FlushFileBuffers(file.value)) { error("FlushFileBuffers(log)", GetLastError()); return false; }
        return true;
    }
    void run() {
        SYSTEMTIME now{};
        GetSystemTime(&now);
        log << "\n=== Storage probe UTC " << now.wYear << '-' << std::setfill('0') << std::setw(2) << now.wMonth
            << '-' << std::setw(2) << now.wDay << 'T' << std::setw(2) << now.wHour << ':' << std::setw(2)
            << now.wMinute << ':' << std::setw(2) << now.wSecond << "Z ===\n";
        std::array<std::pair<char const*, std::wstring>, 5> paths{{
            {"LocalState", {}}, {"LocalCacheFolder", {}}, {"TemporaryFolder", {}},
            {"InstalledLocation", {}}, {"USB", L"E:\\ringracers"}}};
        try { paths[0].second = Windows::Storage::ApplicationData::Current().LocalFolder().Path(); }
        catch (hresult_error const& e) { exception("LocalFolder", e); }
        try { paths[1].second = Windows::Storage::ApplicationData::Current().LocalCacheFolder().Path(); }
        catch (hresult_error const& e) { exception("LocalCacheFolder", e); }
        try { paths[2].second = Windows::Storage::ApplicationData::Current().TemporaryFolder().Path(); }
        catch (hresult_error const& e) { exception("TemporaryFolder", e); }
        try { paths[3].second = Windows::ApplicationModel::Package::Current().InstalledLocation().Path(); }
        catch (hresult_error const& e) { exception("InstalledLocation", e); }
        auto const& local = paths[0].second;
        if (!local.empty()) {
            Handle counter(open(local + L"\\launch-counter.bin", GENERIC_READ | GENERIC_WRITE, OPEN_ALWAYS));
            uint64_t count = 0;
            DWORD read = 0;
            if (!counter) error("CreateFile2(counter)", GetLastError());
            else {
                if (!ReadFile(counter.value, &count, sizeof(count), &read, nullptr)) error("ReadFile(counter)", GetLastError());
                else if (read != 0 && read != sizeof(count)) { error("ReadFile(counter size)", ERROR_INVALID_DATA); count = 0; }
                ++count;
                log << "Launch counter=" << count << '\n';
                LARGE_INTEGER start{};
                if (!SetFilePointerEx(counter.value, start, nullptr, FILE_BEGIN)) error("SetFilePointerEx(counter)", GetLastError());
                else if (write(counter.value, &count, sizeof(count)) && !FlushFileBuffers(counter.value)) error("FlushFileBuffers(counter)", GetLastError());
            }
        }
        log << "\n--- BEFORE ---\n";
        for (auto const& entry : paths) if (!entry.second.empty()) measure(entry.first, entry.second);
        if (!local.empty()) ballast(local + L"\\ballast.bin");
        else log << "Ballast unavailable: no LocalState path.\n";
        log << "\n--- AFTER (ballast handle closed) ---\n";
        for (auto const& entry : paths) if (!entry.second.empty()) measure(entry.first, entry.second);
        log << "\nMeasurements/experiment: " << (ok ? "OK" : "errors; see above") << '\n';
        log << "USB log destination=E:\\ringracers\\storage-probe.txt\n";
        bool usb = append(L"E:\\ringracers\\storage-probe.txt", log.str());
        log << "USB append: " << (usb ? "OK" : "FAILED") << '\n';
        if (!local.empty()) {
            log << "Local fallback destination=" << to_string(local) << "\\storage-probe.txt\n";
            append(local + L"\\storage-probe.txt", log.str());
        }
        OutputDebugStringW(to_hstring(log.str()).c_str());
        status = ok ? 1 : -1;
    }
};

struct App : implements<App, IFrameworkViewSource, IFrameworkView> {
    CoreWindow window{nullptr};
    bool closed = false;
    com_ptr<ID3D11Device> device;
    com_ptr<ID3D11DeviceContext> context;
    com_ptr<IDXGISwapChain1> swap;
    com_ptr<ID3D11RenderTargetView> target;
    IFrameworkView CreateView() { return *this; }
    void Initialize(CoreApplicationView const& view) {
        view.Activated([this](auto const&, auto const&) { window.Activate(); });
    }
    void SetWindow(CoreWindow const& value) {
        window = value;
        window.Closed([this](auto const&, auto const&) { closed = true; });
        check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, device.put(), nullptr, context.put()));
        auto dxgi = device.as<IDXGIDevice>();
        com_ptr<IDXGIAdapter> adapter;
        check_hresult(dxgi->GetAdapter(adapter.put()));
        com_ptr<IDXGIFactory2> factory;
        check_hresult(adapter->GetParent(__uuidof(IDXGIFactory2), factory.put_void()));
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = 1280; desc.Height = 720; desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2; desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        check_hresult(factory->CreateSwapChainForCoreWindow(device.get(), winrt::get_unknown(window), &desc, nullptr, swap.put()));
        com_ptr<ID3D11Texture2D> buffer;
        check_hresult(swap->GetBuffer(0, __uuidof(ID3D11Texture2D), buffer.put_void()));
        check_hresult(device->CreateRenderTargetView(buffer.get(), nullptr, target.put()));
    }
    void Load(hstring const&) {}
    void Run() {
        std::thread worker([] {
            try { init_apartment(apartment_type::multi_threaded); Probe{}.run(); uninit_apartment(); }
            catch (...) { OutputDebugStringW(L"Storage probe: unexpected exception\n"); status = -1; }
        });
        while (!closed) {
            window.Dispatcher().ProcessEvents(CoreProcessEventsOption::ProcessAllIfPresent);
            int result = status.load();
            float color[4] = {result < 0 ? 0.7f : result == 0 ? 0.65f : 0.0f,
                              result > 0 ? 0.6f : result == 0 ? 0.4f : 0.0f, 0.05f, 1.0f};
            context->ClearRenderTargetView(target.get(), color);
            if (FAILED(swap->Present(1, 0))) { status = -1; break; }
        }
        worker.join();
    }
    void Uninitialize() {}
};
}
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    init_apartment(apartment_type::multi_threaded);
    CoreApplication::Run(make<App>());
}
