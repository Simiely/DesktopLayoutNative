/* 桌面布局原生原型（C / Zig cc，Win32 + Shell COM）
 *
 * 功能：枚举桌面图标（IShellWindows -> IServiceProvider -> IShellBrowser ->
 * IFolderView）、保存 JSON、应用布局（应用前自动备份）、交换两枚图标并还原。
 * 无第三方库；只依赖系统 DLL。
 */
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS

#include <windows.h>
#ifndef _PROPID_DEFINED
#define _PROPID_DEFINED
typedef unsigned long PROPID;
#endif
#include <gdiplus.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <exdisp.h>
#include <servprov.h>
#include <oleauto.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include <stdarg.h>
#include <string.h>
#include <limits.h>

/* ---------------------------------------------------------------- 常量 */

#define APP_TITLE L"桌面布局"
/* 外框锁定 500 × 400（含标题栏与边框），96 DPI 下客户区 484 × 361。
 * 固定栈 169 + 列表 6×32 = 361，整行不露出半行。 */
#define WIN_W_OUTER 500
#define WIN_H_OUTER 400
#define ROW_H 32
#define SWC_DESKTOP 0x8
#define SWFO_NEEDDISPATCH 0x1
#define SVGIO_ALLVIEW 0x2
#define SVSI_POSITIONITEM 0x80
#define SIGDN_NORMALDISPLAY ((SIGDN)0)
#define SIGDN_DESKTOPABSOLUTEPARSING ((SIGDN)0x80028000)
#define MAX_MONITORS 8
#define MAX_MISMATCH 20

static const CLSID k_clsidShellWindows = {
    0x9BA05972, 0xF6A8, 0x11CF,
    {0xA4, 0x42, 0x00, 0xA0, 0xC9, 0x0A, 0x8F, 0x39}};
static const IID k_iidShellWindows = {
    0x85CB6900, 0x4D95, 0x11CF,
    {0x96, 0x0C, 0x00, 0x80, 0xC7, 0xF4, 0xEE, 0x85}};
static const IID k_iidServiceProvider = {
    0x6D5140C1, 0x7436, 0x11CE,
    {0x80, 0x34, 0x00, 0xAA, 0x00, 0x60, 0x09, 0xFA}};
static const GUID k_sidTopLevelBrowser = {
    0x4C96BE40, 0x915C, 0x11CF,
    {0x99, 0xD3, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37}};
static const IID k_iidShellBrowser = {
    0x000214E2, 0x0000, 0x0000,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const IID k_iidFolderView = {
    0xCDE725B0, 0xCCC9, 0x4519,
    {0x91, 0x7E, 0x32, 0x5D, 0x72, 0xFA, 0xB4, 0xCE}};
static const GUID k_folderIdDesktop = {
    0xB4BFCC3A, 0xDB2C, 0x424C,
    {0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41}};

/* ---------------------------------------------------------------- 基础结构 */

typedef struct {
    wchar_t *id;
    wchar_t *name;
    int x;
    int y;
    wchar_t monitor[64];
    int view_index;
    int dup_index;
} Icon;

typedef struct {
    Icon *items;
    int count;
    int capacity;
} IconList;

typedef struct {
    wchar_t *id;
    int ex, ey, ax, ay;
} Mismatch;

typedef struct {
    wchar_t device[64];
    RECT bounds;
    RECT work;
    int primary;
    UINT dpi;
} Monitor;

typedef HRESULT (WINAPI *GetDpiForMonitorFn)(HMONITOR, int, UINT *, UINT *);

static Monitor g_monitors[MAX_MONITORS];
static int g_monitor_count = 0;
static GetDpiForMonitorFn g_get_dpi_for_monitor = NULL;
static wchar_t g_data_dir[MAX_PATH * 2];
static wchar_t g_layouts_dir[MAX_PATH * 2];
static wchar_t g_undo_dir[MAX_PATH * 2];
static wchar_t g_undo_path[MAX_PATH * 2];
static HWND g_status = NULL;
static wchar_t g_layout_path[MAX_PATH * 2];
static int g_undo_available = 0;

/* ---------------------------------------------------------------- 缓冲 */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_init(Buf *b) {
    b->cap = 16 * 1024;
    b->len = 0;
    b->data = (char *)malloc(b->cap);
    if (b->data) b->data[0] = 0;
}

static void buf_reserve(Buf *b, size_t extra) {
    if (!b->data) return;
    size_t need = b->len + extra + 1;
    if (need <= b->cap) return;
    size_t cap = b->cap;
    while (cap < need) cap *= 2;
    char *p = (char *)realloc(b->data, cap);
    if (!p) return;
    b->data = p;
    b->cap = cap;
}

static void buf_puts(Buf *b, const char *s) {
    size_t n = strlen(s);
    buf_reserve(b, n);
    if (!b->data) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

static void buf_putc(Buf *b, char c) {
    buf_reserve(b, 1);
    if (!b->data) return;
    b->data[b->len++] = c;
    b->data[b->len] = 0;
}

static void buf_printf(Buf *b, const char *fmt, ...) {
    char tmp[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    buf_puts(b, tmp);
}

static void buf_json_w(Buf *b, const wchar_t *w) {
    if (!w) {
        buf_puts(b, "\"\"");
        return;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) {
        buf_puts(b, "\"\"");
        return;
    }
    char *u = (char *)malloc((size_t)n);
    if (!u) {
        buf_puts(b, "\"\"");
        return;
    }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, u, n, NULL, NULL);
    buf_putc(b, '"');
    for (const unsigned char *p = (const unsigned char *)u; *p; p++) {
        switch (*p) {
        case '"': buf_puts(b, "\\\""); break;
        case '\\': buf_puts(b, "\\\\"); break;
        case '\b': buf_puts(b, "\\b"); break;
        case '\f': buf_puts(b, "\\f"); break;
        case '\n': buf_puts(b, "\\n"); break;
        case '\r': buf_puts(b, "\\r"); break;
        case '\t': buf_puts(b, "\\t"); break;
        default:
            if (*p < 0x20) buf_printf(b, "\\u%04x", *p);
            else buf_putc(b, (char)*p);
        }
    }
    buf_putc(b, '"');
    free(u);
}

/* ---------------------------------------------------------------- 字符串/文件 */

static wchar_t *wdup(const wchar_t *s) {
    if (!s) return NULL;
    size_t n = (wcslen(s) + 1) * sizeof(wchar_t);
    wchar_t *r = (wchar_t *)malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

static char *utf8_from_w(const wchar_t *s) {
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *out = (char *)malloc((size_t)n);
    if (!out) return NULL;
    WideCharToMultiByte(CP_UTF8, 0, s, -1, out, n, NULL, NULL);
    return out;
}

/* 窄错误串里带 UTF-8 路径（避免窄 printf 的 %ls 在 zig libc 下失效）。 */
static void err_path(char *buf, int n, const char *prefix, const wchar_t *path) {
    char *u = utf8_from_w(path);
    snprintf(buf, (size_t)n, "%s%s", prefix, u ? u : "");
    free(u);
}

static wchar_t *utf8_to_w(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static void ensure_dir(const wchar_t *path) {
    if (!path || !*path) return;
    SHCreateDirectoryExW(NULL, path, NULL);
}

static void ensure_parent_dir(const wchar_t *path) {
    wchar_t tmp[MAX_PATH * 2];
    wcsncpy(tmp, path, MAX_PATH * 2 - 1);
    tmp[MAX_PATH * 2 - 1] = 0;
    wchar_t *slash = wcsrchr(tmp, L'\\');
    if (slash) {
        *slash = 0;
        ensure_dir(tmp);
    }
}

static int write_file_bytes(const wchar_t *path, const char *data, DWORD len) {
    ensure_parent_dir(path);
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, len, &written, NULL);
    CloseHandle(h);
    return ok && written == len;
}

static char *read_file_bytes(const wchar_t *path, DWORD *size_out) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    DWORD size = GetFileSize(h, NULL);
    if (size == INVALID_FILE_SIZE) {
        CloseHandle(h);
        return NULL;
    }
    char *data = (char *)malloc((size_t)size + 1);
    if (!data) {
        CloseHandle(h);
        return NULL;
    }
    DWORD read = 0;
    BOOL ok = ReadFile(h, data, size, &read, NULL);
    CloseHandle(h);
    if (!ok) {
        free(data);
        return NULL;
    }
    data[read] = 0;
    if (size_out) *size_out = read;
    return data;
}

static void timestamp_now(wchar_t *out, size_t n, int compact) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    if (compact)
        swprintf(out, n, L"%04d%02d%02d-%02d%02d%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    else
        swprintf(out, n, L"%04d-%02d-%02dT%02d:%02d:%02d",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

/* 删除前确认：默认开启；持久化到 data/settings.json。 */
static int g_confirm_delete = 1;
static wchar_t g_settings_path[MAX_PATH * 2] = L"";

static void init_paths(void) {
    wchar_t base[MAX_PATH] = L"";
    GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    swprintf(g_data_dir, MAX_PATH * 2, L"%s\\DesktopLayoutNative\\data", base);
    swprintf(g_layouts_dir, MAX_PATH * 2, L"%s\\layouts", g_data_dir);
    swprintf(g_undo_dir, MAX_PATH * 2, L"%s\\undo", g_data_dir);
    swprintf(g_undo_path, MAX_PATH * 2, L"%s\\undo_snapshot.json", g_undo_dir);
    swprintf(g_layout_path, MAX_PATH * 2, L"%s\\layout_current.json", g_data_dir);
    swprintf(g_settings_path, MAX_PATH * 2, L"%s\\settings.json", g_data_dir);
    ensure_dir(g_data_dir);
    ensure_dir(g_layouts_dir);
    ensure_dir(g_undo_dir);
}

/* ---------------------------------------------------------------- 显示器 */

static BOOL CALLBACK monitor_cb(HMONITOR hmon, HDC hdc, LPRECT rc, LPARAM lp) {
    (void)hdc; (void)rc; (void)lp;
    if (g_monitor_count >= MAX_MONITORS) return FALSE;
    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hmon, (LPMONITORINFO)&mi)) return TRUE;
    Monitor *m = &g_monitors[g_monitor_count];
    wcsncpy(m->device, mi.szDevice, 63);
    m->device[63] = 0;
    m->bounds = mi.rcMonitor;
    m->work = mi.rcWork;
    m->primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    m->dpi = 96;
    UINT dx = 96, dy = 96;
    if (g_get_dpi_for_monitor &&
        SUCCEEDED(g_get_dpi_for_monitor(hmon, 0 /* MDT_EFFECTIVE_DPI */, &dx, &dy)) && dx)
        m->dpi = dx;
    g_monitor_count++;
    return TRUE;
}

static void enum_monitors(void) {
    g_monitor_count = 0;
    EnumDisplayMonitors(NULL, NULL, monitor_cb, 0);
}

static void monitor_for_point(int x, int y, wchar_t *out, size_t n) {
    if (n) out[0] = 0;
    Monitor *fallback = NULL;
    for (int i = 0; i < g_monitor_count; i++) {
        Monitor *m = &g_monitors[i];
        if (m->primary) fallback = m;
        if (x >= m->bounds.left && x < m->bounds.right &&
            y >= m->bounds.top && y < m->bounds.bottom) {
            wcsncpy(out, m->device, n - 1);
            out[n - 1] = 0;
            return;
        }
    }
    if (!fallback && g_monitor_count) fallback = &g_monitors[0];
    if (fallback) {
        wcsncpy(out, fallback->device, n - 1);
        out[n - 1] = 0;
    }
}

/* ---------------------------------------------------------------- 图标列表 */

static Icon *icon_list_add(IconList *list) {
    if (list->count >= list->capacity) {
        int cap = list->capacity ? list->capacity * 2 : 64;
        Icon *p = (Icon *)realloc(list->items, (size_t)cap * sizeof(Icon));
        if (!p) return NULL;
        list->items = p;
        list->capacity = cap;
    }
    Icon *ic = &list->items[list->count++];
    memset(ic, 0, sizeof(*ic));
    return ic;
}

static void icon_list_free(IconList *list) {
    for (int i = 0; i < list->count; i++) {
        free(list->items[i].id);
        free(list->items[i].name);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

/* ---------------------------------------------------------------- COM 连接 */

typedef struct {
    IShellWindows *shell_windows;
    IDispatch *disp;
    IServiceProvider *provider;
    IShellBrowser *browser;
    IShellView *shell_view;
    IFolderView *view;
    PIDLIST_ABSOLUTE root;
    int com_initialized;
} Desktop;

static HRESULT desktop_open(Desktop *d) {
    memset(d, 0, sizeof(*d));
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        d->com_initialized = 1;
    } else if (hr != RPC_E_CHANGED_MODE) {
        return hr;
    }

    hr = CoCreateInstance(&k_clsidShellWindows, NULL, CLSCTX_ALL,
                          &k_iidShellWindows, (void **)&d->shell_windows);
    if (FAILED(hr) || !d->shell_windows) return hr;

    VARIANT loc, empty;
    VariantInit(&loc);
    VariantInit(&empty);
    loc.vt = VT_I4;
    loc.lVal = CSIDL_DESKTOP;
    LONG hwnd = 0;
    hr = d->shell_windows->lpVtbl->FindWindowSW(
        d->shell_windows, &loc, &empty, SWC_DESKTOP, &hwnd,
        SWFO_NEEDDISPATCH, &d->disp);
    VariantClear(&loc);
    VariantClear(&empty);
    if (FAILED(hr) || !d->disp) return FAILED(hr) ? hr : E_FAIL;

    hr = d->disp->lpVtbl->QueryInterface(
        d->disp, &k_iidServiceProvider, (void **)&d->provider);
    if (FAILED(hr) || !d->provider) return hr;

    hr = d->provider->lpVtbl->QueryService(
        d->provider, &k_sidTopLevelBrowser, &k_iidShellBrowser,
        (void **)&d->browser);
    if (FAILED(hr) || !d->browser) return hr;

    hr = d->browser->lpVtbl->QueryActiveShellView(d->browser, &d->shell_view);
    if (FAILED(hr) || !d->shell_view) return hr;

    hr = d->shell_view->lpVtbl->QueryInterface(
        d->shell_view, &k_iidFolderView, (void **)&d->view);
    if (FAILED(hr) || !d->view) return hr;

    hr = SHGetKnownFolderIDList(&k_folderIdDesktop, 0, NULL, &d->root);
    return hr;
}

static void desktop_close(Desktop *d) {
    if (d->root) ILFree(d->root);
    if (d->view) d->view->lpVtbl->Release(d->view);
    if (d->shell_view) d->shell_view->lpVtbl->Release(d->shell_view);
    if (d->browser) d->browser->lpVtbl->Release(d->browser);
    if (d->provider) d->provider->lpVtbl->Release(d->provider);
    if (d->disp) d->disp->lpVtbl->Release(d->disp);
    if (d->shell_windows) d->shell_windows->lpVtbl->Release(d->shell_windows);
    if (d->com_initialized) CoUninitialize();
    memset(d, 0, sizeof(*d));
}

static wchar_t *name_from_pidl(PIDLIST_ABSOLUTE root, PITEMID_CHILD child, SIGDN sigdn) {
    PIDLIST_ABSOLUTE abs = ILCombine(root, child);
    if (!abs) return NULL;
    LPWSTR p = NULL;
    HRESULT hr = SHGetNameFromIDList(abs, sigdn, &p);
    ILFree(abs);
    if (FAILED(hr) || !p) return NULL;
    wchar_t *r = wdup(p);
    CoTaskMemFree(p);
    return r;
}

static HRESULT desktop_enum(Desktop *d, IconList *list, PITEMID_CHILD **pidls_out) {
    memset(list, 0, sizeof(*list));
    if (pidls_out) *pidls_out = NULL;
    int total = 0;
    HRESULT hr = d->view->lpVtbl->ItemCount(d->view, SVGIO_ALLVIEW, &total);
    if (FAILED(hr)) return hr;
    if (total < 0) total = 0;

    PITEMID_CHILD *pidls = NULL;
    if (pidls_out && total > 0) {
        pidls = (PITEMID_CHILD *)calloc((size_t)total, sizeof(PITEMID_CHILD));
        if (!pidls) return E_OUTOFMEMORY;
    }

    for (int i = 0; i < total; i++) {
        PITEMID_CHILD pidl = NULL;
        hr = d->view->lpVtbl->Item(d->view, i, &pidl);
        if (FAILED(hr) || !pidl) {
            if (pidls) pidls[i] = NULL;
            continue;
        }
        POINT pt;
        pt.x = 0;
        pt.y = 0;
        hr = d->view->lpVtbl->GetItemPosition(d->view, pidl, &pt);
        if (FAILED(hr)) {
            pt.x = 0;
            pt.y = 0;
        }
        wchar_t *id = name_from_pidl(d->root, pidl, SIGDN_DESKTOPABSOLUTEPARSING);
        wchar_t *name = name_from_pidl(d->root, pidl, SIGDN_NORMALDISPLAY);
        if (!id) {
            wchar_t tmp[256];
            swprintf(tmp, 256, L"unknown::%d::%s", i, name ? name : L"");
            id = wdup(tmp);
        }
        if (!name) name = wdup(id ? id : L"");
        int dup = 0;
        for (int k = 0; k < list->count; k++) {
            if (list->items[k].id && id && _wcsicmp(list->items[k].id, id) == 0) dup++;
        }
        Icon *ic = icon_list_add(list);
        if (!ic) {
            free(id);
            free(name);
            if (pidls_out) {
                pidls[i] = pidl;
            } else {
                CoTaskMemFree(pidl);
            }
            continue;
        }
        ic->id = id;
        ic->name = name;
        ic->x = (int)pt.x;
        ic->y = (int)pt.y;
        ic->view_index = i;
        ic->dup_index = dup;
        monitor_for_point(ic->x, ic->y, ic->monitor, 64);
        if (pidls_out) {
            pidls[i] = pidl;
        } else {
            CoTaskMemFree(pidl);
        }
    }
    if (pidls_out) *pidls_out = pidls;
    return S_OK;
}

static void free_pidls(PITEMID_CHILD *pidls, int count) {
    if (!pidls) return;
    for (int i = 0; i < count; i++) {
        if (pidls[i]) CoTaskMemFree(pidls[i]);
    }
    free(pidls);
}

/* ---------------------------------------------------------------- JSON 输出 */

static void timestamp_iso(wchar_t *out, size_t n) {
    wchar_t base[64];
    timestamp_now(base, 64, 0);
    TIME_ZONE_INFORMATION tzi;
    memset(&tzi, 0, sizeof(tzi));
    DWORD tz = GetTimeZoneInformation(&tzi);
    long total = tzi.Bias + (tz == TIME_ZONE_ID_DAYLIGHT ? tzi.DaylightBias : 0);
    int off = (int)(-total);
    swprintf(out, n, L"%s%c%02d:%02d", base, off >= 0 ? L'+' : L'-',
             abs(off) / 60, abs(off) % 60);
}

/* 与 Python 版 1.0.0 相同的字段结构（schema/app/name/kind/created_at/count/
 * displays/icons）。displays 使用 virtual_rect + monitors，图标附带 dup_index。 */
static void write_layout_json(Buf *b, const wchar_t *name, const char *kind,
                              IconList *icons) {
    wchar_t stamp[96];
    timestamp_iso(stamp, 96);
    buf_puts(b, "{\n  \"schema\": 1,\n  \"app\": \"Desktop Layout Lite\",\n  \"name\": ");
    buf_json_w(b, name);
    buf_printf(b, ",\n  \"kind\": \"%s\",\n  \"created_at\": ", kind);
    buf_json_w(b, stamp);
    buf_printf(b, ",\n  \"count\": %d,\n  \"displays\": {\n", icons->count);
    buf_printf(b, "    \"virtual_rect\": {\"left\": %d, \"top\": %d, \"width\": %d, \"height\": %d},\n",
               GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
               GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    buf_puts(b, "    \"monitors\": [");
    for (int i = 0; i < g_monitor_count; i++) {
        Monitor *m = &g_monitors[i];
        if (i) buf_puts(b, ",");
        buf_puts(b, "\n      {\"device\": ");
        buf_json_w(b, m->device);
        buf_printf(b,
                   ", \"primary\": %s, \"dpi\": %u, "
                   "\"bounds\": {\"left\": %d, \"top\": %d, \"right\": %d, \"bottom\": %d, "
                   "\"width\": %d, \"height\": %d}, "
                   "\"work_area\": {\"left\": %d, \"top\": %d, \"right\": %d, \"bottom\": %d, "
                   "\"width\": %d, \"height\": %d}}",
                   m->primary ? "true" : "false", m->dpi,
                   (int)m->bounds.left, (int)m->bounds.top,
                   (int)m->bounds.right, (int)m->bounds.bottom,
                   (int)(m->bounds.right - m->bounds.left),
                   (int)(m->bounds.bottom - m->bounds.top),
                   (int)m->work.left, (int)m->work.top,
                   (int)m->work.right, (int)m->work.bottom,
                   (int)(m->work.right - m->work.left),
                   (int)(m->work.bottom - m->work.top));
    }
    buf_puts(b, "\n    ]\n  },\n  \"icons\": [");
    for (int i = 0; i < icons->count; i++) {
        Icon *ic = &icons->items[i];
        if (i) buf_puts(b, ",");
        buf_puts(b, "\n    {\"id\": ");
        buf_json_w(b, ic->id);
        buf_puts(b, ", \"name\": ");
        buf_json_w(b, ic->name);
        buf_printf(b, ", \"x\": %d, \"y\": %d, \"monitor\": ", ic->x, ic->y);
        buf_json_w(b, ic->monitor);
        buf_printf(b, ", \"view_index\": %d, \"dup_index\": %d, \"pidl\": 0}",
                   ic->view_index, ic->dup_index);
    }
    buf_puts(b, "\n  ]\n}\n");
}

/* ---------------------------------------------------------------- JSON 解析 */

static const char *skip_ws(const char *p) {
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    return p;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int utf8_emit(char *out, unsigned cp) {
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* 解析 JSON 字符串（p 指向开头的引号），返回结束引号后一位；输出 UTF-8。 */
static const char *parse_json_string(const char *p, char **out) {
    *out = NULL;
    if (*p != '"') return p;
    p++;
    char *buf = (char *)malloc(strlen(p) + 1);
    if (!buf) return p;
    size_t n = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"': buf[n++] = '"'; p++; break;
            case '\\': buf[n++] = '\\'; p++; break;
            case '/': buf[n++] = '/'; p++; break;
            case 'b': buf[n++] = '\b'; p++; break;
            case 'f': buf[n++] = '\f'; p++; break;
            case 'n': buf[n++] = '\n'; p++; break;
            case 'r': buf[n++] = '\r'; p++; break;
            case 't': buf[n++] = '\t'; p++; break;
            case 'u': {
                p++;
                int h1 = hexval(p[0]), h2 = hexval(p[1]), h3 = hexval(p[2]), h4 = hexval(p[3]);
                if (h1 < 0 || h2 < 0 || h3 < 0 || h4 < 0) break;
                unsigned cp = (unsigned)((h1 << 12) | (h2 << 8) | (h3 << 4) | h4);
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    int g1 = hexval(p[2]), g2 = hexval(p[3]), g3 = hexval(p[4]), g4 = hexval(p[5]);
                    if (g1 >= 0 && g2 >= 0 && g3 >= 0 && g4 >= 0) {
                        unsigned lo = (unsigned)((g1 << 12) | (g2 << 8) | (g3 << 4) | g4);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            p += 6;
                        }
                    }
                }
                n += (size_t)utf8_emit(buf + n, cp);
                break;
            }
            default: buf[n++] = *p; p++; break;
            }
        } else {
            buf[n++] = *p++;
        }
    }
    buf[n] = 0;
    if (*p == '"') p++;
    *out = buf;
    return p;
}

static const char *skip_value(const char *p) {
    p = skip_ws(p);
    if (*p == '"') {
        char *tmp = NULL;
        p = parse_json_string(p, &tmp);
        free(tmp);
        return p;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                char *tmp = NULL;
                p = parse_json_string(p, &tmp);
                free(tmp);
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            else if (*p == '}' || *p == ']') {
                depth--;
                p++;
                if (depth <= 0) return p;
                continue;
            }
            p++;
        }
        return p;
    }
    while (*p && *p != ',' && *p != '}' && *p != ']') p++;
    return p;
}

