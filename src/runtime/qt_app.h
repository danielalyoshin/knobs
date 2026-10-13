// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>

#include "util/result.h"

namespace knobs::runtime {

// A QCoreApplication, made through the exports of the Qt6Core.dll that
// obs-vst loaded from the runtime copy, so knobs needs no Qt of its own.
//
// obs-vst's filter is a QObject made on the thread that loads the source, and
// it's destroyed on libobs's destruction thread, which can only ask that
// thread to delete it (vst_destroy, obs-vst.cpp: QObject::deleteLater). Qt
// delivers that request, and with it the plugin's effClose and the libobs
// memory it holds, only while a QCoreApplication exists
// (QCoreApplication::notifyInternal2). Without one, every chain knobs let go
// of would leak its VST plugins.
//
// Made and destroyed on the libobs session's thread, which becomes Qt's main
// thread. Nothing in knobs runs a Qt event loop, so DeliverPostedEvents
// delivers what obs-vst posted.
class QtApp {
 public:
  // Fails, saying why, if Qt6Core.dll isn't loaded, lacks an export knobs
  // uses, or already has an application.
  static Result<std::unique_ptr<QtApp>> Start();
  ~QtApp();
  QtApp(const QtApp&) = delete;
  QtApp& operator=(const QtApp&) = delete;

  // Delivers the events posted to this thread's objects, deferred deletes
  // included, as the end of QCoreApplication::exec() does. Call it on the
  // thread that called Start, once libobs's destroy queue has drained.
  void DeliverPostedEvents();

 private:
  struct Exports;
  QtApp() = default;

  std::unique_ptr<Exports> exports_;
  // The QCoreApplication, which in Qt 6 is a QObject's vtable and d-pointer,
  // with room to spare.
  std::unique_ptr<unsigned char[]> object_;
  bool constructed_ = false;
};

}  // namespace knobs::runtime
