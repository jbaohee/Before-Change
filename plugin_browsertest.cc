// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/command_line.h"
#include "base/path_service.h"
#include "base/utf_string_conversions.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/common/content_switches.h"
#include "content/test/net/url_request_mock_http_job.h"

using content::BrowserThread;

namespace {

void SetUrlRequestMock(const FilePath& path) {
  URLRequestMockHTTPJob::AddUrlHandler(path);
}

}

class PluginTest : public InProcessBrowserTest {
 protected:
  PluginTest() {}

  virtual void SetUpCommandLine(CommandLine* command_line) {
    // Some NPAPI tests schedule garbage collection to force object tear-down.
    command_line->AppendSwitchASCII(switches::kJavaScriptFlags, "--expose_gc");
    // For OpenPopupWindowWithPlugin.
    command_line->AppendSwitch(switches::kDisablePopupBlocking);
#if defined(OS_MACOSX)
    FilePath plugin_dir;
    PathService::Get(base::DIR_MODULE, &plugin_dir);
    plugin_dir = plugin_dir.AppendASCII("plugins");
    // The plugins directory isn't read by default on the Mac, so it needs to be
    // explicitly registered.
    command_line->AppendSwitchPath(switches::kExtraPluginDir, plugin_dir);
#endif
  }

  virtual void SetUpOnMainThread() OVERRIDE {
    FilePath path = ui_test_utils::GetTestFilePath(FilePath(), FilePath());
    BrowserThread::PostTask(
        BrowserThread::IO, FROM_HERE, base::Bind(&SetUrlRequestMock, path));
  }

  void LoadAndWait(const GURL& url, const char* title) {
    string16 expected_title(ASCIIToUTF16(title));
    ui_test_utils::TitleWatcher title_watcher(
        browser()->GetSelectedWebContents(), expected_title);
    title_watcher.AlsoWaitForTitle(ASCIIToUTF16("FAIL"));
    ui_test_utils::NavigateToURL(browser(), url);
    EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());
  }

  GURL GetURL(const char* filename) {
    return ui_test_utils::GetTestUrl(
      FilePath().AppendASCII("npapi"), FilePath().AppendASCII(filename));
  }

  void NavigateAway() {
    GURL url = ui_test_utils::GetTestUrl(
      FilePath(), FilePath().AppendASCII("simple.html"));
    LoadAndWait(url, "simple.html");
  }
};

// Make sure that navigating away from a plugin referenced by JS doesn't
// crash.
IN_PROC_BROWSER_TEST_F(PluginTest, UnloadNoCrash) {
  LoadAndWait(GetURL("layout_test_plugin.html"), "Layout Test Plugin Test");
  NavigateAway();
}

// Tests if a plugin executing a self deleting script using NPN_GetURL
// works without crashing or hanging
// Flaky: http://crbug.com/59327
IN_PROC_BROWSER_TEST_F(PluginTest, SelfDeletePluginGetUrl) {
  LoadAndWait(GetURL("self_delete_plugin_geturl.html"), "OK");
}

// Tests if a plugin executing a self deleting script using Invoke
// works without crashing or hanging
// Flaky. See http://crbug.com/30702
IN_PROC_BROWSER_TEST_F(PluginTest, SelfDeletePluginInvoke) {
  LoadAndWait(GetURL("self_delete_plugin_invoke.html"), "OK");
}

IN_PROC_BROWSER_TEST_F(PluginTest, NPObjectReleasedOnDestruction) {
  ui_test_utils::NavigateToURL(
      browser(), GetURL("npobject_released_on_destruction.html"));
  NavigateAway();
}

// Test that a dialog is properly created when a plugin throws an
// exception.  Should be run for in and out of process plugins, but
// the more interesting case is out of process, where we must route
// the exception to the correct renderer.
IN_PROC_BROWSER_TEST_F(PluginTest, NPObjectSetException) {
  LoadAndWait(GetURL("npobject_set_exception.html"), "OK");
}

#if defined(OS_WIN)
// Tests if a plugin executing a self deleting script in the context of
// a synchronous mouseup works correctly.
// This was never ported to Mac. The only thing remaining is to make
// ui_test_utils::SimulateMouseClick get to Mac plugins, currently it doesn't
// work.
IN_PROC_BROWSER_TEST_F(PluginTest,
                       SelfDeletePluginInvokeInSynchronousMouseUp) {
  ui_test_utils::NavigateToURL(
      browser(), GetURL("execute_script_delete_in_mouse_up.html"));

  string16 expected_title(ASCIIToUTF16("OK"));
  ui_test_utils::TitleWatcher title_watcher(
      browser()->GetSelectedWebContents(), expected_title);
  title_watcher.AlsoWaitForTitle(ASCIIToUTF16("FAIL"));
  ui_test_utils::SimulateMouseClick(browser()->GetSelectedWebContents());
  EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());
}
#endif

// Flaky, http://crbug.com/60071.
IN_PROC_BROWSER_TEST_F(PluginTest, GetURLRequest404Response) {
  GURL url(URLRequestMockHTTPJob::GetMockUrl(
      FilePath().AppendASCII("npapi").
                 AppendASCII("plugin_url_request_404.html")));
  LoadAndWait(url, "OK");
}