/* 读取本工具或 Python 版生成的布局；只取 icons 数组中的 id/name/x/y/monitor。 */
static int parse_layout_json(const char *text, IconList *out) {
    memset(out, 0, sizeof(*out));
    const char *p = strstr(text, "\"icons\"");
    if (!p) return 0;
    p = strchr(p, '[');
    if (!p) return 0;
    p++;
    while (*p) {
        p = skip_ws(p);
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') break;
        p++;
        wchar_t *id = NULL, *name = NULL, *mon = NULL;
        int x = 0, y = 0, view_index = 0;
        while (*p) {
            p = skip_ws(p);
            if (*p == '}') { p++; break; }
            char *key = NULL;
            p = parse_json_string(p, &key);
            p = skip_ws(p);
            if (*p == ':') p++;
            p = skip_ws(p);
            if (key && strcmp(key, "id") == 0) {
                char *v = NULL;
                p = parse_json_string(p, &v);
                free(id);
                id = utf8_to_w(v ? v : "");
                free(v);
            } else if (key && strcmp(key, "name") == 0) {
                char *v = NULL;
                p = parse_json_string(p, &v);
                free(name);
                name = utf8_to_w(v ? v : "");
                free(v);
            } else if (key && strcmp(key, "monitor") == 0) {
                char *v = NULL;
                p = parse_json_string(p, &v);
                free(mon);
                mon = utf8_to_w(v ? v : "");
                free(v);
            } else if (key && strcmp(key, "x") == 0) {
                x = (int)strtol(p, (char **)&p, 10);
            } else if (key && strcmp(key, "y") == 0) {
                y = (int)strtol(p, (char **)&p, 10);
            } else if (key && strcmp(key, "view_index") == 0) {
                view_index = (int)strtol(p, (char **)&p, 10);
            } else {
                p = skip_value(p);
            }
            free(key);
            p = skip_ws(p);
            if (*p == ',') { p++; continue; }
            if (*p == '}') { p++; break; }
            if (!*p) break;
        }
        if (id) {
            Icon *ic = icon_list_add(out);
            if (ic) {
                ic->id = id;
                ic->name = name ? name : wdup(id);
                if (name) name = NULL;
                ic->x = x;
                ic->y = y;
                ic->view_index = view_index;
                if (mon) {
                    wcsncpy(ic->monitor, mon, 63);
                    ic->monitor[63] = 0;
                } else {
                    monitor_for_point(x, y, ic->monitor, 64);
                }
            } else {
                free(id);
                free(name);
            }
            id = NULL;
        }
        free(id);
        free(name);
        free(mon);
        p = skip_ws(p);
        if (*p == ',') p++;
    }
    return out->count > 0;
}

/* ---------------------------------------------------------------- 应用/校验 */

static int find_icon(IconList *list, const wchar_t *id) {
    for (int i = 0; i < list->count; i++) {
        if (list->items[i].id && _wcsicmp(list->items[i].id, id) == 0) return i;
    }
    return -1;
}

static HRESULT apply_list(Desktop *d, IconList *targets, int *matched_out) {
    *matched_out = 0;
    IconList current;
    PITEMID_CHILD *pidls = NULL;
    HRESULT hr = desktop_enum(d, &current, &pidls);
    if (FAILED(hr)) return hr;

    PITEMID_CHILD *sel = (PITEMID_CHILD *)calloc((size_t)current.count, sizeof(PITEMID_CHILD));
    POINT *pts = (POINT *)calloc((size_t)current.count, sizeof(POINT));
    int n = 0;
    if (sel && pts) {
        for (int i = 0; i < current.count; i++) {
            int t = find_icon(targets, current.items[i].id);
            if (t < 0) continue;
            sel[n] = pidls[i];
            pts[n].x = targets->items[t].x;
            pts[n].y = targets->items[t].y;
            n++;
        }
        if (n > 0) {
            hr = d->view->lpVtbl->SelectAndPositionItems(
                d->view, (UINT)n, (PCUITEMID_CHILD_ARRAY)sel, pts, SVSI_POSITIONITEM);
        } else {
            hr = S_FALSE;
        }
    } else {
        hr = E_OUTOFMEMORY;
    }
    *matched_out = n;

    free(sel);
    free(pts);
    free_pidls(pidls, current.count);
    icon_list_free(&current);
    return hr;
}

static int verify_list(Desktop *d, IconList *expected, int tolerance,
                       int *checked_out, Mismatch *mm, int max_mm) {
    if (checked_out) *checked_out = 0;
    IconList current;
    HRESULT hr = desktop_enum(d, &current, NULL);
    if (FAILED(hr)) return -1;
    int mismatch = 0, checked = 0;
    for (int i = 0; i < expected->count; i++) {
        int j = find_icon(&current, expected->items[i].id);
        if (j < 0) {
            mismatch++;
            if (mismatch <= max_mm) {
                mm[mismatch - 1].id = wdup(expected->items[i].id);
                mm[mismatch - 1].ex = expected->items[i].x;
                mm[mismatch - 1].ey = expected->items[i].y;
                mm[mismatch - 1].ax = INT_MIN;
                mm[mismatch - 1].ay = INT_MIN;
            }
            continue;
        }
        checked++;
        int dx = current.items[j].x - expected->items[i].x;
        int dy = current.items[j].y - expected->items[i].y;
        if (abs(dx) > tolerance || abs(dy) > tolerance) {
            mismatch++;
            if (mismatch <= max_mm) {
                mm[mismatch - 1].id = wdup(expected->items[i].id);
                mm[mismatch - 1].ex = expected->items[i].x;
                mm[mismatch - 1].ey = expected->items[i].y;
                mm[mismatch - 1].ax = current.items[j].x;
                mm[mismatch - 1].ay = current.items[j].y;
            }
        }
    }
    if (checked_out) *checked_out = checked;
    icon_list_free(&current);
    return mismatch;
}

static void free_mismatches(Mismatch *mm, int n) {
    for (int i = 0; i < n && i < MAX_MISMATCH; i++) free(mm[i].id);
}

/* ---------------------------------------------------------------- 自启/多布局 */

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define RUN_VALUE L"DesktopLayoutNative"

static int g_autostart = 0;

typedef struct {
    wchar_t *path;
    wchar_t *name;
    wchar_t *created;
    int count;
} LayoutEntry;

static LayoutEntry *g_layouts = NULL;
static int g_layout_count = 0;
static int g_layout_invalid = 0;

static int json_top_string(const char *text, const char *key, char **out) {
    const char *limit = strstr(text, "\"icons\"");
    if (!limit) limit = text + strlen(text);
    const char *p = text;
    while ((p = strstr(p, "\"")) != NULL && p < limit) {
        if (strncmp(p + 1, key, strlen(key)) == 0 && p[1 + strlen(key)] == '"') {
            const char *q = skip_ws(p + 2 + strlen(key));
            if (*q != ':') { p++; continue; }
            q = skip_ws(q + 1);
            if (*q != '"') return 0;
            return parse_json_string(q, out) != NULL;
        }
        p++;
    }
    return 0;
}

static int json_top_int(const char *text, const char *key, int *out) {
    const char *limit = strstr(text, "\"icons\"");
    if (!limit) limit = text + strlen(text);
    const char *p = text;
    while ((p = strstr(p, "\"")) != NULL && p < limit) {
        if (strncmp(p + 1, key, strlen(key)) == 0 && p[1 + strlen(key)] == '"') {
            const char *q = skip_ws(p + 2 + strlen(key));
            if (*q != ':') { p++; continue; }
            q = skip_ws(q + 1);
            *out = atoi(q);
            return 1;
        }
        p++;
    }
    return 0;
}

static int autostart_expected_cmd(wchar_t *out, size_t n) {
    wchar_t exe[MAX_PATH * 2] = L"";
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH * 2)) return 0;
    swprintf(out, n, L"\"%s\" --tray", exe);
    return 1;
}

static int autostart_write(int enable) {
    HKEY key = NULL;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, &disp) != ERROR_SUCCESS)
        return 0;
    int ok = 0;
    if (enable) {
        wchar_t cmd[MAX_PATH * 2];
        if (autostart_expected_cmd(cmd, MAX_PATH * 2)) {
            ok = RegSetValueExW(key, RUN_VALUE, 0, REG_SZ, (const BYTE *)cmd,
                                (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
        }
    } else {
        LONG r = RegDeleteValueW(key, RUN_VALUE);
        ok = (r == ERROR_SUCCESS || r == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(key);
    return ok;
}

/* 读取自启项；存在即视为启用。启动时若命令与当前 exe 不一致则就地改写。 */
static int autostart_read(wchar_t *out, size_t n) {
    if (n) out[0] = 0;
    HKEY key = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return 0;
    DWORD type = 0, size = 0;
    LONG r = RegQueryValueExW(key, RUN_VALUE, NULL, &type, NULL, &size);
    int present = 0;
    if (r == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ) && size > 0) {
        wchar_t *val = (wchar_t *)malloc((size_t)size + sizeof(wchar_t));
        if (val) {
            memset(val, 0, (size_t)size + sizeof(wchar_t));
            if (RegQueryValueExW(key, RUN_VALUE, NULL, &type, (LPBYTE)val, &size) == ERROR_SUCCESS) {
                val[size / sizeof(wchar_t)] = 0;
                if (out) {
                    wcsncpy(out, val, n - 1);
                    out[n - 1] = 0;
                }
                present = 1;
            }
            free(val);
        }
    }
    RegCloseKey(key);
    return present;
}

static int autostart_sync(void) {
    wchar_t val[MAX_PATH * 2] = L"";
    if (!autostart_read(val, MAX_PATH * 2)) return 0;
    wchar_t expect[MAX_PATH * 2] = L"";
    if (autostart_expected_cmd(expect, MAX_PATH * 2) && _wcsicmp(val, expect) != 0)
        autostart_write(1);
    return 1;
}

/* 设置持久化：data/settings.json。文件缺失/损坏时回落到安全默认值（确认=开）。 */
static int settings_load(void) {
    DWORD size = 0;
    char *txt = read_file_bytes(g_settings_path, &size);
    (void)size;
    if (!txt) return 1;
    int v = 1;
    if (!json_top_int(txt, "confirm_delete", &v)) v = 1;
    free(txt);
    return v ? 1 : 0;
}

static int settings_save(void) {
    char buf[96];
    snprintf(buf, sizeof(buf),
             "{\n  \"confirm_delete\": %d\n}\n", g_confirm_delete ? 1 : 0);
    return write_file_bytes(g_settings_path, buf, (DWORD)strlen(buf));
}

static int name_illegal(const wchar_t *name, const wchar_t **reason) {
    size_t len = wcslen(name);
    if (len == 0 || (len == 1 && name[0] == L' ')) {
        *reason = L"名称不能为空";
        return 1;
    }
    if (name[0] == L' ' || name[len - 1] == L' ') {
        *reason = L"名称首尾不能有空格";
        return 1;
    }
    if (len > 60) {
        *reason = L"名称不能超过 60 个字符";
        return 1;
    }
    for (size_t i = 0; i < len; i++) {
        wchar_t c = name[i];
        if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
            c == L'/' || c == L'\\' || c == L'|' || c == L'?' || c == L'*') {
            *reason = L"名称不能包含 \\ / : * ? \" < > |";
            return 1;
        }
    }
    return 0;
}

static void filename_slug(const wchar_t *name, wchar_t *out, size_t n) {
    size_t j = 0;
    for (size_t i = 0; name[i] && j + 1 < n; i++) {
        wchar_t c = name[i];
        if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
            c == L'/' || c == L'\\' || c == L'|' || c == L'?' || c == L'*') {
            c = L'_';
        }
        out[j++] = c;
    }
    out[j] = L'\0';
}

static int name_taken(const wchar_t *name, const wchar_t *except_path) {
    for (int i = 0; i < g_layout_count; i++) {
        if (except_path && _wcsicmp(g_layouts[i].path, except_path) == 0) continue;
        if (g_layouts[i].name && _wcsicmp(g_layouts[i].name, name) == 0) return 1;
    }
    return 0;
}

static void unique_name(const wchar_t *base, const wchar_t *except_path,
                        wchar_t *out, size_t n) {
    wcscpy(out, base);
    if (!name_taken(out, except_path)) return;
    for (int i = 2; i < 100; i++) {
        wchar_t candidate[128];
        swprintf(candidate, 128, L"%s (%d)", base, i);
        if (!name_taken(candidate, except_path)) {
            wcscpy(out, candidate);
            return;
        }
    }
}

static void pretty_time(const wchar_t *iso, wchar_t *out, size_t n) {
    if (!iso || !iso[0]) { wcscpy(out, L"—"); return; }
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (swscanf(iso, L"%d-%d-%dT%d:%d", &y, &mo, &d, &h, &mi) == 5) {
        (void)y;
        swprintf(out, n, L"%02d-%02d %02d:%02d", mo, d, h, mi);
    } else {
        wcscpy(out, iso);
    }
}

static void layouts_free(void) {
    for (int i = 0; i < g_layout_count; i++) {
        free(g_layouts[i].path);
        free(g_layouts[i].name);
        free(g_layouts[i].created);
    }
    free(g_layouts);
    g_layouts = NULL;
    g_layout_count = 0;
    g_layout_invalid = 0;
}

static int layout_entry_cmp(const void *a, const void *b) {
    const LayoutEntry *x = a, *y = b;
    int c = _wcsicmp(x->created, y->created);
    if (c) return -c;
    return -_wcsicmp(x->name, y->name);
}

