// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/extensions/extension_tts_api_platform.h"

#include <string>

std::string ExtensionTtsPlatformImpl::error() {
  return error_;
}

void ExtensionTtsPlatformImpl::clear_error() {
  error_ = std::string();
}

void ExtensionTtsPlatformImpl::set_error(const std::string& error) {
  error_ = error;
}