// Tests if a plugin executing a self deleting script using Invoke with
// a modal dialog showing works without crashing or hanging
// Disabled, flakily exceeds timeout, http://crbug.com/46257.
IN_PROC_BROWSER_TEST_F(PluginTest, SelfDeletePluginInvokeAlert) {
  // Navigate asynchronously because if we waitd until it completes, there's a
  // race condition where the alert can come up before we start watching for it.
  ui_test_utils::NavigateToURLWithDisposition(
      browser(), GetURL("self_delete_plugin_invoke_alert.html"), CURRENT_TAB,
      0);

  string16 expected_title(ASCIIToUTF16("OK"));
  ui_test_utils::TitleWatcher title_watcher(
      browser()->GetSelectedWebContents(), expected_title);
  title_watcher.AlsoWaitForTitle(ASCIIToUTF16("FAIL"));

  ui_test_utils::WaitForAppModalDialogAndCloseIt();

  EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());
}

// Test passing arguments to a plugin.
IN_PROC_BROWSER_TEST_F(PluginTest, Arguments) {
  LoadAndWait(GetURL("arguments.html"), "OK");
}

// Test invoking many plugins within a single page.
IN_PROC_BROWSER_TEST_F(PluginTest, ManyPlugins) {
  LoadAndWait(GetURL("many_plugins.html"), "OK");
}

// Test various calls to GetURL from a plugin.
IN_PROC_BROWSER_TEST_F(PluginTest, GetURL) {
  LoadAndWait(GetURL("geturl.html"), "OK");
}

// Test various calls to GetURL for javascript URLs with
// non NULL targets from a plugin.
IN_PROC_BROWSER_TEST_F(PluginTest, GetJavaScriptURL) {
  LoadAndWait(GetURL("get_javascript_url.html"), "OK");
}

// Test that calling GetURL with a javascript URL and target=_self
// works properly when the plugin is embedded in a subframe.
IN_PROC_BROWSER_TEST_F(PluginTest, GetJavaScriptURL2) {
  LoadAndWait(GetURL("get_javascript_url2.html"), "OK");
}

// Test is flaky on linux/cros/win builders.  http://crbug.com/71904
IN_PROC_BROWSER_TEST_F(PluginTest, GetURLRedirectNotification) {
  LoadAndWait(GetURL("geturl_redirect_notify.html"), "OK");
}

// Tests that identity is preserved for NPObjects passed from a plugin
// into JavaScript.
IN_PROC_BROWSER_TEST_F(PluginTest, NPObjectIdentity) {
  LoadAndWait(GetURL("npobject_identity.html"), "OK");
}

// Tests that if an NPObject is proxies back to its original process, the
// original pointer is returned and not a proxy.  If this fails the plugin
// will crash.
IN_PROC_BROWSER_TEST_F(PluginTest, NPObjectProxy) {
  LoadAndWait(GetURL("npobject_proxy.html"), "OK");
}

#if defined(OS_WIN) || defined(OS_MACOSX)
// Tests if a plugin executing a self deleting script in the context of
// a synchronous paint event works correctly
// http://crbug.com/44960
IN_PROC_BROWSER_TEST_F(PluginTest, SelfDeletePluginInvokeInSynchronousPaint) {
  LoadAndWait(GetURL("execute_script_delete_in_paint.html"), "OK");
}
#endif

IN_PROC_BROWSER_TEST_F(PluginTest, SelfDeletePluginInNewStream) {
  LoadAndWait(GetURL("self_delete_plugin_stream.html"), "OK");
}

// This test asserts on Mac in plugin_host in the NPNVWindowNPObject case.
#if !(defined(OS_MACOSX) && !defined(NDEBUG))
// If this test flakes use http://crbug.com/95558.
IN_PROC_BROWSER_TEST_F(PluginTest, DeletePluginInDeallocate) {
  LoadAndWait(GetURL("plugin_delete_in_deallocate.html"), "OK");
}
#endif

#if defined(OS_WIN)

IN_PROC_BROWSER_TEST_F(PluginTest, VerifyPluginWindowRect) {
  LoadAndWait(GetURL("verify_plugin_window_rect.html"), "OK");
}

// Tests that creating a new instance of a plugin while another one is handling
// a paint message doesn't cause deadlock.
IN_PROC_BROWSER_TEST_F(PluginTest, CreateInstanceInPaint) {
  LoadAndWait(GetURL("create_instance_in_paint.html"), "OK");
}

// Tests that putting up an alert in response to a paint doesn't deadlock.
IN_PROC_BROWSER_TEST_F(PluginTest, AlertInWindowMessage) {
  ui_test_utils::NavigateToURL(
      browser(), GetURL("alert_in_window_message.html"));

  ui_test_utils::WaitForAppModalDialogAndCloseIt();
  ui_test_utils::WaitForAppModalDialogAndCloseIt();
}