static void layouts_refresh(void) {
    layouts_free();
    wchar_t pattern[MAX_PATH * 2];
    swprintf(pattern, MAX_PATH * 2, L"%s\\*.json", g_layouts_dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    int cap = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        wchar_t full[MAX_PATH * 2];
        swprintf(full, MAX_PATH * 2, L"%s\\%s", g_layouts_dir, fd.cFileName);
        char *text = read_file_bytes(full, NULL);
        if (!text) { g_layout_invalid++; continue; }
        char *name = NULL, *created = NULL;
        int count = 0;
        int have_name = json_top_string(text, "name", &name);
        json_top_string(text, "created_at", &created);
        json_top_int(text, "count", &count);
        if (!have_name || count <= 0) {
            free(name); free(created); free(text);
            g_layout_invalid++;
            continue;
        }
        if (g_layout_count >= cap) {
            cap = cap ? cap * 2 : 8;
            g_layouts = (LayoutEntry *)realloc(g_layouts, (size_t)cap * sizeof(LayoutEntry));
        }
        LayoutEntry *e = &g_layouts[g_layout_count++];
        e->path = wdup(full);
        e->name = utf8_to_w(name);
        e->created = created ? utf8_to_w(created) : wdup(L"");
        e->count = count;
        free(name); free(created); free(text);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(g_layouts, g_layout_count, sizeof(LayoutEntry), layout_entry_cmp);
}

static int layout_path_for_name(const wchar_t *name, wchar_t *preferred_suffix,
                                wchar_t *out, size_t n) {
    wchar_t slug[80];
    filename_slug(name, slug, 80);
    if (!preferred_suffix || !preferred_suffix[0]) {
        wchar_t stamp[64];
        timestamp_now(stamp, 64, 1);
        wcscpy(preferred_suffix, stamp);
    }
    swprintf(out, n, L"%s\\%s__%s.json", g_layouts_dir, slug, preferred_suffix);
    for (int i = 2; i < 100; i++) {
        WIN32_FILE_ATTRIBUTE_DATA fad;
        if (!GetFileAttributesExW(out, GetFileExInfoStandard, &fad)) return 1;
        swprintf(out, n, L"%s\\%s__%s_%d.json", g_layouts_dir, slug, preferred_suffix, i);
    }
    return 1;
}

static int write_layout_named(const wchar_t *path, const wchar_t *name,
                              const char *kind, IconList *list, DWORD *bytes_out) {
    Buf b;
    buf_init(&b);
    write_layout_json(&b, name, kind, list);
    int ok = b.data && write_file_bytes(path, b.data, (DWORD)b.len);
    if (bytes_out) *bytes_out = (DWORD)b.len;
    free(b.data);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (ok && GetFileAttributesExW(path, GetFileExInfoStandard, &fad)) {
        if (fad.nFileSizeLow == 0) ok = 0;
    } else {
        ok = 0;
    }
    return ok;
}

static int json_replace_name(const char *text, const char *new_utf8, char **out) {
    const char *limit = strstr(text, "\"icons\"");
    if (!limit) limit = text + strlen(text);
    const char *p = strstr(text, "\"name\"");
    if (!p || p >= limit) return 0;
    p = strchr(p, ':');
    if (!p || p >= limit) return 0;
    p = skip_ws(p + 1);
    if (*p != '"') return 0;
    const char *end = p + 1;
    int esc = 0;
    while (*end) {
        if (esc) { esc = 0; end++; continue; }
        if (*end == '\\') { esc = 1; end++; continue; }
        if (*end == '"') break;
        end++;
    }
    if (!*end) return 0;
    size_t pre = (size_t)(p - text) + 1;
    size_t postlen = strlen(end);
    char *buf = (char *)malloc(pre + strlen(new_utf8) + postlen + 2);
    if (!buf) return 0;
    memcpy(buf, text, pre);
    memcpy(buf + pre, new_utf8, strlen(new_utf8));
    memcpy(buf + pre + strlen(new_utf8), end, postlen + 1);
    *out = buf;
    return 1;
}

/* ---------------------------------------------------------------- 撤销快照 */

/* 预留扩展点：若后续确认要多级撤销/历史快照，在此处扩展；
 * 当前只维护一份“上一状态”，覆盖写，不保留历史，也不出现在布局列表。 */

static int write_snapshot(Desktop *d, DWORD *bytes_out) {
    IconList current;
    HRESULT hr = desktop_enum(d, &current, NULL);
    if (FAILED(hr)) return 0;
    Buf b;
    buf_init(&b);
    write_layout_json(&b, L"上一状态", "undo", &current);
    int ok = b.data && write_file_bytes(g_undo_path, b.data, (DWORD)b.len);
    if (bytes_out) *bytes_out = (DWORD)b.len;
    free(b.data);
    icon_list_free(&current);

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (ok && GetFileAttributesExW(g_undo_path, GetFileExInfoStandard, &fad)) {
        if (fad.nFileSizeLow == 0) ok = 0;
    } else {
        ok = 0;
    }
    if (!ok) DeleteFileW(g_undo_path);
    return ok;
}

/* 严格校验：声明 count 必须与实际图标数一致，避免截断的快照被半应用。 */
static int snapshot_valid(int *count_out) {
    if (count_out) *count_out = 0;
    DWORD size = 0;
    char *text = read_file_bytes(g_undo_path, &size);
    if (!text || size == 0) {
        free(text);
        return 0;
    }
    IconList list;
    memset(&list, 0, sizeof(list));
    int declared = 0, have_count = json_top_int(text, "count", &declared);
    int ok = parse_layout_json(text, &list) && list.count > 0 &&
             have_count && declared == list.count;
    if (count_out) *count_out = list.count;
    icon_list_free(&list);
    free(text);
    return ok;
}

static void undo_check(void) {
    g_undo_available = snapshot_valid(NULL) ? 1 : 0;
}

/* ---------------------------------------------------------------- 操作 */

typedef struct {
    int ok;
    int count;
    int matched;
    int checked;
    int mismatch;
    int restore_mismatch;
    int auto_arrange;
    DWORD snapshot_bytes;
    wchar_t saved_path[MAX_PATH * 2];
    wchar_t saved_name[128];
    int a_from_x, a_from_y, a_to_x, a_to_y, a_after_x, a_after_y;
    int b_from_x, b_from_y, b_to_x, b_to_y, b_after_x, b_after_y;
    char error[512];
} OpResult;

static int enumerate_info(Desktop *d, IconList *list) {
    return SUCCEEDED(desktop_enum(d, list, NULL));
}

static int do_save_named(const wchar_t *path, const wchar_t *name,
                         const char *kind, OpResult *res) {
    Desktop d;
    memset(res, 0, sizeof(*res));
    HRESULT hr = desktop_open(&d);
    if (FAILED(hr)) {
        snprintf(res->error, sizeof(res->error), "COM open failed: 0x%08lX", (unsigned long)hr);
        return 0;
    }
    IconList list;
    int ok = enumerate_info(&d, &list);
    if (!ok) {
        snprintf(res->error, sizeof(res->error), "enumeration failed");
        desktop_close(&d);
        return 0;
    }
    DWORD bytes = 0;
    int written = write_layout_named(path, name, kind, &list, &bytes);
    if (!written) err_path(res->error, (int)sizeof(res->error), "write failed: ", path);
    res->ok = written;
    res->count = list.count;
    res->auto_arrange = (d.view->lpVtbl->GetAutoArrange(d.view) == S_OK);
    wcscpy(res->saved_path, path);
    wcscpy(res->saved_name, name);
    icon_list_free(&list);
    desktop_close(&d);
    return written;
}

static int do_save(const wchar_t *path, OpResult *res) {
    return do_save_named(path, L"当前布局", "manual", res);
}

static int do_apply(const wchar_t *path, int make_snapshot, OpResult *res) {
    Desktop d;
    memset(res, 0, sizeof(*res));
    DWORD size = 0;
    char *text = read_file_bytes(path, &size);
    if (!text) {
        err_path(res->error, (int)sizeof(res->error), "read failed: ", path);
        return 0;
    }
    IconList saved;
    if (!parse_layout_json(text, &saved) || saved.count == 0) {
        snprintf(res->error, sizeof(res->error), "no icons in layout");
        free(text);
        return 0;
    }
    res->count = saved.count;
    HRESULT hr = desktop_open(&d);
    if (FAILED(hr)) {
        snprintf(res->error, sizeof(res->error), "COM open failed: 0x%08lX", (unsigned long)hr);
        icon_list_free(&saved);
        return 0;
    }
    res->auto_arrange = (d.view->lpVtbl->GetAutoArrange(d.view) == S_OK);
    if (res->auto_arrange) {
        snprintf(res->error, sizeof(res->error),
                 "desktop auto-arrange is enabled; disable it before applying");
        icon_list_free(&saved);
        desktop_close(&d);
        return 0;
    }

    if (make_snapshot) {
        if (!write_snapshot(&d, &res->snapshot_bytes)) {
            snprintf(res->error, sizeof(res->error), "snapshot failed");
            icon_list_free(&saved);
            desktop_close(&d);
            return 0;
        }
        g_undo_available = 1;
    }

    int matched = 0;
    hr = apply_list(&d, &saved, &matched);
    res->matched = matched;
    int checked = 0;
    Mismatch mm[MAX_MISMATCH];
    memset(mm, 0, sizeof(mm));
    int mismatch = verify_list(&d, &saved, 0, &checked, mm, MAX_MISMATCH);
    res->checked = checked;
    res->mismatch = mismatch;
    res->ok = matched > 0 && SUCCEEDED(hr) && mismatch == 0;
    if (matched <= 0) snprintf(res->error, sizeof(res->error), "no matching icons");
    else if (FAILED(hr)) snprintf(res->error, sizeof(res->error), "apply failed: 0x%08lX", (unsigned long)hr);
    else if (mismatch != 0) snprintf(res->error, sizeof(res->error), "readback mismatch: %d", mismatch);
    free_mismatches(mm, MAX_MISMATCH);
    icon_list_free(&saved);
    desktop_close(&d);
    return res->ok;
}

static int do_save_store(const wchar_t *raw_name, OpResult *res) {
    memset(res, 0, sizeof(*res));
    const wchar_t *reason = NULL;
    if (name_illegal(raw_name, &reason)) {
        snprintf(res->error, sizeof(res->error), "invalid name (empty, blank edges, or illegal chars)");
        return 0;
    }
    layouts_refresh();
    wchar_t final_name[128];
    unique_name(raw_name, NULL, final_name, 128);
    wchar_t suffix[64] = L"";
    wchar_t path[MAX_PATH * 2];
    layout_path_for_name(final_name, suffix, path, MAX_PATH * 2);
    int ok = do_save_named(path, final_name, "manual", res);
    if (ok) {
        wcscpy(res->saved_name, final_name);
        wcscpy(res->saved_path, path);
    }
    return ok;
}

static int do_rename(const wchar_t *path, const wchar_t *raw_name, OpResult *res) {
    memset(res, 0, sizeof(*res));
    const wchar_t *reason = NULL;
    if (name_illegal(raw_name, &reason)) {
        snprintf(res->error, sizeof(res->error), "invalid name (empty, blank edges, or illegal chars)");
        return 0;
    }
    char *text = read_file_bytes(path, NULL);
    if (!text) {
        err_path(res->error, (int)sizeof(res->error), "read failed: ", path);
        return 0;
    }
    layouts_refresh();
    wchar_t final_name[128];
    unique_name(raw_name, path, final_name, 128);
    char *utf8 = utf8_from_w(final_name);
    char *updated = NULL;
    if (!utf8 || !json_replace_name(text, utf8, &updated)) {
        free(text);
        free(utf8);
        if (updated) free(updated);
        snprintf(res->error, sizeof(res->error), "name field not found");
        return 0;
    }
    wchar_t suffix[64] = L"";
    wchar_t new_path[MAX_PATH * 2];
    layout_path_for_name(final_name, suffix, new_path, MAX_PATH * 2);
    int ok = write_file_bytes(new_path, updated, (DWORD)strlen(updated));
    if (ok && _wcsicmp(new_path, path) != 0) DeleteFileW(path);
    if (ok) {
        wcscpy(res->saved_name, final_name);
        wcscpy(res->saved_path, new_path);
        res->ok = 1;
    } else {
        err_path(res->error, (int)sizeof(res->error), "write failed: ", new_path);
    }
    free(text);
    free(utf8);
    free(updated);
    return ok;
}

static int do_delete(const wchar_t *path, OpResult *res) {
    memset(res, 0, sizeof(*res));
    size_t base_len = wcslen(g_layouts_dir);
    if (_wcsnicmp(path, g_layouts_dir, base_len) != 0) {
        snprintf(res->error, sizeof(res->error), "outside layouts dir");
        return 0;
    }
    if (!DeleteFileW(path)) {
        snprintf(res->error, sizeof(res->error), "delete failed");
        return 0;
    }
    res->ok = 1;
    return 1;
}

static int do_undo(OpResult *res) {
    memset(res, 0, sizeof(*res));
    if (!snapshot_valid(NULL)) {
        snprintf(res->error, sizeof(res->error), "no valid undo snapshot");
        return 0;
    }
    return do_apply(g_undo_path, 0, res);
}

static int do_swap_test(OpResult *res) {
    Desktop d;
    memset(res, 0, sizeof(*res));
    HRESULT hr = desktop_open(&d);
    if (FAILED(hr)) {
        snprintf(res->error, sizeof(res->error), "COM open failed: 0x%08lX", (unsigned long)hr);
        return 0;
    }
    res->auto_arrange = (d.view->lpVtbl->GetAutoArrange(d.view) == S_OK);

    IconList original;
    if (!enumerate_info(&d, &original) || original.count < 2) {
        snprintf(res->error, sizeof(res->error), "not enough icons");
        desktop_close(&d);
        return 0;
    }
    res->count = original.count;

    int a = -1, b = -1;
    for (int i = 0; i < original.count && a < 0; i++) {
        for (int j = i + 1; j < original.count; j++) {
            if (original.items[i].x != original.items[j].x ||
                original.items[i].y != original.items[j].y) {
                a = i;
                b = j;
                break;
            }
        }
    }
    if (a < 0 || b < 0) {
        snprintf(res->error, sizeof(res->error), "no two icons with distinct positions");
        icon_list_free(&original);
        desktop_close(&d);
        return 0;
    }

    IconList current;
    PITEMID_CHILD *pidls = NULL;
    hr = desktop_enum(&d, &current, &pidls);
    if (FAILED(hr)) {
        snprintf(res->error, sizeof(res->error), "enumeration failed");
        icon_list_free(&original);
        desktop_close(&d);
        return 0;
    }

    int ca = find_icon(&current, original.items[a].id);
    int cb = find_icon(&current, original.items[b].id);
    PITEMID_CHILD sel[2];
    POINT pts[2];
    sel[0] = pidls[ca];
    pts[0].x = original.items[b].x;
    pts[0].y = original.items[b].y;
    sel[1] = pidls[cb];
    pts[1].x = original.items[a].x;
    pts[1].y = original.items[a].y;
    hr = d.view->lpVtbl->SelectAndPositionItems(
        d.view, 2, (PCUITEMID_CHILD_ARRAY)sel, pts, SVSI_POSITIONITEM);

    res->a_from_x = original.items[a].x;
    res->a_from_y = original.items[a].y;
    res->a_to_x = original.items[b].x;
    res->a_to_y = original.items[b].y;
    res->b_from_x = original.items[b].x;
    res->b_from_y = original.items[b].y;
    res->b_to_x = original.items[a].x;
    res->b_to_y = original.items[a].y;

    IconList after;
    if (enumerate_info(&d, &after)) {
        int ia = find_icon(&after, original.items[a].id);
        int ib = find_icon(&after, original.items[b].id);
        if (ia >= 0) {
            res->a_after_x = after.items[ia].x;
            res->a_after_y = after.items[ia].y;
        }
        if (ib >= 0) {
            res->b_after_x = after.items[ib].x;
            res->b_after_y = after.items[ib].y;
        }
        icon_list_free(&after);
    }

    IconList swapped;
    memset(&swapped, 0, sizeof(swapped));
    Icon *e1 = icon_list_add(&swapped);
    if (e1) {
        e1->id = wdup(original.items[a].id);
        e1->name = wdup(original.items[a].name);
        e1->x = original.items[b].x;
        e1->y = original.items[b].y;
    }
    Icon *e2 = icon_list_add(&swapped);
    if (e2) {
        e2->id = wdup(original.items[b].id);
        e2->name = wdup(original.items[b].name);
        e2->x = original.items[a].x;
        e2->y = original.items[a].y;
    }
    int checked = 0;
    Mismatch mm[MAX_MISMATCH];
    memset(mm, 0, sizeof(mm));
    int swap_mismatch = verify_list(&d, &swapped, 2, &checked, mm, MAX_MISMATCH);
    free_mismatches(mm, MAX_MISMATCH);
    icon_list_free(&swapped);

    free_pidls(pidls, current.count);
    icon_list_free(&current);

    int restore_attempts = 0;
    int restore_mismatch = 0;
    do {
        int matched = 0;
        apply_list(&d, &original, &matched);
        Mismatch rm[MAX_MISMATCH];
        memset(rm, 0, sizeof(rm));
        restore_mismatch = verify_list(&d, &original, 0, NULL, rm, MAX_MISMATCH);
        if (restore_mismatch < 0) restore_mismatch = 9999;
        free_mismatches(rm, MAX_MISMATCH);
        restore_attempts++;
    } while (restore_mismatch != 0 && restore_attempts < 2);

    res->checked = checked;
    res->mismatch = swap_mismatch;
    res->restore_mismatch = restore_mismatch;
    res->ok = SUCCEEDED(hr) && swap_mismatch == 0 && restore_mismatch == 0;
    if (FAILED(hr)) snprintf(res->error, sizeof(res->error), "swap apply failed: 0x%08lX", (unsigned long)hr);
    else if (swap_mismatch != 0) snprintf(res->error, sizeof(res->error), "swap readback mismatch: %d", swap_mismatch);
    else if (restore_mismatch != 0) snprintf(res->error, sizeof(res->error), "restore mismatch: %d", restore_mismatch);

    icon_list_free(&original);
    desktop_close(&d);
    return res->ok;
}

/* ---------------------------------------------------------------- CLI */

static void write_result_json(const wchar_t *path, const char *mode,
                              const wchar_t *layout_path, OpResult *res) {
    Buf b;
    buf_init(&b);
    buf_printf(&b, "{\n  \"ok\": %s,\n  \"mode\": \"%s\",\n  \"layout\": ",
               res->ok ? "true" : "false", mode);
    buf_json_w(&b, layout_path);
    buf_printf(&b, ",\n  \"count\": %d,\n  \"matched\": %d,\n  \"checked\": %d,\n"
                   "  \"mismatch\": %d,\n  \"restore_mismatch\": %d,\n"
                   "  \"auto_arrange\": %s,\n  \"snapshot_bytes\": %lu,\n"
                   "  \"undo_available\": %s",
               res->count, res->matched, res->checked, res->mismatch,
               res->restore_mismatch, res->auto_arrange ? "true" : "false",
               (unsigned long)res->snapshot_bytes,
               g_undo_available ? "true" : "false");
    if (res->saved_name[0]) {
        buf_puts(&b, ",\n  \"saved_name\": ");
        buf_json_w(&b, res->saved_name);
        buf_puts(&b, ",\n  \"saved_path\": ");
        buf_json_w(&b, res->saved_path);
    }
    if (strcmp(mode, "swap-test") == 0) {
        buf_printf(&b,
                   ",\n  \"swap\": {\n"
                   "    \"a\": {\"from\": [%d, %d], \"to\": [%d, %d], \"after\": [%d, %d]},\n"
                   "    \"b\": {\"from\": [%d, %d], \"to\": [%d, %d], \"after\": [%d, %d]}\n"
                   "  }",
                   res->a_from_x, res->a_from_y, res->a_to_x, res->a_to_y,
                   res->a_after_x, res->a_after_y,
                   res->b_from_x, res->b_from_y, res->b_to_x, res->b_to_y,
                   res->b_after_x, res->b_after_y);
    }
    if (res->error[0]) {
        buf_puts(&b, ",\n  \"error\": ");
        wchar_t werr[512];
        mbstowcs(werr, res->error, 511);
        werr[511] = 0;
        buf_json_w(&b, werr);
    }
    buf_puts(&b, "\n}\n");
    write_file_bytes(path, b.data, (DWORD)b.len);
    free(b.data);
}

static int run_cli(int argc, wchar_t **argv) {
    const wchar_t *mode = argc > 1 ? argv[1] : NULL;
    const wchar_t *layout = NULL;
    const wchar_t *name = NULL;
    const wchar_t *result = NULL;
    for (int i = 2; i < argc; i++) {
        if (_wcsicmp(argv[i], L"--result") == 0 && i + 1 < argc) {
            result = argv[++i];
        } else if (_wcsicmp(argv[i], L"--name") == 0 && i + 1 < argc) {
            name = argv[++i];
        } else if (!layout) {
            layout = argv[i];
        } else if (!name) {
            name = argv[i];
        }
    }

    wchar_t result_default[MAX_PATH * 2];
    swprintf(result_default, MAX_PATH * 2, L"%s\\result.json", g_data_dir);
    if (!result) result = result_default;

    OpResult res;
    memset(&res, 0, sizeof(res));
    int ok = 0;
    if (_wcsicmp(mode, L"--save") == 0 && layout) {
        ok = do_save(layout, &res);
        write_result_json(result, "save", layout, &res);
    } else if (_wcsicmp(mode, L"--save-named") == 0) {
        const wchar_t *wanted = name ? name : layout;
        if (!wanted) {
            res.ok = 0;
            snprintf(res.error, sizeof(res.error), "name required");
        } else {
            ok = do_save_store(wanted, &res);
        }
        write_result_json(result, "save-named", res.saved_path, &res);
    } else if (_wcsicmp(mode, L"--apply") == 0 && layout) {
        ok = do_apply(layout, 1, &res);
        write_result_json(result, "apply", layout, &res);
    } else if (_wcsicmp(mode, L"--undo") == 0) {
        ok = do_undo(&res);
        write_result_json(result, "undo", g_undo_path, &res);
    } else if (_wcsicmp(mode, L"--rename") == 0 && layout && name) {
        ok = do_rename(layout, name, &res);
        write_result_json(result, "rename", layout, &res);
    } else if (_wcsicmp(mode, L"--delete") == 0 && layout) {
        ok = do_delete(layout, &res);
        write_result_json(result, "delete", layout, &res);
    } else if (_wcsicmp(mode, L"--list") == 0) {
        layouts_refresh();
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\n  \"ok\": true,\n  \"count\": ");
        buf_printf(&b, "%d,\n  \"invalid\": %d,\n  \"items\": [",
                   g_layout_count, g_layout_invalid);
        for (int i = 0; i < g_layout_count; i++) {
            if (i) buf_puts(&b, ",");
            buf_puts(&b, "\n    {\"name\": ");
            buf_json_w(&b, g_layouts[i].name);
            buf_printf(&b, ", \"count\": %d, \"created_at\": ", g_layouts[i].count);
            buf_json_w(&b, g_layouts[i].created);
            buf_puts(&b, ", \"path\": ");
            buf_json_w(&b, g_layouts[i].path);
            buf_puts(&b, "}");
        }
        buf_puts(&b, "\n  ]\n}\n");
        write_file_bytes(result, b.data, (DWORD)b.len);
        free(b.data);
        ok = 1;
    } else if (_wcsicmp(mode, L"--swap-test") == 0) {
        ok = do_swap_test(&res);
        write_result_json(result, "swap-test", layout ? layout : L"", &res);
    } else {
        res.ok = 0;
        snprintf(res.error, sizeof(res.error), "unknown arguments");
        write_result_json(result, "cli", L"", &res);
    }
    return ok ? 0 : 1;
}

