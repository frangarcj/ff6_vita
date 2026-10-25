/* FF6 configuration companion. UI flow follows ff3_vita; settings are shared
 * with the game and a failed save never launches with silently lost changes. */
#include <vitasdk.h>
#include <vitaGL.h>
#include <imgui_vita.h>
#include <stdio.h>
#include <string.h>
#include "../loader/options.h"

extern "C" {
void *__wrap_memcpy(void *d, const void *s, size_t n) { return sceClibMemcpy(d, s, n); }
void *__wrap_memmove(void *d, const void *s, size_t n) { return sceClibMemmove(d, s, n); }
void *__wrap_memset(void *d, int c, size_t n) { return sceClibMemset(d, c, n); }
}

static const char *const resolution_names[] = {"544p (native)", "720p (experimental)", "1080i (experimental)"};
static const char *const aspect_names[] = {"Native 3:2", "4:3 stretch", "16:9 stretch", "2:1 stretch", "21:9 stretch"};
static const char *const aa_names[] = {"Disabled", "MSAA 2x", "MSAA 4x"};
static const char *const effect_names[] = {"None", "Negative", "FXAA", "Sepia", "Greyscale", "CRT", "CRT-Bleeding", "CRT-Bleeding-Flat"};
static const char *const language_names[] = {"System", "Japanese", "English", "German", "French", "Italian", "Spanish", "Simplified Chinese", "Traditional Chinese", "Korean", "Thai (incomplete font)", "Russian", "Portuguese (Brazil)"};
static const int language_values[] = {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};

static void combo(const char *label, int *value, const char *const *names, int count) {
  if (*value < 0 || *value >= count) *value = 0;
  if (ImGui::BeginCombo(label, names[*value])) {
    for (int i = 0; i < count; ++i) {
      bool selected = *value == i;
      if (ImGui::Selectable(names[i], selected)) *value = i;
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
}

static void toggle(const char *label, int *value) {
  bool checked = *value != 0;
  if (ImGui::Checkbox(label, &checked)) *value = checked ? 1 : 0;
}

int main(int, char **) {
  SceAppUtilInitParam init = {};
  SceAppUtilBootParam boot = {};
  sceAppUtilInit(&init, &boot);
  sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
  sceIoMkdir("ux0:data", 0777);
  sceIoMkdir(DATA_PATH, 0777);
  config_opts opts;
  int loaded = ff6_load_options(CONFIG_FILE_PATH, &opts);
  char message[192] = {};
  if (loaded < 0) snprintf(message, sizeof(message), "Cannot read options.cfg. Saving will report any filesystem error.");

  vglInitExtended(0, 960, 544, 0x1800000, SCE_GXM_MULTISAMPLE_NONE);
  ImGui::CreateContext();
  ImGui_ImplVitaGL_Init();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui_ImplVitaGL_TouchUsage(false);
  ImGui_ImplVitaGL_GamepadUsage(true);
  ImGui::StyleColorsDark();
  ImGui::GetIO().MouseDrawCursor = false;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

  bool finished = false;
  unsigned previous_buttons = 0;
  while (!finished) {
    int action = -1;
    SceCtrlData pad = {};
    sceCtrlPeekBufferPositiveExt2(0, &pad, 1);
    if ((pad.buttons & SCE_CTRL_START) && !(previous_buttons & SCE_CTRL_START))
      action = 1;
    previous_buttons = pad.buttons;
    ImGui_ImplVitaGL_NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiSetCond_Always);
    ImGui::SetNextWindowSize(ImVec2(960, 544), ImGuiSetCond_Always);
    ImGui::Begin("##ff6_config", nullptr, ImGuiWindowFlags_NoTitleBar |
      ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse);
    ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.38f, 1.0f), "FINAL FANTASY VI  |  Configurator");
    ImGui::Text("Settings apply on launch. START: save and launch. Saved games are never changed here.");
    ImGui::Separator();
    ImGui::Columns(2, "settings", false);
    ImGui::PushItemWidth(270);
    ImGui::Text("Graphics");
    combo("Resolution", &opts.res, resolution_names, 3);
    combo("Aspect", &opts.aspect_ratio, aspect_names, 5);
    combo("Anti-aliasing", &opts.msaa, aa_names, 3);
    combo("PostFX", &opts.postfx, effect_names, 8);
    toggle("Bilinear textures", &opts.bilinear);
    toggle("Sharp-bilinear final scale", &opts.sharp_bilinear);
    toggle("Integer scaling (overrides aspect)", &opts.integer_scale);
    ImGui::SliderInt("Font scale (%)", &opts.font_scale, 25, 200);
    ImGui::TextWrapped("544p and native 3:2 are the baseline. Higher resolutions need Sharpscale/PSTV support. Aspect stretches within the game area, keeping the side panels; it does not expand the field of view.");
    ImGui::PopItemWidth();
    ImGui::NextColumn();
    ImGui::PushItemWidth(270);
    ImGui::Text("Language and controls");
    int language = 0;
    for (int i = 0; i < 13; ++i) if (opts.lang == language_values[i]) language = i;
    combo("Language", &language, language_names, 13);
    opts.lang = language_values[language];
    toggle("Sound", &opts.sound);
    toggle("Swap Cross / Circle", &opts.swap_buttons);
    ImGui::TextWrapped("Swap changes confirm/cancel in the game, including releases. The configurator keeps its normal system navigation.");
    ImGui::Separator();
    ImGui::Text("Advanced / experimental");
    toggle("Offscreen world buffer", &opts.offscreen);
    toggle("In-place texture updates", &opts.inplace_tex);
    toggle("Native fixed-point colours", &opts.fixed_color_native);
    ImGui::TextWrapped("Sharp-bilinear and aspect modes enable the world buffer automatically. Keep in-place updates on and native fixed-point colours off for the baseline candidate.");
    ImGui::PopItemWidth();
    ImGui::Columns(1);
    ImGui::Separator();
    if (ImGui::Button("Save and launch")) action = 1;
    ImGui::SameLine();
    if (ImGui::Button("Save and exit")) action = 0;
    ImGui::SameLine();
    if (ImGui::Button("Discard and launch")) action = 3;
    ImGui::SameLine();
    if (ImGui::Button("Discard and exit")) action = 2;
    ImGui::SameLine();
    if (ImGui::Button("Restore defaults")) ff6_default_options(&opts);
    if (*message) ImGui::TextWrapped("%s", message);
    ImGui::End();
    glViewport(0, 0, 960, 544);
    ImGui::Render();
    ImGui_ImplVitaGL_RenderDrawData(ImGui::GetDrawData());
    vglSwapBuffers(GL_FALSE);

    if (action >= 0) {
      if (action < 2 && ff6_save_options(CONFIG_FILE_PATH, &opts) < 0) {
        snprintf(message, sizeof(message), "Save failed. The existing configuration was not replaced. Check free space and write access.");
        continue;
      }
      if (action & 1) {
        int result = sceAppMgrLoadExec("app0:/eboot.bin", NULL, NULL);
        snprintf(message, sizeof(message), "Cannot launch the game (0x%08x). Settings already saved only if Save was selected.", (unsigned)result);
      } else finished = true;
    }
  }
  return 0;
}
