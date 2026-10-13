// SPDX-License-Identifier: GPL-2.0-or-later
#include "runtime/qt_app.h"

#include <windows.h>

#include <cassert>
#include <format>
#include <memory>
#include <string>
#include <type_traits>

#include "app_info.h"

namespace knobs::runtime {
namespace {

// Qt's own name for its core library, the same in every Qt 6.
constexpr wchar_t kQtCore[] = L"Qt6Core.dll";

// QEvent::DeferredDelete, what QObject::deleteLater posts.
constexpr int kDeferredDelete = 52;

// QCoreApplication::ApplicationFlags, which Qt 6 ignores.
constexpr int kApplicationFlags = 0x060000;

// More than sizeof(QCoreApplication), 16 bytes in Qt 6 on x64.
constexpr size_t kObjectSize = 64;

// QCoreApplication keeps a reference to argc and the argv array for as long
// as it lives.
int g_argc = 1;
char g_arg0[] = KNOBS_DISPLAY_NAME;
char* g_argv[] = {g_arg0, nullptr};

}  // namespace

// QCoreApplication's members knobs calls, by their MSVC names. A member
// function takes `this` as its first argument on x64.
struct QtApp::Exports {
  // QCoreApplication::QCoreApplication(int &argc, char **argv, int flags)
  void (*construct)(void* self, int& argc, char** argv, int flags) = nullptr;
  // QCoreApplication::~QCoreApplication()
  void (*destruct)(void* self) = nullptr;
  // static bool QCoreApplication::instanceExists()
  bool (*instance_exists)() = nullptr;
  // static void QCoreApplication::sendPostedEvents(QObject *receiver, int event_type)
  void (*send_posted_events)(void* receiver, int event_type) = nullptr;
  unsigned long thread_id = 0;
};

Result<std::unique_ptr<QtApp>> QtApp::Start() {
  const HMODULE qt = GetModuleHandleW(kQtCore);
  if (!qt) return Error{"Qt6Core.dll isn't loaded."};
  auto exports = std::make_unique<Exports>();
  std::string missing;
  const auto resolve = [&](auto& function, const char* name) {
    function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(GetProcAddress(qt, name));
    if (!function) missing += std::format("{}{}", missing.empty() ? "" : ", ", name);
  };
  resolve(exports->construct, "??0QCoreApplication@@QEAA@AEAHPEAPEADH@Z");
  resolve(exports->destruct, "??1QCoreApplication@@UEAA@XZ");
  resolve(exports->instance_exists, "?instanceExists@QCoreApplication@@SA_NXZ");
  resolve(exports->send_posted_events, "?sendPostedEvents@QCoreApplication@@SAXPEAVQObject@@H@Z");
  if (!missing.empty()) return Error{std::format("Qt6Core.dll lacks functions {} needs: {}.", kDisplayName, missing)};
  if (exports->instance_exists()) return Error{"Something in this process already made a Qt application."};

  std::unique_ptr<QtApp> app(new QtApp());
  exports->thread_id = GetCurrentThreadId();
  app->exports_ = std::move(exports);
  // new[] aligns it for any type.
  app->object_ = std::make_unique<unsigned char[]>(kObjectSize);
  app->exports_->construct(app->object_.get(), g_argc, g_argv, kApplicationFlags);
  app->constructed_ = true;
  return app;
}

QtApp::~QtApp() {
  if (!constructed_) return;
  assert(GetCurrentThreadId() == exports_->thread_id && "QtApp must be destroyed on the thread that made it");
  DeliverPostedEvents();
  exports_->destruct(object_.get());
}

void QtApp::DeliverPostedEvents() {
  assert(GetCurrentThreadId() == exports_->thread_id && "Qt delivers events on the thread that posted them to");
  // Everything first, such as the editor obs-vst asks to close, then the
  // deletes, which Qt only delivers outside an event loop when asked for
  // them by type (QCoreApplicationPrivate::sendPostedEvents).
  exports_->send_posted_events(nullptr, 0);
  exports_->send_posted_events(nullptr, kDeferredDelete);
}

}  // namespace knobs::runtime