/* ---------------------------------------------------------------- GUI */

/* ---------------------------------------------------------------- GUI */

#define ID_NAME 1001
#define ID_SAVE 1002
#define ID_LIST 1003
#define ID_APPLY 1004
#define ID_RENAME 1005
#define ID_DELETE 1006
#define ID_AUTOSTART 1007
#define ID_CONFIRM 1008
#define ID_INLINE 1010
#define ID_EMPTY 1011
#define ID_UNDO 1014
#define ID_DLG_DELETE 1015
#define ID_DLG_CANCEL 1016

#define TRAY_UID 1
#define WM_TRAY_MSG (WM_APP + 1)
#define WM_ACTIVATE_MAIN (WM_APP + 2)
#define ID_TRAY_SAVE 2001
#define ID_TRAY_UNDO 2002
#define ID_TRAY_OPEN 2003
#define ID_TRAY_EXIT 2004
#define ID_TRAY_LAYOUT_BASE 3000

#define CLR_BG RGB(0x1e, 0x1e, 0x1e)
#define CLR_PANEL RGB(0x2d, 0x2d, 0x30)
#define CLR_CARD RGB(0x20, 0x20, 0x21)
#define CLR_TEXT RGB(0xd4, 0xd4, 0xd4)
#define CLR_MUTED RGB(0x88, 0x88, 0x88)
#define CLR_LINE RGB(0x3a, 0x3a, 0x3c)
#define CLR_BORDER CLR_LINE
#define CLR_PRIMARY RGB(0x0e, 0x63, 0x9c)
#define CLR_PRIMARY_HOVER RGB(0x11, 0x77, 0xbb)
#define CLR_PRIMARY_DOWN RGB(0x0a, 0x4d, 0x78)
#define CLR_SELECT RGB(0x09, 0x47, 0x71)
#define CLR_SELECT_HOVER RGB(0x0a, 0x50, 0x7f)
#define CLR_ROW_HOVER RGB(0x2a, 0x2d, 0x2e)
#define CLR_INPUT RGB(0x25, 0x25, 0x26)
#define CLR_INPUT_INLINE RGB(0x1b, 0x1b, 0x1c)
#define CLR_OK RGB(0x34, 0xc7, 0x59)
#define CLR_DANGER RGB(0xff, 0x45, 0x3a)
#define CLR_ERROR_BG RGB(0x3a, 0x1f, 0x1f)
#define CLR_FOCUS RGB(0x3a, 0xa0, 0xff)
#define CLR_SELECT_TEXT RGB(0xea, 0xf2, 0xf8)
#define CLR_SELECT_MUTED RGB(0xb9, 0xd3, 0xe6)
#define CLR_BTN_HOVER RGB(0x35, 0x35, 0x3a)
#define CLR_BTN_DOWN RGB(0x26, 0x26, 0x29)
#define CLR_CHECK_HOVER RGB(0x24, 0x24, 0x26)
#define CLR_CHECK_BORDER RGB(0x5a, 0x5a, 0x5d)
#define CLR_DLG RGB(0x2c, 0x2c, 0x2e)
#define CLR_SCROLL RGB(0x3a, 0x3a, 0x3c)
#define CLR_SCROLL_HOVER RGB(0x4a, 0x4a, 0x4d)

static int g_dpi = 96;
static HWND g_hwnd = NULL, g_hName = NULL, g_hSave = NULL, g_hList = NULL;
static HWND g_hApply = NULL, g_hRename = NULL, g_hDelete = NULL, g_hUndo = NULL;
static HWND g_hAutoStart = NULL, g_hConfirm = NULL;
static HWND g_hInline = NULL, g_hEmpty = NULL;
static HICON g_iconSmall = NULL, g_iconBig = NULL;
static int g_tray_added = 0;
static int g_in_tray = 0;
static int g_quitting = 0;
static UINT g_taskbar_created_msg = 0;
static HANDLE g_instance_mutex = NULL;
static int g_start_in_tray = 0;
static int g_test_focus = 0;   /* --test-focus：键盘导航态自测（焦点环） */
static HFONT g_font = NULL, g_fontSmall = NULL, g_fontBold = NULL;
static HBRUSH g_brBg = NULL, g_brPanel = NULL, g_brInput = NULL, g_brSelect = NULL;
static HBRUSH g_brRowHover = NULL, g_brPrimary = NULL, g_brPrimaryHover = NULL;
static HBRUSH g_brPrimaryDown = NULL, g_brCard = NULL, g_brInline = NULL;
static WNDPROC g_old_list = NULL, g_old_name = NULL, g_old_inline = NULL;
static WNDPROC g_old_btn = NULL;
static int g_sel = -1;
static int g_hover_item = -1;
static int g_btn_hover = 0;
static int g_hint_id = 0;
/* 焦点环只在键盘 Tab 导航后绘制；任何鼠标按下都会清掉。 */
static int g_key_nav = 0;
static HWND g_dlg = NULL;
static HINSTANCE g_inst = NULL;
static int g_dlg_result = 0;
static int g_dlg_hover = 0;
static const wchar_t *g_dlg_name = L"";
static int g_sb_drag = 0;
static int g_sb_off = 0;
static int g_sb_hover = 0;
static int g_rename_active = 0;
static int g_inline_cancel = 0;
static int g_name_error = 0;
static int g_inline_error = 0;
static RECT g_inline_rect;
static HBRUSH g_brInputErr = NULL;
static int g_status_kind = 0;
static wchar_t g_status_text[1024] = L"就绪";
static wchar_t g_rename_path[MAX_PATH * 2] = L"";

#define S(v) MulDiv((v), g_dpi, 96)

static void set_status(const wchar_t *text, int kind) {
    wcsncpy(g_status_text, text, 1023);
    g_status_text[1023] = 0;
    g_status_kind = kind;
    if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
}

/* 把 do_* 的窄错误串转成宽串（%hs 在 zig 的 printf 里不可靠）。 */
static void err_w(const char *src, wchar_t *out, int n) {
    out[0] = 0;
    if (src && src[0]) MultiByteToWideChar(CP_UTF8, 0, src, -1, out, n);
}

static void set_default_name(void) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[128];
    swprintf(name, 128, L"布局 %02d-%02d %02d-%02d", st.wMonth, st.wDay, st.wHour, st.wMinute);
    SetWindowTextW(g_hName, name);
}

static int selected_index(void) {
    if (g_layout_count <= 0) return -1;
    if (g_sel < 0 || g_sel >= g_layout_count) return -1;
    return g_sel;
}

static void update_buttons(void) {
    BOOL sel = selected_index() >= 0;
    EnableWindow(g_hApply, g_layout_count > 0);
    EnableWindow(g_hUndo, g_undo_available);
    EnableWindow(g_hRename, sel);
    EnableWindow(g_hDelete, sel);
}

static void hide_inline(void) {
    if (g_hInline) ShowWindow(g_hInline, SW_HIDE);
    g_rename_active = 0;
    g_rename_path[0] = 0;
    g_inline_error = 0;
    if (g_name_error) g_name_error = 0;
}

static void refresh_list(const wchar_t *select_path) {
    hide_inline();
    layouts_refresh();
    SendMessageW(g_hList, LB_RESETCONTENT, 0, 0);
    for (int i = 0; i < g_layout_count; i++) {
        SendMessageW(g_hList, LB_ADDSTRING, 0, (LPARAM)g_layouts[i].name);
        SendMessageW(g_hList, LB_SETITEMDATA, (WPARAM)i, (LPARAM)i);
    }
    int target = -1;
    if (select_path && select_path[0]) {
        for (int i = 0; i < g_layout_count; i++) {
            if (_wcsicmp(g_layouts[i].path, select_path) == 0) { target = i; break; }
        }
    }
    if (target < 0 && g_layout_count > 0) target = 0;
    g_sel = target;
    if (target >= 0) SendMessageW(g_hList, LB_SETCURSEL, (WPARAM)target, 0);
    ShowWindow(g_hEmpty, g_layout_count == 0 ? SW_SHOW : SW_HIDE);
    update_buttons();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void fill_rect_color(HDC dc, RECT r, COLORREF color) {
    HBRUSH br = CreateSolidBrush(color);
    FillRect(dc, &r, br);
    DeleteObject(br);
}

/* ---- GDI+ 抗锯齿圆角渲染管线（4x 超采样 + HighQualityBicubic 缩小 + 位图缓存） ---- */

#define RC_MAX 48

typedef struct {
    int valid;
    int w, h, rad, bw;
    COLORREF bg, fill, border;
    unsigned long age;
    HDC dc;
    HBITMAP bmp;
    HGDIOBJ old;
} RoundCache;

static RoundCache g_rc[RC_MAX];
static int g_rc_count = 0;
static unsigned long g_rc_tick = 0;
static ULONG_PTR g_gdip_token = 0;
static int g_gdip_ok = 0;
static double g_last_render_ms = 0.0;
static double g_last_blit_ms = 0.0;

static double qpc_ms(LARGE_INTEGER freq, LARGE_INTEGER a, LARGE_INTEGER b) {
    return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)freq.QuadPart;
}

static int gdip_start(void) {
    if (g_gdip_ok) return 1;
    GdiplusStartupInput in;
    memset(&in, 0, sizeof(in));
    in.GdiplusVersion = 1;
    in.DebugEventCallback = NULL;
    in.SuppressBackgroundThread = FALSE;
    in.SuppressExternalCodecs = FALSE;
    g_gdip_ok = (GdiplusStartup(&g_gdip_token, &in, NULL) == Ok);
    return g_gdip_ok;
}

static void gdip_stop(void) {
    if (!g_gdip_ok) return;
    GdiplusShutdown(g_gdip_token);
    g_gdip_ok = 0;
}

static HBITMAP make_dib(int w, int h, void **bits) {
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    return CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, bits, NULL, 0);
}

static DWORD argb_of(COLORREF c) {
    return 0xFF000000u | ((DWORD)GetRValue(c) << 16) |
           ((DWORD)GetGValue(c) << 8) | (DWORD)GetBValue(c);
}

static void add_round_path(GpPath *p, float x, float y, float w, float h,
                           float r) {
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    float d = r * 2;
    GdipAddPathArc(p, x, y, d, d, 180, 90);
    GdipAddPathArc(p, x + w - d, y, d, d, 270, 90);
    GdipAddPathArc(p, x + w - d, y + h - d, d, d, 0, 90);
    GdipAddPathArc(p, x, y + h - d, d, d, 90, 90);
    GdipClosePathFigure(p);
}

static void free_round_entry(RoundCache *e) {
    if (!e->valid) return;
    if (e->dc) {
        if (e->old) SelectObject(e->dc, e->old);
        DeleteDC(e->dc);
    }
    if (e->bmp) DeleteObject(e->bmp);
    e->valid = 0;
    e->dc = NULL;
    e->bmp = NULL;
    e->old = NULL;
}

static void round_cache_clear(void) {
    for (int i = 0; i < RC_MAX; i++) {
        free_round_entry(&g_rc[i]);
        g_rc[i].age = 0;
    }
    g_rc_count = 0;
}

static HDC render_round(int w, int h, int rad, COLORREF bg, COLORREF fill,
                        COLORREF border, int bw) {
    LARGE_INTEGER freq, t0, t1, t2, t3;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    int ss = 4;   /* 统一 4x 超采样 + Bicubic 缩小 */
    int sw = w * ss, sh = h * ss;
    void *sbits = NULL;
    HBITMAP sbmp = make_dib(sw, sh, &sbits);
    if (!sbmp) return NULL;
    DWORD bgv = argb_of(bg);
    DWORD *sp = (DWORD *)sbits;
    int total = sw * sh;
    for (int i = 0; i < total; i++) sp[i] = bgv;

    GpBitmap *sb = NULL;
    GpGraphics *sg = NULL;
    GpPath *path = NULL;
    GpSolidFill *brush = NULL;
    if (GdipCreateBitmapFromScan0(sw, sh, sw * 4, PixelFormat32bppARGB,
                                  (BYTE *)sbits, &sb) != Ok) {
        DeleteObject(sbmp);
        return NULL;
    }
    GdipGetImageGraphicsContext(sb, &sg);
    GdipSetSmoothingMode(sg, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(sg, PixelOffsetModeHighQuality);
    GdipSetCompositingMode(sg, CompositingModeSourceOver);
    GdipCreatePath(FillModeAlternate, &path);
    add_round_path(path, 0.0f, 0.0f, (float)sw, (float)sh, (float)rad * (float)ss);
    GdipCreateSolidFill(argb_of(fill), &brush);
    GdipFillPath(sg, brush, path);
    if (bw > 0 && border != fill) {
        GpPen *pen = NULL;
        GdipCreatePen1(argb_of(border), (float)bw * (float)ss, UnitPixel, &pen);
        GdipDrawPath(sg, pen, path);
        GdipDeletePen(pen);
    }
    if (brush) GdipDeleteBrush(brush);
    if (path) GdipDeletePath(path);
    if (sg) GdipDeleteGraphics(sg);
    GdipDisposeImage(sb);
    QueryPerformanceCounter(&t1);

    void *obits = NULL;
    HBITMAP obmp = make_dib(w, h, &obits);
    if (!obmp) {
        DeleteObject(sbmp);
        return NULL;
    }
    GpBitmap *sb2 = NULL;
    GpBitmap *ob = NULL;
    GpGraphics *og = NULL;
    GdipCreateBitmapFromScan0(sw, sh, sw * 4, PixelFormat32bppARGB,
                              (BYTE *)sbits, &sb2);
    GdipCreateBitmapFromScan0(w, h, w * 4, PixelFormat32bppARGB,
                              (BYTE *)obits, &ob);
    GdipGetImageGraphicsContext(ob, &og);
    GdipSetInterpolationMode(og, InterpolationModeHighQualityBicubic);
    GdipSetPixelOffsetMode(og, PixelOffsetModeHighQuality);
    GdipSetCompositingMode(og, CompositingModeSourceCopy);
    GdipDrawImageRectI(og, sb2, 0, 0, w, h);
    if (og) GdipDeleteGraphics(og);
    if (ob) GdipDisposeImage(ob);
    if (sb2) GdipDisposeImage(sb2);
    DeleteObject(sbmp);
    QueryPerformanceCounter(&t2);

    HDC mdc = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(mdc, obmp);
    QueryPerformanceCounter(&t3);
    g_last_render_ms = qpc_ms(freq, t0, t2);
    g_last_blit_ms = qpc_ms(freq, t2, t3);
    (void)t1;
    return mdc;
}

static HDC round_cache(int w, int h, int rad, COLORREF bg, COLORREF fill,
                       COLORREF border, int bw) {
    if (!g_gdip_ok || w <= 0 || h <= 0) return NULL;
    for (int i = 0; i < g_rc_count; i++) {
        RoundCache *e = &g_rc[i];
        if (e->valid && e->w == w && e->h == h && e->rad == rad && e->bw == bw &&
            e->bg == bg && e->fill == fill && e->border == border) {
            e->age = ++g_rc_tick;
            return e->dc;
        }
    }
    int slot = -1;
    if (g_rc_count < RC_MAX) {
        slot = g_rc_count++;
    } else {
        unsigned long best = (unsigned long)-1;
        for (int i = 0; i < RC_MAX; i++) {
            if (g_rc[i].age < best) {
                best = g_rc[i].age;
                slot = i;
            }
        }
        free_round_entry(&g_rc[slot]);
    }
    HDC dc = render_round(w, h, rad, bg, fill, border, bw);
    if (!dc) return NULL;
    RoundCache *e = &g_rc[slot];
    e->valid = 1;
    e->w = w;
    e->h = h;
    e->rad = rad;
    e->bw = bw;
    e->bg = bg;
    e->fill = fill;
    e->border = border;
    e->age = ++g_rc_tick;
    e->dc = dc;
    e->bmp = (HBITMAP)GetCurrentObject(dc, OBJ_BITMAP);
    e->old = NULL;
    return e->dc;
}

static void fill_square_border(HDC dc, RECT r, COLORREF color, int bw) {
    RECT t;
    t.left = r.left; t.top = r.top; t.right = r.right; t.bottom = r.top + bw;
    fill_rect_color(dc, t, color);
    t.top = r.bottom - bw; t.bottom = r.bottom;
    fill_rect_color(dc, t, color);
    t.left = r.left; t.right = r.left + bw; t.top = r.top; t.bottom = r.bottom;
    fill_rect_color(dc, t, color);
    t.left = r.right - bw; t.right = r.right;
    fill_rect_color(dc, t, color);
}

static void draw_round(HDC dc, RECT r, int rad, COLORREF fill, COLORREF border,
                       int bw, COLORREF bg) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    HDC src = round_cache(w, h, rad, bg, fill, border, bw);
    if (!src) {
        fill_rect_color(dc, r, fill);
        if (bw > 0 && border != fill) fill_square_border(dc, r, border, bw);
        return;
    }
    BitBlt(dc, r.left, r.top, w, h, src, 0, 0, SRCCOPY);
}

