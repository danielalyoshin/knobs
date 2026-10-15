// SPDX-License-Identifier: GPL-2.0-or-later
#include "tray/message_dialog.h"

#include <commctrl.h>

#include "app_info.h"
#include "tray/resource.h"
#include "tray/rich_text.h"

namespace knobs::tray {

HICON LoadAppIcon(HINSTANCE instance, int size, bool on_dark) {
  HICON icon = nullptr;
  if (FAILED(LoadIconMetric(instance, MAKEINTRESOURCEW(on_dark ? IDI_KNOBS_ON_DARK : IDI_KNOBS), size, &icon))) {
    return nullptr;
  }
  return icon;
}

void ShowMessageDialog(HINSTANCE instance, const MessageDialog& dialog) {
  const std::wstring title = dialog.title.empty() ? std::wstring(kDisplayNameW) : dialog.title;
  TASKDIALOGCONFIG config = {sizeof(config)};
  config.hwndParent = dialog.owner;
  config.hInstance = instance;
  config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION;
  if (dialog.owner) config.dwFlags |= TDF_POSITION_RELATIVE_TO_WINDOW;
  config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
  config.pszWindowTitle = title.c_str();
  if (dialog.custom_icon) {
    config.dwFlags |= TDF_USE_HICON_MAIN;
    config.hMainIcon = dialog.custom_icon;
  } else {
    config.pszMainIcon = dialog.icon;
  }
  const std::wstring instruction = CurlQuotes(dialog.instruction);
  const std::wstring content = CurlQuotes(dialog.content);
  const std::wstring footer = CurlQuotes(dialog.footer);
  if (!instruction.empty()) config.pszMainInstruction = instruction.c_str();
  if (!content.empty()) config.pszContent = content.c_str();
  if (!footer.empty()) config.pszFooter = footer.c_str();
  TaskDialogIndirect(&config, nullptr, nullptr, nullptr);
}

}  // namespace knobs::tray
