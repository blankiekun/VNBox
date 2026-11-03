// Host wined3d (HOST_WINED3D=1): the msvcrt/ucrt functions Wine's headers turn printf, assert, errno, fpclassify
// and friends into, implemented on Emscripten's libc. This file uses libc's headers, not Wine's, so each signature
// is spelled out to match Wine's include/msvcrt prototype (a mismatch would make wasm-ld trap the call).
#ifdef BOXEDWINE_HOST_WINED3D
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <malloc.h>

void wd3d_log_fmt(const char* format, ...);
extern "C" void wd3d_guest_fault(const char* what); // thunkHost.cpp

extern "C" {

// stdin/stdout/stderr: the FILE* is only ever handed back to the functions below, so libc's own work.
FILE* __acrt_iob_func(unsigned int index) {
    return index == 0 ? stdin : index == 1 ? stdout : stderr;
}

int _fileno(FILE* file) {
    return fileno(file);
}

// Only the HLSL preprocessor's include reader calls it; host wined3d never gives it a file.
int _fstat32(int fd, void* buf) {
    (void)fd; (void)buf;
    errno = EBADF;
    return -1;
}

int* _errno(void) {
    return &errno;
}

// _CRT_INTERNAL_PRINTF_STANDARD_SNPRINTF_BEHAVIOR: C99 return value on truncation; without it ucrt returns -1.
static const uint64_t STANDARD_SNPRINTF = 2;

// shortcut: MSVC-only conversions (%ls/%S as 2-byte strings, %I64) print wrong; only debug output uses them, fix
// them if a log line needs them.
int __stdio_common_vsprintf(uint64_t options, char* str, size_t len, const char* format, void* locale, va_list args) {
    (void)locale;
    int ret = vsnprintf(str, len, format, args);
    if (ret >= 0 && (size_t)ret >= len && str && !(options & STANDARD_SNPRINTF)) return -1;
    return ret;
}

int __stdio_common_vfprintf(uint64_t options, FILE* file, const char* format, void* locale, va_list args) {
    (void)options; (void)locale;
    return vfprintf(file, format, args);
}

int __stdio_common_vsscanf(uint64_t options, const char* input, size_t length, const char* format, void* locale, va_list args) {
    (void)options; (void)locale;
    if (length == (size_t)-1) return vsscanf(input, format, args);
    char* copy = strndup(input, length);
    int ret = vsscanf(copy, format, args);
    free(copy);
    return ret;
}

// A failed assert is a guest access violation (thunkHost.cpp): where host wined3d asserts, the guest wined3d mostly
// goes on and faults (a NULL map). abort() only without a guest to tell.
[[noreturn]] void _assert(const char* expr, const char* file, unsigned int line) {
    wd3d_log_fmt("assertion failed: %s, %s:%u", expr, file, line);
    wd3d_guest_fault("assertion failed");
    abort();
}

// Wine's math.h classification values (MSVC's), not libc's.
short _dclass(double x) {
    switch (fpclassify(x)) {
    case FP_INFINITE: return 1;
    case FP_NAN: return 2;
    case FP_NORMAL: return -1;
    case FP_SUBNORMAL: return -2;
    default: return 0;
    }
}

short _fdclass(float x) {
    return _dclass(x);
}

int _dsign(double x) {
    return signbit(x) ? 0x8000 : 0;
}

int _fdsign(float x) {
    return signbit(x) ? 0x8000 : 0;
}

int _stricmp(const char* a, const char* b) {
    return strcasecmp(a, b);
}

// wined3d only sets VKD3D_DEBUG/VKD3D_SHADER_DEBUG, from string literals; libc's putenv keeps the pointer.
int _putenv(const char* entry) {
    return putenv((char*)entry);
}

// shortcut: zeroes from the old block's usable size, so bytes between its requested and usable size keep their
// contents (msvcrt zeroes from the requested size); fine for wined3d's one user, which grows a zeroed array.
void* _recalloc(void* ptr, size_t count, size_t size) {
    if (size && count > SIZE_MAX / size) return nullptr;
    size_t total = count * size;
    size_t old = ptr ? malloc_usable_size(ptr) : 0;
    void* result = realloc(ptr, total);
    if (result && total > old) memset((char*)result + old, 0, total - old);
    return result;
}

// wcslen for Wine's 2-byte WCHAR (sources.mk renames wcslen to this; libc's counts 4-byte characters).
size_t wd3d_wcslen(const char16_t* str) {
    size_t len = 0;
    while (str[len]) len++;
    return len;
}

// vkd3d-utils isn't built (only vkd3d-shader is); wined3d just routes its log through here.
void vkd3d_utils_set_log_callback(void* callback) {
    (void)callback;
}

}
#endif