static void aa_stroke(HDC dc, RECT r, int rad, COLORREF color, int width) {
    if (!g_gdip_ok) {
        fill_square_border(dc, r, color, width);
        return;
    }
    GpGraphics *g = NULL;
    GdipCreateFromHDC(dc, &g);
    if (!g) {
        fill_square_border(dc, r, color, width);
        return;
    }
    GdipSetSmoothingMode(g, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(g, PixelOffsetModeHighQuality);
    GpPath *path = NULL;
    GdipCreatePath(FillModeAlternate, &path);
    add_round_path(path, (float)r.left, (float)r.top,
                   (float)(r.right - r.left - 1), (float)(r.bottom - r.top - 1),
                   (float)rad);
    GpPen *pen = NULL;
    GdipCreatePen1(argb_of(color), (float)width, UnitPixel, &pen);
    GdipDrawPath(g, pen, path);
    GdipDeletePen(pen);
    GdipDeletePath(path);
    GdipDeleteGraphics(g);
}

static const wchar_t *hint_text(int id) {
    switch (id) {
    case ID_SAVE: return L"保存当前图标位置";
    case ID_APPLY: return L"把选中布局应用到桌面";
    case ID_UNDO: return L"恢复到应用前的桌面";
    case ID_RENAME: return L"重命名选中布局";
    case ID_AUTOSTART: return L"启动时只驻留托盘";
    case ID_CONFIRM: return L"删除前是否弹出确认框";
    }
    return NULL;
}

/* ---- 自绘滚动条（列表右侧 12px 槽，滑块半径 4 走圆角管线） ---- */

typedef struct {
    int active;
    int left, width, top, height;
    int thumb_y, thumb_h;
    int count, vis, topidx;
} SbInfo;

static void sb_get(SbInfo *si) {
    memset(si, 0, sizeof(*si));
    if (!g_hList || !g_hwnd) return;
    RECT lr, lc;
    GetWindowRect(g_hList, &lr);
    MapWindowPoints(NULL, g_hwnd, (POINT *)&lr, 2);
    GetClientRect(g_hList, &lc);
    int count = (int)SendMessageW(g_hList, LB_GETCOUNT, 0, 0);
    int top = (int)SendMessageW(g_hList, LB_GETTOPINDEX, 0, 0);
    int vis = (lc.bottom - lc.top) / S(ROW_H);
    si->left = lr.right + S(2);
    si->width = S(8);
    si->top = lr.top;
    si->height = lr.bottom - lr.top;
    si->count = count;
    si->vis = vis;
    si->topidx = top;
    if (vis <= 0 || count <= vis) return;
    si->active = 1;
    int th = (int)((double)si->height * vis / count);
    if (th < S(24)) th = S(24);
    if (th > si->height) th = si->height;
    si->thumb_h = th;
    int maxtop = count - vis;
    si->thumb_y = si->top +
        (maxtop > 0 ? (int)((double)(si->height - th) * top / maxtop) : 0);
}

static void sb_invalidate(void) {
    SbInfo si;
    sb_get(&si);
    if (!si.active) return;
    RECT r = {si.left - S(2), si.top, si.left + si.width + S(2), si.top + si.height};
    InvalidateRect(g_hwnd, &r, FALSE);
}

static void sb_set_top(int top) {
    if (!g_hList) return;
    int count = (int)SendMessageW(g_hList, LB_GETCOUNT, 0, 0);
    RECT lc;
    GetClientRect(g_hList, &lc);
    int vis = (lc.bottom - lc.top) / S(ROW_H);
    int maxtop = count - vis;
    if (maxtop < 0) maxtop = 0;
    if (top < 0) top = 0;
    if (top > maxtop) top = maxtop;
    SendMessageW(g_hList, LB_SETTOPINDEX, (WPARAM)top, 0);
    sb_invalidate();
}

static void draw_scrollbar(HDC dc) {
    SbInfo si;
    sb_get(&si);
    if (!si.active) return;
    int hover = g_sb_hover || g_sb_drag;
    RECT th = {si.left, si.thumb_y, si.left + si.width, si.thumb_y + si.thumb_h};
    draw_round(dc, th, S(4), hover ? CLR_SCROLL_HOVER : CLR_SCROLL,
               hover ? CLR_SCROLL_HOVER : CLR_SCROLL, 0, CLR_CARD);
}

static void focus_step(HWND cur, int back) {
    HWND order[] = {g_hName, g_hSave, g_hList, g_hApply, g_hUndo,
                    g_hRename, g_hDelete, g_hAutoStart, g_hConfirm};
    int n = (int)(sizeof(order) / sizeof(order[0]));
    int start = -1;
    for (int i = 0; i < n; i++) if (order[i] == cur) start = i;
    if (start < 0) start = back ? 0 : n - 1;
    for (int k = 1; k <= n; k++) {
        int i = (start + (back ? -k : k) + n * 2) % n;
        HWND h = order[i];
        if (h && IsWindowVisible(h) && IsWindowEnabled(h)) {
            g_key_nav = 1;
            SetFocus(h);
            InvalidateRect(h, NULL, FALSE);
            return;
        }
    }
}

static void layout_ui(void) {
    if (!g_hwnd || !g_hName) return;
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int m = S(12);
    int gap = S(8);
    int nameH = S(30);
    int saveW = S(88);
    int headH = S(26);
    int statusH = S(26);
    int statusY = H - m - statusH;
    int actionsH = S(30);
    int actionsY = statusY - gap - actionsH;
    int cardX = m;
    int cardW = W - m * 2;
    int cardY = m + nameH + S(10);
    int cardBottom = actionsY - gap;
    int cardH = cardBottom - cardY;
    if (cardH < S(120)) cardH = S(120);
    int sbW = S(12);
    int listH = cardH - headH - S(7);
    if (listH < S(ROW_H)) listH = S(ROW_H);
    int nameW = cardW - saveW - gap;

    MoveWindow(g_hName, m + S(2), m + S(2), nameW - S(4), nameH - S(4), TRUE);
    MoveWindow(g_hSave, W - m - saveW, m, saveW, nameH, TRUE);
    MoveWindow(g_hList, cardX + 1, cardY + headH, cardW - 2 - sbW, listH, TRUE);
    MoveWindow(g_hEmpty, cardX + 1, cardY + headH, cardW - 2 - sbW, listH, TRUE);

    int applyW = S(72), undoW = S(72), renameW = S(80), delW = S(80);
    int btnGap = S(6);
    int x = m;
    MoveWindow(g_hApply, x, actionsY, applyW, actionsH, TRUE);
    x += applyW + btnGap;
    MoveWindow(g_hUndo, x, actionsY, undoW, actionsH, TRUE);
    x += undoW + btnGap;
    MoveWindow(g_hRename, x, actionsY, renameW, actionsH, TRUE);
    x += renameW + btnGap;
    MoveWindow(g_hDelete, x, actionsY, delW, actionsH, TRUE);

    MoveWindow(g_hAutoStart, W - m - S(112), actionsY, S(112), actionsH, TRUE);

    /* 「删除前确认」放在状态栏行尾：不新增行、不改字号、不压缩按钮。 */
    MoveWindow(g_hConfirm, W - m - S(104), statusY, S(104), statusH, TRUE);

    if (g_hInline && g_rename_active && g_sel >= 0) {
        RECT ir = {0, 0, 0, 0};
        SendMessageW(g_hList, LB_GETITEMRECT, (WPARAM)g_sel, (LPARAM)&ir);
        POINT pt = {0, 0};
        ClientToScreen(g_hList, &pt);
        ScreenToClient(g_hwnd, &pt);
        int eh = S(24);
        int top = pt.y + ir.top + (S(ROW_H) - eh) / 2;
        int right = pt.x + (cardW - 2 - sbW) - S(12);
        int left = pt.x + S(12);
        int editW = right - left;
        if (editW < S(80)) editW = S(80);
        g_inline_rect.left = left;
        g_inline_rect.top = top;
        g_inline_rect.right = left + editW;
        g_inline_rect.bottom = top + eh;
        MoveWindow(g_hInline, left + S(2), top + S(2), editW - S(4), eh - S(4), TRUE);
        InvalidateRect(g_hList, NULL, FALSE);
    }
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static void create_fonts(void) {
    if (g_font) DeleteObject(g_font);
    if (g_fontSmall) DeleteObject(g_fontSmall);
    if (g_fontBold) DeleteObject(g_fontBold);
    g_font = CreateFontW(-MulDiv(10, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                         DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                         CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_fontSmall = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                              DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    g_fontBold = CreateFontW(-MulDiv(10, g_dpi, 72), 0, 0, 0, FW_BOLD, 0, 0, 0,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
}

static void apply_dark_title(HWND hwnd) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    typedef HRESULT (WINAPI *DwmSetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
    DwmSetAttrFn fn = (DwmSetAttrFn)(void *)GetProcAddress(dwm, "DwmSetWindowAttribute");
    if (fn) {
        DWORD dark = 1;
        if (FAILED(fn(hwnd, 20, &dark, sizeof(dark)))) fn(hwnd, 19, &dark, sizeof(dark));
        COLORREF caption = CLR_BG;
        fn(hwnd, 35, &caption, sizeof(caption));
        COLORREF text = CLR_TEXT;
        fn(hwnd, 36, &text, sizeof(text));
        COLORREF border = RGB(0x33, 0x33, 0x36);
        fn(hwnd, 34, &border, sizeof(border));
    }
    FreeLibrary(dwm);
}

static void use_dark_scrollbar(HWND h) {
    typedef HRESULT (WINAPI *SetThemeFn)(HWND, LPCWSTR, LPCWSTR);
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    if (!ux) return;
    SetThemeFn fn = (SetThemeFn)(void *)GetProcAddress(ux, "SetWindowTheme");
    if (fn) fn(h, L"DarkMode_Explorer", NULL);
    FreeLibrary(ux);
}

static void draw_button(HDC dc, const DRAWITEMSTRUCT *dis) {
    RECT r = dis->rcItem;
    int id = (int)dis->CtlID;
    int disabled = (dis->itemState & ODS_DISABLED) != 0;
    int pressed = (dis->itemState & ODS_SELECTED) != 0 && !disabled;
    int hover = (g_btn_hover == id) && !disabled;
    int focused = (GetFocus() == dis->hwndItem) && !disabled && g_key_nav;
    int rad = S(8);

    if (id == ID_AUTOSTART || id == ID_CONFIRM) {
        int on = (id == ID_AUTOSTART) ? g_autostart : g_confirm_delete;
        COLORREF hbg = hover ? CLR_CHECK_HOVER : CLR_BG;
        draw_round(dc, r, rad, hbg, hbg, 0, CLR_BG);
        int bs = S(16);
        RECT box = {r.left + S(4), r.top + (r.bottom - r.top - bs) / 2,
                    r.left + S(4) + bs, r.top + (r.bottom - r.top - bs) / 2 + bs};
        draw_round(dc, box, S(4), on ? CLR_PRIMARY : CLR_INPUT,
                   on ? CLR_PRIMARY : CLR_CHECK_BORDER, 1, hbg);
        if (on) {
            HPEN pen = CreatePen(PS_SOLID, 2, RGB(0xff, 0xff, 0xff));
            HGDIOBJ old = SelectObject(dc, pen);
            MoveToEx(dc, box.left + S(3), box.top + S(8), NULL);
            LineTo(dc, box.left + S(6), box.top + S(11));
            LineTo(dc, box.right - S(4), box.top + S(4));
            SelectObject(dc, old);
            DeleteObject(pen);
        }
        if (focused) {
            RECT ring = {box.left - S(4), box.top - S(4),
                         box.right + S(4), box.bottom + S(4)};
            aa_stroke(dc, ring, S(6), CLR_FOCUS, 2);
        }
        RECT tr = {box.right + S(8), r.top, r.right - S(4), r.bottom};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, disabled ? RGB(0x6a, 0x6a, 0x6a) : CLR_TEXT);
        SelectObject(dc, id == ID_CONFIRM ? g_fontSmall : g_font);
        wchar_t text[128] = L"";
        GetWindowTextW(dis->hwndItem, text, 128);
        DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                  DT_NOPREFIX | DT_END_ELLIPSIS);
        return;
    }

    int primary = (id == ID_SAVE || id == ID_APPLY);
    COLORREF bg, fg, border;
    if (disabled) {
        bg = CLR_PANEL;
        fg = RGB(0x6a, 0x6a, 0x6a);
        border = CLR_LINE;
    } else if (primary) {
        bg = pressed ? CLR_PRIMARY_DOWN : (hover ? CLR_PRIMARY_HOVER : CLR_PRIMARY);
        fg = RGB(0xff, 0xff, 0xff);
        border = bg;
    } else {
        bg = pressed ? CLR_BTN_DOWN : (hover ? CLR_BTN_HOVER : CLR_PANEL);
        fg = CLR_TEXT;
        border = CLR_LINE;
    }

    draw_round(dc, r, rad, bg, border, border != bg ? 1 : 0, CLR_BG);

    wchar_t text[128] = L"";
    GetWindowTextW(dis->hwndItem, text, 128);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    SelectObject(dc, g_font);
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (focused) {
        RECT fr = {r.left + S(1), r.top + S(1), r.right - S(1), r.bottom - S(1)};
        aa_stroke(dc, fr, S(7), CLR_FOCUS, 2);
    }
}

static LRESULT CALLBACK BtnProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK) {
        if (g_key_nav) {
            g_key_nav = 0;
            if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
            InvalidateRect(h, NULL, FALSE);
        }
    }
    if (m == WM_MOUSEMOVE) {
        int id = (int)GetDlgCtrlID(h);
        if (g_btn_hover != id) {
            g_btn_hover = id;
            InvalidateRect(h, NULL, FALSE);
        }
        if (g_hint_id != id) {
            g_hint_id = id;
            if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
        }
        TRACKMOUSEEVENT tme;
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = h;
        tme.dwHoverTime = 0;
        TrackMouseEvent(&tme);
        SetCursor(LoadCursorW(NULL, IDC_HAND));
    } else if (m == WM_MOUSELEAVE) {
        if (g_btn_hover == (int)GetDlgCtrlID(h)) {
            g_btn_hover = 0;
            InvalidateRect(h, NULL, FALSE);
        }
        if (g_hint_id == (int)GetDlgCtrlID(h)) {
            g_hint_id = 0;
            if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
        }
    } else if (m == WM_SETFOCUS || m == WM_KILLFOCUS) {
        InvalidateRect(h, NULL, FALSE);
    } else if (m == WM_KEYDOWN && w == VK_TAB) {
        focus_step(h, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        return 0;
    }
    return CallWindowProcW(g_old_btn, h, m, w, l);
}

static LRESULT CALLBACK ListProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK) {
        if (g_key_nav) {
            g_key_nav = 0;
            if (g_hwnd) InvalidateRect(g_hwnd, NULL, FALSE);
            InvalidateRect(h, NULL, FALSE);
        }
    }
    if (m == WM_MOUSEMOVE) {
        DWORD pos = (DWORD)l;
        int x = (int)(short)LOWORD(pos), y = (int)(short)HIWORD(pos);
        DWORD hit = (DWORD)SendMessageW(h, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y));
        int idx = (HIWORD(hit) == 0) ? (int)LOWORD(hit) : -1;
        if (idx != g_hover_item) {
            g_hover_item = idx;
            InvalidateRect(h, NULL, FALSE);
        }
        TRACKMOUSEEVENT tme;
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = h;
        tme.dwHoverTime = 0;
        TrackMouseEvent(&tme);
    } else if (m == WM_MOUSELEAVE) {
        if (g_hover_item != -1) {
            g_hover_item = -1;
            InvalidateRect(h, NULL, FALSE);
        }
    } else if (m == WM_SETFOCUS || m == WM_KILLFOCUS) {
        InvalidateRect(h, NULL, FALSE);
    } else if (m == WM_KEYDOWN && w == VK_TAB) {
        focus_step(h, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        return 0;
    }
    LRESULT res = CallWindowProcW(g_old_list, h, m, w, l);
    if (m == WM_MOUSEWHEEL || (m == WM_KEYDOWN && w != VK_TAB) || m == WM_LBUTTONUP)
        sb_invalidate();
    return res;
}

static void begin_rename(void);
static void commit_rename(int cancel);
static void on_save_click(void);
static int confirm_delete(const wchar_t *name);

static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN) {
        if (w == VK_RETURN) {
            if (h == g_hName) on_save_click();
            else if (h == g_hInline) commit_rename(0);
            return 0;
        }
        if (w == VK_ESCAPE) {
            if (h == g_hInline) commit_rename(1);
            else SetFocus(g_hList);
            return 0;
        }
        if (w == VK_TAB) {
            int back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (h == g_hInline) {
                commit_rename(0);
                if (!g_rename_active) focus_step(g_hList, back);
            } else {
                focus_step(h, back);
            }
            return 0;
        }
    }
    if (m == WM_CHAR && w == 13) return 0;
    WNDPROC old = (h == g_hName) ? g_old_name : g_old_inline;
    return CallWindowProcW(old, h, m, w, l);
}

/* ---------------------------------------------------------------- 删除确认（模态自绘） */

/* 分层窗口接管整窗像素后，子窗口不会再被合成上屏（实测按钮不可见）。
 * 因此弹窗按钮改为自绘：状态机 + 命中测试在 DlgProc 内完成。 */
static int g_dlg_focus_id = ID_DLG_CANCEL;   /* 默认焦点：取消 */
static int g_dlg_press_id = 0;

/* 高度由内容计算：上下内边距 + 标题 + 间距 + 副文案 + 间距 + 按钮行。 */
static int dlg_height(void) {
    return S(18) + S(24) + S(6) + S(20) + S(16) + S(32) + S(18);
}