IN_PROC_BROWSER_TEST_F(PluginTest, VerifyNPObjectLifetimeTest) {
  LoadAndWait(GetURL("npobject_lifetime_test.html"), "OK");
}

// Tests that we don't crash or assert if NPP_New fails
IN_PROC_BROWSER_TEST_F(PluginTest, NewFails) {
  LoadAndWait(GetURL("new_fails.html"), "OK");
}

IN_PROC_BROWSER_TEST_F(PluginTest, SelfDeletePluginInNPNEvaluate) {
  LoadAndWait(GetURL("execute_script_delete_in_npn_evaluate.html"), "OK");
}

IN_PROC_BROWSER_TEST_F(PluginTest, SelfDeleteCreatePluginInNPNEvaluate) {
  LoadAndWait(GetURL("npn_plugin_delete_create_in_evaluate.html"), "OK");
}

#endif  // OS_WIN

// If this flakes, reopen http://crbug.com/17645
// As of 6 July 2011, this test is flaky on Windows (perhaps due to timing out).
#if !defined(OS_MACOSX)
// Disabled on Mac because the plugin side isn't implemented yet, see
// "TODO(port)" in plugin_javascript_open_popup.cc.
IN_PROC_BROWSER_TEST_F(PluginTest, OpenPopupWindowWithPlugin) {
  LoadAndWait(GetURL("get_javascript_open_popup_with_plugin.html"), "OK");
}
#endif

// Test checking the privacy mode is off.
IN_PROC_BROWSER_TEST_F(PluginTest, PrivateDisabled) {
  LoadAndWait(GetURL("private.html"), "OK");
}

IN_PROC_BROWSER_TEST_F(PluginTest, ScheduleTimer) {
  LoadAndWait(GetURL("schedule_timer.html"), "OK");
}

IN_PROC_BROWSER_TEST_F(PluginTest, PluginThreadAsyncCall) {
  LoadAndWait(GetURL("plugin_thread_async_call.html"), "OK");
}

// Test checking the privacy mode is on.
// If this flakes on Linux, use http://crbug.com/104380
IN_PROC_BROWSER_TEST_F(PluginTest, PrivateEnabled) {
  LoadAndWait(GetURL("private.html"), "OK");
}

#if defined(OS_WIN) || defined(OS_MACOSX)
// Test a browser hang due to special case of multiple
// plugin instances indulged in sync calls across renderer.
IN_PROC_BROWSER_TEST_F(PluginTest, MultipleInstancesSyncCalls) {
  LoadAndWait(GetURL("multiple_instances_sync_calls.html"), "OK");
}
#endif

IN_PROC_BROWSER_TEST_F(PluginTest, GetURLRequestFailWrite) {
  GURL url(URLRequestMockHTTPJob::GetMockUrl(
      FilePath().AppendASCII("npapi").
                 AppendASCII("plugin_url_request_fail_write.html")));
  LoadAndWait(url, "OK");
}

#if defined(OS_WIN)
IN_PROC_BROWSER_TEST_F(PluginTest, EnsureScriptingWorksInDestroy) {
  LoadAndWait(GetURL("ensure_scripting_works_in_destroy.html"), "OK");
}

// This test uses a Windows Event to signal to the plugin that it should crash
// on NP_Initialize.
IN_PROC_BROWSER_TEST_F(PluginTest, NoHangIfInitCrashes) {
  HANDLE crash_event = CreateEvent(NULL, TRUE, FALSE, L"TestPluginCrashOnInit");
  SetEvent(crash_event);
  LoadAndWait(GetURL("no_hang_if_init_crashes.html"), "OK");
  CloseHandle(crash_event);
}
#endif

// If this flakes on Mac, use http://crbug.com/111508
IN_PROC_BROWSER_TEST_F(PluginTest, PluginReferrerTest) {
  GURL url(URLRequestMockHTTPJob::GetMockUrl(
      FilePath().AppendASCII("npapi").
                 AppendASCII("plugin_url_request_referrer_test.html")));
  LoadAndWait(url, "OK");
}

#if defined(OS_MACOSX)
IN_PROC_BROWSER_TEST_F(PluginTest, PluginConvertPointTest) {
  gfx::NativeWindow window = NULL;
  gfx::Rect bounds(50, 50, 400, 400);
  ui_test_utils::GetNativeWindow(browser(), &window);
  ui_test_utils::SetWindowBounds(window, bounds);

  ui_test_utils::NavigateToURL(browser(), GetURL("convert_point.html"));

  string16 expected_title(ASCIIToUTF16("OK"));
  ui_test_utils::TitleWatcher title_watcher(
      browser()->GetSelectedWebContents(), expected_title);
  title_watcher.AlsoWaitForTitle(ASCIIToUTF16("FAIL"));
  // TODO(stuartmorgan): When the automation system supports sending clicks,
  // change the test to trigger on mouse-down rather than window focus.
  static_cast<content::WebContentsDelegate*>(browser())->
      ActivateContents(browser()->GetSelectedWebContents());
  EXPECT_EQ(expected_title, title_watcher.WaitAndGetTitle());
}
#endif
