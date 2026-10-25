#ifndef FF6_SCENE_UPSCALE_HPP
#define FF6_SCENE_UPSCALE_HPP

#include "sharp_bilinear.h"
#include "scene_clip.h"
#include <atomic>

static GLuint scene_fbo, scene_tex, scene_depth;
static GLuint scene_program, scene_vao, scene_vbo;
static GLint scene_size_uniform, scene_prescale_uniform;
static int scene_w, scene_h, scene_bound, scene_presented;
static GLint scene_rect[4];
static GLint scene_native_rect[4];
/* Rendering and input run on different threads. Publish a coherent layout
 * without making the input thread read partially updated viewport arrays. */
static std::atomic<unsigned> scene_layout_epoch{0};
static std::atomic<int> scene_touch_layout[8];
static void publish_scene_layout(void) {
  bool changed = scene_layout_epoch.load(std::memory_order_relaxed) == 0;
  for (int i = 0; i < 4; ++i)
    changed = changed || scene_touch_layout[i].load(std::memory_order_relaxed) != scene_native_rect[i] ||
              scene_touch_layout[i + 4].load(std::memory_order_relaxed) != scene_rect[i];
  if (!changed) return;
  scene_layout_epoch.fetch_add(1);
  for (int i = 0; i < 4; ++i) {
    scene_touch_layout[i].store(scene_native_rect[i]);
    scene_touch_layout[i + 4].store(scene_rect[i]);
  }
  scene_layout_epoch.fetch_add(1);
}
static void map_scene_touch(float *x, float *y) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    unsigned epoch = scene_layout_epoch.load();
    if (!epoch) return;
    if (epoch & 1) continue;
    int layout[8];
    for (int i = 0; i < 8; ++i)
      layout[i] = scene_touch_layout[i].load();
    if (epoch == scene_layout_epoch.load()) {
      ff6_display_touch(layout, layout + 4, SCREEN_H, x, y);
      return;
    }
  }
  *x = *y = -1.0f; /* Ignore a tap during a layout transition. */
}
static GLint scene_scissor[4];
static int scene_scissor_valid;

static int get_scene_scissor(GLint *params) {
  if (!scene_scissor_valid)
    return 0;
  memcpy(params, scene_scissor, sizeof(scene_scissor));
  return 1;
}

static void apply_scene_scissor(void) {
  if (!scene_scissor_valid) {
    glGetIntegerv(GL_SCISSOR_BOX, scene_scissor);
    scene_scissor_valid = 1;
  }
  GLint mapped[4];
  const GLint *clip = scene_scissor;
  if (scene_bound && ff6_scene_clip(scene_scissor, scene_native_rect,
                                    scene_w, scene_h, mapped))
    clip = mapped;
  glScissor(clip[0], clip[1], clip[2], clip[3]);
}

static void glScissorHook(GLint x, GLint y, GLsizei width, GLsizei height) {
  if (width < 0 || height < 0) {
    glScissor(x, y, width, height);
    return;
  }
  scene_scissor[0] = x;
  scene_scissor[1] = y;
  scene_scissor[2] = width;
  scene_scissor[3] = height;
  scene_scissor_valid = 1;
  apply_scene_scissor();
#ifdef FF6_DEBUG
  static int logged;
  if (scene_bound && logged++ < 32) {
    GLint mapped[4];
    glGetIntegerv(GL_SCISSOR_BOX, mapped);
    trace("[clip] screen=(%d,%d %dx%d) fbo=(%d,%d %dx%d)\n",
          x, y, width, height, mapped[0], mapped[1], mapped[2], mapped[3]);
  }
#endif
}

static GLuint compile_scene_shader(GLenum type, const char *source) {
  GLuint shader = glCreateShader(type);
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  GLint compiled = GL_FALSE;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
  if (!compiled) {
    char log[1024] = {};
    glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    fatal_error("Scene upscale shader failed: %s", log);
  }
  return shader;
}

