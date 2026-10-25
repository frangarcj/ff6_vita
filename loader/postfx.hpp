#ifndef FF6_POSTFX_HPP
#define FF6_POSTFX_HPP

/* Full-screen effects follow the ff3_vita shader interface. This pass runs
 * after world scaling and UI composition, but before the OS common dialog. */
static GLuint postfx_fbo, postfx_texture, postfx_depth;
static GLuint postfx_program, postfx_vao, postfx_vbo;
static GLint postfx_time = -1, postfx_size = -1, postfx_vertex_size = -1;
static GLuint display_target(void) { return postfx_fbo; }

static GLuint load_postfx_shader(GLenum type, const char *path) {
  FILE *file = fopen(path, "rb");
  if (!file) fatal_error("Missing PostFX shader: %s. Reinstall the VPK.", path);
  if (fseek(file, 0, SEEK_END)) fatal_error("Cannot read PostFX shader: %s", path);
  long length = ftell(file);
  if (length <= 0 || length > 65536 || fseek(file, 0, SEEK_SET))
    fatal_error("Invalid PostFX shader: %s", path);
  char *source = (char *)malloc((size_t)length + 1);
  if (!source) fatal_error("Not enough memory for PostFX shader.");
  size_t received = fread(source, 1, (size_t)length, file);
  int close_result = fclose(file);
  if (received != (size_t)length || close_result)
    fatal_error("Incomplete PostFX shader: %s", path);
  source[length] = 0;
  GLuint shader = compile_scene_shader(type, source);
  free(source);
  return shader;
}

static void init_postfx(void) {
  if (!options.postfx || postfx_fbo) return;
  static const char *const names[] = {"", "1_Negative", "2_FXAA", "3_Sepia",
    "4_Greyscale", "5_CRT", "6_CRT-Bleeding", "7_CRT-Bleeding-Flat"};
  if (options.postfx < 1 || options.postfx > 7) return;
  char path[128];
  snprintf(path, sizeof(path), "app0:shaders/%s_v.cg", names[options.postfx]);
  GLuint vs = load_postfx_shader(GL_CG_VERTEX_SHADER_EXT, path);
  snprintf(path, sizeof(path), "app0:shaders/%s_f.cg", names[options.postfx]);
  GLuint fs = load_postfx_shader(GL_CG_FRAGMENT_SHADER_EXT, path);
  postfx_program = glCreateProgram();
  glAttachShader(postfx_program, vs);
  glAttachShader(postfx_program, fs);
  glBindAttribLocation(postfx_program, 0, "position");
  glBindAttribLocation(postfx_program, 1, "texcoord");
  glLinkProgram(postfx_program);
  GLint linked = GL_FALSE;
  glGetProgramiv(postfx_program, GL_LINK_STATUS, &linked);
  if (!linked) {
    char log[1024] = {};
    glGetProgramInfoLog(postfx_program, sizeof(log), NULL, log);
    fatal_error("Cannot link PostFX program: %s", log);
  }
  glDeleteShader(vs); glDeleteShader(fs);
  postfx_time = glGetUniformLocation(postfx_program, "iTime");
  postfx_size = glGetUniformLocation(postfx_program, "uOutputSize");
  postfx_vertex_size = glGetUniformLocation(postfx_program, "uInputSize");

  GLint old_texture = 0, old_rb = 0, old_buffer = 0, old_fb = 0;
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_texture);
  glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rb);
  glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_buffer);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fb);
  glGenTextures(1, &postfx_texture);
  glBindTexture(GL_TEXTURE_2D, postfx_texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SCREEN_W, SCREEN_H, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenRenderbuffers(1, &postfx_depth);
  glBindRenderbuffer(GL_RENDERBUFFER, postfx_depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, SCREEN_W, SCREEN_H);
  glGenFramebuffers(1, &postfx_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, postfx_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                         GL_TEXTURE_2D, postfx_texture, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                            GL_RENDERBUFFER, postfx_depth);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
    fatal_error("Cannot create PostFX framebuffer. Try native resolution.");
  /* The shader pairs use ff3's top-left UV convention, independently of the
   * world compositor's quad. Keeping separate VAOs avoids changing FFP arrays. */
  static const GLfloat quad[] = {
    -1, 1, 0, 0, -1, -1, 0, 1, 1, 1, 1, 0, 1, -1, 1, 1};
  glGenVertexArrays(1, &postfx_vao);
  glBindVertexArray(postfx_vao);
  glGenBuffers(1, &postfx_vbo);
  glBindBuffer(GL_ARRAY_BUFFER, postfx_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0); glEnableVertexAttribArray(1);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), NULL);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat),
                        (const void *)(2 * sizeof(GLfloat)));
  glBindVertexArray(0);
  glBindBuffer(GL_ARRAY_BUFFER, old_buffer);
  glBindTexture(GL_TEXTURE_2D, old_texture);
  glBindRenderbuffer(GL_RENDERBUFFER, old_rb);
  glBindFramebuffer(GL_FRAMEBUFFER, old_fb);
  bound_texture_2d = UINT32_MAX;
#ifdef FF6_DEBUG
  trace("[postfx] linked effect=%d size=%dx%d program=%u\n",
        options.postfx, SCREEN_W, SCREEN_H, postfx_program);
#endif
}

static void begin_postfx(void) {
  if (postfx_fbo) glBindFramebuffer(GL_FRAMEBUFFER, postfx_fbo);
}

static void present_postfx(void) {
  if (!postfx_fbo) return;
  GLint active = GL_TEXTURE0, texture = 0, program = 0, viewport[4];
  glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
  glGetIntegerv(GL_CURRENT_PROGRAM, &program);
  glGetIntegerv(GL_VIEWPORT, viewport);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
  const GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_ALPHA_TEST,
                         GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST};
  GLboolean enabled[6], mask[4];
  glGetBooleanv(GL_COLOR_WRITEMASK, mask);
  for (int i = 0; i < 6; ++i) {
    enabled[i] = glIsEnabled(caps[i]); glDisable(caps[i]);
  }
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, SCREEN_W, SCREEN_H);
  glUseProgram(postfx_program);
  glBindVertexArray(postfx_vao);
  glBindTexture(GL_TEXTURE_2D, postfx_texture);
  if (postfx_time >= 0)
    glUniform1f(postfx_time, (float)(sceKernelGetSystemTimeWide() % 3600000000ULL) / 1000000.0f);
  if (postfx_size >= 0) glUniform2f(postfx_size, (float)SCREEN_W, (float)SCREEN_H);
  if (postfx_vertex_size >= 0) glUniform2f(postfx_vertex_size, (float)SCREEN_W, (float)SCREEN_H);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
  glUseProgram(program);
  glBindTexture(GL_TEXTURE_2D, texture);
  glActiveTexture(active);
  glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
  glColorMask(mask[0], mask[1], mask[2], mask[3]);
  for (int i = 0; i < 6; ++i) if (enabled[i]) glEnable(caps[i]);
  bound_texture_2d = UINT32_MAX;
}
#endif
