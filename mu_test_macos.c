#include <CoreFoundation/CoreFoundation.h>

MU_TEST_INTERNAL
int platform_get_resource_path(char *buffer, int buffer_n, char const *const relative_path, int relative_path_n) {
    CFBundleRef mainBundle = CFBundleGetMainBundle();
    CFURLRef resourceURL = CFBundleCopyResourcesDirectoryURL(mainBundle);
    if (CFURLGetFileSystemRepresentation(resourceURL, /*resoveAgainstBase*/ 1, (UInt8 *)buffer, buffer_n)) {
        int buffer_i = strlen(buffer);
        if (buffer_i != buffer_n)
            buffer[buffer_i++] = '/';
        char const *s = relative_path;
        int const s_n = relative_path_n;
        if (buffer_i + s_n < buffer_n)
            memcpy(buffer + buffer_i, s, s_n), buffer_i += s_n;
        if (buffer_i != buffer_n)
            buffer[buffer_i++] = 0;
        return buffer_i;
    }
    CFRelease(resourceURL), CFRelease(mainBundle);
    return buffer_n;
}