static void ensure_scene_program(void) {
  if (scene_program)
    return;
  const char *vertex =
      "void main(float2 position, float2 texcoord, "
      "out float2 uv : TEXCOORD0, out float4 clip : POSITION) { "
      "uv = texcoord; clip = float4(position, 0.0, 1.0); }";
  const char *fragment =
      "uniform sampler2D image : TEXUNIT0; "
      "uniform float2 source_size; uniform float2 prescale; "
      "float4 main(float2 uv : TEXCOORD0) : COLOR { "
      "float2 texel = uv * source_size; "
      "float2 delta = frac(texel) - 0.5; "
      "float2 radius = 0.5 - 0.5 / prescale; "
      "float2 sample_uv = (floor(texel) + 0.5 + "
      "(delta - clamp(delta, -radius, radius)) * prescale) / source_size; "
      "return tex2D(image, sample_uv); }";
  GLuint vs = compile_scene_shader(GL_CG_VERTEX_SHADER_EXT, vertex);
  GLuint fs = compile_scene_shader(GL_CG_FRAGMENT_SHADER_EXT, fragment);
  scene_program = glCreateProgram();
  glAttachShader(scene_program, vs);
  glAttachShader(scene_program, fs);
  glBindAttribLocation(scene_program, 0, "position");
  glBindAttribLocation(scene_program, 1, "texcoord");
  glLinkProgram(scene_program);
  GLint linked = GL_FALSE;
  glGetProgramiv(scene_program, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[1024] = {};
    glGetProgramInfoLog(scene_program, sizeof(log), NULL, log);
    fatal_error("Scene upscale program failed: %s", log);
  }
  scene_size_uniform = glGetUniformLocation(scene_program, "source_size");
  scene_prescale_uniform = glGetUniformLocation(scene_program, "prescale");
  if (scene_size_uniform < 0 || scene_prescale_uniform < 0)
    fatal_error("Scene upscale uniforms are missing.");
  glDeleteShader(vs);
  glDeleteShader(fs);

  /* The GLES 1.x engine never binds a VAO or uses generic attributes. Keep
   * our programmable quad separate from its fixed-function vertex pointers. */
  GLint old_buffer = 0;
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_buffer);
  static const GLfloat quad[] = {
      -1, -1, 0, 0,  1, -1, 1, 0,  -1, 1, 0, 1,  1, 1, 1, 1};
  glGenVertexArrays(1, &scene_vao);
  glBindVertexArray(scene_vao);
  glGenBuffers(1, &scene_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, scene_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), NULL);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                        (const void *)(2 * sizeof(GLfloat)));
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, old_buffer);
}

static int scene_fbo_wanted(void) {
  return (options.offscreen || options.sharp_bilinear || options.aspect_ratio ||
          options.integer_scale) &&
         ortho_base_w > 0 && ortho_base_h > 0;
}

static int ensure_scene_fbo(void) {
  if (scene_fbo && scene_w == ortho_base_w && scene_h == ortho_base_h)
    return 1;
  ensure_scene_program();
  GLint old_texture = 0, old_fbo = 0, old_depth = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
  glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_depth);
  if (scene_fbo) {
    glDeleteFramebuffers(1, &scene_fbo);
    glDeleteTextures(1, &scene_tex);
    glDeleteRenderbuffers(1, &scene_depth);
  }
  glGenTextures(1, &scene_tex);
  glBindTexture(GL_TEXTURE_2D, scene_tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ortho_base_w, ortho_base_h, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenRenderbuffers(1, &scene_depth);
  glBindRenderbuffer(GL_RENDERBUFFER, scene_depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16,
                        ortho_base_w, ortho_base_h);
  glGenFramebuffers(1, &scene_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, scene_tex, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, scene_depth);
  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  glBindFramebuffer(GL_FRAMEBUFFER, old_fbo);
  glBindRenderbuffer(GL_RENDERBUFFER, old_depth);
  glBindTexture(GL_TEXTURE_2D, old_texture);
  bound_texture_2d = UINT32_MAX;
  if (status != GL_FRAMEBUFFER_COMPLETE)
    fatal_error("Scene upscale framebuffer incomplete: 0x%x", status);
  scene_w = ortho_base_w;
  scene_h = ortho_base_h;
  trace("[fbo] world buffer %dx%d\n", scene_w, scene_h);
  return 1;
}

