/* dialog.c -- common dialog for error messages and cheats input
 *
 * Copyright (C) 2021 fgsfds, Andy Nguyen
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <psp2/kernel/processmgr.h>
#include <psp2/ctrl.h>
#include <psp2/ime_dialog.h>
#include <psp2/message_dialog.h>
#include <vitaGL.h>

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "main.h"
#include "dialog.h"
#include "text_encoding.h"

static uint16_t ime_title_utf16[SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1];
static uint16_t ime_initial_text_utf16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static uint16_t ime_input_text_utf16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
/* One UTF-16 unit can expand to three UTF-8 bytes. */
static char ime_input_text_utf8[3 * SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];

int init_ime_dialog(const char *title, const char *initial_text, int max_length) {
  if (max_length <= 0 || max_length > SCE_IME_DIALOG_MAX_TEXT_LENGTH)
    max_length = SCE_IME_DIALOG_MAX_TEXT_LENGTH;

  memset(ime_title_utf16, 0, sizeof(ime_title_utf16));
  memset(ime_initial_text_utf16, 0, sizeof(ime_initial_text_utf16));
  memset(ime_input_text_utf16, 0, sizeof(ime_input_text_utf16));
  memset(ime_input_text_utf8, 0, sizeof(ime_input_text_utf8));

  ff6_utf8_to_utf16(ime_title_utf16,
                    sizeof(ime_title_utf16) / sizeof(ime_title_utf16[0]), title);
  ff6_utf8_to_utf16(ime_initial_text_utf16, (size_t)max_length + 1,
                    initial_text);

  SceImeDialogParam param;
  sceImeDialogParamInit(&param);

  param.supportedLanguages = 0x0001FFFF;
  param.languagesForced = SCE_TRUE;
  param.type = SCE_IME_TYPE_BASIC_LATIN;
  param.title = ime_title_utf16;
  param.maxTextLength = max_length;
  param.initialText = ime_initial_text_utf16;
  param.inputTextBuffer = ime_input_text_utf16;

  return sceImeDialogInit(&param);
}

char *get_ime_dialog_result(void) {
  if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
    return NULL;

  SceImeDialogResult result;
  memset(&result, 0, sizeof(SceImeDialogResult));
  sceImeDialogGetResult(&result);
  if (result.button == SCE_IME_DIALOG_BUTTON_ENTER)
    ff6_utf16_to_utf8(ime_input_text_utf8, sizeof(ime_input_text_utf8),
                      ime_input_text_utf16, SCE_IME_DIALOG_MAX_TEXT_LENGTH);
  sceImeDialogTerm();
  // For some reason analog stick stops working after ime
  sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);

  return ime_input_text_utf8;
}

int init_msg_dialog(const char *msg) {
  SceMsgDialogUserMessageParam msg_param;
  memset(&msg_param, 0, sizeof(msg_param));
  msg_param.buttonType = SCE_MSG_DIALOG_BUTTON_TYPE_OK;
  msg_param.msg = (SceChar8 *)msg;

  SceMsgDialogParam param;
  sceMsgDialogParamInit(&param);
  _sceCommonDialogSetMagicNumber(&param.commonParam);
  param.mode = SCE_MSG_DIALOG_MODE_USER_MSG;
  param.userMsgParam = &msg_param;

  return sceMsgDialogInit(&param);
}

int get_msg_dialog_result(void) {
  if (sceMsgDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
    return 0;
  sceMsgDialogTerm();
  return 1;
}

void fatal_error(const char *fmt, ...) {
  va_list list;
  char string[512];

  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);

  vglInit(0);

  init_msg_dialog(string);

  while (!get_msg_dialog_result())
    vglSwapBuffers(GL_TRUE);

  sceKernelExitProcess(0);
  while (1);
}
