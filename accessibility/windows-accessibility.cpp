// Windows semantic accessibility observer. Runs in the application's Wine prefix.
// No injected code, simulated mouse clicks, or application-specific control rules.
#include <windows.h>
#include <oleacc.h>
#include <servprov.h>
#include <uiautomationcore.h>
#include <uiautomationclient.h>
#include <cstdio>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>

struct Node {
    IAccessible *accessible = nullptr;
    VARIANT child;
    HWND window = nullptr;
    DWORD process = 0;
    unsigned long id = 0, parent = 0;
    long role = 0, state = 0;
    std::string name, value, action;
    RECT rect{};
    bool editable = false;
    bool uia_invoke = false;
    Node() { VariantInit(&child); }
    ~Node() { if (accessible) accessible->Release(); VariantClear(&child); }
};
static std::map<unsigned long, Node *> nodes;
static std::vector<unsigned long> current;
static unsigned long next_id = 1;
static std::atomic<unsigned> pending_actions(0);
static std::map<std::string, unsigned long> identities;

static std::string utf8(const wchar_t *text) {
    if (!text) return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    std::string result(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    if (!result.empty()) result.pop_back();
    return result;
}
static std::string quote(const std::string &text) {
    std::string result = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += c; }
        else if (c < 32) { char escaped[7]; std::snprintf(escaped, sizeof escaped, "\\u%04x", c); result += escaped; }
        else result += c;
    }
    return result + '"';
}
static std::string property(IAccessible *a, VARIANT child, int kind) {
    BSTR value = nullptr;
    if (kind == 0) a->get_accName(child, &value);
    else if (kind == 1) a->get_accValue(child, &value);
    else a->get_accDefaultAction(child, &value);
    auto result = utf8(value); SysFreeString(value); return result;
}
// WinForms can expose UIA patterns through the documented MSAA extension even
// when its legacy default action is empty. Use that provider, never BM_CLICK
// on an arbitrary custom class or pointer coordinates from another desktop.
static HRESULT invoke_provider(IAccessible *a, VARIANT child, IInvokeProvider **out) {
    *out = nullptr;
    IServiceProvider *services = nullptr;
    HRESULT hr = a->QueryInterface(IID_IServiceProvider, reinterpret_cast<void **>(&services));
    if (FAILED(hr)) return hr;
    IAccessibleEx *extended = nullptr;
    hr = services->QueryService(IID_IAccessibleEx, IID_IAccessibleEx, reinterpret_cast<void **>(&extended));
    services->Release();
    if (FAILED(hr)) return hr;
    if (child.vt == VT_I4 && child.lVal != CHILDID_SELF) {
        IAccessibleEx *descendant = nullptr;
        hr = extended->GetObjectForChild(child.lVal, &descendant);
        extended->Release();
        if (FAILED(hr) || !descendant) return FAILED(hr) ? hr : E_NOINTERFACE;
        extended = descendant;
    } else if (child.vt != VT_I4) {
        extended->Release();
        return E_INVALIDARG;
    }
    IRawElementProviderSimple *provider = nullptr;
    hr = extended->QueryInterface(IID_IRawElementProviderSimple, reinterpret_cast<void **>(&provider));
    extended->Release();
    if (FAILED(hr)) return hr;
    IUnknown *pattern = nullptr;
    hr = provider->GetPatternProvider(UIA_InvokePatternId, &pattern);
    provider->Release();
    if (FAILED(hr) || !pattern) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = pattern->QueryInterface(IID_IInvokeProvider, reinterpret_cast<void **>(out));
    pattern->Release();
    return hr;
}
static void refresh(Node *n) {
    VARIANT value; VariantInit(&value);
    n->accessible->get_accRole(n->child, &value);
    n->role = value.vt == VT_I4 ? value.lVal : 0; VariantClear(&value);
    n->accessible->get_accState(n->child, &value);
    n->state = value.vt == VT_I4 ? value.lVal : 0; VariantClear(&value);
    n->name = property(n->accessible, n->child, 0);
    n->value = (n->state & STATE_SYSTEM_PROTECTED) ? "" : property(n->accessible, n->child, 1);
    n->action = property(n->accessible, n->child, 2);
    n->uia_invoke = false;
    if (n->action.empty()) {
        IInvokeProvider *invoke = nullptr;
        if (SUCCEEDED(invoke_provider(n->accessible, n->child, &invoke))) {
            n->uia_invoke = true;
            n->action = "Invoke";
            invoke->Release();
        }
    }
    if (GetAncestor(n->window, GA_ROOT) == n->window && n->child.vt == VT_I4 && n->child.lVal == CHILDID_SELF)
        n->action = "Close window";
    if (n->action.empty() && n->child.vt == VT_I4 && n->child.lVal == CHILDID_SELF) {
        wchar_t cls[128]{}; GetClassNameW(n->window, cls, 128);
        if (!_wcsicmp(cls, L"Button")) n->action = "Press";
    }
    LONG x = 0, y = 0, w = 0, h = 0;
    n->accessible->accLocation(&x, &y, &w, &h, n->child);
    n->rect = {x, y, x + w, y + h};
    n->editable = n->role == ROLE_SYSTEM_TEXT && !(n->state & (STATE_SYSTEM_READONLY | STATE_SYSTEM_PROTECTED));
}
static void emit(Node *n) {
    std::cout << "{\"id\":" << n->id << ",\"parent\":" << n->parent
              << ",\"window\":" << reinterpret_cast<uintptr_t>(n->window)
              << ",\"process\":" << n->process << ",\"role\":" << n->role
              << ",\"state\":" << n->state << ",\"name\":" << quote(n->name)
              << ",\"value\":" << quote(n->value) << ",\"action\":" << quote(n->action)
              << ",\"uiaInvoke\":" << (n->uia_invoke ? "true" : "false")
              << ",\"editable\":" << (n->editable ? "true" : "false")
              << ",\"rect\":[" << n->rect.left << ',' << n->rect.top << ','
              << n->rect.right - n->rect.left << ',' << n->rect.bottom - n->rect.top << "]}";
}
static unsigned long add(IAccessible *a, VARIANT child, HWND window, unsigned long parent, unsigned depth, unsigned ordinal = 0) {
    if (depth > 40 || current.size() >= 10000) return 0;
    auto n = new Node;
    n->accessible = a; a->AddRef(); VariantCopy(&n->child, &child);
    n->window = window; GetWindowThreadProcessId(window, &n->process);
    n->parent = parent;
    refresh(n);
    // Wine's default MSAA provider reports HWND children as invalid numeric IDs.
    // Those children are visited separately through AccessibleObjectFromWindow.
    if (!n->role) { delete n; return 0; }
    auto key = std::to_string(reinterpret_cast<uintptr_t>(window)) + ':' + std::to_string(n->process)
        + ':' + std::to_string(parent) + ':' + std::to_string(ordinal) + ':' + std::to_string(n->role) + ':' + n->name;
    auto found = identities.find(key);
    n->id = found == identities.end() ? next_id++ : found->second;
    identities[key] = n->id;
    if (nodes.count(n->id)) delete nodes[n->id];
    nodes[n->id] = n; current.push_back(n->id);
    if (child.vt != VT_I4 || child.lVal != CHILDID_SELF) return n->id;
    LONG count = 0, got = 0;
    if (FAILED(a->get_accChildCount(&count)) || count <= 0 || count > 10000) return n->id;
    std::vector<VARIANT> children(count);
    for (auto &item : children) VariantInit(&item);
    if (SUCCEEDED(AccessibleChildren(a, 0, count, children.data(), &got))) {
        for (LONG i = 0; i < got; ++i) {
            if (children[i].vt == VT_DISPATCH) {
                IAccessible *descendant = nullptr;
                if (SUCCEEDED(children[i].pdispVal->QueryInterface(IID_IAccessible, reinterpret_cast<void **>(&descendant)))) {
                    VARIANT self; VariantInit(&self); self.vt = VT_I4; self.lVal = CHILDID_SELF;
                    add(descendant, self, window, n->id, depth + 1, i + 1); descendant->Release();
                }
            } else if (children[i].vt == VT_I4) add(a, children[i], window, n->id, depth + 1, i + 1);
        }
    }
    for (auto &item : children) VariantClear(&item);
    return n->id;
}
static void window_tree(HWND window, unsigned long parent, unsigned depth) {
    if (!IsWindowVisible(window) || depth > 40) return;
    IAccessible *a = nullptr;
    unsigned long id = parent;
    if (SUCCEEDED(AccessibleObjectFromWindow(window, OBJID_CLIENT, IID_IAccessible, reinterpret_cast<void **>(&a)))) {
        VARIANT self; VariantInit(&self); self.vt = VT_I4; self.lVal = CHILDID_SELF;
        auto added = add(a, self, window, parent, depth); if (added) id = added;
        a->Release();
    }
    for (auto child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        window_tree(child, id, depth + 1);
}
static BOOL CALLBACK top_window(HWND window, LPARAM) {
    window_tree(window, 0, 0); return TRUE;
}
struct WindowInventory { bool first = true; bool include_hidden = false; };
static void emit_window(HWND w, WindowInventory &inventory) {
    if (!inventory.include_hidden && !IsWindowVisible(w)) return;
    wchar_t title[512]{}, cls[256]{};
    GetWindowTextW(w, title, 512); GetClassNameW(w, cls, 256);
    DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
    BSTR name = SysAllocString(title), type = SysAllocString(cls);
    if (!inventory.first) std::cout << ','; inventory.first = false;
    std::cout << "{\"window\":" << reinterpret_cast<uintptr_t>(w)
        << ",\"parent\":" << reinterpret_cast<uintptr_t>(GetParent(w))
        << ",\"process\":" << pid << ",\"name\":" << quote(utf8(name))
        << ",\"class\":" << quote(utf8(type))
        << ",\"thread\":" << GetWindowThreadProcessId(w, nullptr)
        << ",\"visible\":" << (IsWindowVisible(w) ? "true" : "false")
        << ",\"style\":" << static_cast<DWORD>(GetWindowLongPtrW(w, GWL_STYLE))
        << ",\"extendedStyle\":" << static_cast<DWORD>(GetWindowLongPtrW(w, GWL_EXSTYLE)) << '}';
    SysFreeString(name); SysFreeString(type);
    for (auto c = GetWindow(w, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) emit_window(c, inventory);
}
static BOOL CALLBACK window_metadata(HWND w, LPARAM param) {
    emit_window(w, *reinterpret_cast<WindowInventory *>(param)); return TRUE;
}
static bool live(Node *n) {
    DWORD process = 0; GetWindowThreadProcessId(n->window, &process);
    return IsWindow(n->window) && process == n->process;
}
static bool perform(Node *n, const std::string &operation, const std::string &argument) {
    if (!live(n) || !IsWindowVisible(n->window) || !IsWindowEnabled(GetAncestor(n->window, GA_ROOT))) return false;
    refresh(n);
    if (n->state & (STATE_SYSTEM_UNAVAILABLE | STATE_SYSTEM_INVISIBLE)) return false;
    if (operation == "invoke") {
        if (n->action.empty()) return false;
        if (n->action == "Close window") return PostMessageW(n->window, WM_CLOSE, 0, 0) != 0;
        if (pending_actions.fetch_add(1) >= 8) { --pending_actions; return false; }
        // Providers may open a modal dialog synchronously. Invoke on a COM
        // worker so the observer can continue querying the new dialog.
        IStream *stream = nullptr;
        if (FAILED(CoMarshalInterThreadInterfaceInStream(IID_IAccessible, n->accessible, &stream))) { --pending_actions; return false; }
        VARIANT child; VariantInit(&child); VariantCopy(&child, &n->child);
        auto result = std::make_shared<std::atomic<int>>(-1);
        HWND hwnd = n->window;
        bool uia_invoke = n->uia_invoke;
        std::thread([stream, child, result, hwnd, uia_invoke]() mutable {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            IAccessible *a = nullptr;
            HRESULT hr = CoGetInterfaceAndReleaseStream(stream, IID_IAccessible, reinterpret_cast<void **>(&a));
            if (SUCCEEDED(hr)) {
                if (uia_invoke) {
                    IInvokeProvider *invoke = nullptr;
                    hr = invoke_provider(a, child, &invoke);
                    if (SUCCEEDED(hr)) { hr = invoke->Invoke(); invoke->Release(); }
                } else hr = a->accDoDefaultAction(child);
                a->Release();
            }
            if (!uia_invoke && hr != S_OK && child.vt == VT_I4 && child.lVal == CHILDID_SELF) {
                wchar_t cls[128]{}; GetClassNameW(hwnd, cls, 128);
                if (!_wcsicmp(cls, L"Button")) {
                    DWORD_PTR value = 0;
                    hr = SendMessageTimeoutW(hwnd, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 3000, &value) ? S_OK : E_FAIL;
                }
            }
            result->store(hr == S_OK ? 1 : 0); VariantClear(&child); CoUninitialize(); --pending_actions;
        }).detach();
        for (int i = 0; i < 10 && result->load() < 0; ++i) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return result->load() != 0;
    }
    if (operation == "focus") {
        if (n->accessible->accSelect(SELFLAG_TAKEFOCUS, n->child) == S_OK) return true;
        if (n->child.vt != VT_I4 || n->child.lVal != CHILDID_SELF) return false;
        auto thread = GetWindowThreadProcessId(n->window, nullptr);
        auto attached = AttachThreadInput(GetCurrentThreadId(), thread, TRUE);
        SetFocus(n->window);
        bool focused = GetFocus() == n->window;
        if (attached) AttachThreadInput(GetCurrentThreadId(), thread, FALSE);
        return focused;
    }
    if (operation != "set" || !n->editable || argument.size() % 2) return false;
    std::string bytes;
    for (size_t i = 0; i < argument.size(); i += 2) {
        auto pair = argument.substr(i, 2);
        char *end = nullptr; auto value = std::strtoul(pair.c_str(), &end, 16);
        if (!end || *end) return false; bytes += static_cast<char>(value);
    }
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (!length && !bytes.empty()) return false;
    BSTR value = SysAllocStringLen(nullptr, length);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), value, length);
    auto result = n->accessible->put_accValue(n->child, value); SysFreeString(value);
    return result == S_OK;
}
int main() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream request(line); std::string operation, argument; unsigned long id = 0;
        request >> operation >> id >> argument;
        if (operation == "input-state") {
            // Read queue ownership without invoking a foreign MSAA provider,
            // which may itself be blocked on the focus problem being observed.
            HWND foreground = GetForegroundWindow();
            HWND selected = id ? reinterpret_cast<HWND>(static_cast<uintptr_t>(id)) : foreground;
            DWORD process = 0;
            DWORD thread = GetWindowThreadProcessId(selected, &process);
            GUITHREADINFO global{}, target{};
            global.cbSize = sizeof(global);
            target.cbSize = sizeof(target);
            BOOL global_ok = GetGUIThreadInfo(0, &global);
            BOOL target_ok = thread && GetGUIThreadInfo(thread, &target);
            std::cout << "{\"foreground\":" << reinterpret_cast<uintptr_t>(foreground)
                << ",\"window\":" << reinterpret_cast<uintptr_t>(selected)
                << ",\"process\":" << process << ",\"thread\":" << thread
                << ",\"foregroundQueueAvailable\":" << (global_ok ? "true" : "false")
                << ",\"foregroundActive\":" << reinterpret_cast<uintptr_t>(global.hwndActive)
                << ",\"foregroundFocus\":" << reinterpret_cast<uintptr_t>(global.hwndFocus)
                << ",\"foregroundCapture\":" << reinterpret_cast<uintptr_t>(global.hwndCapture)
                << ",\"foregroundFlags\":" << global.flags
                << ",\"targetQueueAvailable\":" << (target_ok ? "true" : "false")
                << ",\"targetActive\":" << reinterpret_cast<uintptr_t>(target.hwndActive)
                << ",\"targetFocus\":" << reinterpret_cast<uintptr_t>(target.hwndFocus)
                << ",\"targetCapture\":" << reinterpret_cast<uintptr_t>(target.hwndCapture)
                << ",\"targetFlags\":" << target.flags << "}\n";
        } else if (operation == "post-null") {
            // Explicit diagnostic isolation only: wake an observed HWND's queue
            // without invoking its window procedure or changing window state.
            HWND selected = reinterpret_cast<HWND>(static_cast<uintptr_t>(id));
            std::cout << (IsWindow(selected) && PostMessageW(selected, WM_NULL, 0, 0) ? "true\n" : "false\n");
        } else if (operation == "windows" || operation == "windows-all") {
            WindowInventory inventory;
            inventory.include_hidden = operation == "windows-all";
            std::cout << '[';
            EnumWindows(window_metadata, reinterpret_cast<LPARAM>(&inventory));
            std::cout << "]\n";
        } else if (operation == "snapshot" || operation == "foreground") {
            // Keep preceding objects alive for clients holding a snapshot while
            // another client refreshes. Bound memory without reusing object IDs.
            if (nodes.size() > 30000) { for (auto &[key, n] : nodes) delete n; nodes.clear(); identities.clear(); }
            current.clear();
            HWND selected = operation == "foreground" ? GetForegroundWindow() : reinterpret_cast<HWND>(static_cast<uintptr_t>(id));
            if (selected && IsWindow(selected)) window_tree(selected, 0, 0);
            else if (operation == "snapshot" && !id) EnumWindows(top_window, 0);
            std::cout << '['; bool first = true;
            for (auto key : current) { if (!first) std::cout << ','; first = false; emit(nodes[key]); }
            std::cout << "]\n";
        } else {
            auto it = nodes.find(id);
            if (it == nodes.end() || !live(it->second)) std::cout << "{\"error\":\"stale Windows accessibility object\"}\n";
            else if (operation == "get") { refresh(it->second); emit(it->second); std::cout << '\n'; }
            else std::cout << (perform(it->second, operation, argument) ? "true\n" : "false\n");
        }
        std::cout.flush();
    }
    for (auto &[key, n] : nodes) delete n;
    CoUninitialize(); return 0;
}