/* Reset at swap; do not capture a full-screen menu or clear the game's GL
 * state before its next viewport tells us that a world pass is starting. */
static void bind_scene_fbo(void) {
  scene_presented = 0;
}

static void start_scene_fbo(void) {
  if (!ensure_scene_fbo())
    return;
  glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo);
  scene_bound = 1;
  glViewport(0, 0, scene_w, scene_h);
  GLfloat clear_color[4], clear_depth;
  GLboolean color_mask[4], depth_mask;
  GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
  glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);
  glGetFloatv(GL_DEPTH_CLEAR_VALUE, &clear_depth);
  glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
  glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glDepthMask(GL_TRUE);
  glClearColor(0, 0, 0, 1);
  glClearDepthf(1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
  glClearDepthf(clear_depth);
  glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
  glDepthMask(depth_mask);
  apply_scene_scissor();
  if (scissor)
    glEnable(GL_SCISSOR_TEST);
}

static void present_scene_fbo(void) {
  if (!scene_bound)
    return;
  scene_bound = 0;
  scene_presented = 1;
  GLint active_texture = GL_TEXTURE0, old_texture = 0, old_program = 0;
  glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
  glGetIntegerv(GL_CURRENT_PROGRAM, &old_program);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
  const GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_ALPHA_TEST,
                         GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST};
  GLboolean enabled[6], color_mask[4];
  glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
  for (int i = 0; i < 6; i++) {
    enabled[i] = glIsEnabled(caps[i]);
    glDisable(caps[i]);
  }
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glBindFramebuffer(GL_FRAMEBUFFER, display_target());
  glViewport(scene_rect[0], scene_rect[1], scene_rect[2], scene_rect[3]);
  glUseProgram(scene_program);
  glBindVertexArray(scene_vao);
  glBindTexture(GL_TEXTURE_2D, scene_tex);
  float sx = options.sharp_bilinear ? ff6_sharp_prescale(scene_w, scene_rect[2]) : 1;
  float sy = options.sharp_bilinear ? ff6_sharp_prescale(scene_h, scene_rect[3]) : 1;
  glUniform2f(scene_size_uniform, (float)scene_w, (float)scene_h);
  glUniform2f(scene_prescale_uniform, sx, sy);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  glUseProgram(old_program);
  glBindTexture(GL_TEXTURE_2D, old_texture);
  glActiveTexture(active_texture);
  bound_texture_2d = UINT32_MAX;
  glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
  apply_scene_scissor();
  for (int i = 0; i < 6; i++)
    if (enabled[i])
      glEnable(caps[i]);

#ifdef FF6_DEBUG
  static int logged;
  if (!logged) {
    logged = 1;
    trace("[upscale] %s active: %dx%d -> %dx%d, prescale=%dx%d, program=%u\n",
          options.sharp_bilinear ? "sharp-bilinear" : "bilinear",
          scene_w, scene_h, scene_rect[2], scene_rect[3], (int)sx, (int)sy,
          scene_program);
    FILE *status = fopen(DATA_PATH "/upscale-status.txt", "wb");
    if (status) {
      fprintf(status, "filter=%s\nsource=%dx%d\ndestination=%dx%d\nprescale=%dx%d\nshader_linked=1\n",
              options.sharp_bilinear ? "sharp-bilinear" : "bilinear",
              scene_w, scene_h, scene_rect[2], scene_rect[3], (int)sx, (int)sy);
      fclose(status);
    }
  }
#endif
}

#endif
