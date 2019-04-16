#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef NOMINMAX
#undef WIN32_LEAN_AND_MEAN

MU_TEST_INTERNAL
int platform_get_resource_path(char *buffer, int buffer_n, char const *const relative_path, int relative_path_n) {
    wchar_t wbuffer[4096];
    DWORD wbuffer_n = GetModuleFileNameW(/* executable */ NULL, wbuffer, sizeof wbuffer);
    if (wbuffer_n == sizeof wbuffer)
        return buffer_n;
    int buffer_i = WideCharToMultiByte(CP_UTF8, 0, wbuffer, wbuffer_n, buffer, buffer_n, NULL, NULL);
    while (buffer_i && buffer[buffer_i] != '\\')
        --buffer_i;
    if (buffer[buffer_i] == '\\')
        ++buffer_i;
    if (buffer_n - buffer_i < relative_path_n)
        return buffer_n;
    memcpy(buffer + buffer_i, relative_path, relative_path_n), buffer_i += relative_path_n;
    buffer[buffer_i] = 0;
    return buffer_i;
}