static void dlg_btn_rect(int w, int hh, int id, RECT *out) {
    int padX = S(20), bw = S(80), gap = S(8), bh = S(32), padY = S(18);
    int y = hh - padY - bh;
    out->left = (id == ID_DLG_DELETE) ? w - padX - bw : w - padX - bw - gap - bw;
    out->top = y;
    out->right = out->left + bw;
    out->bottom = y + bh;
}

static int dlg_hit_test(int w, int hh, int x, int y) {
    RECT r;
    dlg_btn_rect(w, hh, ID_DLG_DELETE, &r);
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return ID_DLG_DELETE;
    dlg_btn_rect(w, hh, ID_DLG_CANCEL, &r);
    if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return ID_DLG_CANCEL;
    return 0;
}

static void dlg_draw_button(HDC dc, int w, int hh, int id) {
    RECT r;
    dlg_btn_rect(w, hh, id, &r);
    int pressed = (g_dlg_press_id == id);
    int hover = (g_dlg_hover == id) && !pressed;
    COLORREF bg = pressed ? CLR_BTN_DOWN : (hover ? CLR_BTN_HOVER : CLR_PANEL);
    draw_round(dc, r, S(8), bg, CLR_LINE, 1, CLR_DLG);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, CLR_TEXT);
    SelectObject(dc, g_font);
    DrawTextW(dc, id == ID_DLG_DELETE ? L"删除" : L"取消", -1, &r,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (g_key_nav && g_dlg_focus_id == id) {
        RECT fr = {r.left + S(1), r.top + S(1), r.right - S(1), r.bottom - S(1)};
        aa_stroke(dc, fr, S(7), CLR_FOCUS, 2);
    }
}

static void dialog_draw_buttons(HDC dc, int w, int hh) {
    dlg_draw_button(dc, w, hh, ID_DLG_CANCEL);
    dlg_draw_button(dc, w, hh, ID_DLG_DELETE);
}

static void dialog_draw_text(HDC dc, RECT rc) {
    int padX = S(20), padY = S(18);
    wchar_t title[256];
    swprintf(title, 256, L"删除「%ls」？", g_dlg_name);
    RECT tr = {padX, padY, rc.right - padX, padY + S(24)};
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, CLR_TEXT);
    SelectObject(dc, g_fontBold);
    DrawTextW(dc, title, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
              DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT br = {padX, tr.bottom + S(6), rc.right - padX, tr.bottom + S(6) + S(20)};
    SetTextColor(dc, CLR_MUTED);
    SelectObject(dc, g_fontSmall);
    DrawTextW(dc, L"此操作不可撤销", -1, &br, DT_LEFT | DT_VCENTER |
              DT_SINGLELINE | DT_NOPREFIX);
}

/* 非分层兜底：整窗矩形绘制（仅在 GDI+ 不可用时走这里）。 */
static void paint_dialog(HDC dc, RECT rc) {
    fill_rect_color(dc, rc, CLR_BG);
    draw_round(dc, rc, S(12), CLR_DLG, CLR_LINE, 1, CLR_BG);
    dialog_draw_text(dc, rc);
    dialog_draw_buttons(dc, rc.right, rc.bottom);
}

/* 分层窗口整窗绘制：4x 超采样圆角面板 -> 预乘 alpha -> 1x 文字。
 * 四角（含底边）逐像素透明，边缘由超采样得出，无 SetWindowRgn 硬锯齿。 */
static HDC g_dlg_layer_dc = NULL;
static HBITMAP g_dlg_layer_bmp = NULL;
static HGDIOBJ g_dlg_layer_old = NULL;
static DWORD *g_dlg_layer_bits = NULL;      /* 上屏像素（含文字/按钮） */
static HBITMAP g_dlg_layer_base_bmp = NULL;
static DWORD *g_dlg_layer_base = NULL;      /* 纯圆角面板，重绘底色用 */
static int g_dlg_layer_w = 0, g_dlg_layer_h = 0;
static int g_dlg_layer_ok = 0;

static void dlg_layer_free(void) {
    if (g_dlg_layer_dc) {
        if (g_dlg_layer_old) SelectObject(g_dlg_layer_dc, g_dlg_layer_old);
        DeleteDC(g_dlg_layer_dc);
        g_dlg_layer_dc = NULL;
    }
    if (g_dlg_layer_bmp) {
        DeleteObject(g_dlg_layer_bmp);
        g_dlg_layer_bmp = NULL;
    }
    if (g_dlg_layer_base_bmp) {
        DeleteObject(g_dlg_layer_base_bmp);
        g_dlg_layer_base_bmp = NULL;
    }
    g_dlg_layer_old = NULL;
    g_dlg_layer_bits = NULL;
    g_dlg_layer_base = NULL;
    g_dlg_layer_w = g_dlg_layer_h = 0;
    g_dlg_layer_ok = 0;
}

