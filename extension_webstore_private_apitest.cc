// Copyright (c) 2011 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/stringprintf.h"
#include "chrome/browser/extensions/extension_apitest.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/extensions/extension_webstore_private_api.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/test/ui_test_utils.h"
#include "content/common/notification_observer.h"
#include "content/common/notification_registrar.h"
#include "content/common/notification_service.h"
#include "net/base/mock_host_resolver.h"

// A base class for tests below.
class ExtensionWebstorePrivateApiTest : public ExtensionApiTest {
 public:
  void SetUpCommandLine(CommandLine* command_line) OVERRIDE {
    ExtensionApiTest::SetUpCommandLine(command_line);
    command_line->AppendSwitchASCII(switches::kAppsGalleryURL,
        "http://www.example.com");
  }

  void SetUpInProcessBrowserTestFixture() OVERRIDE {
    // Start up the test server and get us ready for calling the install
    // API functions.
    host_resolver()->AddRule("www.example.com", "127.0.0.1");
    ASSERT_TRUE(test_server()->Start());
    BeginInstallWithManifestFunction::SetIgnoreUserGestureForTests(true);
    BeginInstallWithManifestFunction::SetAutoConfirmForTests(true);
  }

 protected:
  // Returns a test server URL, but with host 'www.example.com' so it matches
  // the web store app's extent that we set up via command line flags.
  GURL GetTestServerURL(const std::string& path) {
    GURL url = test_server()->GetURL(
        std::string("files/extensions/api_test/webstore_private/") + path);

    // Replace the host with 'www.example.com' so it matches the web store
    // app's extent.
    GURL::Replacements replace_host;
    std::string host_str("www.example.com");
    replace_host.SetHostStr(host_str);

    return url.ReplaceComponents(replace_host);
  }

  // Navigates to |page| and runs the Extension API test there.
  bool RunInstallTest(const std::string& page) {
    GURL crx_url = GetTestServerURL("extension.crx");
    CommandLine::ForCurrentProcess()->AppendSwitchASCII(
        switches::kAppsGalleryUpdateURL, crx_url.spec());

    GURL page_url = GetTestServerURL(page);
    return RunPageTest(page_url.spec());
  }

  ExtensionService* service() {
    return browser()->profile()->GetExtensionService();
  }
};

// Tests several basic install cases.
IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest,
                       Install) {
  // Test cases where the user accepts the install confirmation dialog.
  ASSERT_TRUE(RunInstallTest("accepted.html"));

  // Now test the case where the user cancels the confirmation dialog.
  BeginInstallWithManifestFunction::SetAutoConfirmForTests(false);
  ASSERT_TRUE(RunInstallTest("cancelled.html"));

  BeginInstallFunction::SetIgnoreUserGestureForTests(false);
  ASSERT_TRUE(RunInstallTest("no_user_gesture.html"));
}

IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest,
                       IncorrectManifest1) {
  ui_test_utils::WindowedNotificationObserver observer(
      NotificationType::EXTENSION_INSTALL_ERROR,
      NotificationService::AllSources());
  ASSERT_TRUE(RunInstallTest("incorrect_manifest1.html"));
  observer.Wait();
}

IN_PROC_BROWSER_TEST_F(ExtensionWebstorePrivateApiTest,
                       IncorrectManifest2) {
  ui_test_utils::WindowedNotificationObserver observer(
      NotificationType::EXTENSION_INSTALL_ERROR,
      NotificationService::AllSources());
  ASSERT_TRUE(RunInstallTest("incorrect_manifest2.html"));
  observer.Wait();
}
