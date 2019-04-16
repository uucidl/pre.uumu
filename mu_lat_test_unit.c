// Low latency test.
// @url: https://timothylottes.github.io/20170408.html
//
// Terminology
// @definition{scanout}: the frame is sent out the cable and to the monitor

#define MU_LAT_TEST_INTERNAL static

#include "xxxx_mu.h"

#if defined(__APPLE__)
#include "OpenGL/gl.h" // @todo: @platform{macos} specific location
#endif

#if defined(_WIN32)
#if !defined(APIENTRY)
#define APIENTRY __stdcall
#define MU_LAT_TEST_DEFINED_APIENTRY
#endif
#if !defined(WINGDIAPI)
#define WINGDIAPI __declspec(dllimport)
#define MU_LAT_TEST_DEFINED_WINGDIAPI
#endif
#include <gl/GL.h>
#if defined(MU_LAT_TEST_DEFINED_APIENTRY)
#undef APIENTRY
#undef MU_LAT_TEST_DEFINED_APIENTRY
#endif
#if defined(MU_LAT_TEST_DEFINED_WINGDIAPI)
#undef WINGDIAPI
#undef MU_LAT_TEST_DEFINED_WINGDIAPI
#endif
#endif // _WIN32

#include <errno.h>
#include <stdio.h>
#include <time.h>

MU_LAT_TEST_INTERNAL
void immquad2(int x, int y, int w, int h) {
    glBegin(GL_QUADS);
    glVertex2f(x + w, y + h);
    glVertex2f(x + w, y);
    glVertex2f(x, y);
    glVertex2f(x, y + h);
    glEnd();
}

#if defined(_WIN32)
int win32_thrd_sleep(const struct timespec *time_point, struct timespec *remaining);
#endif

#if defined(_WIN32)
MU_LAT_TEST_INTERNAL
int thrd_sleep(const struct timespec *time_point, struct timespec *remaining) {
    return win32_thrd_sleep(time_point, remaining);
}
#endif

#if defined(__APPLE__)
MU_LAT_TEST_INTERNAL
int thrd_sleep(const struct timespec *time_point, struct timespec *remaining) {
    return nanosleep(time_point, remaining);
}
#endif

MU_LAT_TEST_INTERNAL
void minmax_action_d(double *min, double *max, double value) {
    *min = *min < value ? *min : value;
    *max = *max > value ? *max : value;
}

MU_LAT_TEST_INTERNAL
void minmax_action_u64(uint64_t *min, uint64_t *max, uint64_t value) {
    *min = *min < value ? *min : value;
    *max = *max > value ? *max : value;
}

int main(int argc, char **argv) {
    struct Mu mu = {.window.title = "low latency test"};
    if (!Mu_Initialize(&mu))
        return 1;

    double scan_out_wait_ms = 15.0;

    double max_push_to_pull_wait_ms = 0.0;
    double min_push_to_pull_wait_ms = 1e6;
    uint64_t last_frame_ms = 0;
    uint64_t max_frame_ms = 0;
    uint64_t min_frame_ms = 1e6;
    while (Mu_Pull(&mu)) {
        uint64_t frame_ms = mu.time.milliseconds;
        uint64_t frame_period_ms = 0;
        if (last_frame_ms) {
            double const frame_delta_ms = frame_ms - last_frame_ms;
            minmax_action_u64(&min_frame_ms, &max_frame_ms, frame_delta_ms);
            if (frame_ms < last_frame_ms)
                printf("ERROR: %llu %llu\n", frame_ms, last_frame_ms);
            else
                frame_period_ms = frame_ms - last_frame_ms;
        }
        last_frame_ms = frame_ms;
        double const push_to_pull_waited_ms = mu.time.delta_seconds * 1000.0;
        minmax_action_d(&min_push_to_pull_wait_ms, &max_push_to_pull_wait_ms, push_to_pull_waited_ms);
        for (char *tp = &mu.text[0], *tpl = &mu.text[mu.text_length]; tp < tpl; ++tp) {
            if (*tp == 033)
                mu.quit = MU_TRUE;
        }

        if (mu.mouse.delta_wheel) {
            scan_out_wait_ms += scan_out_wait_ms * mu.mouse.delta_wheel / 16;
            printf("scan_out_wait_ms: %f\n", scan_out_wait_ms);
        }

        glViewport(0, 0, mu.window.size.x, mu.window.size.y);
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, mu.window.size.x, mu.window.size.y, 0, -1, 1);
        glClearColor(0.60, 0.59, 0.62, 0.0);
        glClear(GL_COLOR_BUFFER_BIT);

        int mouse_was_pressed = 0;
        mouse_was_pressed += mu.mouse.left_button.pressed ? 1 : 0;

        /* display mouse */ if (1) {
            int bw = mouse_was_pressed ? 32 : 8;
            glColor3f(1.0, 1.0, 1.0), immquad2(mu.mouse.position.x - bw / 2, mu.mouse.position.y - bw / 2, bw, bw);
        }

        // Waiting and pulling events once more just before the frame-flip, to be able to show
        // interactions at the lowest-latency possible.
        struct timespec just_before_scan_out_timepoint = {
                .tv_sec = 0,
                .tv_nsec = scan_out_wait_ms * 1000 * 1000,
        };
        while (0 != thrd_sleep(&just_before_scan_out_timepoint, &just_before_scan_out_timepoint) && errno == EINTR)
            ;

        Mu_Pull(&mu);
        mouse_was_pressed += mu.mouse.left_button.pressed ? 1 : 0;

        /* display mouse */ if (1) {
            int bw = mouse_was_pressed ? 32 : 8;
            glColor3f(1.0, 0.0, 1.0), immquad2(mu.mouse.position.x - bw / 2, mu.mouse.position.y - bw / 2, bw, bw);
        }

        for (char *tp = &mu.text[0], *tpl = &mu.text[mu.text_length]; tp < tpl; ++tp) {
            if (*tp == 033)
                mu.quit = MU_TRUE;
        }
        if (mu.mouse.delta_wheel) {
            scan_out_wait_ms += scan_out_wait_ms * mu.mouse.delta_wheel / 16;
            printf("scan_out_wait_ms: %f\n", scan_out_wait_ms);
        }

        Mu_Push(&mu);
    }
}

#if defined(_WIN32) && defined(_MSC_VER)
// needed by pervognsen_mu
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "Mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "Windowscodecs.lib")
#endif