static int dlg_layer_build(int w, int hh) {
    dlg_layer_free();
    if (w <= 0 || hh <= 0 || !g_gdip_ok) return 0;
    int ss = 4;
    int sw = w * ss, sh = hh * ss;
    int rad = S(12);
    void *sbits = NULL;
    HBITMAP sbmp = make_dib(sw, sh, &sbits);
    if (!sbmp) return 0;
    DWORD *sp = (DWORD *)sbits;
    int total = sw * sh;
    for (int i = 0; i < total; i++) sp[i] = 0;   /* 全透明 */

    GpBitmap *sb = NULL;
    GpGraphics *sg = NULL;
    GpPath *p1 = NULL, *p2 = NULL;
    GpSolidFill *brush = NULL;
    GpPen *pen = NULL;
    if (GdipCreateBitmapFromScan0(sw, sh, sw * 4, PixelFormat32bppARGB,
                                  (BYTE *)sbits, &sb) != Ok) {
        DeleteObject(sbmp);
        return 0;
    }
    GdipGetImageGraphicsContext(sb, &sg);
    GdipSetSmoothingMode(sg, SmoothingModeAntiAlias);
    GdipSetPixelOffsetMode(sg, PixelOffsetModeHighQuality);
    GdipSetCompositingMode(sg, CompositingModeSourceOver);
    GdipCreatePath(FillModeAlternate, &p1);
    add_round_path(p1, 0.0f, 0.0f, (float)sw, (float)sh, (float)rad * ss);
    GdipCreateSolidFill(argb_of(CLR_DLG), &brush);
    GdipFillPath(sg, brush, p1);
    GdipCreatePath(FillModeAlternate, &p2);
    add_round_path(p2, (float)ss, (float)ss, (float)(sw - 2 * ss),
                   (float)(sh - 2 * ss), (float)rad * ss);
    GdipCreatePen1(argb_of(CLR_LINE), (float)ss, UnitPixel, &pen);
    GdipDrawPath(sg, pen, p2);
    GdipDeletePen(pen);
    GdipDeleteBrush(brush);
    GdipDeletePath(p2);
    GdipDeletePath(p1);
    GdipDeleteGraphics(sg);
    GdipDisposeImage(sb);

    /* 预乘 alpha：透明区为 (0,0,0,0)，边缘为 (fill*a, a)，缩小后仍正确。 */
    for (int i = 0; i < total; i++) {
        DWORD v = sp[i];
        DWORD a = (v >> 24) & 0xFF;
        if (a == 255) continue;
        DWORD r = (((v >> 16) & 0xFF) * a + 127) / 255;
        DWORD g = (((v >> 8) & 0xFF) * a + 127) / 255;
        DWORD b = ((v & 0xFF) * a + 127) / 255;
        sp[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }

    void *obits = NULL;
    HBITMAP obmp = make_dib(w, hh, &obits);
    if (!obmp) {
        DeleteObject(sbmp);
        return 0;
    }
    GpBitmap *sb2 = NULL, *ob = NULL;
    GpGraphics *og = NULL;
    GdipCreateBitmapFromScan0(sw, sh, sw * 4, PixelFormat32bppARGB,
                              (BYTE *)sbits, &sb2);
    GdipCreateBitmapFromScan0(w, hh, w * 4, PixelFormat32bppARGB,
                              (BYTE *)obits, &ob);
    GdipGetImageGraphicsContext(ob, &og);
    GdipSetInterpolationMode(og, InterpolationModeHighQualityBicubic);
    GdipSetPixelOffsetMode(og, PixelOffsetModeHighQuality);
    GdipSetCompositingMode(og, CompositingModeSourceCopy);
    GdipDrawImageRectI(og, sb2, 0, 0, w, hh);
    GdipDeleteGraphics(og);
    GdipDisposeImage(ob);
    GdipDisposeImage(sb2);
    DeleteObject(sbmp);

    HDC mdc = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(mdc, obmp);
    void *bbits = NULL;
    HBITMAP bbmp = make_dib(w, hh, &bbits);
    if (!bbmp) {
        SelectObject(mdc, old);
        DeleteDC(mdc);
        DeleteObject(obmp);
        return 0;
    }
    GdiFlush();
    memcpy(bbits, obits, (size_t)w * hh * 4);   /* 纯面板底图 */
    g_dlg_layer_dc = mdc;
    g_dlg_layer_bmp = obmp;
    g_dlg_layer_old = old;
    g_dlg_layer_bits = (DWORD *)obits;
    g_dlg_layer_base_bmp = bbmp;
    g_dlg_layer_base = (DWORD *)bbits;
    g_dlg_layer_w = w;
    g_dlg_layer_h = hh;
    g_dlg_layer_ok = 1;
    return 1;
}

/* 在 1x 位图上重绘文字 + 按钮；圆角安全区内强制不透明，四角 alpha 不动。 */
static void dlg_layer_paint(void) {
    if (!g_dlg_layer_ok || !g_dlg_layer_dc || !g_dlg_layer_base) return;
    int w = g_dlg_layer_w, hh = g_dlg_layer_h;
    GdiFlush();
    memcpy(g_dlg_layer_bits, g_dlg_layer_base, (size_t)w * hh * 4);
    RECT rc = {0, 0, w, hh};
    dialog_draw_text(g_dlg_layer_dc, rc);
    dialog_draw_buttons(g_dlg_layer_dc, w, hh);
    GdiFlush();
    int rad = S(12);
    for (int y = rad; y < hh - rad; y++) {
        for (int x = rad; x < w - rad; x++)
            g_dlg_layer_bits[y * w + x] |= 0xFF000000u;
    }
}

static void dlg_layer_present(HWND h) {
    if (!g_dlg_layer_ok || !g_dlg_layer_dc) return;
    RECT wr;
    GetWindowRect(h, &wr);
    POINT dst = {wr.left, wr.top};
    int w = wr.right - wr.left, hh = wr.bottom - wr.top;
    SIZE size = {w, hh};
    POINT src = {0, 0};
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(h, NULL, &dst, &size, g_dlg_layer_dc, &src, 0, &bf, ULW_ALPHA);
}

static void dlg_finish(int result) {
    g_dlg_result = result;
    if (g_dlg) DestroyWindow(g_dlg);
}

static void dlg_refresh(void) {
    if (!g_dlg) return;
    if (g_dlg_layer_ok) {
        dlg_layer_paint();
        dlg_layer_present(g_dlg);
    } else {
        InvalidateRect(g_dlg, NULL, FALSE);
    }
}

static LRESULT CALLBACK DlgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        if (g_dlg_layer_ok) {   /* 分层窗口：内容由 UpdateLayeredWindow 呈现 */
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            EndPaint(h, &ps);
            return 0;
        }
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint_dialog(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT: {
        /* 离屏取证：把预乘 alpha 的分层位图合成到 CLR_BG 上再输出，
         * 这样 PrintWindow 得到的就是带透明圆角的真实外观。 */
        HDC dc = (HDC)w;
        RECT rc;
        GetClientRect(h, &rc);
        if (g_dlg_layer_ok && g_dlg_layer_dc) {
            HDC mem = CreateCompatibleDC(dc);
            HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
            HGDIOBJ old = SelectObject(mem, bmp);
            fill_rect_color(mem, rc, CLR_BG);
            BLENDFUNCTION bf;
            bf.BlendOp = AC_SRC_OVER;
            bf.BlendFlags = 0;
            bf.SourceConstantAlpha = 255;
            bf.AlphaFormat = AC_SRC_ALPHA;
            AlphaBlend(mem, 0, 0, rc.right, rc.bottom, g_dlg_layer_dc, 0, 0,
                       rc.right, rc.bottom, bf);
            BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
        } else {
            paint_dialog(dc, rc);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        RECT rc;
        GetClientRect(h, &rc);
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        int id = dlg_hit_test(rc.right, rc.bottom, x, y);
        if (id != g_dlg_hover) {
            g_dlg_hover = id;
            if (id && g_key_nav) g_key_nav = 0;   /* 鼠标介入即收起焦点环 */
            dlg_refresh();
        }
        TRACKMOUSEEVENT tme;
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = h;
        tme.dwHoverTime = 0;
        TrackMouseEvent(&tme);
        SetCursor(LoadCursorW(NULL, id ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_dlg_hover) {
            g_dlg_hover = 0;
            dlg_refresh();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        RECT rc;
        GetClientRect(h, &rc);
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        int id = dlg_hit_test(rc.right, rc.bottom, x, y);
        if (id) {
            g_dlg_press_id = id;
            g_dlg_focus_id = id;
            if (g_key_nav) g_key_nav = 0;
            SetCapture(h);
            dlg_refresh();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        RECT rc;
        GetClientRect(h, &rc);
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        int id = dlg_hit_test(rc.right, rc.bottom, x, y);
        int was = g_dlg_press_id;
        g_dlg_press_id = 0;
        if (GetCapture() == h) ReleaseCapture();
        if (id && id == was) {
            dlg_finish(id == ID_DLG_DELETE ? 1 : 0);
            return 0;
        }
        dlg_refresh();
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (g_dlg_press_id) {
            g_dlg_press_id = 0;
            dlg_refresh();
        }
        return 0;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        dlg_refresh();
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE || w == VK_RETURN) {
            dlg_finish(0);
            return 0;
        }
        if (w == VK_TAB) {
            g_key_nav = 1;
            g_dlg_focus_id = (g_dlg_focus_id == ID_DLG_CANCEL) ? ID_DLG_DELETE : ID_DLG_CANCEL;
            dlg_refresh();
            return 0;
        }
        break;
    case WM_COMMAND: {
        int id = LOWORD(w), code = HIWORD(w);
        if ((code == BN_CLICKED && id == ID_DLG_CANCEL) || id == IDCANCEL) {
            dlg_finish(0);
            return 0;
        }
        break;
    }
    case WM_CLOSE:
        dlg_finish(0);
        return 0;
    case WM_DESTROY:
        dlg_layer_free();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static int confirm_delete(const wchar_t *name) {
    if (!g_hwnd || !name) return 0;
    static int cls_ready = 0;
    if (!cls_ready) {
        WNDCLASSEXW wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DlgProc;
        wc.hInstance = g_inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.lpszClassName = L"DesktopLayoutDialog";
        if (!RegisterClassExW(&wc)) return 0;
        cls_ready = 1;
    }
    g_dlg_name = name;
    g_dlg_result = 0;
    g_dlg_hover = 0;
    g_dlg_press_id = 0;
    g_dlg_focus_id = ID_DLG_CANCEL;   /* 默认焦点：取消 */
    RECT pr;
    GetWindowRect(g_hwnd, &pr);
    int w = S(360), h = dlg_height();
    int x = pr.left + ((pr.right - pr.left) - w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - h) / 2;
    g_dlg = CreateWindowExW(WS_EX_TOOLWINDOW, L"DesktopLayoutDialog", L"删除确认",
                            WS_POPUP, x, y, w, h, g_hwnd, NULL, g_inst, NULL);
    if (!g_dlg) return 0;
    EnableWindow(g_hwnd, FALSE);
    if (dlg_layer_build(w, h)) {
        SetWindowLongPtrW(g_dlg, GWL_EXSTYLE,
                          GetWindowLongPtrW(g_dlg, GWL_EXSTYLE) | WS_EX_LAYERED);
        dlg_layer_paint();
        dlg_layer_present(g_dlg);
    }
    ShowWindow(g_dlg, SW_SHOW);
    UpdateWindow(g_dlg);
    SetFocus(g_dlg);
    MSG msg;
    msg.message = WM_NULL;
    while (IsWindow(g_dlg) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (msg.message == WM_QUIT) PostQuitMessage(0);
    g_dlg = NULL;
    EnableWindow(g_hwnd, TRUE);
    SetFocus(g_hList);
    return g_dlg_result;
}

static void on_save_click(void) {
    wchar_t name[128] = L"";
    GetWindowTextW(g_hName, name, 128);
    const wchar_t *reason = NULL;
    if (name_illegal(name, &reason)) {
        g_name_error = 1;
        set_status(reason, 2);
        InvalidateRect(g_hName, NULL, FALSE);
        return;
    }
    OpResult res;
    if (!do_save_store(name, &res)) {
        g_name_error = 1;
        wchar_t msg[512], ew[512];
        err_w(res.error, ew, 512);
        swprintf(msg, 512, L"保存失败：%ls", ew[0] ? ew : L"未知原因");
        set_status(msg, 2);
        InvalidateRect(g_hName, NULL, FALSE);
        return;
    }
    g_name_error = 0;
    wchar_t msg[512];
    swprintf(msg, 512, L"已保存「%ls」：%d 个图标", res.saved_name, res.count);
    set_status(msg, 1);
    refresh_list(res.saved_path);
    set_default_name();
}

static void apply_layout_at(int idx) {
    if (idx < 0 || idx >= g_layout_count) return;
    OpResult res;
    if (do_apply(g_layouts[idx].path, 1, &res)) {
        wchar_t msg[512];
        int missing = res.count - res.matched;
        if (missing > 0)
            swprintf(msg, 512, L"已应用「%ls」：%d/%d（%d 个未找到）",
                     g_layouts[idx].name, res.matched, res.count, missing);
        else
            swprintf(msg, 512, L"已应用「%ls」：%d/%d",
                     g_layouts[idx].name, res.matched, res.count);
        set_status(msg, missing > 0 ? 2 : 1);
        undo_check();
        update_buttons();
    } else {
        wchar_t msg[512];
        if (res.auto_arrange) {
            swprintf(msg, 512, L"应用失败：请先在桌面关闭「自动排列图标」");
        } else if (res.error[0] && strstr(res.error, "snapshot failed")) {
            swprintf(msg, 512, L"应用失败：快照写入失败");
        } else if (res.error[0] && strstr(res.error, "no matching")) {
            swprintf(msg, 512, L"应用失败：图标缺失（布局 %d 个，匹配 %d 个）",
                     res.count, res.matched);
        } else if (res.error[0] && strstr(res.error, "mismatch")) {
            swprintf(msg, 512, L"应用失败：回读不一致（%d 个）", res.mismatch);
        } else if (res.error[0] && strstr(res.error, "read failed")) {
            swprintf(msg, 512, L"应用失败：布局文件已不存在");
        } else if (res.error[0] && strstr(res.error, "no icons in layout")) {
            swprintf(msg, 512, L"应用失败：布局文件无效");
        } else {
            wchar_t ew[512];
            err_w(res.error, ew, 512);
            swprintf(msg, 512, L"应用失败：%ls", ew[0] ? ew : L"未知原因");
        }
        set_status(msg, 2);
        undo_check();
        update_buttons();
    }
}

static void on_apply_click(void) {
    int idx = selected_index();
    if (idx < 0) return;
    apply_layout_at(idx);
}

static void on_undo_click(void) {
    if (!snapshot_valid(NULL)) {
        set_status(L"撤销失败：无可用快照", 2);
        undo_check();
        update_buttons();
        return;
    }
    OpResult res;
    if (do_undo(&res)) {
        wchar_t msg[512];
        int missing = res.count - res.matched;
        if (missing > 0)
            swprintf(msg, 512, L"已撤销：恢复到「上一状态」（%d/%d，%d 个未找到）",
                     res.matched, res.count, missing);
        else
            swprintf(msg, 512, L"已撤销：恢复到「上一状态」（%d/%d）",
                     res.matched, res.count);
        set_status(msg, missing > 0 ? 2 : 1);
    } else if (res.auto_arrange) {
        set_status(L"撤销失败：请先在桌面关闭「自动排列图标」", 2);
    } else {
        wchar_t msg[512], ew[512];
        err_w(res.error, ew, 512);
        swprintf(msg, 512, L"撤销失败：%ls", ew[0] ? ew : L"未知原因");
        set_status(msg, 2);
    }
    undo_check();
    update_buttons();
}

static void on_rename_click(void) {
    int idx = selected_index();
    if (idx < 0) return;
    begin_rename();
}

static void begin_rename(void) {
    int idx = selected_index();
    if (idx < 0) return;
    hide_inline();
    g_rename_active = 1;
    g_inline_cancel = 0;
    g_name_error = 0;
    g_inline_error = 0;
    wcscpy(g_rename_path, g_layouts[idx].path);
    SetWindowTextW(g_hInline, g_layouts[idx].name);
    layout_ui();
    ShowWindow(g_hInline, SW_SHOW);
    SetWindowPos(g_hInline, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    SetFocus(g_hInline);
    SendMessageW(g_hInline, EM_SETSEL, 0, -1);
}

static void commit_rename(int cancel) {
    if (!g_rename_active) return;
    if (cancel) {
        hide_inline();
        SetFocus(g_hList);
        return;
    }
    wchar_t name[128] = L"";
    GetWindowTextW(g_hInline, name, 128);
    const wchar_t *reason = NULL;
    if (name_illegal(name, &reason)) {
        g_inline_error = 1;
        layout_ui();
        InvalidateRect(g_hInline, NULL, TRUE);
        InvalidateRect(g_hwnd, NULL, FALSE);
        SetFocus(g_hInline);
        SendMessageW(g_hInline, EM_SETSEL, (WPARAM)0, (LPARAM)-1);
        return;
    }
    OpResult res;
    if (!do_rename(g_rename_path, name, &res)) {
        wchar_t msg[512], ew[512];
        err_w(res.error, ew, 512);
        swprintf(msg, 512, L"重命名失败：%ls", ew[0] ? ew : L"未知原因");
        set_status(msg, 2);
        SetFocus(g_hInline);
        return;
    }
    hide_inline();
    wchar_t msg[512];
    swprintf(msg, 512, L"已重命名为「%ls」", res.saved_name);
    set_status(msg, 1);
    refresh_list(res.saved_path);
    SetFocus(g_hList);
}

static void on_delete_click(void) {
    int idx = selected_index();
    if (idx < 0) return;
    wchar_t name[128];
    wcsncpy(name, g_layouts[idx].name, 127);
    name[127] = 0;
    wchar_t path[MAX_PATH * 2];
    wcsncpy(path, g_layouts[idx].path, MAX_PATH * 2 - 1);
    path[MAX_PATH * 2 - 1] = 0;
    if (g_confirm_delete && !confirm_delete(name)) {
        set_status(L"已取消删除", 0);
        return;
    }
    OpResult res;
    if (do_delete(path, &res)) {
        wchar_t msg[512];
        swprintf(msg, 512, L"已删除「%ls」", name);
        set_status(msg, 1);
        refresh_list(NULL);
    } else {
        wchar_t msg[512], ew[512];
        err_w(res.error, ew, 512);
        swprintf(msg, 512, L"删除失败：%ls", ew[0] ? ew : L"未知原因");
        set_status(msg, 2);
    }
}

static void on_autostart_click(void) {
    int want = !g_autostart;
    if (!autostart_write(want)) {
        set_status(want ? L"开启失败：无法写入注册表"
                        : L"关闭失败：无法删除注册表项", 2);
        InvalidateRect(g_hAutoStart, NULL, FALSE);
        return;
    }
    g_autostart = want;
    set_status(want ? L"已开启开机自动启动" : L"已关闭开机自动启动", 1);
    InvalidateRect(g_hAutoStart, NULL, FALSE);
}

static void on_confirm_toggle(void) {
    int want = !g_confirm_delete;
    g_confirm_delete = want;
    if (!settings_save()) {
        g_confirm_delete = !want;
        set_status(L"设置保存失败：无法写入 settings.json", 2);
    } else {
        set_status(want ? L"已开启删除前确认"
                        : L"已关闭删除前确认：删除将直接生效", 1);
    }
    InvalidateRect(g_hConfirm, NULL, FALSE);
}

static void paint_ui(HDC dc, RECT rc) {
    FillRect(dc, &rc, g_brBg);
    int m = S(12);
    int gap = S(8);
    int nameH = S(30);
    int saveW = S(88);
    int headH = S(26);
    int statusH = S(26);
    int statusY = rc.bottom - m - statusH;

    RECT listrc;
    GetWindowRect(g_hList, &listrc);
    MapWindowPoints(NULL, g_hwnd, (POINT *)&listrc, 2);
    RECT lc;
    GetClientRect(g_hList, &lc);

    int cardX = m;
    int cardW = rc.right - m * 2;
    RECT card = {cardX, listrc.top - headH, cardX + cardW, listrc.bottom + S(7)};
    draw_round(dc, card, S(10), CLR_CARD, CLR_LINE, 1, CLR_BG);

    int pad = S(12);
    int timeW = S(100);
    int countW = S(64);
    int gapCol = S(12);
    int rightEdge = listrc.left + lc.right - pad;
    int timeRight = rightEdge;
    int timeLeft = timeRight - timeW;
    int countRight = timeLeft - gapCol;
    int countLeft = countRight - countW;
    RECT hdr = {cardX + pad, card.top, rightEdge, card.top + headH};
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g_fontSmall);
    SetTextColor(dc, CLR_MUTED);
    DrawTextW(dc, L"布局", -1, &hdr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT hc = {countLeft, hdr.top, countRight, hdr.bottom};
    DrawTextW(dc, L"图标", -1, &hc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT ht = {timeLeft, hdr.top, timeRight, hdr.bottom};
    DrawTextW(dc, L"时间", -1, &ht, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    RECT hline = {card.left + 1, card.top + headH - 1, card.right - 1, card.top + headH};
    fill_rect_color(dc, hline, CLR_LINE);

    RECT nb = {m, m, rc.right - m - saveW - gap, m + nameH};
    int nfocus = GetFocus() == g_hName;
    draw_round(dc, nb, S(8), g_name_error ? CLR_ERROR_BG : CLR_INPUT,
               g_name_error ? CLR_DANGER : (nfocus ? CLR_FOCUS : CLR_LINE),
               nfocus ? 2 : 1, CLR_BG);

    RECT sline = {m, statusY - 1, rc.right - m, statusY};
    fill_rect_color(dc, sline, CLR_LINE);
    const wchar_t *hint = (g_hint_id && g_status_kind == 0) ? hint_text(g_hint_id) : NULL;
    COLORREF dotc = g_status_kind == 1 ? CLR_OK :
                    (g_status_kind == 2 ? CLR_DANGER : CLR_MUTED);
    if (hint) dotc = CLR_MUTED;
    RECT dotr = {m, statusY + S(9), m + S(8), statusY + S(17)};
    draw_round(dc, dotr, S(4), dotc, dotc, 0, CLR_BG);

    RECT sr = {m + S(16), statusY, rc.right - m - S(112), statusY + statusH};
    SelectObject(dc, g_fontSmall);
    SetTextColor(dc, hint ? CLR_MUTED :
                 (g_status_kind == 1 ? CLR_TEXT :
                  (g_status_kind == 2 ? CLR_DANGER : CLR_MUTED)));
    DrawTextW(dc, hint ? hint : g_status_text, -1, &sr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    draw_scrollbar(dc);
}

/* ---------------------------------------------------------------- 托盘 */

static void show_main_window(void) {
    if (!g_hwnd) return;
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    else ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    g_in_tray = 0;
}

static void tray_add(void) {
    if (!g_hwnd) return;
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_UID;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAY_MSG;
    nid.hIcon = g_iconBig ? g_iconBig : g_iconSmall;
    wcsncpy(nid.szTip, APP_TITLE, 127);
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) Shell_NotifyIconW(NIM_MODIFY, &nid);
    g_tray_added = 1;
}

static void tray_remove(void) {
    if (!g_hwnd || !g_tray_added) return;
    NOTIFYICONDATAW nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_hwnd;
    nid.uID = TRAY_UID;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_tray_added = 0;
}

static void tray_save_layout(void) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t name[128];
    swprintf(name, 128, L"布局 %02d-%02d %02d-%02d",
             st.wMonth, st.wDay, st.wHour, st.wMinute);
    OpResult res;
    if (do_save_store(name, &res)) {
        wchar_t msg[512];
        swprintf(msg, 512, L"已保存「%ls」（%d 个图标）", res.saved_name, res.count);
        set_status(msg, 1);
        refresh_list(res.saved_path);
    } else {
        wchar_t msg[512], ew[512];
        err_w(res.error, ew, 512);
        swprintf(msg, 512, L"保存失败：%ls", ew[0] ? ew : L"未知原因");
        set_status(msg, 2);
    }
}

static void tray_menu(void) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING, ID_TRAY_SAVE, L"保存当前布局");
    HMENU sub = CreatePopupMenu();
    if (g_layout_count > 0) {
        for (int i = 0; i < g_layout_count; i++)
            AppendMenuW(sub, MF_STRING, ID_TRAY_LAYOUT_BASE + i, g_layouts[i].name);
    } else {
        AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"（暂无布局）");
    }
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)sub, L"使用布局");
    AppendMenuW(menu, MF_STRING | (g_undo_available ? 0 : MF_GRAYED),
                ID_TRAY_UNDO, L"撤销上一次应用");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"打开主界面");
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"退出");

    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_hwnd);
    UINT cmd = (UINT)TrackPopupMenu(menu,
                                    TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                    pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);
    PostMessageW(g_hwnd, WM_NULL, 0, 0);

    if (cmd == ID_TRAY_SAVE) {
        tray_save_layout();
    } else if (cmd >= ID_TRAY_LAYOUT_BASE && cmd < ID_TRAY_LAYOUT_BASE + (UINT)g_layout_count) {
        apply_layout_at((int)(cmd - ID_TRAY_LAYOUT_BASE));
    } else if (cmd == ID_TRAY_UNDO) {
        on_undo_click();
    } else if (cmd == ID_TRAY_OPEN) {
        show_main_window();
    } else if (cmd == ID_TRAY_EXIT) {
        g_quitting = 1;
        DestroyWindow(g_hwnd);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbar_created_msg && msg == g_taskbar_created_msg) {
        g_tray_added = 0;
        tray_add();
        return 0;
    }
    if (msg == WM_ACTIVATE_MAIN) {
        /* 第二实例只能发请求：前台锁按“窗口所属进程”判定，必须由本进程自己置前 */
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        ShowWindow(hwnd, SW_SHOW);
        for (int attempt = 0; attempt < 3 && GetForegroundWindow() != hwnd; attempt++) {
            HWND fg = GetForegroundWindow();
            DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
            DWORD self_tid = GetCurrentThreadId();
            int attached = 0;
            if (fg_tid && fg_tid != self_tid)
                attached = (int)AttachThreadInput(fg_tid, self_tid, TRUE);
            BringWindowToTop(hwnd);
            SetForegroundWindow(hwnd);
            if (attached) AttachThreadInput(fg_tid, self_tid, FALSE);
            if (GetForegroundWindow() == hwnd) break;
            Sleep(30);
        }
        if (GetForegroundWindow() != hwnd) {
            /* 系统前台锁拒绝时不硬抢，闪烁任务栏按钮提示用户 */
            FLASHWINFO fi;
            memset(&fi, 0, sizeof(fi));
            fi.cbSize = sizeof(fi);
            fi.hwnd = hwnd;
            fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
            fi.uCount = 3;
            FlashWindowEx(&fi);
        }
        return 0;
    }
    if (msg == WM_TRAY_MSG) {
        UINT ev = LOWORD(lp);
        if (ev == WM_LBUTTONDBLCLK) {
            show_main_window();
        } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
            tray_menu();
        }
        return 0;
    }
    switch (msg) {
    case WM_CREATE: {
        g_hwnd = hwnd;
        g_dpi = 96;
        typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
        HMODULE u = GetModuleHandleW(L"user32.dll");
        if (u) {
            GetDpiForWindowFn fn = (GetDpiForWindowFn)(void *)GetProcAddress(u, "GetDpiForWindow");
            if (fn) g_dpi = (int)fn(hwnd);
        }
        create_fonts();
        g_brBg = CreateSolidBrush(CLR_BG);
        g_brPanel = CreateSolidBrush(CLR_PANEL);
        g_brInput = CreateSolidBrush(CLR_INPUT);
        g_brSelect = CreateSolidBrush(CLR_SELECT);
        g_brRowHover = CreateSolidBrush(CLR_ROW_HOVER);
        g_brPrimary = CreateSolidBrush(CLR_PRIMARY);
        g_brPrimaryHover = CreateSolidBrush(CLR_PRIMARY_HOVER);
        g_brPrimaryDown = CreateSolidBrush(CLR_PRIMARY_DOWN);
        g_brCard = CreateSolidBrush(CLR_CARD);
        g_brInline = CreateSolidBrush(CLR_INPUT_INLINE);
        g_brInputErr = CreateSolidBrush(CLR_ERROR_BG);

        g_hName = CreateWindowExW(0, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  0, 0, 10, 10, hwnd, (HMENU)ID_NAME, NULL, NULL);
        g_hSave = CreateWindowExW(0, L"BUTTON", L"保存布局",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  0, 0, 10, 10, hwnd, (HMENU)ID_SAVE, NULL, NULL);
        g_hList = CreateWindowExW(0, L"LISTBOX", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS |
                                  LBS_OWNERDRAWFIXED | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_HASSTRINGS,
                                  0, 0, 10, 10, hwnd, (HMENU)ID_LIST, NULL, NULL);
        g_hEmpty = CreateWindowExW(0, L"STATIC", L"暂无布局",
                                   WS_CHILD | SS_CENTER | SS_CENTERIMAGE | SS_NOPREFIX,
                                   0, 0, 10, 10, hwnd, (HMENU)ID_EMPTY, NULL, NULL);
        g_hApply = CreateWindowExW(0, L"BUTTON", L"应用",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                   0, 0, 10, 10, hwnd, (HMENU)ID_APPLY, NULL, NULL);
        g_hRename = CreateWindowExW(0, L"BUTTON", L"重命名",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    0, 0, 10, 10, hwnd, (HMENU)ID_RENAME, NULL, NULL);
        g_hDelete = CreateWindowExW(0, L"BUTTON", L"删除",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                    0, 0, 10, 10, hwnd, (HMENU)ID_DELETE, NULL, NULL);
        g_hUndo = CreateWindowExW(0, L"BUTTON", L"撤销",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                  0, 0, 10, 10, hwnd, (HMENU)ID_UNDO, NULL, NULL);
        g_hAutoStart = CreateWindowExW(0, L"BUTTON", L"开机自动启动",
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                       0, 0, 10, 10, hwnd, (HMENU)ID_AUTOSTART, NULL, NULL);
        g_hConfirm = CreateWindowExW(0, L"BUTTON", L"删除前确认",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     0, 0, 10, 10, hwnd, (HMENU)ID_CONFIRM, NULL, NULL);
        g_hInline = CreateWindowExW(0, L"EDIT", L"",
                                    WS_CHILD | WS_CLIPSIBLINGS | ES_AUTOHSCROLL,
                                    0, 0, 10, 10, hwnd, (HMENU)ID_INLINE, NULL, NULL);

        SendMessageW(g_hList, LB_SETITEMHEIGHT, 0, (LPARAM)S(ROW_H));
        SendMessageW(g_hName, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(S(10), S(10)));
        HWND ctrls[] = {g_hName, g_hSave, g_hApply, g_hUndo, g_hRename, g_hDelete,
                        g_hAutoStart, g_hConfirm};
        for (int i = 0; i < (int)(sizeof(ctrls) / sizeof(ctrls[0])); i++) {
            SendMessageW(ctrls[i], WM_SETFONT, (WPARAM)g_font, TRUE);
        }
        SendMessageW(g_hEmpty, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
        SendMessageW(g_hInline, WM_SETFONT, (WPARAM)g_font, TRUE);

        g_old_list = (WNDPROC)SetWindowLongPtrW(g_hList, GWLP_WNDPROC, (LONG_PTR)ListProc);
        g_old_name = (WNDPROC)SetWindowLongPtrW(g_hName, GWLP_WNDPROC, (LONG_PTR)EditProc);
        g_old_inline = (WNDPROC)SetWindowLongPtrW(g_hInline, GWLP_WNDPROC, (LONG_PTR)EditProc);
        HWND btns[] = {g_hSave, g_hApply, g_hUndo, g_hRename, g_hDelete, g_hAutoStart,
                       g_hConfirm};
        g_old_btn = (WNDPROC)SetWindowLongPtrW(btns[0], GWLP_WNDPROC, (LONG_PTR)BtnProc);
        for (int i = 1; i < (int)(sizeof(btns) / sizeof(btns[0])); i++) {
            SetWindowLongPtrW(btns[i], GWLP_WNDPROC, (LONG_PTR)BtnProc);
        }

        use_dark_scrollbar(g_hList);
        use_dark_scrollbar(g_hName);
        use_dark_scrollbar(g_hInline);
        apply_dark_title(hwnd);

        g_autostart = autostart_sync();
        g_confirm_delete = settings_load();
        undo_check();
        SetWindowTextW(g_hEmpty, L"暂无布局");
        layouts_refresh();
        refresh_list(NULL);
        set_default_name();
        layout_ui();
        tray_add();
        return 0;
    }
    case WM_SIZE:
        layout_ui();
        return 0;
    case WM_DPICHANGED: {
        g_dpi = (int)HIWORD(wp);
        create_fonts();
        HWND ctrls[] = {g_hName, g_hSave, g_hApply, g_hUndo, g_hRename, g_hDelete,
                        g_hAutoStart, g_hConfirm, g_hInline};
        for (int i = 0; i < (int)(sizeof(ctrls) / sizeof(ctrls[0])); i++) {
            SendMessageW(ctrls[i], WM_SETFONT, (WPARAM)g_font, TRUE);
        }
        SendMessageW(g_hEmpty, WM_SETFONT, (WPARAM)g_fontSmall, TRUE);
        SendMessageW(g_hList, LB_SETITEMHEIGHT, 0, (LPARAM)S(ROW_H));
        RECT *r = (RECT *)lp;
        SetWindowPos(hwnd, NULL, r->left, r->top, S(WIN_W_OUTER), S(WIN_H_OUTER),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        layout_ui();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(WIN_W_OUTER);
        mm->ptMinTrackSize.y = S(WIN_H_OUTER);
        mm->ptMaxTrackSize.x = S(WIN_W_OUTER);
        mm->ptMaxTrackSize.y = S(WIN_H_OUTER);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
        SbInfo si;
        sb_get(&si);
        if (g_sb_drag && si.active) {
            int span = si.height - si.thumb_h;
            int maxtop = si.count - si.vis;
            int nt = 0;
            if (span > 0)
                nt = (int)((double)(y - g_sb_off - si.top) * maxtop / span);
            sb_set_top(nt);
            return 0;
        }
        int over = si.active && x >= si.left - S(2) && x < si.left + si.width + S(2) &&
                   y >= si.thumb_y && y < si.thumb_y + si.thumb_h;
        if (over != g_sb_hover) {
            g_sb_hover = over;
            sb_invalidate();
        }
        TRACKMOUSEEVENT tme;
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        tme.dwHoverTime = 0;
        TrackMouseEvent(&tme);
        break;
    }
    case WM_MOUSELEAVE:
        if (g_sb_hover) {
            g_sb_hover = 0;
            sb_invalidate();
        }
        break;
    case WM_LBUTTONDOWN: {
        if (g_key_nav) {
            g_key_nav = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        int x = (int)(short)LOWORD(lp), y = (int)(short)HIWORD(lp);
        SbInfo si;
        sb_get(&si);
        if (si.active && x >= si.left - S(2) && x < si.left + si.width + S(2) &&
            y >= si.top && y < si.top + si.height) {
            if (y >= si.thumb_y && y < si.thumb_y + si.thumb_h) {
                g_sb_drag = 1;
                g_sb_off = y - si.thumb_y;
                SetCapture(hwnd);
            } else {
                sb_set_top(si.topidx + (y < si.thumb_y ? -si.vis : si.vis));
            }
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP:
        if (g_sb_drag) {
            g_sb_drag = 0;
            ReleaseCapture();
            sb_invalidate();
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint_ui(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT *dis = (DRAWITEMSTRUCT *)lp;
        if (dis->CtlType == ODT_LISTBOX && dis->CtlID == ID_LIST) {
            int idx = (int)dis->itemID;
            RECT r = dis->rcItem;
            int selected = (dis->itemState & ODS_SELECTED) != 0;
            int hover = (idx == g_hover_item);
            fill_rect_color(dis->hDC, r, CLR_CARD);
            RECT hl = {r.left + S(4), r.top + S(1), r.right - S(4), r.bottom - S(1)};
            COLORREF hbg = selected ? (hover ? CLR_SELECT_HOVER : CLR_SELECT)
                                    : (hover ? CLR_ROW_HOVER : CLR_CARD);
            if (selected || hover)
                draw_round(dis->hDC, hl, S(8), hbg, hbg, 0, CLR_CARD);
            if (idx >= 0 && idx < g_layout_count) {
                int pad = S(12);
                int timeW = S(100);
                int countW = S(64);
                int gapCol = S(12);
                int rightEdge = r.right - pad;
                int timeLeft = rightEdge - timeW;
                int countRight = timeLeft - gapCol;
                int countLeft = countRight - countW;
                wchar_t when[64] = L"";
                wchar_t cnt[32] = L"";
                pretty_time(g_layouts[idx].created, when, 64);
                swprintf(cnt, 32, L"%d", g_layouts[idx].count);
                SetBkMode(dis->hDC, TRANSPARENT);
                RECT nr = {r.left + pad, r.top, countLeft - gapCol, r.bottom};
                SelectObject(dis->hDC, g_font);
                SetTextColor(dis->hDC, selected ? CLR_SELECT_TEXT : CLR_TEXT);
                DrawTextW(dis->hDC, g_layouts[idx].name, -1, &nr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                if (!(g_rename_active && selected && idx == g_sel)) {
                    SelectObject(dis->hDC, g_fontSmall);
                    SetTextColor(dis->hDC, selected ? CLR_SELECT_MUTED : CLR_MUTED);
                    RECT cr = {countLeft, r.top, countRight, r.bottom};
                    DrawTextW(dis->hDC, cnt, -1, &cr,
                              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    RECT tr = {timeLeft, r.top, rightEdge, r.bottom};
                    DrawTextW(dis->hDC, when, -1, &tr,
                              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
            }
            if (g_rename_active && selected && idx == g_sel) {
                POINT pts[2] = {{g_inline_rect.left, g_inline_rect.top},
                                {g_inline_rect.right, g_inline_rect.bottom}};
                MapWindowPoints(g_hwnd, dis->hwndItem, pts, 2);
                RECT sh = {pts[0].x - r.left, pts[0].y - r.top,
                           pts[1].x - r.left, pts[1].y - r.top};
                draw_round(dis->hDC, sh, S(8),
                           g_inline_error ? CLR_ERROR_BG : CLR_INPUT_INLINE,
                           g_inline_error ? CLR_DANGER : CLR_FOCUS,
                           g_inline_error ? 1 : 2, hbg);
                if (g_inline_error) {
                    RECT hr = {sh.right + S(8), sh.top, r.right - S(12), sh.bottom};
                    SelectObject(dis->hDC, g_fontSmall);
                    SetTextColor(dis->hDC, CLR_DANGER);
                    SetBkMode(dis->hDC, TRANSPARENT);
                    DrawTextW(dis->hDC, L"名称不可用", -1, &hr,
                              DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
            }
            if (selected && GetFocus() == g_hList && !g_rename_active && g_key_nav) {
                aa_stroke(dis->hDC, hl, S(8), CLR_FOCUS, 2);
            }
            return TRUE;
        }
        if (dis->CtlType == ODT_BUTTON) {
            draw_button(dis->hDC, dis);
            return TRUE;
        }
        break;
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, CLR_TEXT);
        if ((HWND)lp == g_hInline) {
            if (g_inline_error) {
                SetBkColor(dc, CLR_ERROR_BG);
                return (LRESULT)g_brInputErr;
            }
            SetBkColor(dc, CLR_INPUT_INLINE);
            return (LRESULT)g_brInline;
        }
        SetBkColor(dc, CLR_INPUT);
        return (LRESULT)g_brInput;
    }
    case WM_CTLCOLORLISTBOX: {
        HDC dc = (HDC)wp;
        SetTextColor(dc, CLR_TEXT);
        SetBkColor(dc, CLR_CARD);
        return (LRESULT)g_brCard;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)wp;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, CLR_MUTED);
        if ((HWND)lp == g_hEmpty) {
            SetBkColor(dc, CLR_CARD);
            return (LRESULT)g_brCard;
        }
        return (LRESULT)g_brBg;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        if (id == ID_SAVE && code == BN_CLICKED) { on_save_click(); return 0; }
        if (id == ID_APPLY && code == BN_CLICKED) { on_apply_click(); return 0; }
        if (id == ID_RENAME && code == BN_CLICKED) { on_rename_click(); return 0; }
        if (id == ID_DELETE && code == BN_CLICKED) { on_delete_click(); return 0; }
        if (id == ID_UNDO && code == BN_CLICKED) { on_undo_click(); return 0; }
        if (id == ID_AUTOSTART && code == BN_CLICKED) { on_autostart_click(); return 0; }
        if (id == ID_CONFIRM && code == BN_CLICKED) { on_confirm_toggle(); return 0; }
        if (id == ID_NAME && code == EN_CHANGE) {
            if (g_name_error) {
                g_name_error = 0;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            return 0;
        }
        if (id == ID_INLINE && code == EN_KILLFOCUS) {
            if (g_rename_active && !g_inline_cancel) commit_rename(0);
            return 0;
        }
        if (id == ID_INLINE && code == EN_CHANGE) {
            if (g_inline_error) {
                g_inline_error = 0;
                InvalidateRect(g_hInline, NULL, TRUE);
            }
            return 0;
        }
        if (id == ID_LIST && code == LBN_SELCHANGE) {
            g_sel = (int)SendMessageW(g_hList, LB_GETCURSEL, 0, 0);
            update_buttons();
            return 0;
        }
        if (id == ID_LIST && code == LBN_DBLCLK) { on_apply_click(); return 0; }
        break;
    }
    case WM_CLOSE:
        if (!g_quitting) {
            if (g_rename_active) commit_rename(0);
            ShowWindow(hwnd, SW_HIDE);
            g_in_tray = 1;
            if (!g_tray_added) tray_add();
            return 0;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        tray_remove();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void enable_dpi_awareness(void) {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        typedef BOOL (WINAPI *SetDpiCtxFn)(HANDLE);
        SetDpiCtxFn fn = (SetDpiCtxFn)(void *)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (fn && fn((HANDLE)-4)) return;
    }
    SetProcessDPIAware();
}

/* 运行时绘制 16/32 px 图标：深色任务栏下用蓝底浅色方块，透明背景。 */
static HICON make_app_icon(int size) {
    const int ART = 128;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = ART;
    bi.bmiHeader.biHeight = -ART;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP art = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!art || !bits) return NULL;
    memset(bits, 0, (size_t)ART * ART * 4);

    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ oldbmp = SelectObject(dc, art);
    HGDIOBJ oldpen = SelectObject(dc, GetStockObject(NULL_PEN));
    HBRUSH blue = CreateSolidBrush(CLR_PRIMARY);
    HGDIOBJ oldbr = SelectObject(dc, blue);
    RoundRect(dc, 4, 4, 124, 124, 30, 30);
    HBRUSH light = CreateSolidBrush(RGB(0xe6, 0xe6, 0xe6));
    SelectObject(dc, light);
    RoundRect(dc, 30, 26, 60, 56, 8, 8);
    RoundRect(dc, 68, 26, 98, 56, 8, 8);
    RoundRect(dc, 30, 64, 60, 94, 8, 8);
    RoundRect(dc, 68, 64, 98, 94, 8, 8);
    SelectObject(dc, oldbr);
    SelectObject(dc, oldpen);
    DeleteObject(blue);
    DeleteObject(light);
    GdiFlush();

    int scale = ART / size;
    if (scale < 1) scale = 1;
    BITMAPINFO bi2;
    memset(&bi2, 0, sizeof(bi2));
    bi2.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi2.bmiHeader.biWidth = size;
    bi2.bmiHeader.biHeight = -size;
    bi2.bmiHeader.biPlanes = 1;
    bi2.bmiHeader.biBitCount = 32;
    bi2.bmiHeader.biCompression = BI_RGB;
    void *bits2 = NULL;
    HBITMAP out = CreateDIBSection(NULL, &bi2, DIB_RGB_COLORS, &bits2, NULL, 0);
    if (!out || !bits2) {
        SelectObject(dc, oldbmp);
        DeleteDC(dc);
        DeleteObject(art);
        return NULL;
    }
    memset(bits2, 0, (size_t)size * size * 4);
    const unsigned char *src = (const unsigned char *)bits;
    unsigned char *dst = (unsigned char *)bits2;
    int total = scale * scale;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int sb = 0, sg = 0, sr = 0, n = 0;
            for (int yy = 0; yy < scale; yy++) {
                for (int xx = 0; xx < scale; xx++) {
                    const unsigned char *p = src +
                        (((size_t)(y * scale + yy) * ART) + (x * scale + xx)) * 4;
                    if (p[0] || p[1] || p[2]) {
                        sb += p[0]; sg += p[1]; sr += p[2]; n++;
                    }
                }
            }
            int o = (y * size + x) * 4;
            if (n) {
                dst[o] = (unsigned char)(sb / n);
                dst[o + 1] = (unsigned char)(sg / n);
                dst[o + 2] = (unsigned char)(sr / n);
                dst[o + 3] = (unsigned char)(255 * n / total);
            }
        }
    }
    SelectObject(dc, oldbmp);
    DeleteDC(dc);
    DeleteObject(art);

    BITMAPINFO mbi;
    memset(&mbi, 0, sizeof(mbi));
    mbi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    mbi.bmiHeader.biWidth = size;
    mbi.bmiHeader.biHeight = -size;
    mbi.bmiHeader.biPlanes = 1;
    mbi.bmiHeader.biBitCount = 1;
    mbi.bmiHeader.biCompression = BI_RGB;
    void *mbits = NULL;
    HBITMAP mask = CreateDIBSection(NULL, &mbi, DIB_RGB_COLORS, &mbits, NULL, 0);
    if (mask && mbits) memset(mbits, 0, (size_t)(((size + 31) / 32) * 4) * size);
    ICONINFO ii;
    memset(&ii, 0, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmColor = out;
    ii.hbmMask = mask;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(out);
    if (mask) DeleteObject(mask);
    return icon;
}

static int single_instance_allow(void) {
    g_instance_mutex = CreateMutexW(NULL, TRUE, L"Local\\DesktopLayoutNative.SingleInstance");
    if (!g_instance_mutex) return 1;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return 1;
    for (int i = 0; i < 60; i++) {
        HWND w = FindWindowW(L"DesktopLayoutNativeWindow", APP_TITLE);
        if (w) {
            if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
            ShowWindow(w, SW_SHOW);
            /* 后台进程直接 SetForegroundWindow 常被前台锁拒绝：临时附加到当前
             * 前台线程，并在几十毫秒内重试几次（前台锁本身存在竞态）。 */
            for (int attempt = 0; attempt < 4; attempt++) {
                HWND fg = GetForegroundWindow();
                DWORD fg_tid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
                DWORD self_tid = GetCurrentThreadId();
                int attached = 0;
                if (fg_tid && fg_tid != self_tid)
                    attached = (int)AttachThreadInput(fg_tid, self_tid, TRUE);
                BringWindowToTop(w);
                SetForegroundWindow(w);
                if (attached) AttachThreadInput(fg_tid, self_tid, FALSE);
                if (GetForegroundWindow() == w) break;
                Sleep(40);
            }
            if (GetForegroundWindow() != w)
                PostMessageW(w, WM_ACTIVATE_MAIN, 0, 0);   /* 交给持有窗口的进程自己置前 */
            return 0;
        }
        Sleep(20);
    }
    return 0;
}

/* 隐藏基准：冷渲染（4x 超采样 + Bicubic）与缓存命中 BitBlt 的单次耗时。 */
static int ui_bench(void) {
    LARGE_INTEGER freq, a, b;
    QueryPerformanceFrequency(&freq);
    LARGE_INTEGER s0, s1;
    QueryPerformanceCounter(&s0);
    if (!gdip_start()) {
        printf("gdiplus=FAIL\n");
        return 1;
    }
    QueryPerformanceCounter(&s1);
    printf("gdiplus=ok init_ms=%.2f\n", qpc_ms(freq, s0, s1));

    void *bits = NULL;
    HBITMAP target = make_dib(900, 620, &bits);
    if (!target) {
        gdip_stop();
        return 2;
    }
    HDC dc = CreateCompatibleDC(NULL);
    HGDIOBJ old = SelectObject(dc, target);

    struct { const char *n; int w, h, rad; COLORREF fill, border, bg; } cs[] = {
        {"card", 868, 316, S(10), CLR_CARD, CLR_LINE, CLR_BG},
        {"input", 700, 32, S(8), CLR_INPUT, CLR_LINE, CLR_BG},
        {"btn-primary", 96, 32, S(8), CLR_PRIMARY, CLR_PRIMARY, CLR_BG},
        {"btn-28", 72, 28, S(8), CLR_PANEL, CLR_LINE, CLR_BG},
        {"thumb", 8, 100, S(4), CLR_SCROLL, CLR_SCROLL, CLR_CARD},
        {"check-16", 16, 16, S(4), CLR_INPUT, CLR_CHECK_BORDER, CLR_BG},
        {"dot-8", 8, 8, S(4), CLR_OK, CLR_OK, CLR_BG},
    };
    int n = (int)(sizeof(cs) / sizeof(cs[0]));
    for (int i = 0; i < n; i++) {
        round_cache_clear();
        QueryPerformanceCounter(&a);
        HDC src = round_cache(cs[i].w, cs[i].h, cs[i].rad, cs[i].bg, cs[i].fill,
                              cs[i].border, 1);
        QueryPerformanceCounter(&b);
        double cold = qpc_ms(freq, a, b);
        (void)src;
        RECT r = {0, 0, cs[i].w, cs[i].h};
        int iters = 200;
        QueryPerformanceCounter(&a);
        for (int k = 0; k < iters; k++)
            draw_round(dc, r, cs[i].rad, cs[i].fill, cs[i].border, 1, cs[i].bg);
        QueryPerformanceCounter(&b);
        printf("%s %dx%d r=%d cold_ms=%.2f blit_us=%.1f\n", cs[i].n, cs[i].w,
               cs[i].h, cs[i].rad, cold, qpc_ms(freq, a, b) * 1000.0 / iters);
    }
    round_cache_clear();
    SelectObject(dc, old);
    DeleteObject(target);
    DeleteDC(dc);
    gdip_stop();
    return 0;
}

static int run_gui(HINSTANCE hInst, int show) {
    g_iconSmall = make_app_icon(16);
    g_iconBig = make_app_icon(32);
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"DesktopLayoutNativeWindow";
    wc.hIcon = g_iconBig;
    wc.hIconSm = g_iconSmall;
    if (!RegisterClassExW(&wc)) return 2;
    gdip_start();

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, APP_TITLE, style,
        CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(WIN_W_OUTER, 96, 96),
        MulDiv(WIN_H_OUTER, 96, 96), NULL, NULL, hInst, NULL);
    if (!hwnd) return 3;
    if (g_start_in_tray) {
        ShowWindow(hwnd, SW_HIDE);
    } else {
        ShowWindow(hwnd, show);
        UpdateWindow(hwnd);
        if (g_test_focus) {
            g_key_nav = 1;
            SetFocus(g_hSave);
            InvalidateRect(hwnd, NULL, FALSE);
            UpdateWindow(hwnd);
        }
    }

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_iconSmall) DestroyIcon(g_iconSmall);
    if (g_iconBig) DestroyIcon(g_iconBig);
    round_cache_clear();
    gdip_stop();
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR lpCmd, int nShow) {
    (void)hPrev; (void)lpCmd;
    g_inst = hInst;
    enable_dpi_awareness();
    HMODULE shcore = LoadLibraryW(L"shcore.dll");
    if (shcore)
        g_get_dpi_for_monitor = (GetDpiForMonitorFn)(void *)GetProcAddress(shcore, "GetDpiForMonitor");
    init_paths();
    enum_monitors();

    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        /* GUI 子系统无控制台；带参数从终端运行时附到父控制台，保留 CLI 诊断输出。 */
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
        }
        if (_wcsicmp(argv[1], L"--tray") == 0 || _wcsicmp(argv[1], L"--silent") == 0) {
            g_start_in_tray = 1;
        } else if (_wcsicmp(argv[1], L"--test-focus") == 0) {
            g_test_focus = 1;
        } else if (_wcsicmp(argv[1], L"--ui-bench") == 0) {
            LocalFree(argv);
            return ui_bench();
        } else {
            int code = run_cli(argc, argv);
            LocalFree(argv);
            return code;
        }
    }
    if (argv) LocalFree(argv);
    if (!single_instance_allow()) return 0;
    g_taskbar_created_msg = RegisterWindowMessageW(L"TaskbarCreated");
    return run_gui(hInst, nShow);
}
