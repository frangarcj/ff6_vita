#!/usr/bin/env python3
"""Exercise the production swap routine with host-side GL/scheduler stubs.

This is a control-flow regression, not a GPU/emulator performance test. Extract
only the swap entrypoint so both compile profiles exercise the same body that
ships in the loader. Scheduler stubs count requested waits without sleeping.
"""

from pathlib import Path
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
START = "static unsigned fake_eglSwapBuffers(void *dpy, void *surface) {"
END = "\nstatic void ANativeWindow_setBuffersGeometry"

STUBS = r"""
#include <cassert>
#include <cstdint>
#include <cstdio>

using GLfloat = float;
enum { GL_FALSE = 0, GL_TRUE = 1, GL_TEXTURE_2D, GL_BLEND, GL_PROJECTION,
       GL_MODELVIEW, GL_VERTEX_ARRAY, GL_TEXTURE_COORD_ARRAY, GL_COLOR_ARRAY,
       GL_FLOAT, GL_TRIANGLE_STRIP };
static int SCREEN_W = 960, SCREEN_H = 544;
static int frame_has_bright_scene_draw;
static unsigned wait_calls, report_calls, probe_calls, injected_draws, stage;
static bool dialog_open;
static void trace(const char *, ...) {}
static void gl_stats_report() { ++report_calls; }
static void probe_pixel(const char *, int, int) { ++probe_calls; }
static void draw_viewport_probe() { ++injected_draws; }
static void sceKernelDelayThread(unsigned) { ++wait_calls; }
static bool isInputDialogOpen() { return dialog_open; }
static void present_scene_fbo() { assert(stage == 0); stage = 1; }
static void present_postfx() { assert(stage == 1); stage = 2; }
static void vglSwapBuffers(int dialog) {
  assert(stage == 2);
  assert(dialog == (dialog_open ? GL_TRUE : GL_FALSE));
  stage = 3;
}
static void bind_scene_fbo() { assert(stage == 3); stage = 4; }
static void begin_postfx() { assert(stage == 4); stage = 5; }
template<class... T> static void glDisable(T...) {}
template<class... T> static void glMatrixMode(T...) {}
template<class... T> static void glLoadIdentity(T...) {}
template<class... T> static void glOrthof(T...) {}
template<class... T> static void glEnableClientState(T...) {}
template<class... T> static void glDisableClientState(T...) {}
template<class... T> static void glViewport(T...) {}
template<class... T> static void glColor4f(T...) {}
template<class... T> static void glVertexPointer(T...) {}
template<class... T> static void glDrawArrays(T...) { ++injected_draws; }
template<class... T> static void glFlush(T...) { ++probe_calls; }
"""

DRIVER = r"""
int main() {
  for (unsigned frame = 1; frame <= 2100; ++frame) {
    // Reproduce the bright-draw condition of the reported battle stall.
    frame_has_bright_scene_draw = frame == 2068;
    dialog_open = (frame % 3 == 0);
    stage = 0;
    assert(fake_eglSwapBuffers(nullptr, nullptr) == 1);
    assert(stage == 5);
  }
  if (wait_calls) {
    std::fprintf(stderr, "swap requested %u diagnostic sleep(s)\n", wait_calls);
    return 1;
  }
  if (probe_calls || injected_draws) {
    std::fprintf(stderr, "swap requested %u diagnostic probes and %u injected draws\n",
                 probe_calls, injected_draws);
    return 1;
  }
#ifdef FF6_DEBUG
  assert(report_calls == 2100);
#else
  assert(report_calls == 0);
#endif
  std::puts("2100 frames: world/postfx/swap/rearm order and dialog flag correct; no diagnostic injection");
}
"""


def main() -> None:
    source = (ROOT / "loader/main.cpp").read_text()
    if source.count(START) != 1:
        raise RuntimeError("The swap entrypoint changed; update this test's extraction anchors")
    start = source.index(START)
    end = source.index(END, start)
    compiler = shutil.which(os.environ.get("CXX", "clang++"))
    if not compiler:
        raise RuntimeError("A host C++ compiler is required (set CXX)")
    with tempfile.TemporaryDirectory(prefix="ff6-frame-present-") as directory:
        directory = Path(directory)
        fixture = directory / "frame_present.cpp"
        fixture.write_text(STUBS + source[start:end] + DRIVER)
        for debug in (False, True):
            profile = "debug" if debug else "release"
            binary = directory / profile
            command = [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-Wno-unused-function",
                       "-Wno-unused-variable", "-fsanitize=address,undefined",
                       "-fno-omit-frame-pointer", "-g"]
            if debug:
                command.append("-DFF6_DEBUG")
            subprocess.run(command + [str(fixture), "-o", str(binary)],
                           check=True, timeout=60)
            print(f"{profile}:", flush=True)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    main()
