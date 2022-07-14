// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "content/browser/web_contents/web_contents_impl.h"

#include <stddef.h>

#include <cmath>
#include <utility>
#include <vector>

#include "base/command_line.h"
#include "base/debug/dump_without_crashing.h"
#include "base/feature_list.h"
#include "base/i18n/character_encoding.h"
#include "base/lazy_instance.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/memory/ptr_util.h"
#include "base/memory/ref_counted.h"
#include "base/metrics/histogram_macros.h"
#include "base/metrics/user_metrics.h"
#include "base/process/process.h"
#include "base/profiler/scoped_tracker.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/string16.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/utf_string_conversions.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "base/trace_event/trace_event.h"
#include "build/build_config.h"
#include "components/mime_util/mime_util.h"
#include "components/rappor/public/rappor_utils.h"
#include "components/ukm/public/ukm_recorder.h"
#include "components/url_formatter/url_formatter.h"
#include "content/browser/accessibility/browser_accessibility_state_impl.h"
#include "content/browser/bad_message.h"
#include "content/browser/browser_plugin/browser_plugin_embedder.h"
#include "content/browser/browser_plugin/browser_plugin_guest.h"
#include "content/browser/child_process_security_policy_impl.h"
#include "content/browser/devtools/render_frame_devtools_agent_host.h"
#include "content/browser/dom_storage/dom_storage_context_wrapper.h"
#include "content/browser/dom_storage/session_storage_namespace_impl.h"
#include "content/browser/download/download_stats.h"
#include "content/browser/download/mhtml_generation_manager.h"
#include "content/browser/download/save_package.h"
#include "content/browser/find_request_manager.h"
#include "content/browser/frame_host/cross_process_frame_connector.h"
#include "content/browser/frame_host/frame_tree_node.h"
#include "content/browser/frame_host/interstitial_page_impl.h"
#include "content/browser/frame_host/navigation_entry_impl.h"
#include "content/browser/frame_host/navigation_handle_impl.h"
#include "content/browser/frame_host/navigator_impl.h"
#include "content/browser/frame_host/render_frame_host_impl.h"
#include "content/browser/frame_host/render_frame_proxy_host.h"
#include "content/browser/frame_host/render_widget_host_view_child_frame.h"
#include "content/browser/loader/loader_io_thread_notifier.h"
#include "content/browser/loader/resource_dispatcher_host_impl.h"
#include "content/browser/manifest/manifest_manager_host.h"
#include "content/browser/media/audio_stream_monitor.h"
#include "content/browser/media/capture/web_contents_audio_muter.h"
#include "content/browser/media/media_web_contents_observer.h"
#include "content/browser/media/session/media_session_impl.h"
#include "content/browser/plugin_content_origin_whitelist.h"
#include "content/browser/renderer_host/render_process_host_impl.h"
#include "content/browser/renderer_host/render_view_host_delegate_view.h"
#include "content/browser/renderer_host/render_view_host_impl.h"
#include "content/browser/renderer_host/render_widget_host_impl.h"
#include "content/browser/renderer_host/render_widget_host_input_event_router.h"
#include "content/browser/renderer_host/render_widget_host_view_base.h"
#include "content/browser/renderer_host/text_input_manager.h"
#include "content/browser/screen_orientation/screen_orientation_provider.h"
#include "content/browser/site_instance_impl.h"
#include "content/browser/web_contents/web_contents_view_child_frame.h"
#include "content/browser/web_contents/web_contents_view_guest.h"
#include "content/browser/webui/generic_handler.h"
#include "content/browser/webui/web_ui_controller_factory_registry.h"
#include "content/browser/webui/web_ui_impl.h"
#include "content/common/browser_plugin/browser_plugin_constants.h"
#include "content/common/browser_plugin/browser_plugin_messages.h"
#include "content/common/drag_messages.h"
#include "content/common/frame_messages.h"
#include "content/common/input_messages.h"
#include "content/common/page_messages.h"
#include "content/common/page_state_serialization.h"
#include "content/common/render_message_filter.mojom.h"
#include "content/common/site_isolation_policy.h"
#include "content/common/view_messages.h"
#include "content/public/browser/ax_event_notification_details.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_plugin_guest_manager.h"
#include "content/public/browser/content_browser_client.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/download_url_parameters.h"
#include "content/public/browser/focused_node_details.h"
#include "content/public/browser/guest_mode.h"
#include "content/public/browser/invalidate_type.h"
#include "content/public/browser/javascript_dialog_manager.h"
#include "content/public/browser/keyboard_event_processing_result.h"
#include "content/public/browser/load_notification_details.h"
#include "content/public/browser/navigation_details.h"
#include "content/public/browser/notification_details.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/notification_types.h"
#include "content/public/browser/render_widget_host_iterator.h"
#include "content/public/browser/resource_request_details.h"
#include "content/public/browser/security_style_explanations.h"
#include "content/public/browser/ssl_status.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents_binding_set.h"
#include "content/public/browser/web_contents_delegate.h"
#include "content/public/browser/web_contents_unresponsive_state.h"
#include "content/public/common/bindings_policy.h"
#include "content/public/common/browser_side_navigation_policy.h"
#include "content/public/common/child_process_host.h"
#include "content/public/common/content_constants.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/page_zoom.h"
#include "content/public/common/result_codes.h"
#include "content/public/common/url_utils.h"
#include "content/public/common/web_preferences.h"
#include "device/geolocation/geolocation_service_context.h"
#include "net/base/url_util.h"
#include "net/http/http_cache.h"
#include "net/http/http_transaction_factory.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "net/url_request/url_request_context.h"
#include "net/url_request/url_request_context_getter.h"
#include "ppapi/features/features.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#include "third_party/WebKit/public/platform/WebSecurityStyle.h"
#include "third_party/WebKit/public/web/WebSandboxFlags.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/accessibility/ax_tree_combiner.h"
#include "ui/base/layout.h"
#include "ui/events/blink/web_input_event_traits.h"
#include "ui/gl/gl_switches.h"

#if defined(OS_WIN)
#include "content/browser/renderer_host/dip_util.h"
#include "ui/gfx/geometry/dip_util.h"
#endif

#if defined(OS_ANDROID)
#include "content/browser/android/content_video_view.h"
#include "content/browser/android/date_time_chooser_android.h"
#include "content/browser/android/java_interfaces_impl.h"
#include "content/browser/media/android/media_web_contents_observer_android.h"
#include "content/browser/web_contents/web_contents_android.h"
#include "services/device/public/interfaces/nfc.mojom.h"
#else  // !OS_ANDROID
#include "content/browser/host_zoom_map_impl.h"
#include "content/browser/host_zoom_map_observer.h"
#endif  // OS_ANDROID

#if defined(OS_MACOSX)
#include "base/mac/foundation_util.h"
#endif

#if defined(USE_AURA)
#include "content/public/common/service_manager_connection.h"
#endif

#if BUILDFLAG(ENABLE_PLUGINS)
#include "content/browser/media/session/pepper_playback_observer.h"
#endif  // ENABLE_PLUGINS

namespace content {
namespace {

const int kMinimumDelayBetweenLoadingUpdatesMS = 100;
const char kDotGoogleDotCom[] = ".google.com";

#if defined(OS_ANDROID)
const void* const kWebContentsAndroidKey = &kWebContentsAndroidKey;
#endif  // OS_ANDROID

base::LazyInstance<std::vector<WebContentsImpl::CreatedCallback>>::
    DestructorAtExit g_created_callbacks = LAZY_INSTANCE_INITIALIZER;

void NotifyCacheOnIO(
    scoped_refptr<net::URLRequestContextGetter> request_context,
    const GURL& url,
    const std::string& http_method) {
  net::HttpCache* cache = request_context->GetURLRequestContext()->
      http_transaction_factory()->GetCache();
  if (cache)
    cache->OnExternalCacheHit(url, http_method);
}

bool HasMatchingProcess(FrameTree* tree, int render_process_id) {
  for (FrameTreeNode* node : tree->Nodes()) {
    if (node->current_frame_host()->GetProcess()->GetID() == render_process_id)
      return true;
  }
  return false;
}

bool HasMatchingWidgetHost(FrameTree* tree, RenderWidgetHost* host) {
  // This method scans the frame tree rather than checking whether
  // host->delegate() == this, which allows it to return false when the host
  // for a frame that is pending or pending deletion.
  if (!host)
    return false;

  for (FrameTreeNode* node : tree->Nodes()) {
    if (node->current_frame_host()->GetRenderWidgetHost() == host)
      return true;
  }
  return false;
}

void UpdateAccessibilityModeOnFrame(RenderFrameHost* frame_host) {
  static_cast<RenderFrameHostImpl*>(frame_host)->UpdateAccessibilityMode();
}

void ResetAccessibility(RenderFrameHost* rfh) {
  static_cast<RenderFrameHostImpl*>(rfh)->AccessibilityReset();
}

using AXTreeSnapshotCallback =
      base::Callback<void(const ui::AXTreeUpdate&)>;

// Helper class used by WebContentsImpl::RequestAXTreeSnapshot.
// Handles the callbacks from parallel snapshot requests to each frame,
// and feeds the results to an AXTreeCombiner, which converts them into a
// single combined accessibility tree.
class AXTreeSnapshotCombiner : public base::RefCounted<AXTreeSnapshotCombiner> {
 public:
  explicit AXTreeSnapshotCombiner(AXTreeSnapshotCallback callback)
      : callback_(callback) {
  }

  AXTreeSnapshotCallback AddFrame(bool is_root) {
    // Adds a reference to |this|.
    return base::Bind(&AXTreeSnapshotCombiner::ReceiveSnapshot,
                      this,
                      is_root);
  }

  void ReceiveSnapshot(bool is_root, const ui::AXTreeUpdate& snapshot) {
    combiner_.AddTree(snapshot, is_root);
  }

 private:
  friend class base::RefCounted<AXTreeSnapshotCombiner>;

  // This is called automatically after the last call to ReceiveSnapshot
  // when there are no more references to this object.
  ~AXTreeSnapshotCombiner() {
    combiner_.Combine();
    callback_.Run(combiner_.combined());
  }

  ui::AXTreeCombiner combiner_;
  AXTreeSnapshotCallback callback_;
};

// Helper for GetInnerWebContents().
bool GetInnerWebContentsHelper(
    std::vector<WebContentsImpl*>* all_guest_contents,
    WebContents* guest_contents) {
  all_guest_contents->push_back(static_cast<WebContentsImpl*>(guest_contents));
  return false;
}

FrameTreeNode* FindOpener(const WebContents::CreateParams& params) {
  FrameTreeNode* opener_node = nullptr;
  if (params.opener_render_frame_id != MSG_ROUTING_NONE) {
    RenderFrameHostImpl* opener_rfh = RenderFrameHostImpl::FromID(
        params.opener_render_process_id, params.opener_render_frame_id);
    if (opener_rfh)
      opener_node = opener_rfh->frame_tree_node();
  }
  return opener_node;
}

}  // namespace

WebContents* WebContents::Create(const WebContents::CreateParams& params) {
  return WebContentsImpl::CreateWithOpener(params, FindOpener(params));
}

WebContents* WebContents::CreateWithSessionStorage(
    const WebContents::CreateParams& params,
    const SessionStorageNamespaceMap& session_storage_namespace_map) {
  WebContentsImpl* new_contents = new WebContentsImpl(params.browser_context);
  new_contents->SetOpenerForNewContents(FindOpener(params),
                                        params.opener_suppressed);

  for (SessionStorageNamespaceMap::const_iterator it =
           session_storage_namespace_map.begin();
       it != session_storage_namespace_map.end();
       ++it) {
    new_contents->GetController()
        .SetSessionStorageNamespace(it->first, it->second.get());
  }

  new_contents->Init(params);
  return new_contents;
}

void WebContentsImpl::FriendWrapper::AddCreatedCallbackForTesting(
    const CreatedCallback& callback) {
  g_created_callbacks.Get().push_back(callback);
}

void WebContentsImpl::FriendWrapper::RemoveCreatedCallbackForTesting(
    const CreatedCallback& callback) {
  for (size_t i = 0; i < g_created_callbacks.Get().size(); ++i) {
    if (g_created_callbacks.Get().at(i).Equals(callback)) {
      g_created_callbacks.Get().erase(g_created_callbacks.Get().begin() + i);
      return;
    }
  }
}

WebContents* WebContents::FromRenderViewHost(RenderViewHost* rvh) {
  if (!rvh)
    return nullptr;
  return rvh->GetDelegate()->GetAsWebContents();
}

WebContents* WebContents::FromRenderFrameHost(RenderFrameHost* rfh) {
  if (!rfh)
    return nullptr;
  return static_cast<RenderFrameHostImpl*>(rfh)->delegate()->GetAsWebContents();
}

WebContents* WebContents::FromFrameTreeNodeId(int frame_tree_node_id) {
  FrameTreeNode* frame_tree_node =
      FrameTreeNode::GloballyFindByID(frame_tree_node_id);
  if (!frame_tree_node)
    return nullptr;
  return WebContentsImpl::FromFrameTreeNode(frame_tree_node);
}

void WebContents::SetScreenOrientationDelegate(
    ScreenOrientationDelegate* delegate) {
  ScreenOrientationProvider::SetDelegate(delegate);
}

// WebContentsImpl::DestructionObserver ----------------------------------------

class WebContentsImpl::DestructionObserver : public WebContentsObserver {
 public:
  DestructionObserver(WebContentsImpl* owner, WebContents* watched_contents)
      : WebContentsObserver(watched_contents),
        owner_(owner) {
  }

  // WebContentsObserver:
  void WebContentsDestroyed() override {
    owner_->OnWebContentsDestroyed(
        static_cast<WebContentsImpl*>(web_contents()));
  }

 private:
  WebContentsImpl* owner_;

  DISALLOW_COPY_AND_ASSIGN(DestructionObserver);
};

WebContentsImpl::ColorChooserInfo::ColorChooserInfo(int render_process_id,
                                                    int render_frame_id,
                                                    ColorChooser* chooser,
                                                    int identifier)
    : render_process_id(render_process_id),
      render_frame_id(render_frame_id),
      chooser(chooser),
      identifier(identifier) {
}

WebContentsImpl::ColorChooserInfo::~ColorChooserInfo() {
}

bool WebContentsImpl::ColorChooserInfo::Matches(
    RenderFrameHostImpl* render_frame_host,
    int color_chooser_id) {
  return this->render_process_id == render_frame_host->GetProcess()->GetID() &&
         this->render_frame_id == render_frame_host->GetRoutingID() &&
         this->identifier == color_chooser_id;
}

// WebContentsImpl::WebContentsTreeNode ----------------------------------------
WebContentsImpl::WebContentsTreeNode::WebContentsTreeNode(
    WebContentsImpl* current_web_contents)
    : current_web_contents_(current_web_contents),
      outer_web_contents_(nullptr),
      outer_contents_frame_tree_node_id_(
          FrameTreeNode::kFrameTreeNodeInvalidId),
      focused_web_contents_(current_web_contents) {}

WebContentsImpl::WebContentsTreeNode::~WebContentsTreeNode() {
  if (OuterContentsFrameTreeNode())
    OuterContentsFrameTreeNode()->RemoveObserver(this);

  if (outer_web_contents_)
    outer_web_contents_->node_.DetachInnerWebContents(current_web_contents_);
}

void WebContentsImpl::WebContentsTreeNode::ConnectToOuterWebContents(
    WebContentsImpl* outer_web_contents,
    RenderFrameHostImpl* outer_contents_frame) {
  focused_web_contents_ = nullptr;
  outer_web_contents_ = outer_web_contents;
  outer_contents_frame_tree_node_id_ =
      outer_contents_frame->frame_tree_node()->frame_tree_node_id();

  outer_web_contents_->node_.AttachInnerWebContents(current_web_contents_);
  outer_contents_frame->frame_tree_node()->AddObserver(this);
}

void WebContentsImpl::WebContentsTreeNode::AttachInnerWebContents(
    WebContentsImpl* inner_web_contents) {
  inner_web_contents_.push_back(inner_web_contents);
}

void WebContentsImpl::WebContentsTreeNode::DetachInnerWebContents(
    WebContentsImpl* inner_web_contents) {
  DCHECK(std::find(inner_web_contents_.begin(), inner_web_contents_.end(),
                   inner_web_contents) != inner_web_contents_.end());
  inner_web_contents_.erase(
      std::remove(inner_web_contents_.begin(), inner_web_contents_.end(),
                  inner_web_contents),
      inner_web_contents_.end());
}

FrameTreeNode*
WebContentsImpl::WebContentsTreeNode::OuterContentsFrameTreeNode() const {
  return FrameTreeNode::GloballyFindByID(outer_contents_frame_tree_node_id_);
}

void WebContentsImpl::WebContentsTreeNode::OnFrameTreeNodeDestroyed(
    FrameTreeNode* node) {
  DCHECK_EQ(outer_contents_frame_tree_node_id_, node->frame_tree_node_id())
      << "WebContentsTreeNode should only receive notifications for the "
         "FrameTreeNode in its outer WebContents that hosts it.";
  delete current_web_contents_;  // deletes |this| too.
}

void WebContentsImpl::WebContentsTreeNode::SetFocusedWebContents(
    WebContentsImpl* web_contents) {
  DCHECK(!outer_web_contents())
      << "Only the outermost WebContents tracks focus.";
  focused_web_contents_ = web_contents;
}

WebContentsImpl*
WebContentsImpl::WebContentsTreeNode::GetInnerWebContentsInFrame(
    const FrameTreeNode* frame) {
  auto ftn_id = frame->frame_tree_node_id();
  for (WebContentsImpl* contents : inner_web_contents_) {
    if (contents->node_.outer_contents_frame_tree_node_id() == ftn_id) {
      return contents;
    }
  }
  return nullptr;
}

const std::vector<WebContentsImpl*>&
WebContentsImpl::WebContentsTreeNode::inner_web_contents() const {
  return inner_web_contents_;
}

// WebContentsImpl -------------------------------------------------------------

WebContentsImpl::WebContentsImpl(BrowserContext* browser_context)
    : delegate_(NULL),
      controller_(this, browser_context),
      render_view_host_delegate_view_(NULL),
      created_with_opener_(false),
      frame_tree_(new NavigatorImpl(&controller_, this),
                  this,
                  this,
                  this,
                  this),
      node_(this),
      is_load_to_different_document_(false),
      crashed_status_(base::TERMINATION_STATUS_STILL_RUNNING),
      crashed_error_code_(0),
      waiting_for_response_(false),
      load_state_(net::LOAD_STATE_IDLE, base::string16()),
      upload_size_(0),
      upload_position_(0),
      is_resume_pending_(false),
      has_accessed_initial_document_(false),
      theme_color_(SK_ColorTRANSPARENT),
      last_sent_theme_color_(SK_ColorTRANSPARENT),
      did_first_visually_non_empty_paint_(false),
      capturer_count_(0),
      should_normally_be_visible_(true),
      did_first_set_visible_(false),
      is_being_destroyed_(false),
      is_notifying_observers_(false),
      notify_disconnection_(false),
      dialog_manager_(NULL),
      is_showing_before_unload_dialog_(false),
      last_active_time_(base::TimeTicks::Now()),
      closed_by_user_gesture_(false),
      minimum_zoom_percent_(static_cast<int>(kMinimumZoomFactor * 100)),
      maximum_zoom_percent_(static_cast<int>(kMaximumZoomFactor * 100)),
      zoom_scroll_remainder_(0),
      fullscreen_widget_process_id_(ChildProcessHost::kInvalidUniqueID),
      fullscreen_widget_routing_id_(MSG_ROUTING_NONE),
      fullscreen_widget_had_focus_at_shutdown_(false),
      is_subframe_(false),
      force_disable_overscroll_content_(false),
      last_dialog_suppressed_(false),
      geolocation_service_context_(new device::GeolocationServiceContext()),
      accessibility_mode_(
          BrowserAccessibilityStateImpl::GetInstance()->accessibility_mode()),
      audio_stream_monitor_(this),
      bluetooth_connected_device_count_(0),
      virtual_keyboard_requested_(false),
#if !defined(OS_ANDROID)
      page_scale_factor_is_one_(true),
#endif  // !defined(OS_ANDROID)
      mouse_lock_widget_(nullptr),
      is_overlay_content_(false),
      showing_context_menu_(false),
      loading_weak_factory_(this),
      weak_factory_(this) {
  frame_tree_.SetFrameRemoveListener(
      base::Bind(&WebContentsImpl::OnFrameRemoved,
                 base::Unretained(this)));
#if defined(OS_ANDROID)
  media_web_contents_observer_.reset(new MediaWebContentsObserverAndroid(this));
#else
  media_web_contents_observer_.reset(new MediaWebContentsObserver(this));
#endif
#if BUILDFLAG(ENABLE_PLUGINS)
  pepper_playback_observer_.reset(new PepperPlaybackObserver(this));
#endif

  loader_io_thread_notifier_.reset(new LoaderIOThreadNotifier(this));
#if !defined(OS_ANDROID)
  host_zoom_map_observer_.reset(new HostZoomMapObserver(this));
#endif  // !defined(OS_ANDROID)
}

WebContentsImpl::~WebContentsImpl() {
  // Imperfect sanity check against double free, given some crashes unexpectedly
  // observed in the wild.
  CHECK(!is_being_destroyed_);

  // We generally keep track of is_being_destroyed_ to let other features know
  // to avoid certain actions during destruction.
  is_being_destroyed_ = true;

  // A WebContents should never be deleted while it is notifying observers,
  // since this will lead to a use-after-free as it continues to notify later
  // observers.
  CHECK(!is_notifying_observers_);

  rwh_input_event_router_.reset();

  for (auto& entry : binding_sets_)
    entry.second->CloseAllBindings();

  WebContentsImpl* outermost = GetOutermostWebContents();
  if (this != outermost && ContainsOrIsFocusedWebContents()) {
    // If the current WebContents is in focus, unset it.
    outermost->SetAsFocusedWebContentsIfNecessary();
  }

  for (FrameTreeNode* node : frame_tree_.Nodes()) {
    // Delete all RFHs pending shutdown, which will lead the corresponding RVHs
    // to be shutdown and be deleted as well.
    node->render_manager()->ClearRFHsPendingShutdown();
    node->render_manager()->ClearWebUIInstances();
  }

  for (RenderWidgetHostImpl* widget : created_widgets_)
    widget->DetachDelegate();
  created_widgets_.clear();

  // Clear out any JavaScript state.
  if (dialog_manager_) {
    dialog_manager_->CancelDialogs(this, /*reset_state=*/true);
  }

  if (color_chooser_info_.get())
    color_chooser_info_->chooser->End();

  NotifyDisconnected();

  // Notify any observer that have a reference on this WebContents.
  NotificationService::current()->Notify(
      NOTIFICATION_WEB_CONTENTS_DESTROYED,
      Source<WebContents>(this),
      NotificationService::NoDetails());

  // Destroy all frame tree nodes except for the root; this notifies observers.
  frame_tree_.root()->ResetForNewProcess();
  GetRenderManager()->ResetProxyHosts();

  // Manually call the observer methods for the root frame tree node. It is
  // necessary to manually delete all objects tracking navigations
  // (NavigationHandle, NavigationRequest) for observers to be properly
  // notified of these navigations stopping before the WebContents is
  // destroyed.
  RenderFrameHostManager* root = GetRenderManager();

  if (root->pending_frame_host()) {
    root->pending_frame_host()->SetRenderFrameCreated(false);
    root->pending_frame_host()->SetNavigationHandle(
        std::unique_ptr<NavigationHandleImpl>());
  }
  root->current_frame_host()->SetRenderFrameCreated(false);
  root->current_frame_host()->SetNavigationHandle(
      std::unique_ptr<NavigationHandleImpl>());

  // PlzNavigate: clear up state specific to browser-side navigation.
  if (IsBrowserSideNavigationEnabled()) {
    // Do not update state as the WebContents is being destroyed.
    frame_tree_.root()->ResetNavigationRequest(true, true);
    if (root->speculative_frame_host()) {
      root->speculative_frame_host()->SetRenderFrameCreated(false);
      root->speculative_frame_host()->SetNavigationHandle(
          std::unique_ptr<NavigationHandleImpl>());
    }
  }

#if BUILDFLAG(ENABLE_PLUGINS)
  // Call this before WebContentsDestroyed() is broadcasted since
  // AudioFocusManager will be destroyed after that.
  pepper_playback_observer_.reset();
#endif  // defined(ENABLED_PLUGINS)

  for (auto& observer : observers_)
    observer.FrameDeleted(root->current_frame_host());

  if (root->pending_render_view_host()) {
    for (auto& observer : observers_)
      observer.RenderViewDeleted(root->pending_render_view_host());
  }

  for (auto& observer : observers_)
    observer.RenderViewDeleted(root->current_host());

  for (auto& observer : observers_)
    observer.WebContentsDestroyed();

  for (auto& observer : observers_)
    observer.ResetWebContents();

  SetDelegate(NULL);
}

WebContentsImpl* WebContentsImpl::CreateWithOpener(
    const WebContents::CreateParams& params,
    FrameTreeNode* opener) {
  TRACE_EVENT0("browser", "WebContentsImpl::CreateWithOpener");
  WebContentsImpl* new_contents = new WebContentsImpl(params.browser_context);
  new_contents->SetOpenerForNewContents(opener, params.opener_suppressed);

  // If the opener is sandboxed, a new popup must inherit the opener's sandbox
  // flags, and these flags take effect immediately.  An exception is if the
  // opener's sandbox flags lack the PropagatesToAuxiliaryBrowsingContexts
  // bit (which is controlled by the "allow-popups-to-escape-sandbox" token).
  // See https://html.spec.whatwg.org/#attr-iframe-sandbox.
  FrameTreeNode* new_root = new_contents->GetFrameTree()->root();
  if (opener) {
    blink::WebSandboxFlags opener_flags = opener->effective_sandbox_flags();
    const blink::WebSandboxFlags inherit_flag =
        blink::WebSandboxFlags::kPropagatesToAuxiliaryBrowsingContexts;
    if ((opener_flags & inherit_flag) == inherit_flag) {
      new_root->SetPendingSandboxFlags(opener_flags);
      new_root->CommitPendingFramePolicy();
    }
  }

  // This may be true even when opener is null, such as when opening blocked
  // popups.
  if (params.created_with_opener)
    new_contents->created_with_opener_ = true;

  if (params.guest_delegate) {
    // This makes |new_contents| act as a guest.
    // For more info, see comment above class BrowserPluginGuest.
    BrowserPluginGuest::Create(new_contents, params.guest_delegate);
    // We are instantiating a WebContents for browser plugin. Set its subframe
    // bit to true.
    new_contents->is_subframe_ = true;
  }
  new_contents->Init(params);
  return new_contents;
}

// static
std::vector<WebContentsImpl*> WebContentsImpl::GetAllWebContents() {
  std::vector<WebContentsImpl*> result;
  std::unique_ptr<RenderWidgetHostIterator> widgets(
      RenderWidgetHostImpl::GetRenderWidgetHosts());
  while (RenderWidgetHost* rwh = widgets->GetNextHost()) {
    RenderViewHost* rvh = RenderViewHost::From(rwh);
    if (!rvh)
      continue;
    WebContents* web_contents = WebContents::FromRenderViewHost(rvh);
    if (!web_contents)
      continue;
    if (web_contents->GetRenderViewHost() != rvh)
      continue;
    // Because a WebContents can only have one current RVH at a time, there will
    // be no duplicate WebContents here.
    result.push_back(static_cast<WebContentsImpl*>(web_contents));
  }
  return result;
}

// static
WebContentsImpl* WebContentsImpl::FromFrameTreeNode(
    const FrameTreeNode* frame_tree_node) {
  return static_cast<WebContentsImpl*>(
      WebContents::FromRenderFrameHost(frame_tree_node->current_frame_host()));
}

// static
WebContents* WebContentsImpl::FromRenderFrameHostID(int render_process_host_id,
                                                    int render_frame_host_id) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  RenderFrameHost* render_frame_host =
      RenderFrameHost::FromID(render_process_host_id, render_frame_host_id);
  if (!render_frame_host)
    return nullptr;

  return WebContents::FromRenderFrameHost(render_frame_host);
}

// static
WebContentsImpl* WebContentsImpl::FromOuterFrameTreeNode(
    const FrameTreeNode* frame_tree_node) {
  return WebContentsImpl::FromFrameTreeNode(frame_tree_node)
      ->node_.GetInnerWebContentsInFrame(frame_tree_node);
}

RenderFrameHostManager* WebContentsImpl::GetRenderManagerForTesting() {
  return GetRenderManager();
}

bool WebContentsImpl::OnMessageReceived(RenderViewHostImpl* render_view_host,
                                        const IPC::Message& message) {
  RenderFrameHost* main_frame = render_view_host->GetMainFrame();
  if (main_frame) {
    WebUIImpl* web_ui = static_cast<RenderFrameHostImpl*>(main_frame)->web_ui();
    if (web_ui && web_ui->OnMessageReceived(message))
      return true;
  }

  for (auto& observer : observers_) {
    // TODO(nick, creis): Replace all uses of this variant of OnMessageReceived
    // with the version that takes a RenderFrameHost, and delete it.
    if (observer.OnMessageReceived(message))
      return true;
  }

  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP_WITH_PARAM(WebContentsImpl, message, render_view_host)
    IPC_MESSAGE_HANDLER(ViewHostMsg_DidFirstVisuallyNonEmptyPaint,
                        OnFirstVisuallyNonEmptyPaint)
    IPC_MESSAGE_HANDLER(ViewHostMsg_GoToEntryAtOffset, OnGoToEntryAtOffset)
    IPC_MESSAGE_HANDLER(ViewHostMsg_UpdateZoomLimits, OnUpdateZoomLimits)
    IPC_MESSAGE_HANDLER(ViewHostMsg_PageScaleFactorChanged,
                        OnPageScaleFactorChanged)
    IPC_MESSAGE_HANDLER(ViewHostMsg_EnumerateDirectory, OnEnumerateDirectory)
    IPC_MESSAGE_HANDLER(ViewHostMsg_AppCacheAccessed, OnAppCacheAccessed)
    IPC_MESSAGE_HANDLER(ViewHostMsg_WebUISend, OnWebUISend)
#if BUILDFLAG(ENABLE_PLUGINS)
    IPC_MESSAGE_HANDLER(ViewHostMsg_RequestPpapiBrokerPermission,
                        OnRequestPpapiBrokerPermission)
#endif
    IPC_MESSAGE_HANDLER(ViewHostMsg_ShowValidationMessage,
                        OnShowValidationMessage)
    IPC_MESSAGE_HANDLER(ViewHostMsg_HideValidationMessage,
                        OnHideValidationMessage)
    IPC_MESSAGE_HANDLER(ViewHostMsg_MoveValidationMessage,
                        OnMoveValidationMessage)
#if defined(OS_ANDROID)
    IPC_MESSAGE_HANDLER(ViewHostMsg_OpenDateTimeDialog, OnOpenDateTimeDialog)
#endif
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()

  return handled;
}

bool WebContentsImpl::OnMessageReceived(RenderFrameHostImpl* render_frame_host,
                                        const IPC::Message& message) {
  for (auto& observer : observers_) {
    if (observer.OnMessageReceived(message, render_frame_host))
      return true;
  }

  bool handled = true;
  IPC_BEGIN_MESSAGE_MAP_WITH_PARAM(WebContentsImpl, message, render_frame_host)
    IPC_MESSAGE_HANDLER(FrameHostMsg_DomOperationResponse,
                        OnDomOperationResponse)
    IPC_MESSAGE_HANDLER(FrameHostMsg_DidChangeThemeColor,
                        OnThemeColorChanged)
    IPC_MESSAGE_HANDLER(FrameHostMsg_DidFinishDocumentLoad,
                        OnDocumentLoadedInFrame)
    IPC_MESSAGE_HANDLER(FrameHostMsg_DidFinishLoad, OnDidFinishLoad)
    IPC_MESSAGE_HANDLER(FrameHostMsg_OpenColorChooser, OnOpenColorChooser)
    IPC_MESSAGE_HANDLER(FrameHostMsg_EndColorChooser, OnEndColorChooser)
    IPC_MESSAGE_HANDLER(FrameHostMsg_SetSelectedColorInColorChooser,
                        OnSetSelectedColorInColorChooser)
    IPC_MESSAGE_HANDLER(FrameHostMsg_DidLoadResourceFromMemoryCache,
                        OnDidLoadResourceFromMemoryCache)
    IPC_MESSAGE_HANDLER(FrameHostMsg_DidDisplayInsecureContent,
                        OnDidDisplayInsecureContent)
    IPC_MESSAGE_HANDLER(FrameHostMsg_DidContainInsecureFormAction,
                        OnDidContainInsecureFormAction)
    IPC_MESSAGE_HANDLER(FrameHostMsg_DidRunInsecureContent,
                        OnDidRunInsecureContent)
    IPC_MESSAGE_HANDLER(FrameHostMsg_DidDisplayContentWithCertificateErrors,
                        OnDidDisplayContentWithCertificateErrors)
    IPC_MESSAGE_HANDLER(FrameHostMsg_DidRunContentWithCertificateErrors,
                        OnDidRunContentWithCertificateErrors)
    IPC_MESSAGE_HANDLER(FrameHostMsg_RegisterProtocolHandler,
                        OnRegisterProtocolHandler)
    IPC_MESSAGE_HANDLER(FrameHostMsg_UnregisterProtocolHandler,
                        OnUnregisterProtocolHandler)
    IPC_MESSAGE_HANDLER(FrameHostMsg_UpdatePageImportanceSignals,
                        OnUpdatePageImportanceSignals)
    IPC_MESSAGE_HANDLER(FrameHostMsg_Find_Reply, OnFindReply)
    IPC_MESSAGE_HANDLER(FrameHostMsg_UpdateFaviconURL, OnUpdateFaviconURL)
#if BUILDFLAG(ENABLE_PLUGINS)
    IPC_MESSAGE_HANDLER(FrameHostMsg_PepperInstanceCreated,
                        OnPepperInstanceCreated)
    IPC_MESSAGE_HANDLER(FrameHostMsg_PepperInstanceDeleted,
                        OnPepperInstanceDeleted)
    IPC_MESSAGE_HANDLER(FrameHostMsg_PepperPluginHung, OnPepperPluginHung)
    IPC_MESSAGE_HANDLER(FrameHostMsg_PepperStartsPlayback,
                        OnPepperStartsPlayback)
    IPC_MESSAGE_HANDLER(FrameHostMsg_PepperStopsPlayback,
                        OnPepperStopsPlayback)
    IPC_MESSAGE_HANDLER(FrameHostMsg_PluginCrashed, OnPluginCrashed)
    IPC_MESSAGE_HANDLER_GENERIC(BrowserPluginHostMsg_Attach,
                                OnBrowserPluginMessage(render_frame_host,
                                                       message))
#endif
#if defined(OS_ANDROID)
    IPC_MESSAGE_HANDLER(FrameHostMsg_FindMatchRects_Reply,
                        OnFindMatchRectsReply)
    IPC_MESSAGE_HANDLER(FrameHostMsg_GetNearestFindResult_Reply,
                        OnGetNearestFindResultReply)
#endif
    IPC_MESSAGE_UNHANDLED(handled = false)
  IPC_END_MESSAGE_MAP()

  return handled;
}

NavigationControllerImpl& WebContentsImpl::GetController() {
  return controller_;
}

const NavigationControllerImpl& WebContentsImpl::GetController() const {
  return controller_;
}

BrowserContext* WebContentsImpl::GetBrowserContext() const {
  return controller_.GetBrowserContext();
}

const GURL& WebContentsImpl::GetURL() const {
  // We may not have a navigation entry yet.
  NavigationEntry* entry = controller_.GetVisibleEntry();
  return entry ? entry->GetVirtualURL() : GURL::EmptyGURL();
}

const GURL& WebContentsImpl::GetVisibleURL() const {
  // We may not have a navigation entry yet.
  NavigationEntry* entry = controller_.GetVisibleEntry();
  return entry ? entry->GetVirtualURL() : GURL::EmptyGURL();
}

const GURL& WebContentsImpl::GetLastCommittedURL() const {
  // We may not have a navigation entry yet.
  NavigationEntry* entry = controller_.GetLastCommittedEntry();
  return entry ? entry->GetVirtualURL() : GURL::EmptyGURL();
}

WebContentsDelegate* WebContentsImpl::GetDelegate() {
  return delegate_;
}

void WebContentsImpl::SetDelegate(WebContentsDelegate* delegate) {
  // TODO(cbentzel): remove this debugging code?
  if (delegate == delegate_)
    return;
  if (delegate_)
    delegate_->Detach(this);
  delegate_ = delegate;
  if (delegate_) {
    delegate_->Attach(this);
    // Ensure the visible RVH reflects the new delegate's preferences.
    if (view_)
      view_->SetOverscrollControllerEnabled(CanOverscrollContent());
  }
}

RenderProcessHost* WebContentsImpl::GetRenderProcessHost() const {
  RenderViewHostImpl* host = GetRenderManager()->current_host();
  return host ? host->GetProcess() : NULL;
}

RenderFrameHostImpl* WebContentsImpl::GetMainFrame() {
  return frame_tree_.root()->current_frame_host();
}

RenderFrameHostImpl* WebContentsImpl::GetFocusedFrame() {
  FrameTreeNode* focused_node = frame_tree_.GetFocusedFrame();
  if (!focused_node)
    return nullptr;
  return focused_node->current_frame_host();
}

RenderFrameHostImpl* WebContentsImpl::FindFrameByFrameTreeNodeId(
    int frame_tree_node_id,
    int process_id) {
  FrameTreeNode* frame = frame_tree_.FindByID(frame_tree_node_id);

  // Sanity check that this is in the caller's expected process. Otherwise a
  // recent cross-process navigation may have led to a privilege change that the
  // caller is not expecting.
  if (!frame ||
      frame->current_frame_host()->GetProcess()->GetID() != process_id)
    return nullptr;

  return frame->current_frame_host();
}

RenderFrameHostImpl* WebContentsImpl::UnsafeFindFrameByFrameTreeNodeId(
    int frame_tree_node_id) {
  // Beware using this! The RenderFrameHost may have changed since the caller
  // obtained frame_tree_node_id.
  FrameTreeNode* frame = frame_tree_.FindByID(frame_tree_node_id);
  return frame ? frame->current_frame_host() : nullptr;
}

void WebContentsImpl::ForEachFrame(
    const base::Callback<void(RenderFrameHost*)>& on_frame) {
  for (FrameTreeNode* node : frame_tree_.Nodes()) {
    on_frame.Run(node->current_frame_host());
  }
}

std::vector<RenderFrameHost*> WebContentsImpl::GetAllFrames() {
  std::vector<RenderFrameHost*> frame_hosts;
  for (FrameTreeNode* node : frame_tree_.Nodes())
    frame_hosts.push_back(node->current_frame_host());
  return frame_hosts;
}

int WebContentsImpl::SendToAllFrames(IPC::Message* message) {
  int number_of_messages = 0;
  for (RenderFrameHost* rfh : GetAllFrames()) {
    if (!rfh->IsRenderFrameLive())
      continue;

    ++number_of_messages;
    IPC::Message* message_copy = new IPC::Message(*message);
    message_copy->set_routing_id(rfh->GetRoutingID());
    rfh->Send(message_copy);
  }
  delete message;
  return number_of_messages;
}

void WebContentsImpl::SendPageMessage(IPC::Message* msg) {
  frame_tree_.root()->render_manager()->SendPageMessage(msg, nullptr);
}

RenderViewHostImpl* WebContentsImpl::GetRenderViewHost() const {
  return GetRenderManager()->current_host();
}

void WebContentsImpl::CancelActiveAndPendingDialogs() {
  if (dialog_manager_) {
    dialog_manager_->CancelDialogs(this, /*reset_state=*/false);
  }
  if (browser_plugin_embedder_)
    browser_plugin_embedder_->CancelGuestDialogs();
}

void WebContentsImpl::ClosePage() {
  GetRenderViewHost()->ClosePage();
}

RenderWidgetHostView* WebContentsImpl::GetRenderWidgetHostView() const {
  return GetRenderManager()->GetRenderWidgetHostView();
}

RenderWidgetHostView* WebContentsImpl::GetTopLevelRenderWidgetHostView() {
  if (GetOuterWebContents())
    return GetOuterWebContents()->GetTopLevelRenderWidgetHostView();
  return GetRenderManager()->GetRenderWidgetHostView();
}

RenderWidgetHostView* WebContentsImpl::GetFullscreenRenderWidgetHostView()
    const {
  if (auto* widget_host = GetFullscreenRenderWidgetHost())
    return widget_host->GetView();
  return nullptr;
}

WebContentsView* WebContentsImpl::GetView() const {
  return view_.get();
}

void WebContentsImpl::OnScreenOrientationChange() {
  DCHECK(screen_orientation_provider_);
  return screen_orientation_provider_->OnOrientationChange();
}

SkColor WebContentsImpl::GetThemeColor() const {
  return theme_color_;
}

void WebContentsImpl::SetAccessibilityMode(AccessibilityMode mode) {
  if (mode == accessibility_mode_)
    return;

  // Don't allow accessibility to be enabled for WebContents that are never
  // visible, like background pages.
  if (IsNeverVisible())
    return;

  accessibility_mode_ = mode;

  for (FrameTreeNode* node : frame_tree_.Nodes()) {
    UpdateAccessibilityModeOnFrame(node->current_frame_host());
    RenderFrameHost* pending_frame_host =
        node->render_manager()->pending_frame_host();
    if (pending_frame_host)
      UpdateAccessibilityModeOnFrame(pending_frame_host);
  }
}

void WebContentsImpl::AddAccessibilityMode(AccessibilityMode mode) {
  AccessibilityMode new_mode(accessibility_mode_);
  new_mode |= mode;
  SetAccessibilityMode(new_mode);
}

void WebContentsImpl::RequestAXTreeSnapshot(
    const AXTreeSnapshotCallback& callback) {
  // Send a request to each of the frames in parallel. Each one will return
  // an accessibility tree snapshot, and AXTreeSnapshotCombiner will combine
  // them into a single tree and call |callback| with that result, then
  // delete |combiner|.
  AXTreeSnapshotCombiner* combiner = new AXTreeSnapshotCombiner(callback);
  for (FrameTreeNode* frame_tree_node : frame_tree_.Nodes()) {
    bool is_root = frame_tree_node->parent() == nullptr;
    frame_tree_node->current_frame_host()->RequestAXTreeSnapshot(
        combiner->AddFrame(is_root));
  }
}

#if !defined(OS_ANDROID)
void WebContentsImpl::SetTemporaryZoomLevel(double level,
                                            bool temporary_zoom_enabled) {
  SendPageMessage(new PageMsg_SetZoomLevel(
      MSG_ROUTING_NONE,
      temporary_zoom_enabled ? PageMsg_SetZoomLevel_Command::SET_TEMPORARY
                             : PageMsg_SetZoomLevel_Command::CLEAR_TEMPORARY,
      level));
}

void WebContentsImpl::UpdateZoom(double level) {
  // Individual frames may still ignore the new zoom level if their RenderView
  // contains a plugin document or if it uses a temporary zoom level.
  SendPageMessage(new PageMsg_SetZoomLevel(
      MSG_ROUTING_NONE,
      PageMsg_SetZoomLevel_Command::USE_CURRENT_TEMPORARY_MODE, level));
}

void WebContentsImpl::UpdateZoomIfNecessary(const std::string& scheme,
                                            const std::string& host,
                                            double level) {
  NavigationEntry* entry = GetController().GetLastCommittedEntry();
  if (!entry)
    return;

  GURL url = HostZoomMap::GetURLFromEntry(entry);
  if (host != net::GetHostOrSpecFromURL(url) ||
      (!scheme.empty() && !url.SchemeIs(scheme))) {
    return;
  }

  UpdateZoom(level);
}
#endif  // !defined(OS_ANDROID)

base::Closure WebContentsImpl::AddBindingSet(
    const std::string& interface_name,
    WebContentsBindingSet* binding_set) {
  auto result =
      binding_sets_.insert(std::make_pair(interface_name, binding_set));
  DCHECK(result.second);
  return base::Bind(&WebContentsImpl::RemoveBindingSet,
                    weak_factory_.GetWeakPtr(), interface_name);
}

WebContentsBindingSet* WebContentsImpl::GetBindingSet(
    const std::string& interface_name) {
  auto it = binding_sets_.find(interface_name);
  if (it == binding_sets_.end())
    return nullptr;
  return it->second;
}

std::vector<WebContentsImpl*> WebContentsImpl::GetInnerWebContents() {
  if (browser_plugin_embedder_) {
    std::vector<WebContentsImpl*> inner_contents;
    GetBrowserContext()->GetGuestManager()->ForEachGuest(
        this, base::Bind(&GetInnerWebContentsHelper, &inner_contents));
    return inner_contents;
  }

  return node_.inner_web_contents();
}

std::vector<WebContentsImpl*> WebContentsImpl::GetWebContentsAndAllInner() {
  std::vector<WebContentsImpl*> all_contents(1, this);

  for (size_t i = 0; i != all_contents.size(); ++i) {
    for (auto* inner_contents : all_contents[i]->GetInnerWebContents()) {
      all_contents.push_back(inner_contents);
    }
  }

  return all_contents;
}

void WebContentsImpl::NotifyManifestUrlChanged(
    const base::Optional<GURL>& manifest_url) {
  for (auto& observer : observers_)
    observer.DidUpdateWebManifestURL(manifest_url);
}

void WebContentsImpl::UpdateDeviceScaleFactor(double device_scale_factor) {
  SendPageMessage(
      new PageMsg_SetDeviceScaleFactor(MSG_ROUTING_NONE, device_scale_factor));
}

void WebContentsImpl::GetScreenInfo(ScreenInfo* screen_info) {
  if (GetView())
    GetView()->GetScreenInfo(screen_info);
}

std::unique_ptr<WebUI> WebContentsImpl::CreateSubframeWebUI(
    const GURL& url,
    const std::string& frame_name) {
  DCHECK(!frame_name.empty());
  return CreateWebUI(url, frame_name);
}

WebUI* WebContentsImpl::GetWebUI() const {
  WebUI* commited_web_ui = GetCommittedWebUI();
  return commited_web_ui ? commited_web_ui
                         : GetRenderManager()->GetNavigatingWebUI();
}

WebUI* WebContentsImpl::GetCommittedWebUI() const {
  return frame_tree_.root()->current_frame_host()->web_ui();
}

void WebContentsImpl::SetUserAgentOverride(const std::string& override) {
  if (GetUserAgentOverride() == override)
    return;

  renderer_preferences_.user_agent_override = override;

  // Send the new override string to the renderer.
  RenderViewHost* host = GetRenderViewHost();
  if (host)
    host->SyncRendererPrefs();

  // Reload the page if a load is currently in progress to avoid having
  // different parts of the page loaded using different user agents.
  NavigationEntry* entry = controller_.GetVisibleEntry();
  if (IsLoading() && entry != NULL && entry->GetIsOverridingUserAgent())
    controller_.Reload(ReloadType::BYPASSING_CACHE, true);

  for (auto& observer : observers_)
    observer.UserAgentOverrideSet(override);
}

const std::string& WebContentsImpl::GetUserAgentOverride() const {
  return renderer_preferences_.user_agent_override;
}

void WebContentsImpl::EnableWebContentsOnlyAccessibilityMode() {
  if (!GetAccessibilityMode().is_mode_off()) {
    for (RenderFrameHost* rfh : GetAllFrames())
      ResetAccessibility(rfh);
  } else {
    AddAccessibilityMode(kAccessibilityModeWebContentsOnly);
  }
}

bool WebContentsImpl::IsWebContentsOnlyAccessibilityModeForTesting() const {
  return accessibility_mode_ == kAccessibilityModeWebContentsOnly;
}

bool WebContentsImpl::IsFullAccessibilityModeForTesting() const {
  return accessibility_mode_ == kAccessibilityModeComplete;
}

const PageImportanceSignals& WebContentsImpl::GetPageImportanceSignals() const {
  return page_importance_signals_;
}

const base::string16& WebContentsImpl::GetTitle() const {
  // Transient entries take precedence. They are used for interstitial pages
  // that are shown on top of existing pages.
  NavigationEntry* entry = controller_.GetTransientEntry();
  if (entry) {
    return entry->GetTitleForDisplay();
  }

  WebUI* navigating_web_ui = GetRenderManager()->GetNavigatingWebUI();
  WebUI* our_web_ui = navigating_web_ui
                          ? navigating_web_ui
                          : GetRenderManager()->current_frame_host()->web_ui();

  if (our_web_ui) {
    // Don't override the title in view source mode.
    entry = controller_.GetVisibleEntry();
    if (!(entry && entry->IsViewSourceMode())) {
      // Give the Web UI the chance to override our title.
      const base::string16& title = our_web_ui->GetOverriddenTitle();
      if (!title.empty())
        return title;
    }
  }

  // We use the title for the last committed entry rather than a pending
  // navigation entry. For example, when the user types in a URL, we want to
  // keep the old page's title until the new load has committed and we get a new
  // title.
  entry = controller_.GetLastCommittedEntry();

  // We make an exception for initial navigations. We only want to use the title
  // from the visible entry if:
  // 1. The pending entry has been explicitly assigned a title to display.
  // 2. The user is doing a history navigation in a new tab (e.g., Ctrl+Back),
  //    which case there is a pending entry index other than -1.
  //
  // Otherwise, we want to stick with the last committed entry's title during
  // new navigations, which have pending entries at index -1 with no title.
  if (controller_.IsInitialNavigation() &&
      ((controller_.GetVisibleEntry() &&
        !controller_.GetVisibleEntry()->GetTitle().empty()) ||
       controller_.GetPendingEntryIndex() != -1)) {
    entry = controller_.GetVisibleEntry();
  }

  if (entry) {
    return entry->GetTitleForDisplay();
  }

  // |page_title_when_no_navigation_entry_| is finally used
  // if no title cannot be retrieved.
  return page_title_when_no_navigation_entry_;
}

SiteInstanceImpl* WebContentsImpl::GetSiteInstance() const {
  return GetRenderManager()->current_host()->GetSiteInstance();
}

SiteInstanceImpl* WebContentsImpl::GetPendingSiteInstance() const {
  RenderViewHostImpl* dest_rvh =
      GetRenderManager()->pending_render_view_host() ?
          GetRenderManager()->pending_render_view_host() :
          GetRenderManager()->current_host();
  return dest_rvh->GetSiteInstance();
}

bool WebContentsImpl::IsLoading() const {
  return frame_tree_.IsLoading() &&
         !(ShowingInterstitialPage() &&
           GetRenderManager()->interstitial_page()->pause_throbber());
}

bool WebContentsImpl::IsLoadingToDifferentDocument() const {
  return IsLoading() && is_load_to_different_document_;
}

bool WebContentsImpl::IsWaitingForResponse() const {
  return waiting_for_response_ && is_load_to_different_document_;
}

const net::LoadStateWithParam& WebContentsImpl::GetLoadState() const {
  return load_state_;
}

const base::string16& WebContentsImpl::GetLoadStateHost() const {
  return load_state_host_;
}

uint64_t WebContentsImpl::GetUploadSize() const {
  return upload_size_;
}

uint64_t WebContentsImpl::GetUploadPosition() const {
  return upload_position_;
}

const std::string& WebContentsImpl::GetEncoding() const {
  return canonical_encoding_;
}

void WebContentsImpl::IncrementCapturerCount(const gfx::Size& capture_size) {
  DCHECK(!is_being_destroyed_);
  ++capturer_count_;
  DVLOG(1) << "There are now " << capturer_count_
           << " capturing(s) of WebContentsImpl@" << this;

  // Note: This provides a hint to upstream code to size the views optimally
  // for quality (e.g., to avoid scaling).
  if (!capture_size.IsEmpty() && preferred_size_for_capture_.IsEmpty()) {
    preferred_size_for_capture_ = capture_size;
    OnPreferredSizeChanged(preferred_size_);
  }

  // Ensure that all views are un-occluded before capture begins.
  WasUnOccluded();
}

void WebContentsImpl::DecrementCapturerCount() {
  --capturer_count_;
  DVLOG(1) << "There are now " << capturer_count_
           << " capturing(s) of WebContentsImpl@" << this;
  DCHECK_LE(0, capturer_count_);

  if (is_being_destroyed_)
    return;

  if (capturer_count_ == 0) {
    const gfx::Size old_size = preferred_size_for_capture_;
    preferred_size_for_capture_ = gfx::Size();
    OnPreferredSizeChanged(old_size);
  }

  if (IsHidden()) {
    DVLOG(1) << "Executing delayed WasHidden().";
    WasHidden();
  }
}

int WebContentsImpl::GetCapturerCount() const {
  return capturer_count_;
}

bool WebContentsImpl::IsAudioMuted() const {
  return audio_muter_.get() && audio_muter_->is_muting();
}

void WebContentsImpl::SetAudioMuted(bool mute) {
  DVLOG(1) << "SetAudioMuted(mute=" << mute << "), was " << IsAudioMuted()
           << " for WebContentsImpl@" << this;

  if (mute == IsAudioMuted())
    return;

  if (mute) {
    if (!audio_muter_)
      audio_muter_.reset(new WebContentsAudioMuter(this));
    audio_muter_->StartMuting();
  } else {
    DCHECK(audio_muter_);
    audio_muter_->StopMuting();
  }

  for (auto& observer : observers_)
    observer.DidUpdateAudioMutingState(mute);

  OnAudioStateChanged(!mute && audio_stream_monitor_.IsCurrentlyAudible());
}

bool WebContentsImpl::IsConnectedToBluetoothDevice() const {
  return bluetooth_connected_device_count_ > 0;
}

bool WebContentsImpl::IsCrashed() const {
  return (crashed_status_ == base::TERMINATION_STATUS_PROCESS_CRASHED ||
          crashed_status_ == base::TERMINATION_STATUS_ABNORMAL_TERMINATION ||
          crashed_status_ == base::TERMINATION_STATUS_PROCESS_WAS_KILLED ||
#if defined(OS_CHROMEOS)
          crashed_status_ ==
              base::TERMINATION_STATUS_PROCESS_WAS_KILLED_BY_OOM ||
#endif
          crashed_status_ == base::TERMINATION_STATUS_LAUNCH_FAILED);
}

void WebContentsImpl::SetIsCrashed(base::TerminationStatus status,
                                   int error_code) {
  if (status == crashed_status_)
    return;

  crashed_status_ = status;
  crashed_error_code_ = error_code;
  NotifyNavigationStateChanged(INVALIDATE_TYPE_TAB);
}

base::TerminationStatus WebContentsImpl::GetCrashedStatus() const {
  return crashed_status_;
}

int WebContentsImpl::GetCrashedErrorCode() const {
  return crashed_error_code_;
}

bool WebContentsImpl::IsBeingDestroyed() const {
  return is_being_destroyed_;
}

void WebContentsImpl::NotifyNavigationStateChanged(
    InvalidateTypes changed_flags) {
  // TODO(erikchen): Remove ScopedTracker below once http://crbug.com/466285
  // is fixed.
  tracked_objects::ScopedTracker tracking_profile(
      FROM_HERE_WITH_EXPLICIT_FUNCTION(
          "466285 WebContentsImpl::NotifyNavigationStateChanged"));
  // Notify the media observer of potential audibility changes.
  if (changed_flags & INVALIDATE_TYPE_TAB) {
    media_web_contents_observer_->MaybeUpdateAudibleState();
  }

  if (delegate_)
    delegate_->NavigationStateChanged(this, changed_flags);

  if (GetOuterWebContents())
    GetOuterWebContents()->NotifyNavigationStateChanged(changed_flags);
}

void WebContentsImpl::OnAudioStateChanged(bool is_audible) {
  SendPageMessage(new PageMsg_AudioStateChanged(MSG_ROUTING_NONE, is_audible));

  // Notification for UI updates in response to the changed audio state.
  NotifyNavigationStateChanged(INVALIDATE_TYPE_TAB);
}

base::TimeTicks WebContentsImpl::GetLastActiveTime() const {
  return last_active_time_;
}

void WebContentsImpl::SetLastActiveTime(base::TimeTicks last_active_time) {
  last_active_time_ = last_active_time;
}

base::TimeTicks WebContentsImpl::GetLastHiddenTime() const {
  return last_hidden_time_;
}

void WebContentsImpl::WasShown() {
  controller_.SetActive(true);

  for (RenderWidgetHostView* view : GetRenderWidgetHostViewsInTree()) {
    view->Show();
#if defined(OS_MACOSX)
    view->SetActive(true);
#endif
  }

  SendPageMessage(new PageMsg_WasShown(MSG_ROUTING_NONE));

  last_active_time_ = base::TimeTicks::Now();

  for (auto& observer : observers_)
    observer.WasShown();

  should_normally_be_visible_ = true;
}

void WebContentsImpl::WasHidden() {
  // If there are entities capturing screenshots or video (e.g., mirroring),
  // don't activate the "disable rendering" optimization.
  if (capturer_count_ == 0) {
    // |GetRenderViewHost()| can be NULL if the user middle clicks a link to
    // open a tab in the background, then closes the tab before selecting it.
    // This is because closing the tab calls WebContentsImpl::Destroy(), which
    // removes the |GetRenderViewHost()|; then when we actually destroy the
    // window, OnWindowPosChanged() notices and calls WasHidden() (which
    // calls us).
    for (RenderWidgetHostView* view : GetRenderWidgetHostViewsInTree())
      view->Hide();

    SendPageMessage(new PageMsg_WasHidden(MSG_ROUTING_NONE));
  }

  last_hidden_time_ = base::TimeTicks::Now();

  for (auto& observer : observers_)
    observer.WasHidden();

  should_normally_be_visible_ = false;
}

void WebContentsImpl::WasOccluded() {
  if (capturer_count_ > 0)
    return;

  for (RenderWidgetHostView* view : GetRenderWidgetHostViewsInTree())
    view->WasOccluded();
}

void WebContentsImpl::WasUnOccluded() {
  for (RenderWidgetHostView* view : GetRenderWidgetHostViewsInTree())
    view->WasUnOccluded();
}

bool WebContentsImpl::NeedToFireBeforeUnload() {
  // TODO(creis): Should we fire even for interstitial pages?
  return WillNotifyDisconnection() && !ShowingInterstitialPage() &&
         !GetRenderViewHost()->SuddenTerminationAllowed();
}

void WebContentsImpl::DispatchBeforeUnload() {
  bool for_cross_site_transition = false;
  GetMainFrame()->DispatchBeforeUnload(for_cross_site_transition, false);
}

void WebContentsImpl::AttachToOuterWebContentsFrame(
    WebContents* outer_web_contents,
    RenderFrameHost* outer_contents_frame) {
  CHECK(GuestMode::IsCrossProcessFrameGuest(this));
  RenderFrameHostManager* render_manager = GetRenderManager();

  // When the WebContents being initialized has an opener, the  browser side
  // Render{View,Frame}Host must be initialized and the RenderWidgetHostView
  // created. This is needed because the usual initialization happens during
  // the first navigation, but when attaching a new window we don't navigate
  // before attaching. If the browser side is already initialized, the calls
  // below will just early return.
  render_manager->InitRenderView(GetRenderViewHost(), nullptr);
  GetMainFrame()->Init();
  if (!render_manager->GetRenderWidgetHostView())
    CreateRenderWidgetHostViewForRenderManager(GetRenderViewHost());

  auto* outer_web_contents_impl =
      static_cast<WebContentsImpl*>(outer_web_contents);
  auto* outer_contents_frame_impl =
      static_cast<RenderFrameHostImpl*>(outer_contents_frame);
  // Create a link to our outer WebContents.
  node_.ConnectToOuterWebContents(outer_web_contents_impl,
                                  outer_contents_frame_impl);

  DCHECK(outer_contents_frame);

  // Create a proxy in top-level RenderFrameHostManager, pointing to the
  // SiteInstance of the outer WebContents. The proxy will be used to send
  // postMessage to the inner WebContents.
  render_manager->CreateOuterDelegateProxy(
      outer_contents_frame->GetSiteInstance(), outer_contents_frame_impl);

  render_manager->SetRWHViewForInnerContents(
      render_manager->GetRenderWidgetHostView());

  static_cast<RenderWidgetHostViewChildFrame*>(
      render_manager->GetRenderWidgetHostView())
      ->RegisterFrameSinkId();

  if (outer_web_contents_impl->frame_tree_.GetFocusedFrame() ==
      outer_contents_frame_impl->frame_tree_node()) {
    SetFocusedFrame(frame_tree_.root(), nullptr);
  }

  // At this point, we should destroy the TextInputManager which will notify all
  // the RWHV in this WebContents. The RWHV in this WebContents should use the
  // TextInputManager owned by the outer WebContents.
  // TODO(ekaramad): Is it possible to have TextInputState before attaching to
  // outer WebContents? In such a case, is this still the right way to hand off
  // state tracking from inner WebContents's TextInputManager to that of the
  // outer WebContent (crbug.com/609846)?
  text_input_manager_.reset(nullptr);
}

void WebContentsImpl::DidChangeVisibleSecurityState() {
  if (delegate_) {
    delegate_->VisibleSecurityStateChanged(this);
    for (auto& observer : observers_)
      observer.DidChangeVisibleSecurityState();
  }
}

void WebContentsImpl::Stop() {
  for (FrameTreeNode* node : frame_tree_.Nodes())
    node->StopLoading();
  for (auto& observer : observers_)
    observer.NavigationStopped();
}

WebContents* WebContentsImpl::Clone() {
  // We use our current SiteInstance since the cloned entry will use it anyway.
  // We pass our own opener so that the cloned page can access it if it was set
  // before.
  CreateParams create_params(GetBrowserContext(), GetSiteInstance());
  create_params.initial_size = GetContainerBounds().size();
  WebContentsImpl* tc =
      CreateWithOpener(create_params, frame_tree_.root()->opener());
  tc->GetController().CopyStateFrom(controller_);
  for (auto& observer : observers_)
    observer.DidCloneToNewWebContents(this, tc);
  return tc;
}

void WebContentsImpl::Observe(int type,
                              const NotificationSource& source,
                              const NotificationDetails& details) {
  switch (type) {
    case NOTIFICATION_RENDER_WIDGET_HOST_DESTROYED: {
      RenderWidgetHost* host = Source<RenderWidgetHost>(source).ptr();
      RenderWidgetHostView* view = host->GetView();
      if (view == GetFullscreenRenderWidgetHostView()) {
        // We cannot just call view_->RestoreFocus() here.  On some platforms,
        // attempting to focus the currently-invisible WebContentsView will be
        // flat-out ignored.  Therefore, this boolean is used to track whether
        // we will request focus after the fullscreen widget has been
        // destroyed.
        fullscreen_widget_had_focus_at_shutdown_ = (view && view->HasFocus());
      } else {
        for (auto i = pending_widget_views_.begin();
             i != pending_widget_views_.end(); ++i) {
          if (host->GetView() == i->second) {
            pending_widget_views_.erase(i);
            break;
          }
        }
      }
      break;
    }
    default:
      NOTREACHED();
  }
}

WebContents* WebContentsImpl::GetWebContents() {
  return this;
}

void WebContentsImpl::Init(const WebContents::CreateParams& params) {
  // This is set before initializing the render manager since
  // RenderFrameHostManager::Init calls back into us via its delegate to ask if
  // it should be hidden.
  should_normally_be_visible_ = !params.initially_hidden;

  // The routing ids must either all be set or all be unset.
  DCHECK((params.routing_id == MSG_ROUTING_NONE &&
          params.main_frame_routing_id == MSG_ROUTING_NONE &&
          params.main_frame_widget_routing_id == MSG_ROUTING_NONE) ||
         (params.routing_id != MSG_ROUTING_NONE &&
          params.main_frame_routing_id != MSG_ROUTING_NONE &&
          params.main_frame_widget_routing_id != MSG_ROUTING_NONE));

  scoped_refptr<SiteInstance> site_instance = params.site_instance;
  if (!site_instance)
    site_instance = SiteInstance::Create(params.browser_context);

  // A main RenderFrameHost always has a RenderWidgetHost, since it is always a
  // local root by definition.
  // TODO(avi): Once RenderViewHostImpl has-a RenderWidgetHostImpl, it will no
  // longer be necessary to eagerly grab a routing ID for the view.
  // https://crbug.com/545684
  int32_t view_routing_id = params.routing_id;
  int32_t main_frame_widget_routing_id = params.main_frame_widget_routing_id;
  if (main_frame_widget_routing_id == MSG_ROUTING_NONE) {
    view_routing_id = main_frame_widget_routing_id =
        site_instance->GetProcess()->GetNextRoutingID();
  }

  GetRenderManager()->Init(site_instance.get(), view_routing_id,
                           params.main_frame_routing_id,
                           main_frame_widget_routing_id,
                           params.renderer_initiated_creation);

  // blink::FrameTree::setName always keeps |unique_name| empty in case of a
  // main frame - let's do the same thing here.
  std::string unique_name;
  frame_tree_.root()->SetFrameName(params.main_frame_name, unique_name);

  WebContentsViewDelegate* delegate =
      GetContentClient()->browser()->GetWebContentsViewDelegate(this);

  if (GuestMode::IsCrossProcessFrameGuest(this)) {
    view_.reset(new WebContentsViewChildFrame(
        this, delegate, &render_view_host_delegate_view_));
  } else {
    view_.reset(CreateWebContentsView(this, delegate,
                                      &render_view_host_delegate_view_));
  }

  if (browser_plugin_guest_ && !GuestMode::IsCrossProcessFrameGuest(this)) {
    view_.reset(new WebContentsViewGuest(this, browser_plugin_guest_.get(),
                                         std::move(view_),
                                         &render_view_host_delegate_view_));
  }
  CHECK(render_view_host_delegate_view_);
  CHECK(view_.get());

  gfx::Size initial_size = params.initial_size;
  view_->CreateView(initial_size, params.context);

#if BUILDFLAG(ENABLE_PLUGINS)
  plugin_content_origin_whitelist_.reset(
      new PluginContentOriginWhitelist(this));
#endif

  registrar_.Add(this,
                 NOTIFICATION_RENDER_WIDGET_HOST_DESTROYED,
                 NotificationService::AllBrowserContextsAndSources());

  screen_orientation_provider_.reset(new ScreenOrientationProvider(this));

  manifest_manager_host_.reset(new ManifestManagerHost(this));

#if defined(OS_ANDROID)
  date_time_chooser_.reset(new DateTimeChooserAndroid());
#endif

  // BrowserPluginGuest::Init needs to be called after this WebContents has
  // a RenderWidgetHostViewGuest. That is, |view_->CreateView| above.
  if (browser_plugin_guest_)
    browser_plugin_guest_->Init();

  for (size_t i = 0; i < g_created_callbacks.Get().size(); i++)
    g_created_callbacks.Get().at(i).Run(this);

  // If the WebContents creation was renderer-initiated, it means that the
  // corresponding RenderView and main RenderFrame have already been created.
  // Ensure observers are notified about this.
  if (params.renderer_initiated_creation) {
    GetRenderViewHost()->GetWidget()->set_renderer_initialized(true);
    RenderViewCreated(GetRenderViewHost());
    GetRenderManager()->current_frame_host()->SetRenderFrameCreated(true);
  }

  // Create the renderer process in advance if requested.
  if (params.initialize_renderer) {
    if (!GetRenderManager()->current_frame_host()->IsRenderFrameLive()) {
      GetRenderManager()->InitRenderView(GetRenderViewHost(), nullptr);
    }
  }

  // Ensure that observers are notified of the creation of this WebContents's
  // main RenderFrameHost. It must be done here for main frames, since the
  // NotifySwappedFromRenderManager expects view_ to already be created and that
  // happens after RenderFrameHostManager::Init.
  NotifySwappedFromRenderManager(
      nullptr, GetRenderManager()->current_frame_host(), true);
}

void WebContentsImpl::OnWebContentsDestroyed(WebContentsImpl* web_contents) {
  RemoveDestructionObserver(web_contents);

  // Clear a pending contents that has been closed before being shown.
  for (auto iter = pending_contents_.begin(); iter != pending_contents_.end();
       ++iter) {
    if (iter->second != web_contents)
      continue;
    pending_contents_.erase(iter);
    return;
  }
  NOTREACHED();
}

void WebContentsImpl::AddDestructionObserver(WebContentsImpl* web_contents) {
  if (!ContainsKey(destruction_observers_, web_contents)) {
    destruction_observers_[web_contents] =
        base::MakeUnique<DestructionObserver>(this, web_contents);
  }
}

void WebContentsImpl::RemoveDestructionObserver(WebContentsImpl* web_contents) {
  destruction_observers_.erase(web_contents);
}

void WebContentsImpl::AddObserver(WebContentsObserver* observer) {
  observers_.AddObserver(observer);
}

void WebContentsImpl::RemoveObserver(WebContentsObserver* observer) {
  observers_.RemoveObserver(observer);
}

std::set<RenderWidgetHostView*>
WebContentsImpl::GetRenderWidgetHostViewsInTree() {
  std::set<RenderWidgetHostView*> set;
  if (ShowingInterstitialPage()) {
    if (RenderWidgetHostView* rwhv = GetRenderWidgetHostView())
      set.insert(rwhv);
  } else {
    for (RenderFrameHost* rfh : GetAllFrames()) {
      if (RenderWidgetHostView* rwhv = static_cast<RenderFrameHostImpl*>(rfh)
                                           ->frame_tree_node()
                                           ->render_manager()
                                           ->GetRenderWidgetHostView()) {
        set.insert(rwhv);
      }
    }
  }
  return set;
}

void WebContentsImpl::Activate() {
  if (delegate_)
    delegate_->ActivateContents(this);
}

void WebContentsImpl::LostCapture(RenderWidgetHostImpl* render_widget_host) {
  if (!RenderViewHostImpl::From(render_widget_host))
    return;

  if (delegate_)
    delegate_->LostCapture();
}

void WebContentsImpl::RenderWidgetCreated(
    RenderWidgetHostImpl* render_widget_host) {
  created_widgets_.insert(render_widget_host);
}

void WebContentsImpl::RenderWidgetDeleted(
    RenderWidgetHostImpl* render_widget_host) {
  // Note that |is_being_destroyed_| can be true at this point as
  // ~WebContentsImpl() calls RFHM::ClearRFHsPendingShutdown(), which might lead
  // us here.
  created_widgets_.erase(render_widget_host);

  if (is_being_destroyed_)
    return;

  if (render_widget_host &&
      render_widget_host->GetRoutingID() == fullscreen_widget_routing_id_ &&
      render_widget_host->GetProcess()->GetID() ==
          fullscreen_widget_process_id_) {
    if (delegate_ && delegate_->EmbedsFullscreenWidget())
      delegate_->ExitFullscreenModeForTab(this);
    for (auto& observer : observers_)
      observer.DidDestroyFullscreenWidget();
    fullscreen_widget_process_id_ = ChildProcessHost::kInvalidUniqueID;
    fullscreen_widget_routing_id_ = MSG_ROUTING_NONE;
    if (fullscreen_widget_had_focus_at_shutdown_)
      view_->RestoreFocus();
  }

  if (render_widget_host == mouse_lock_widget_)
    LostMouseLock(mouse_lock_widget_);
}

void WebContentsImpl::RenderWidgetGotFocus(
    RenderWidgetHostImpl* render_widget_host) {
  // Notify the observers if an embedded fullscreen widget was focused.
  if (delegate_ && render_widget_host && delegate_->EmbedsFullscreenWidget() &&
      render_widget_host->GetView() == GetFullscreenRenderWidgetHostView()) {
    NotifyWebContentsFocused();
  }
}

void WebContentsImpl::RenderWidgetLostFocus(
    RenderWidgetHostImpl* render_widget_host) {
  NotifyWebContentsLostFocus();
}

void WebContentsImpl::RenderWidgetWasResized(
    RenderWidgetHostImpl* render_widget_host,
    bool width_changed) {
  RenderFrameHostImpl* rfh = GetMainFrame();
  if (!rfh || render_widget_host != rfh->GetRenderWidgetHost())
    return;

  ScreenInfo screen_info;
  GetScreenInfo(&screen_info);
  SendPageMessage(new PageMsg_UpdateScreenInfo(MSG_ROUTING_NONE, screen_info));

  // Send resize message to subframes.
  for (RenderWidgetHostView* view : GetRenderWidgetHostViewsInTree()) {
    if (view != rfh->GetView())
      view->GetRenderWidgetHost()->WasResized();
  }

  for (auto& observer : observers_)
    observer.MainFrameWasResized(width_changed);
}

void WebContentsImpl::ScreenInfoChanged() {
  if (browser_plugin_embedder_)
    browser_plugin_embedder_->ScreenInfoChanged();
}

KeyboardEventProcessingResult WebContentsImpl::PreHandleKeyboardEvent(
    const NativeWebKeyboardEvent& event) {
  return delegate_ ? delegate_->PreHandleKeyboardEvent(this, event)
                   : KeyboardEventProcessingResult::NOT_HANDLED;
}

void WebContentsImpl::HandleKeyboardEvent(const NativeWebKeyboardEvent& event) {
  if (browser_plugin_embedder_ &&
      browser_plugin_embedder_->HandleKeyboardEvent(event)) {
    return;
  }
  if (delegate_)
    delegate_->HandleKeyboardEvent(this, event);
}

bool WebContentsImpl::HandleWheelEvent(
    const blink::WebMouseWheelEvent& event) {
#if !defined(OS_MACOSX)
  // On platforms other than Mac, control+mousewheel may change zoom. On Mac,
  // this isn't done for two reasons:
  //   -the OS already has a gesture to do this through pinch-zoom
  //   -if a user starts an inertial scroll, let's go, and presses control
  //      (i.e. control+tab) then the OS's buffered scroll events will come in
  //      with control key set which isn't what the user wants
  if (delegate_ && event.wheel_ticks_y &&
      !ui::WebInputEventTraits::CanCauseScroll(event)) {
    // Count only integer cumulative scrolls as zoom events; this handles
    // smooth scroll and regular scroll device behavior.
    zoom_scroll_remainder_ += event.wheel_ticks_y;
    int whole_zoom_scroll_remainder_ = std::lround(zoom_scroll_remainder_);
    zoom_scroll_remainder_ -= whole_zoom_scroll_remainder_;
    if (whole_zoom_scroll_remainder_ != 0) {
      delegate_->ContentsZoomChange(whole_zoom_scroll_remainder_ > 0);
    }
    return true;
  }
#endif
  return false;
}

bool WebContentsImpl::PreHandleGestureEvent(
    const blink::WebGestureEvent& event) {
  return delegate_ && delegate_->PreHandleGestureEvent(this, event);
}

RenderWidgetHostInputEventRouter* WebContentsImpl::GetInputEventRouter() {
  if (!is_being_destroyed_ && GetOuterWebContents())
    return GetOuterWebContents()->GetInputEventRouter();

  if (!rwh_input_event_router_.get() && !is_being_destroyed_)
    rwh_input_event_router_.reset(new RenderWidgetHostInputEventRouter);
  return rwh_input_event_router_.get();
}

void WebContentsImpl::ReplicatePageFocus(bool is_focused) {
  // Focus loss may occur while this WebContents is being destroyed.  Don't
  // send the message in this case, as the main frame's RenderFrameHost and
  // other state has already been cleared.
  if (is_being_destroyed_)
    return;

  frame_tree_.ReplicatePageFocus(is_focused);
}

RenderWidgetHostImpl* WebContentsImpl::GetFocusedRenderWidgetHost(
    RenderWidgetHostImpl* receiving_widget) {
  if (!SiteIsolationPolicy::AreCrossProcessFramesPossible())
    return receiving_widget;

  // Events for widgets other than the main frame (e.g., popup menus) should be
  // forwarded directly to the widget they arrived on.
  if (receiving_widget != GetMainFrame()->GetRenderWidgetHost())
    return receiving_widget;

  WebContentsImpl* focused_contents = GetFocusedWebContents();

  // If the focused WebContents is showing an interstitial, return the
  // interstitial's widget.
  if (focused_contents->ShowingInterstitialPage()) {
    return static_cast<RenderFrameHostImpl*>(
               focused_contents->GetRenderManager()
                   ->interstitial_page()
                   ->GetMainFrame())
        ->GetRenderWidgetHost();
  }

  // If the focused WebContents is a guest WebContents, then get the focused
  // frame in the embedder WebContents instead.
  FrameTreeNode* focused_frame = nullptr;
  if (focused_contents->browser_plugin_guest_ &&
      !GuestMode::IsCrossProcessFrameGuest(focused_contents)) {
    focused_frame = frame_tree_.GetFocusedFrame();
  } else {
    focused_frame = GetFocusedWebContents()->frame_tree_.GetFocusedFrame();
  }

  if (!focused_frame)
    return receiving_widget;

  // The view may be null if a subframe's renderer process has crashed while
  // the subframe has focus.  Drop the event in that case.  Do not give
  // it to the main frame, so that the user doesn't unexpectedly type into the
  // wrong frame if a focused subframe renderer crashes while they type.
  RenderWidgetHostView* view = focused_frame->current_frame_host()->GetView();
  if (!view)
    return nullptr;

  return RenderWidgetHostImpl::From(view->GetRenderWidgetHost());
}

RenderWidgetHostImpl* WebContentsImpl::GetRenderWidgetHostWithPageFocus() {
  WebContentsImpl* focused_web_contents = GetFocusedWebContents();

  if (focused_web_contents->ShowingInterstitialPage()) {
    return static_cast<RenderFrameHostImpl*>(
               focused_web_contents->GetRenderManager()
                   ->interstitial_page()
                   ->GetMainFrame())
        ->GetRenderWidgetHost();
  }

  return focused_web_contents->GetMainFrame()->GetRenderWidgetHost();
}

void WebContentsImpl::EnterFullscreenMode(const GURL& origin) {
  // This method is being called to enter renderer-initiated fullscreen mode.
  // Make sure any existing fullscreen widget is shut down first.
  RenderWidgetHostView* const widget_view = GetFullscreenRenderWidgetHostView();
  if (widget_view) {
    RenderWidgetHostImpl::From(widget_view->GetRenderWidgetHost())
        ->ShutdownAndDestroyWidget(true);
  }

  if (delegate_)
    delegate_->EnterFullscreenModeForTab(this, origin);

  for (auto& observer : observers_)
    observer.DidToggleFullscreenModeForTab(IsFullscreenForCurrentTab(), false);
}

void WebContentsImpl::ExitFullscreenMode(bool will_cause_resize) {
  // This method is being called to leave renderer-initiated fullscreen mode.
  // Make sure any existing fullscreen widget is shut down first.
  RenderWidgetHostView* const widget_view = GetFullscreenRenderWidgetHostView();
  if (widget_view) {
    RenderWidgetHostImpl::From(widget_view->GetRenderWidgetHost())
        ->ShutdownAndDestroyWidget(true);
  }

#if defined(OS_ANDROID)
  ContentVideoView* video_view = ContentVideoView::GetInstance();
  if (video_view != NULL)
    video_view->ExitFullscreen();
#endif

  if (delegate_)
    delegate_->ExitFullscreenModeForTab(this);

  // The fullscreen state is communicated to the renderer through a resize
  // message. If the change in fullscreen state doesn't cause a view resize
  // then we must ensure web contents exit the fullscreen state by explicitly
  // sending a resize message. This is required for the situation of the browser
  // moving the view into a "browser fullscreen" state and then the contents
  // entering "tab fullscreen". Exiting the contents "tab fullscreen" then won't
  // have the side effect of the view resizing, hence the explicit call here is
  // required.
  if (!will_cause_resize) {
    if (RenderWidgetHostView* rwhv = GetRenderWidgetHostView()) {
        if (RenderWidgetHost* render_widget_host = rwhv->GetRenderWidgetHost())
          render_widget_host->WasResized();
    }
  }

  for (auto& observer : observers_) {
    observer.DidToggleFullscreenModeForTab(IsFullscreenForCurrentTab(),
                                           will_cause_resize);
  }
}

bool WebContentsImpl::IsFullscreenForCurrentTab() const {
  return delegate_ ? delegate_->IsFullscreenForTabOrPending(this) : false;
}

bool WebContentsImpl::IsFullscreen() {
  return IsFullscreenForCurrentTab();
}

blink::WebDisplayMode WebContentsImpl::GetDisplayMode(
    RenderWidgetHostImpl* render_widget_host) const {
  if (!RenderViewHostImpl::From(render_widget_host))
    return blink::kWebDisplayModeBrowser;

  return delegate_ ? delegate_->GetDisplayMode(this)
                   : blink::kWebDisplayModeBrowser;
}

void WebContentsImpl::RequestToLockMouse(
    RenderWidgetHostImpl* render_widget_host,
    bool user_gesture,
    bool last_unlocked_by_target,
    bool privileged) {
  for (WebContentsImpl* current = this; current;
       current = current->GetOuterWebContents()) {
    if (current->mouse_lock_widget_) {
      render_widget_host->GotResponseToLockMouseRequest(false);
      return;
    }
  }

  if (privileged) {
    DCHECK(!GetOuterWebContents());
    mouse_lock_widget_ = render_widget_host;
    render_widget_host->GotResponseToLockMouseRequest(true);
    return;
  }

  bool widget_in_frame_tree = false;
  for (FrameTreeNode* node : frame_tree_.Nodes()) {
    if (node->current_frame_host()->GetRenderWidgetHost() ==
        render_widget_host) {
      widget_in_frame_tree = true;
      break;
    }
  }

  if (widget_in_frame_tree && delegate_) {
    for (WebContentsImpl* current = this; current;
         current = current->GetOuterWebContents()) {
      current->mouse_lock_widget_ = render_widget_host;
    }

    delegate_->RequestToLockMouse(this, user_gesture, last_unlocked_by_target);
  } else {
    render_widget_host->GotResponseToLockMouseRequest(false);
  }
}

void WebContentsImpl::LostMouseLock(RenderWidgetHostImpl* render_widget_host) {
  CHECK(mouse_lock_widget_);

  if (mouse_lock_widget_->delegate()->GetAsWebContents() != this)
    return mouse_lock_widget_->delegate()->LostMouseLock(render_widget_host);

  mouse_lock_widget_->SendMouseLockLost();
  for (WebContentsImpl* current = this; current;
       current = current->GetOuterWebContents()) {
    current->mouse_lock_widget_ = nullptr;
  }

  if (delegate_)
    delegate_->LostMouseLock();
}

bool WebContentsImpl::HasMouseLock(RenderWidgetHostImpl* render_widget_host) {
  // To verify if the mouse is locked, the mouse_lock_widget_ needs to be
  // assigned to the widget that requested the mouse lock, and the top-level
  // platform RenderWidgetHostView needs to hold the mouse lock from the OS.
  return mouse_lock_widget_ == render_widget_host &&
         GetTopLevelRenderWidgetHostView()->IsMouseLocked();
}

RenderWidgetHostImpl* WebContentsImpl::GetMouseLockWidget() {
  if (GetTopLevelRenderWidgetHostView()->IsMouseLocked() ||
      (GetFullscreenRenderWidgetHostView() &&
       GetFullscreenRenderWidgetHostView()->IsMouseLocked()))
    return mouse_lock_widget_;

  return nullptr;
}

void WebContentsImpl::OnRenderFrameProxyVisibilityChanged(bool visible) {
  if (visible && !GetOuterWebContents()->IsHidden())
    WasShown();
  else if (!visible)
    WasHidden();
}

void WebContentsImpl::CreateNewWindow(
    RenderFrameHost* opener,
    int32_t render_view_route_id,
    int32_t main_frame_route_id,
    int32_t main_frame_widget_route_id,
    const mojom::CreateNewWindowParams& params,
    SessionStorageNamespace* session_storage_namespace) {
  // We should have zero valid routing ids, or three valid routing IDs.
  DCHECK_EQ((render_view_route_id == MSG_ROUTING_NONE),
            (main_frame_route_id == MSG_ROUTING_NONE));
  DCHECK_EQ((render_view_route_id == MSG_ROUTING_NONE),
            (main_frame_widget_route_id == MSG_ROUTING_NONE));
  DCHECK(opener);

  int render_process_id = opener->GetProcess()->GetID();
  SiteInstance* source_site_instance = opener->GetSiteInstance();

  // The route IDs passed into this function can be trusted not to already
  // be in use; they were allocated by the RenderWidgetHelper by the caller.
  DCHECK(!RenderFrameHostImpl::FromID(render_process_id, main_frame_route_id));

  // We usually create the new window in the same BrowsingInstance (group of
  // script-related windows), by passing in the current SiteInstance.  However,
  // if the opener is being suppressed (in a non-guest), we create a new
  // SiteInstance in its own BrowsingInstance.
  bool is_guest = BrowserPluginGuest::IsGuest(this);

  // If the opener is to be suppressed, the new window can be in any process.
  // Since routing ids are process specific, we must not have one passed in
  // as argument here.
  DCHECK(!params.opener_suppressed || render_view_route_id == MSG_ROUTING_NONE);

  scoped_refptr<SiteInstance> site_instance =
      params.opener_suppressed && !is_guest
          ? SiteInstance::CreateForURL(GetBrowserContext(), params.target_url)
          : source_site_instance;

  // We must assign the SessionStorageNamespace before calling Init().
  //
  // http://crbug.com/142685
  const std::string& partition_id =
      GetContentClient()->browser()->
          GetStoragePartitionIdForSite(GetBrowserContext(),
                                       site_instance->GetSiteURL());
  StoragePartition* partition = BrowserContext::GetStoragePartition(
      GetBrowserContext(), site_instance.get());
  DOMStorageContextWrapper* dom_storage_context =
      static_cast<DOMStorageContextWrapper*>(partition->GetDOMStorageContext());
  SessionStorageNamespaceImpl* session_storage_namespace_impl =
      static_cast<SessionStorageNamespaceImpl*>(session_storage_namespace);
  CHECK(session_storage_namespace_impl->IsFromContext(dom_storage_context));

  if (delegate_ &&
      !delegate_->ShouldCreateWebContents(
          this, opener, source_site_instance, render_view_route_id,
          main_frame_route_id, main_frame_widget_route_id,
          params.window_container_type, opener->GetLastCommittedURL(),
          params.frame_name, params.target_url, partition_id,
          session_storage_namespace)) {
    // Note: even though we're not creating a WebContents here, it could have
    // been created by the embedder so ensure that the RenderFrameHost is
    // properly initialized.
    // It's safe to only target the frame because the render process will not
    // have a chance to create more frames at this point.
    RenderFrameHostImpl* rfh =
        RenderFrameHostImpl::FromID(render_process_id, main_frame_route_id);
    if (rfh) {
      DCHECK(rfh->IsRenderFrameLive());
      rfh->Init();
    }
    return;
  }

  // Create the new web contents. This will automatically create the new
  // WebContentsView. In the future, we may want to create the view separately.
  CreateParams create_params(GetBrowserContext(), site_instance.get());
  create_params.routing_id = render_view_route_id;
  create_params.main_frame_routing_id = main_frame_route_id;
  create_params.main_frame_widget_routing_id = main_frame_widget_route_id;
  create_params.main_frame_name = params.frame_name;
  create_params.opener_render_process_id = render_process_id;
  create_params.opener_render_frame_id = opener->GetRoutingID();
  create_params.opener_suppressed = params.opener_suppressed;
  if (params.disposition == WindowOpenDisposition::NEW_BACKGROUND_TAB)
    create_params.initially_hidden = true;
  create_params.renderer_initiated_creation =
      main_frame_route_id != MSG_ROUTING_NONE;

  WebContentsImpl* new_contents = NULL;
  if (!is_guest) {
    create_params.context = view_->GetNativeView();
    create_params.initial_size = GetContainerBounds().size();
    new_contents = static_cast<WebContentsImpl*>(
        WebContents::Create(create_params));
  }  else {
    new_contents = GetBrowserPluginGuest()->CreateNewGuestWindow(create_params);
  }
  new_contents->GetController().SetSessionStorageNamespace(
      partition_id,
      session_storage_namespace);

  // If the new frame has a name, make sure any SiteInstances that can find
  // this named frame have proxies for it.  Must be called after
  // SetSessionStorageNamespace, since this calls CreateRenderView, which uses
  // GetSessionStorageNamespace.
  if (!params.frame_name.empty())
    new_contents->GetRenderManager()->CreateProxiesForNewNamedFrame();

  // Save the window for later if we're not suppressing the opener (since it
  // will be shown immediately).
  if (!params.opener_suppressed) {
    if (!is_guest) {
      WebContentsView* new_view = new_contents->view_.get();

      // TODO(brettw): It seems bogus that we have to call this function on the
      // newly created object and give it one of its own member variables.
      new_view->CreateViewForWidget(
          new_contents->GetRenderViewHost()->GetWidget(), false);
    }
    // Save the created window associated with the route so we can show it
    // later.
    DCHECK_NE(MSG_ROUTING_NONE, main_frame_widget_route_id);
    pending_contents_[std::make_pair(
        render_process_id, main_frame_widget_route_id)] = new_contents;
    AddDestructionObserver(new_contents);
  }

  if (delegate_) {
    delegate_->WebContentsCreated(
        this, render_process_id, opener->GetRoutingID(), params.frame_name,
        params.target_url, new_contents, create_params);
  }

  if (opener) {
    for (auto& observer : observers_) {
      observer.DidOpenRequestedURL(new_contents, opener, params.target_url,
                                   params.referrer, params.disposition,
                                   ui::PAGE_TRANSITION_LINK,
                                   false,  // started_from_context_menu
                                   true);  // renderer_initiated
    }
  }

  if (params.opener_suppressed) {
    // When the opener is suppressed, the original renderer cannot access the
    // new window.  As a result, we need to show and navigate the window here.
    bool was_blocked = false;
    if (delegate_) {
      gfx::Rect initial_rect;
      base::WeakPtr<WebContentsImpl> weak_new_contents =
          new_contents->weak_factory_.GetWeakPtr();

      delegate_->AddNewContents(
          this, new_contents, params.disposition, initial_rect,
          params.user_gesture, &was_blocked);

      if (!weak_new_contents)
        return;  // The delegate deleted |new_contents| during AddNewContents().
    }
    if (!was_blocked) {
      OpenURLParams open_params(params.target_url, params.referrer,
                                WindowOpenDisposition::CURRENT_TAB,
                                ui::PAGE_TRANSITION_LINK,
                                true /* is_renderer_initiated */);
      open_params.user_gesture = params.user_gesture;

      if (delegate_ && !is_guest &&
          !delegate_->ShouldResumeRequestsForCreatedWindow()) {
        // We are in asynchronous add new contents path, delay opening url
        new_contents->delayed_open_url_params_.reset(
            new OpenURLParams(open_params));
      } else {
        new_contents->OpenURL(open_params);
      }
    }
  }
}

void WebContentsImpl::CreateNewWidget(int32_t render_process_id,
                                      int32_t route_id,
                                      blink::WebPopupType popup_type) {
  CreateNewWidget(render_process_id, route_id, false, popup_type);
}

void WebContentsImpl::CreateNewFullscreenWidget(int32_t render_process_id,
                                                int32_t route_id) {
  CreateNewWidget(render_process_id, route_id, true, blink::kWebPopupTypeNone);
}

void WebContentsImpl::CreateNewWidget(int32_t render_process_id,
                                      int32_t route_id,
                                      bool is_fullscreen,
                                      blink::WebPopupType popup_type) {
  RenderProcessHost* process = RenderProcessHost::FromID(render_process_id);
  // A message to create a new widget can only come from an active process for
  // this WebContentsImpl instance. If any other process sends the request,
  // it is invalid and the process must be terminated.
  if (!HasMatchingProcess(&frame_tree_, render_process_id)) {
    base::ProcessHandle process_handle = process->GetHandle();
    if (process_handle != base::kNullProcessHandle) {
      RecordAction(
          base::UserMetricsAction("Terminate_ProcessMismatch_CreateNewWidget"));
      process->Shutdown(RESULT_CODE_KILLED, false);
    }
    return;
  }

  RenderWidgetHostImpl* widget_host =
      new RenderWidgetHostImpl(this, process, route_id, IsHidden());

  RenderWidgetHostViewBase* widget_view =
      static_cast<RenderWidgetHostViewBase*>(
          view_->CreateViewForPopupWidget(widget_host));
  if (!widget_view)
    return;
  if (!is_fullscreen) {
    // Popups should not get activated.
    widget_view->SetPopupType(popup_type);
  }
  // Save the created widget associated with the route so we can show it later.
  pending_widget_views_[std::make_pair(render_process_id, route_id)] =
      widget_view;

#if defined(OS_MACOSX)
  // A RenderWidgetHostViewMac has lifetime scoped to the view. We'll retain it
  // to allow it to survive the trip without being hosted.
  base::mac::NSObjectRetain(widget_view->GetNativeView());
#endif
}

void WebContentsImpl::ShowCreatedWindow(int process_id,
                                        int main_frame_widget_route_id,
                                        WindowOpenDisposition disposition,
                                        const gfx::Rect& initial_rect,
                                        bool user_gesture) {
  WebContentsImpl* popup =
      GetCreatedWindow(process_id, main_frame_widget_route_id);
  if (popup) {
    WebContentsDelegate* delegate = GetDelegate();
    popup->is_resume_pending_ = true;
    if (!delegate || delegate->ShouldResumeRequestsForCreatedWindow())
      popup->ResumeLoadingCreatedWebContents();

    if (delegate) {
      base::WeakPtr<WebContentsImpl> weak_popup =
          popup->weak_factory_.GetWeakPtr();
      delegate->AddNewContents(this, popup, disposition, initial_rect,
                               user_gesture, nullptr);
      if (!weak_popup)
        return;  // The delegate deleted |popup| during AddNewContents().
    }

    RenderWidgetHostImpl* rwh = popup->GetMainFrame()->GetRenderWidgetHost();
    DCHECK_EQ(main_frame_widget_route_id, rwh->GetRoutingID());
    rwh->Send(new ViewMsg_Move_ACK(rwh->GetRoutingID()));
  }
}

void WebContentsImpl::ShowCreatedWidget(int process_id,
                                        int route_id,
                                        const gfx::Rect& initial_rect) {
  ShowCreatedWidget(process_id, route_id, false, initial_rect);
}

void WebContentsImpl::ShowCreatedFullscreenWidget(int process_id,
                                                  int route_id) {
  ShowCreatedWidget(process_id, route_id, true, gfx::Rect());
}

void WebContentsImpl::ShowCreatedWidget(int process_id,
                                        int route_id,
                                        bool is_fullscreen,
                                        const gfx::Rect& initial_rect) {
  RenderWidgetHostViewBase* widget_host_view =
      static_cast<RenderWidgetHostViewBase*>(
          GetCreatedWidget(process_id, route_id));
  if (!widget_host_view)
    return;

  RenderWidgetHostView* view = NULL;
  if (GetOuterWebContents()) {
    view = GetOuterWebContents()->GetRenderWidgetHostView();
  } else {
    view = GetRenderWidgetHostView();
  }

  if (is_fullscreen) {
    DCHECK_EQ(MSG_ROUTING_NONE, fullscreen_widget_routing_id_);
    view_->StoreFocus();
    fullscreen_widget_process_id_ =
        widget_host_view->GetRenderWidgetHost()->GetProcess()->GetID();
    fullscreen_widget_routing_id_ = route_id;
    if (delegate_ && delegate_->EmbedsFullscreenWidget()) {
      widget_host_view->InitAsChild(GetRenderWidgetHostView()->GetNativeView());
      delegate_->EnterFullscreenModeForTab(this, GURL());
    } else {
      widget_host_view->InitAsFullscreen(view);
    }
    for (auto& observer : observers_)
      observer.DidShowFullscreenWidget();
    if (!widget_host_view->HasFocus())
      widget_host_view->Focus();
  } else {
    widget_host_view->InitAsPopup(view, initial_rect);
  }

  RenderWidgetHostImpl* render_widget_host_impl =
      RenderWidgetHostImpl::From(widget_host_view->GetRenderWidgetHost());
  render_widget_host_impl->Init();
  // Only allow privileged mouse lock for fullscreen render widget, which is
  // used to implement Pepper Flash fullscreen.
  render_widget_host_impl->set_allow_privileged_mouse_lock(is_fullscreen);

#if defined(OS_MACOSX)
  // A RenderWidgetHostViewMac has lifetime scoped to the view. Now that it's
  // properly embedded (or purposefully ignored) we can release the retain we
  // took in CreateNewWidget().
  base::mac::NSObjectRelease(widget_host_view->GetNativeView());
#endif
}

WebContentsImpl* WebContentsImpl::GetCreatedWindow(
    int process_id,
    int main_frame_widget_route_id) {
  auto key = std::make_pair(process_id, main_frame_widget_route_id);
  auto iter = pending_contents_.find(key);

  // Certain systems can block the creation of new windows. If we didn't succeed
  // in creating one, just return NULL.
  if (iter == pending_contents_.end())
    return nullptr;

  WebContentsImpl* new_contents = iter->second;
  pending_contents_.erase(key);
  RemoveDestructionObserver(new_contents);

  // Don't initialize the guest WebContents immediately.
  if (BrowserPluginGuest::IsGuest(new_contents))
    return new_contents;

  if (!new_contents->GetMainFrame()->GetProcess()->HasConnection() ||
      !new_contents->GetMainFrame()->GetView()) {
    // TODO(nick): http://crbug.com/674318 -- Who deletes |new_contents|?
    return nullptr;
  }

  return new_contents;
}

RenderWidgetHostView* WebContentsImpl::GetCreatedWidget(int process_id,
                                                        int route_id) {
  auto iter = pending_widget_views_.find(std::make_pair(process_id, route_id));
  if (iter == pending_widget_views_.end()) {
    DCHECK(false);
    return nullptr;
  }

  RenderWidgetHostView* widget_host_view = iter->second;
  pending_widget_views_.erase(std::make_pair(process_id, route_id));

  RenderWidgetHost* widget_host = widget_host_view->GetRenderWidgetHost();
  if (!widget_host->GetProcess()->HasConnection()) {
    // The view has gone away or the renderer crashed. Nothing to do.
    return nullptr;
  }

  return widget_host_view;
}

void WebContentsImpl::RequestMediaAccessPermission(
    const MediaStreamRequest& request,
    const MediaResponseCallback& callback) {
  if (delegate_) {
    delegate_->RequestMediaAccessPermission(this, request, callback);
  } else {
    callback.Run(MediaStreamDevices(), MEDIA_DEVICE_FAILED_DUE_TO_SHUTDOWN,
                 std::unique_ptr<MediaStreamUI>());
  }
}

bool WebContentsImpl::CheckMediaAccessPermission(const GURL& security_origin,
                                                 MediaStreamType type) {
  DCHECK(type == MEDIA_DEVICE_AUDIO_CAPTURE ||
         type == MEDIA_DEVICE_VIDEO_CAPTURE);
  return delegate_ &&
         delegate_->CheckMediaAccessPermission(this, security_origin, type);
}

std::string WebContentsImpl::GetDefaultMediaDeviceID(MediaStreamType type) {
  if (!delegate_)
    return std::string();
  return delegate_->GetDefaultMediaDeviceID(this, type);
}

SessionStorageNamespace* WebContentsImpl::GetSessionStorageNamespace(
    SiteInstance* instance) {
  return controller_.GetSessionStorageNamespace(instance);
}

SessionStorageNamespaceMap WebContentsImpl::GetSessionStorageNamespaceMap() {
  return controller_.GetSessionStorageNamespaceMap();
}

FrameTree* WebContentsImpl::GetFrameTree() {
  return &frame_tree_;
}

void WebContentsImpl::SetIsVirtualKeyboardRequested(bool requested) {
  virtual_keyboard_requested_ = requested;
}

bool WebContentsImpl::IsVirtualKeyboardRequested() {
  return virtual_keyboard_requested_;
}

bool WebContentsImpl::IsOverridingUserAgent() {
  return GetController().GetVisibleEntry() &&
         GetController().GetVisibleEntry()->GetIsOverridingUserAgent();
}

bool WebContentsImpl::IsJavaScriptDialogShowing() const {
  return is_showing_javascript_dialog_;
}

bool WebContentsImpl::ShouldIgnoreUnresponsiveRenderer() {
  // Ignore unresponsive renderers if the debugger is attached to them since the
  // unresponsiveness might be a result of the renderer sitting on a breakpoint.
  //
  // TODO(pfeldman): Fix this to only return true if the renderer is *actually*
  // sitting on a breakpoint. https://crbug.com/684202
  return DevToolsAgentHost::IsDebuggerAttached(this);
}

AccessibilityMode WebContentsImpl::GetAccessibilityMode() const {
  return accessibility_mode_;
}

void WebContentsImpl::AccessibilityEventReceived(
    const std::vector<AXEventNotificationDetails>& details) {
  for (auto& observer : observers_)
    observer.AccessibilityEventReceived(details);
}

void WebContentsImpl::AccessibilityLocationChangesReceived(
    const std::vector<AXLocationChangeNotificationDetails>& details) {
  for (auto& observer : observers_)
    observer.AccessibilityLocationChangesReceived(details);
}

RenderFrameHost* WebContentsImpl::GetGuestByInstanceID(
    RenderFrameHost* render_frame_host,
    int browser_plugin_instance_id) {
  BrowserPluginGuestManager* guest_manager =
      GetBrowserContext()->GetGuestManager();
  if (!guest_manager)
    return nullptr;

  WebContents* guest = guest_manager->GetGuestByInstanceID(
      render_frame_host->GetProcess()->GetID(), browser_plugin_instance_id);
  if (!guest)
    return nullptr;

  return guest->GetMainFrame();
}

device::GeolocationServiceContext*
WebContentsImpl::GetGeolocationServiceContext() {
  return geolocation_service_context_.get();
}

device::mojom::WakeLockContext* WebContentsImpl::GetWakeLockContext() {
  if (!wake_lock_context_host_)
    wake_lock_context_host_.reset(new WakeLockContextHost(this));
  return wake_lock_context_host_->GetWakeLockContext();
}

device::mojom::WakeLock* WebContentsImpl::GetRendererWakeLock() {
  // WebContents creates a long-lived connection to one WakeLock.
  // All the frames' requests will be added into the BindingSet of
  // WakeLock via this connection.
  if (!renderer_wake_lock_) {
    device::mojom::WakeLockContext* wake_lock_context = GetWakeLockContext();
    if (!wake_lock_context) {
      return nullptr;
    }
    wake_lock_context->GetWakeLock(
        device::mojom::WakeLockType::PreventDisplaySleep,
        device::mojom::WakeLockReason::ReasonOther, "Wake Lock API",
        mojo::MakeRequest(&renderer_wake_lock_));
  }
  return renderer_wake_lock_.get();
}

#if defined(OS_ANDROID)
void WebContentsImpl::GetNFC(device::mojom::NFCRequest request) {
  if (!nfc_host_)
    nfc_host_.reset(new NFCHost(this));
  nfc_host_->GetNFC(std::move(request));
}
#endif

void WebContentsImpl::OnShowValidationMessage(
    RenderViewHostImpl* source,
    const gfx::Rect& anchor_in_root_view,
    const base::string16& main_text,
    const base::string16& sub_text) {
  // TODO(nick): Should we consider |source| here or pass it to the delegate?
  if (delegate_)
    delegate_->ShowValidationMessage(
        this, anchor_in_root_view, main_text, sub_text);
}

void WebContentsImpl::OnHideValidationMessage(RenderViewHostImpl* source) {
  // TODO(nick): Should we consider |source| here or pass it to the delegate?
  if (delegate_)
    delegate_->HideValidationMessage(this);
}

void WebContentsImpl::OnMoveValidationMessage(
    RenderViewHostImpl* source,
    const gfx::Rect& anchor_in_root_view) {
  // TODO(nick): Should we consider |source| here or pass it to the delegate?
  if (delegate_)
    delegate_->MoveValidationMessage(this, anchor_in_root_view);
}

void WebContentsImpl::SendScreenRects() {
  for (FrameTreeNode* node : frame_tree_.Nodes()) {
    if (node->current_frame_host()->is_local_root())
      node->current_frame_host()->GetRenderWidgetHost()->SendScreenRects();
  }

  RenderWidgetHostViewBase* rwhv =
      static_cast<RenderWidgetHostViewBase*>(GetRenderWidgetHostView());
  if (rwhv) {
    SendPageMessage(new PageMsg_UpdateWindowScreenRect(
        MSG_ROUTING_NONE, rwhv->GetBoundsInRootWindow()));
  }

  if (browser_plugin_embedder_ && !is_being_destroyed_)
    browser_plugin_embedder_->DidSendScreenRects();
}

TextInputManager* WebContentsImpl::GetTextInputManager() {
  if (GetOuterWebContents())
    return GetOuterWebContents()->GetTextInputManager();

  if (!text_input_manager_)
    text_input_manager_.reset(new TextInputManager());

  return text_input_manager_.get();
}

bool WebContentsImpl::OnUpdateDragCursor() {
  if (browser_plugin_embedder_)
    return browser_plugin_embedder_->OnUpdateDragCursor();
  return false;
}

bool WebContentsImpl::IsWidgetForMainFrame(
    RenderWidgetHostImpl* render_widget_host) {
  return render_widget_host == GetMainFrame()->GetRenderWidgetHost();
}

BrowserAccessibilityManager*
    WebContentsImpl::GetRootBrowserAccessibilityManager() {
  RenderFrameHostImpl* rfh = GetMainFrame();
  return rfh ? rfh->browser_accessibility_manager() : nullptr;
}

BrowserAccessibilityManager*
    WebContentsImpl::GetOrCreateRootBrowserAccessibilityManager() {
  RenderFrameHostImpl* rfh = GetMainFrame();
  return rfh ? rfh->GetOrCreateBrowserAccessibilityManager() : nullptr;
}

void WebContentsImpl::MoveRangeSelectionExtent(const gfx::Point& extent) {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  focused_frame->GetFrameInputHandler()->MoveRangeSelectionExtent(extent);
}

void WebContentsImpl::SelectRange(const gfx::Point& base,
                                  const gfx::Point& extent) {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  focused_frame->GetFrameInputHandler()->SelectRange(base, extent);
}

void WebContentsImpl::AdjustSelectionByCharacterOffset(int start_adjust,
                                                       int end_adjust) {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  focused_frame->GetFrameInputHandler()->AdjustSelectionByCharacterOffset(
      start_adjust, end_adjust);
}

void WebContentsImpl::UpdatePreferredSize(const gfx::Size& pref_size) {
  const gfx::Size old_size = GetPreferredSize();
  preferred_size_ = pref_size;
  OnPreferredSizeChanged(old_size);
}

void WebContentsImpl::ResizeDueToAutoResize(
    RenderWidgetHostImpl* render_widget_host,
    const gfx::Size& new_size) {
  if (render_widget_host != GetRenderViewHost()->GetWidget())
    return;

  auto_resize_size_ = new_size;

  // Out-of-process iframe visible viewport sizes usually come from the
  // top-level RenderWidgetHostView, but when auto-resize is enabled on the
  // top frame then that size is used instead.
  for (FrameTreeNode* node : frame_tree_.Nodes()) {
    if (node->current_frame_host()->is_local_root()) {
      RenderWidgetHostImpl* host =
          node->current_frame_host()->GetRenderWidgetHost();
      if (host != render_widget_host)
        host->WasResized();
    }
  }

  if (delegate_)
    delegate_->ResizeDueToAutoResize(this, new_size);
}

gfx::Size WebContentsImpl::GetAutoResizeSize() {
  return auto_resize_size_;
}

void WebContentsImpl::ResetAutoResizeSize() {
  auto_resize_size_ = gfx::Size();
}

WebContents* WebContentsImpl::OpenURL(const OpenURLParams& params) {
  if (!delegate_)
    return NULL;

  WebContents* new_contents = delegate_->OpenURLFromTab(this, params);

  RenderFrameHost* source_render_frame_host = RenderFrameHost::FromID(
      params.source_render_process_id, params.source_render_frame_id);

  if (source_render_frame_host && params.source_site_instance) {
    CHECK_EQ(source_render_frame_host->GetSiteInstance(),
             params.source_site_instance.get());
  }

  if (new_contents && source_render_frame_host && new_contents != this) {
    for (auto& observer : observers_) {
      observer.DidOpenRequestedURL(new_contents, source_render_frame_host,
                                   params.url, params.referrer,
                                   params.disposition, params.transition,
                                   params.started_from_context_menu, false);
    }
  }

  return new_contents;
}

bool WebContentsImpl::Send(IPC::Message* message) {
  if (!GetRenderViewHost()) {
    delete message;
    return false;
  }

  return GetRenderViewHost()->Send(message);
}

void WebContentsImpl::RenderFrameForInterstitialPageCreated(
    RenderFrameHost* render_frame_host) {
  for (auto& observer : observers_)
    observer.RenderFrameForInterstitialPageCreated(render_frame_host);
}

void WebContentsImpl::AttachInterstitialPage(
    InterstitialPageImpl* interstitial_page) {
  DCHECK(interstitial_page);
  GetRenderManager()->set_interstitial_page(interstitial_page);

  // Cancel any visible dialogs so that they don't interfere with the
  // interstitial.
  CancelActiveAndPendingDialogs();

  for (auto& observer : observers_)
    observer.DidAttachInterstitialPage();

  // Stop the throbber if needed while the interstitial page is shown.
  if (frame_tree_.IsLoading())
    LoadingStateChanged(true, true, nullptr);

  // Connect to outer WebContents if necessary.
  if (node_.OuterContentsFrameTreeNode()) {
    if (GetRenderManager()->GetProxyToOuterDelegate()) {
      DCHECK(
          static_cast<RenderWidgetHostViewBase*>(interstitial_page->GetView())
              ->IsRenderWidgetHostViewChildFrame());
      RenderWidgetHostViewChildFrame* view =
          static_cast<RenderWidgetHostViewChildFrame*>(
              interstitial_page->GetView());
      GetRenderManager()->SetRWHViewForInnerContents(view);
    }
  }
}

void WebContentsImpl::DidProceedOnInterstitial() {
  // The interstitial page should no longer be pausing the throbber.
  DCHECK(!(ShowingInterstitialPage() &&
           GetRenderManager()->interstitial_page()->pause_throbber()));

  // Restart the throbber now that the interstitial page no longer pauses it.
  if (ShowingInterstitialPage() && frame_tree_.IsLoading())
    LoadingStateChanged(true, true, nullptr);
}

void WebContentsImpl::DetachInterstitialPage() {
  // Disconnect from outer WebContents if necessary.
  if (node_.OuterContentsFrameTreeNode()) {
    if (GetRenderManager()->GetProxyToOuterDelegate()) {
      DCHECK(static_cast<RenderWidgetHostViewBase*>(
                 GetRenderManager()->current_frame_host()->GetView())
                 ->IsRenderWidgetHostViewChildFrame());
      RenderWidgetHostViewChildFrame* view =
          static_cast<RenderWidgetHostViewChildFrame*>(
              GetRenderManager()->current_frame_host()->GetView());
      GetRenderManager()->SetRWHViewForInnerContents(view);
    }
  }

  bool interstitial_pausing_throbber =
      ShowingInterstitialPage() &&
      GetRenderManager()->interstitial_page()->pause_throbber();
  if (ShowingInterstitialPage())
    GetRenderManager()->remove_interstitial_page();
  for (auto& observer : observers_)
    observer.DidDetachInterstitialPage();

  // Restart the throbber if needed now that the interstitial page is going
  // away.
  if (interstitial_pausing_throbber && frame_tree_.IsLoading())
    LoadingStateChanged(true, true, nullptr);
}

void WebContentsImpl::SetHistoryOffsetAndLength(int history_offset,
                                                int history_length) {
  SendPageMessage(new PageMsg_SetHistoryOffsetAndLength(
      MSG_ROUTING_NONE, history_offset, history_length));
}

void WebContentsImpl::SetHistoryOffsetAndLengthForView(
    RenderViewHost* render_view_host,
    int history_offset,
    int history_length) {
  render_view_host->Send(new PageMsg_SetHistoryOffsetAndLength(
      render_view_host->GetRoutingID(), history_offset, history_length));
}

void WebContentsImpl::ReloadFocusedFrame(bool bypass_cache) {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  focused_frame->Send(new FrameMsg_Reload(
      focused_frame->GetRoutingID(), bypass_cache));
}

void WebContentsImpl::ReloadLoFiImages() {
  SendToAllFrames(new FrameMsg_ReloadLoFiImages(MSG_ROUTING_NONE));
}

void WebContentsImpl::Undo() {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  focused_frame->GetFrameInputHandler()->Undo();
  RecordAction(base::UserMetricsAction("Undo"));
}

void WebContentsImpl::Redo() {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;
  focused_frame->GetFrameInputHandler()->Redo();
  RecordAction(base::UserMetricsAction("Redo"));
}

void WebContentsImpl::Cut() {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  focused_frame->GetFrameInputHandler()->Cut();
  RecordAction(base::UserMetricsAction("Cut"));
}

void WebContentsImpl::Copy() {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  focused_frame->GetFrameInputHandler()->Copy();
  RecordAction(base::UserMetricsAction("Copy"));
}

void WebContentsImpl::CopyToFindPboard() {
#if defined(OS_MACOSX)
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  // Windows/Linux don't have the concept of a find pasteboard.
  focused_frame->GetFrameInputHandler()->CopyToFindPboard();
  RecordAction(base::UserMetricsAction("CopyToFindPboard"));
#endif
}

void WebContentsImpl::Paste() {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  focused_frame->GetFrameInputHandler()->Paste();
  RecordAction(base::UserMetricsAction("Paste"));
}

void WebContentsImpl::PasteAndMatchStyle() {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  focused_frame->GetFrameInputHandler()->PasteAndMatchStyle();
  RecordAction(base::UserMetricsAction("PasteAndMatchStyle"));
}

void WebContentsImpl::Delete() {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  focused_frame->GetFrameInputHandler()->Delete();
  RecordAction(base::UserMetricsAction("DeleteSelection"));
}

void WebContentsImpl::SelectAll() {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  focused_frame->GetFrameInputHandler()->SelectAll();
  RecordAction(base::UserMetricsAction("SelectAll"));
}

void WebContentsImpl::CollapseSelection() {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  focused_frame->GetFrameInputHandler()->CollapseSelection();
}

void WebContentsImpl::Replace(const base::string16& word) {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  focused_frame->GetFrameInputHandler()->Replace(word);
}

void WebContentsImpl::ReplaceMisspelling(const base::string16& word) {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  focused_frame->GetFrameInputHandler()->ReplaceMisspelling(word);
}

void WebContentsImpl::NotifyContextMenuClosed(
    const CustomContextMenuContext& context) {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  focused_frame->Send(new FrameMsg_ContextMenuClosed(
      focused_frame->GetRoutingID(), context));
}

void WebContentsImpl::ExecuteCustomContextMenuCommand(
    int action, const CustomContextMenuContext& context) {
  RenderFrameHost* focused_frame = GetFocusedFrame();
  if (!focused_frame)
    return;

  focused_frame->Send(new FrameMsg_CustomContextMenuAction(
      focused_frame->GetRoutingID(), context, action));
}

gfx::NativeView WebContentsImpl::GetNativeView() {
  return view_->GetNativeView();
}

gfx::NativeView WebContentsImpl::GetContentNativeView() {
  return view_->GetContentNativeView();
}

gfx::NativeWindow WebContentsImpl::GetTopLevelNativeWindow() {
  return view_->GetTopLevelNativeWindow();
}

gfx::Rect WebContentsImpl::GetViewBounds() {
  return view_->GetViewBounds();
}

gfx::Rect WebContentsImpl::GetContainerBounds() {
  gfx::Rect rv;
  view_->GetContainerBounds(&rv);
  return rv;
}

DropData* WebContentsImpl::GetDropData() {
  return view_->GetDropData();
}

void WebContentsImpl::Focus() {
  view_->Focus();
}

void WebContentsImpl::SetInitialFocus() {
  view_->SetInitialFocus();
}

void WebContentsImpl::StoreFocus() {
  view_->StoreFocus();
}

void WebContentsImpl::RestoreFocus() {
  view_->RestoreFocus();
}

void WebContentsImpl::FocusThroughTabTraversal(bool reverse) {
  if (ShowingInterstitialPage()) {
    GetRenderManager()->interstitial_page()->FocusThroughTabTraversal(reverse);
    return;
  }
  RenderWidgetHostView* const fullscreen_view =
      GetFullscreenRenderWidgetHostView();
  if (fullscreen_view) {
    fullscreen_view->Focus();
    return;
  }
  GetRenderViewHost()->SetInitialFocus(reverse);
}

bool WebContentsImpl::ShowingInterstitialPage() const {
  return GetRenderManager()->interstitial_page() != NULL;
}

InterstitialPage* WebContentsImpl::GetInterstitialPage() const {
  return GetRenderManager()->interstitial_page();
}

bool WebContentsImpl::IsSavable() {
  // WebKit creates Document object when MIME type is application/xhtml+xml,
  // so we also support this MIME type.
  return contents_mime_type_ == "text/html" ||
         contents_mime_type_ == "text/xml" ||
         contents_mime_type_ == "application/xhtml+xml" ||
         contents_mime_type_ == "text/plain" ||
         contents_mime_type_ == "text/css" ||
         mime_util::IsSupportedJavascriptMimeType(contents_mime_type_);
}

void WebContentsImpl::OnSavePage() {
  // If we can not save the page, try to download it.
  if (!IsSavable()) {
    RecordDownloadSource(INITIATED_BY_SAVE_PACKAGE_ON_NON_HTML);
    SaveFrame(GetLastCommittedURL(), Referrer());
    return;
  }

  Stop();

  // Create the save package and possibly prompt the user for the name to save
  // the page as. The user prompt is an asynchronous operation that runs on
  // another thread.
  save_package_ = new SavePackage(this);
  save_package_->GetSaveInfo();
}

// Used in automated testing to bypass prompting the user for file names.
// Instead, the names and paths are hard coded rather than running them through
// file name sanitation and extension / mime checking.
bool WebContentsImpl::SavePage(const base::FilePath& main_file,
                               const base::FilePath& dir_path,
                               SavePageType save_type) {
  // Stop the page from navigating.
  Stop();

  save_package_ = new SavePackage(this, save_type, main_file, dir_path);
  return save_package_->Init(SavePackageDownloadCreatedCallback());
}

void WebContentsImpl::SaveFrame(const GURL& url,
                                const Referrer& referrer) {
  SaveFrameWithHeaders(url, referrer, std::string());
}

void WebContentsImpl::SaveFrameWithHeaders(const GURL& url,
                                           const Referrer& referrer,
                                           const std::string& headers) {
  if (!GetLastCommittedURL().is_valid())
    return;
  if (delegate_ && delegate_->SaveFrame(url, referrer))
    return;

  // TODO(nasko): This check for main frame is incorrect and should be fixed
  // by explicitly passing in which frame this method should target. This would
  // indicate whether it's the main frame, and also tell us the frame pointer
  // to use for routing.
  bool is_main_frame = (url == GetLastCommittedURL());
  RenderFrameHost* frame_host = GetMainFrame();

  StoragePartition* storage_partition = BrowserContext::GetStoragePartition(
      GetBrowserContext(), frame_host->GetSiteInstance());
  int64_t post_id = -1;
  if (is_main_frame) {
    const NavigationEntry* entry = controller_.GetLastCommittedEntry();
    if (entry)
      post_id = entry->GetPostID();
  }
  auto params = base::MakeUnique<DownloadUrlParameters>(
      url, frame_host->GetProcess()->GetID(),
      frame_host->GetRenderViewHost()->GetRoutingID(),
      frame_host->GetRoutingID(), storage_partition->GetURLRequestContext());
  params->set_referrer(referrer);
  params->set_post_id(post_id);
  if (post_id >= 0)
    params->set_method("POST");
  params->set_prompt(true);

  if (headers.empty()) {
    params->set_prefer_cache(true);
  } else {
    for (const base::StringPiece& key_value :
         base::SplitStringPiece(
             headers, "\n", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL)) {
      std::vector<std::string> pair = base::SplitString(
          key_value, ":", base::TRIM_WHITESPACE, base::SPLIT_WANT_ALL);
      DCHECK_EQ(2ul, pair.size());
      params->add_request_header(pair[0], pair[1]);
    }
  }
  BrowserContext::GetDownloadManager(GetBrowserContext())
      ->DownloadUrl(std::move(params), NO_TRAFFIC_ANNOTATION_YET);
}

void WebContentsImpl::GenerateMHTML(
    const MHTMLGenerationParams& params,
    const base::Callback<void(int64_t)>& callback) {
  MHTMLGenerationManager::GetInstance()->SaveMHTML(this, params, callback);
}

const std::string& WebContentsImpl::GetContentsMimeType() const {
  return contents_mime_type_;
}

bool WebContentsImpl::WillNotifyDisconnection() const {
  return notify_disconnection_;
}

RendererPreferences* WebContentsImpl::GetMutableRendererPrefs() {
  return &renderer_preferences_;
}

void WebContentsImpl::Close() {
  Close(GetRenderViewHost());
}

void WebContentsImpl::DragSourceEndedAt(int client_x,
                                        int client_y,
                                        int screen_x,
                                        int screen_y,
                                        blink::WebDragOperation operation,
                                        RenderWidgetHost* source_rwh) {
  if (browser_plugin_embedder_.get())
    browser_plugin_embedder_->DragSourceEndedAt(
        client_x, client_y, screen_x, screen_y, operation);
  if (source_rwh) {
    source_rwh->DragSourceEndedAt(gfx::Point(client_x, client_y),
                                  gfx::Point(screen_x, screen_y),
                                  operation);
  }
}

void WebContentsImpl::LoadStateChanged(
    const GURL& url,
    const net::LoadStateWithParam& load_state,
    uint64_t upload_position,
    uint64_t upload_size) {
  // TODO(erikchen): Remove ScopedTracker below once http://crbug.com/466285
  // is fixed.
  tracked_objects::ScopedTracker tracking_profile1(
      FROM_HERE_WITH_EXPLICIT_FUNCTION(
          "466285 WebContentsImpl::LoadStateChanged::Start"));
  load_state_ = load_state;
  upload_position_ = upload_position;
  upload_size_ = upload_size;
  load_state_host_ = url_formatter::IDNToUnicode(url.host());
  if (load_state_.state == net::LOAD_STATE_READING_RESPONSE)
    SetNotWaitingForResponse();
  if (IsLoading()) {
    NotifyNavigationStateChanged(static_cast<InvalidateTypes>(
        INVALIDATE_TYPE_LOAD | INVALIDATE_TYPE_TAB));
  }
}

void WebContentsImpl::DidGetResourceResponseStart(
  const ResourceRequestDetails& details) {
  controller_.ssl_manager()->DidStartResourceResponse(
      details.url, details.has_certificate, details.ssl_cert_status);

  for (auto& observer : observers_)
    observer.DidGetResourceResponseStart(details);
}

void WebContentsImpl::DidGetRedirectForResourceRequest(
  const ResourceRedirectDetails& details) {
  for (auto& observer : observers_)
    observer.DidGetRedirectForResourceRequest(details);

  // TODO(avi): Remove. http://crbug.com/170921
  NotificationService::current()->Notify(
      NOTIFICATION_RESOURCE_RECEIVED_REDIRECT,
      Source<WebContents>(this),
      Details<const ResourceRedirectDetails>(&details));
}

void WebContentsImpl::NotifyWebContentsFocused() {
  for (auto& observer : observers_)
    observer.OnWebContentsFocused();
}

void WebContentsImpl::NotifyWebContentsLostFocus() {
  for (auto& observer : observers_)
    observer.OnWebContentsLostFocus();
}

void WebContentsImpl::SystemDragEnded(RenderWidgetHost* source_rwh) {
  if (source_rwh)
    source_rwh->DragSourceSystemDragEnded();
  if (browser_plugin_embedder_.get())
    browser_plugin_embedder_->SystemDragEnded();
}

void WebContentsImpl::UserGestureDone() {
  OnUserInteraction(GetRenderViewHost()->GetWidget(),
                    blink::WebInputEvent::kUndefined);
}

void WebContentsImpl::SetClosedByUserGesture(bool value) {
  closed_by_user_gesture_ = value;
}

bool WebContentsImpl::GetClosedByUserGesture() const {
  return closed_by_user_gesture_;
}

void WebContentsImpl::ViewSource() {
  if (!delegate_)
    return;

  NavigationEntry* entry = GetController().GetLastCommittedEntry();
  if (!entry)
    return;

  delegate_->ViewSourceForTab(this, entry->GetURL());
}

void WebContentsImpl::ViewFrameSource(const GURL& url,
                                      const PageState& page_state) {
  if (!delegate_)
    return;

  delegate_->ViewSourceForFrame(this, url, page_state);
}

int WebContentsImpl::GetMinimumZoomPercent() const {
  return minimum_zoom_percent_;
}

int WebContentsImpl::GetMaximumZoomPercent() const {
  return maximum_zoom_percent_;
}

void WebContentsImpl::SetPageScale(float page_scale_factor) {
  Send(new ViewMsg_SetPageScale(GetRenderViewHost()->GetRoutingID(),
                                page_scale_factor));
}

gfx::Size WebContentsImpl::GetPreferredSize() const {
  return capturer_count_ == 0 ? preferred_size_ : preferred_size_for_capture_;
}

bool WebContentsImpl::GotResponseToLockMouseRequest(bool allowed) {
  if (!GuestMode::IsCrossProcessFrameGuest(GetWebContents()) &&
      GetBrowserPluginGuest())
    return GetBrowserPluginGuest()->LockMouse(allowed);

  if (mouse_lock_widget_) {
    if (mouse_lock_widget_->delegate()->GetAsWebContents() != this) {
      return mouse_lock_widget_->delegate()
          ->GetAsWebContents()
          ->GotResponseToLockMouseRequest(allowed);
    }

    if (mouse_lock_widget_->GotResponseToLockMouseRequest(allowed))
      return true;
  }

  for (WebContentsImpl* current = this; current;
       current = current->GetOuterWebContents()) {
    current->mouse_lock_widget_ = nullptr;
  }

  return false;
}

bool WebContentsImpl::HasOpener() const {
  return GetOpener() != NULL;
}

RenderFrameHostImpl* WebContentsImpl::GetOpener() const {
  FrameTreeNode* opener_ftn = frame_tree_.root()->opener();
  return opener_ftn ? opener_ftn->current_frame_host() : nullptr;
}

bool WebContentsImpl::HasOriginalOpener() const {
  return GetOriginalOpener() != NULL;
}

RenderFrameHostImpl* WebContentsImpl::GetOriginalOpener() const {
  FrameTreeNode* opener_ftn = frame_tree_.root()->original_opener();
  return opener_ftn ? opener_ftn->current_frame_host() : nullptr;
}

void WebContentsImpl::DidChooseColorInColorChooser(SkColor color) {
  if (!color_chooser_info_.get())
    return;
  RenderFrameHost* rfh = RenderFrameHost::FromID(
      color_chooser_info_->render_process_id,
      color_chooser_info_->render_frame_id);
  if (!rfh)
    return;

  rfh->Send(new FrameMsg_DidChooseColorResponse(
      rfh->GetRoutingID(), color_chooser_info_->identifier, color));
}

void WebContentsImpl::DidEndColorChooser() {
  if (!color_chooser_info_.get())
    return;
  RenderFrameHost* rfh = RenderFrameHost::FromID(
      color_chooser_info_->render_process_id,
      color_chooser_info_->render_frame_id);
  if (!rfh)
    return;

  rfh->Send(new FrameMsg_DidEndColorChooser(
      rfh->GetRoutingID(), color_chooser_info_->identifier));
  color_chooser_info_.reset();
}

int WebContentsImpl::DownloadImage(
    const GURL& url,
    bool is_favicon,
    uint32_t max_bitmap_size,
    bool bypass_cache,
    const WebContents::ImageDownloadCallback& callback) {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  static int next_image_download_id = 0;
  const content::mojom::ImageDownloaderPtr& mojo_image_downloader =
      GetMainFrame()->GetMojoImageDownloader();
  const int download_id = ++next_image_download_id;
  if (!mojo_image_downloader) {
    // If the renderer process is dead (i.e. crash, or memory pressure on
    // Android), the downloader service will be invalid. Pre-Mojo, this would
    // hang the callback indefinetly since the IPC would be dropped. Now,
    // respond with a 400 HTTP error code to indicate that something went wrong.
    BrowserThread::PostTask(
        BrowserThread::UI, FROM_HERE,
        base::Bind(&WebContentsImpl::OnDidDownloadImage,
                   weak_factory_.GetWeakPtr(), callback, download_id, url, 400,
                   std::vector<SkBitmap>(), std::vector<gfx::Size>()));
    return download_id;
  }

  mojo_image_downloader->DownloadImage(
      url, is_favicon, max_bitmap_size, bypass_cache,
      base::Bind(&WebContentsImpl::OnDidDownloadImage,
                 weak_factory_.GetWeakPtr(), callback, download_id, url));
  return download_id;
}

bool WebContentsImpl::IsSubframe() const {
  return is_subframe_;
}

void WebContentsImpl::Find(int request_id,
                           const base::string16& search_text,
                           const blink::WebFindOptions& options) {
  // Cowardly refuse to search for no text.
  if (search_text.empty()) {
    NOTREACHED();
    return;
  }

  GetOrCreateFindRequestManager()->Find(request_id, search_text, options);
}

void WebContentsImpl::StopFinding(StopFindAction action) {
  if (FindRequestManager* manager = GetFindRequestManager())
    manager->StopFinding(action);
}

bool WebContentsImpl::WasRecentlyAudible() {
  return audio_stream_monitor_.WasRecentlyAudible() ||
         (browser_plugin_embedder_ &&
          browser_plugin_embedder_->WereAnyGuestsRecentlyAudible());
}

void WebContentsImpl::GetManifest(const GetManifestCallback& callback) {
  manifest_manager_host_->GetManifest(GetMainFrame(), callback);
}

void WebContentsImpl::ExitFullscreen(bool will_cause_resize) {
  // Clean up related state and initiate the fullscreen exit.
  GetRenderViewHost()->GetWidget()->RejectMouseLockOrUnlockIfNecessary();
  ExitFullscreenMode(will_cause_resize);
}

void WebContentsImpl::ResumeLoadingCreatedWebContents() {
  if (delayed_open_url_params_.get()) {
    OpenURL(*delayed_open_url_params_.get());
    delayed_open_url_params_.reset(nullptr);
    return;
  }

  // Resume blocked requests for both the RenderViewHost and RenderFrameHost.
  // TODO(brettw): It seems bogus to reach into here and initialize the host.
  if (is_resume_pending_) {
    is_resume_pending_ = false;
    GetRenderViewHost()->GetWidget()->Init();
    GetMainFrame()->Init();
  }
}

bool WebContentsImpl::FocusLocationBarByDefault() {
  // When the browser is started with about:blank as the startup URL, focus
  // the location bar (which will also select its contents) so people can
  // simply begin typing to navigate elsewhere.
  //
  // We need to be careful not to trigger this for anything other than the
  // startup navigation. In particular, if we allow an attacker to open a
  // popup to about:blank, then navigate, focusing the Omnibox will cause the
  // end of the new URL to be scrolled into view instead of the start,
  // allowing the attacker to spoof other URLs. The conditions checked here
  // are all aimed at ensuring no such attacker-controlled navigation can
  // trigger this.
  //
  // Note that we check the pending entry instead of the visible one; for the
  // startup URL case these are the same, but for the attacker-controlled
  // navigation case the visible entry is the committed "about:blank" URL and
  // the pending entry is the problematic navigation elsewhere.
  NavigationEntryImpl* entry = controller_.GetPendingEntry();
  if (controller_.IsInitialNavigation() && entry &&
      !entry->is_renderer_initiated() &&
      entry->GetURL() == url::kAboutBlankURL) {
    return true;
  }
  return delegate_ && delegate_->ShouldFocusLocationBarByDefault(this);
}

void WebContentsImpl::SetFocusToLocationBar(bool select_all) {
  if (delegate_)
    delegate_->SetFocusToLocationBar(select_all);
}

void WebContentsImpl::DidStartNavigation(NavigationHandle* navigation_handle) {
  for (auto& observer : observers_)
    observer.DidStartNavigation(navigation_handle);
}

void WebContentsImpl::DidRedirectNavigation(
    NavigationHandle* navigation_handle) {
  for (auto& observer : observers_)
    observer.DidRedirectNavigation(navigation_handle);

  // Notify accessibility if this is a reload. This has to called on the
  // BrowserAccessibilityManager associated with the old RFHI.
  if (navigation_handle->GetReloadType() != ReloadType::NONE) {
    NavigationHandleImpl* nhi =
        static_cast<NavigationHandleImpl*>(navigation_handle);
    BrowserAccessibilityManager* manager =
        nhi->frame_tree_node()
            ->current_frame_host()
            ->browser_accessibility_manager();
    if (manager)
      manager->UserIsReloading();
  }
}

void WebContentsImpl::ReadyToCommitNavigation(
    NavigationHandle* navigation_handle) {
  for (auto& observer : observers_)
    observer.ReadyToCommitNavigation(navigation_handle);
}

void WebContentsImpl::DidFinishNavigation(NavigationHandle* navigation_handle) {
  for (auto& observer : observers_)
    observer.DidFinishNavigation(navigation_handle);

  if (navigation_handle->HasCommitted()) {
    BrowserAccessibilityManager* manager =
        static_cast<RenderFrameHostImpl*>(
            navigation_handle->GetRenderFrameHost())
            ->browser_accessibility_manager();
    if (manager) {
      if (navigation_handle->IsErrorPage()) {
        manager->NavigationFailed();
      } else {
        manager->NavigationSucceeded();
      }
    }
  }
}

void WebContentsImpl::DidFailLoadWithError(
    RenderFrameHostImpl* render_frame_host,
    const GURL& url,
    int error_code,
    const base::string16& error_description,
    bool was_ignored_by_handler) {
  for (auto& observer : observers_) {
    observer.DidFailLoad(render_frame_host, url, error_code, error_description,
                         was_ignored_by_handler);
  }
}

void WebContentsImpl::NotifyChangedNavigationState(
    InvalidateTypes changed_flags) {
  NotifyNavigationStateChanged(changed_flags);
}

void WebContentsImpl::DidStartNavigationToPendingEntry(const GURL& url,
                                                       ReloadType reload_type) {
  // Notify observers about navigation.
  for (auto& observer : observers_)
    observer.DidStartNavigationToPendingEntry(url, reload_type);
}

bool WebContentsImpl::ShouldTransferNavigation(bool is_main_frame_navigation) {
  if (!delegate_)
    return true;
  return delegate_->ShouldTransferNavigation(is_main_frame_navigation);
}

bool WebContentsImpl::ShouldPreserveAbortedURLs() {
  if (!delegate_)
    return false;
  return delegate_->ShouldPreserveAbortedURLs(this);
}

void WebContentsImpl::DidNavigateMainFramePreCommit(
    bool navigation_is_within_page) {
  // Ensure fullscreen mode is exited before committing the navigation to a
  // different page.  The next page will not start out assuming it is in
  // fullscreen mode.
  if (navigation_is_within_page) {
    // No page change?  Then, the renderer and browser can remain in fullscreen.
    return;
  }
  if (IsFullscreenForCurrentTab())
    ExitFullscreen(false);
  DCHECK(!IsFullscreenForCurrentTab());
}

void WebContentsImpl::DidNavigateMainFramePostCommit(
    RenderFrameHostImpl* render_frame_host,
    const LoadCommittedDetails& details,
    const FrameHostMsg_DidCommitProvisionalLoad_Params& params) {
  if (details.is_navigation_to_different_page()) {
    // Clear the status bubble. This is a workaround for a bug where WebKit
    // doesn't let us know that the cursor left an element during a
    // transition (this is also why the mouse cursor remains as a hand after
    // clicking on a link); see bugs 1184641 and 980803. We don't want to
    // clear the bubble when a user navigates to a named anchor in the same
    // page.
    UpdateTargetURL(render_frame_host->GetRenderViewHost(), GURL());

    RenderWidgetHostViewBase* rwhvb =
        static_cast<RenderWidgetHostViewBase*>(GetRenderWidgetHostView());
    if (rwhvb)
      rwhvb->OnDidNavigateMainFrameToNewPage();

    did_first_visually_non_empty_paint_ = false;

    // Reset theme color on navigation to new page.
    theme_color_ = SK_ColorTRANSPARENT;
  }

  if (delegate_)
    delegate_->DidNavigateMainFramePostCommit(this);
  view_->SetOverscrollControllerEnabled(CanOverscrollContent());
}

void WebContentsImpl::DidNavigateAnyFramePostCommit(
    RenderFrameHostImpl* render_frame_host,
    const LoadCommittedDetails& details,
    const FrameHostMsg_DidCommitProvisionalLoad_Params& params) {
  // Now that something has committed, we don't need to track whether the
  // initial page has been accessed.
  has_accessed_initial_document_ = false;

  // If we navigate off the page, close all JavaScript dialogs.
  if (!details.is_same_document)
    CancelActiveAndPendingDialogs();

  // If this is a user-initiated navigation, start allowing JavaScript dialogs
  // again.
  if (params.gesture == NavigationGestureUser && dialog_manager_) {
    dialog_manager_->CancelDialogs(this, /*reset_state=*/true);
  }
}

void WebContentsImpl::SetMainFrameMimeType(const std::string& mime_type) {
  contents_mime_type_ = mime_type;
}

bool WebContentsImpl::CanOverscrollContent() const {
  // Disable overscroll when touch emulation is on. See crbug.com/369938.
  if (force_disable_overscroll_content_)
    return false;

  if (delegate_)
    return delegate_->CanOverscrollContent();

  return false;
}

void WebContentsImpl::OnThemeColorChanged(RenderFrameHostImpl* source,
                                          SkColor theme_color) {
  if (source != GetMainFrame()) {
    // Only the main frame may control the theme.
    return;
  }

  // Update the theme color. This is to be published to observers after the
  // first visually non-empty paint.
  theme_color_ = theme_color;

  if (did_first_visually_non_empty_paint_ &&
      last_sent_theme_color_ != theme_color_) {
    for (auto& observer : observers_)
      observer.DidChangeThemeColor(theme_color_);
    last_sent_theme_color_ = theme_color_;
  }
}

void WebContentsImpl::OnDidLoadResourceFromMemoryCache(
    RenderFrameHostImpl* source,
    const GURL& url,
    const std::string& http_method,
    const std::string& mime_type,
    ResourceType resource_type) {
  for (auto& observer : observers_)
    observer.DidLoadResourceFromMemoryCache(url, mime_type, resource_type);

  if (url.is_valid() && url.SchemeIsHTTPOrHTTPS()) {
    StoragePartition* partition = source->GetProcess()->GetStoragePartition();
    scoped_refptr<net::URLRequestContextGetter> request_context(
        resource_type == RESOURCE_TYPE_MEDIA
            ? partition->GetMediaURLRequestContext()
            : partition->GetURLRequestContext());
    BrowserThread::PostTask(
        BrowserThread::IO, FROM_HERE,
        base::Bind(&NotifyCacheOnIO, request_context, url, http_method));
  }
}

void WebContentsImpl::OnDidDisplayInsecureContent(RenderFrameHostImpl* source) {
  // Any frame can trigger display of insecure content, so we don't check
  // |source| here.
  DidDisplayInsecureContent();
}

void WebContentsImpl::DidDisplayInsecureContent() {
  controller_.ssl_manager()->DidDisplayMixedContent();
}

void WebContentsImpl::OnDidContainInsecureFormAction(
    RenderFrameHostImpl* source) {
  controller_.ssl_manager()->DidContainInsecureFormAction();
}

void WebContentsImpl::OnDidRunInsecureContent(RenderFrameHostImpl* source,
                                              const GURL& security_origin,
                                              const GURL& target_url) {
  // TODO(nick, estark): Should we call FilterURL using |source|'s process on
  // these parameters? |target_url| seems unused, except for a log message. And
  // |security_origin| might be replaceable with the origin of the main frame.
  DidRunInsecureContent(security_origin, target_url);
}

void WebContentsImpl::DidRunInsecureContent(const GURL& security_origin,
                                            const GURL& target_url) {
  LOG(WARNING) << security_origin << " ran insecure content from "
               << target_url.possibly_invalid_spec();
  RecordAction(base::UserMetricsAction("SSL.RanInsecureContent"));
  if (base::EndsWith(security_origin.spec(), kDotGoogleDotCom,
                     base::CompareCase::INSENSITIVE_ASCII))
    RecordAction(base::UserMetricsAction("SSL.RanInsecureContentGoogle"));
  controller_.ssl_manager()->DidRunMixedContent(security_origin);
}

void WebContentsImpl::PassiveInsecureContentFound(const GURL& resource_url) {
  GetDelegate()->PassiveInsecureContentFound(resource_url);
}

bool WebContentsImpl::ShouldAllowRunningInsecureContent(
    WebContents* web_contents,
    bool allowed_per_prefs,
    const url::Origin& origin,
    const GURL& resource_url) {
  return GetDelegate()->ShouldAllowRunningInsecureContent(
      web_contents, allowed_per_prefs, origin, resource_url);
}

#if defined(OS_ANDROID)
base::android::ScopedJavaLocalRef<jobject>
WebContentsImpl::GetJavaRenderFrameHostDelegate() {
  return GetJavaWebContents();
}
#endif

void WebContentsImpl::OnDidDisplayContentWithCertificateErrors(
    RenderFrameHostImpl* source,
    const GURL& url) {
  // TODO(nick): |url| is unused; get rid of it.
  controller_.ssl_manager()->DidDisplayContentWithCertErrors();
}

void WebContentsImpl::OnDidRunContentWithCertificateErrors(
    RenderFrameHostImpl* source,
    const GURL& url) {
  // TODO(nick, estark): Do we need to consider |source| here somehow?
  NavigationEntry* entry = controller_.GetVisibleEntry();
  if (!entry)
    return;

  // TODO(estark): check that this does something reasonable for
  // about:blank and sandboxed origins. https://crbug.com/609527
  controller_.ssl_manager()->DidRunContentWithCertErrors(
      entry->GetURL().GetOrigin());
}

void WebContentsImpl::OnDocumentLoadedInFrame(RenderFrameHostImpl* source) {
  for (auto& observer : observers_)
    observer.DocumentLoadedInFrame(source);
}

void WebContentsImpl::OnDidFinishLoad(RenderFrameHostImpl* source,
                                      const GURL& url) {
  GURL validated_url(url);
  source->GetProcess()->FilterURL(false, &validated_url);

  for (auto& observer : observers_)
    observer.DidFinishLoad(source, validated_url);
}

void WebContentsImpl::OnGoToEntryAtOffset(RenderViewHostImpl* source,
                                          int offset) {
  // All frames are allowed to navigate the global history.
  if (!delegate_ || delegate_->OnGoToEntryOffset(offset))
    controller_.GoToOffset(offset);
}

void WebContentsImpl::OnUpdateZoomLimits(RenderViewHostImpl* source,
                                         int minimum_percent,
                                         int maximum_percent) {
  minimum_zoom_percent_ = minimum_percent;
  maximum_zoom_percent_ = maximum_percent;
}

void WebContentsImpl::OnPageScaleFactorChanged(RenderViewHostImpl* source,
                                               float page_scale_factor) {
#if !defined(OS_ANDROID)
  // While page scale factor is used on mobile, this PageScaleFactorIsOne logic
  // is only needed on desktop.
  bool is_one = page_scale_factor == 1.f;
  if (is_one != page_scale_factor_is_one_) {
    page_scale_factor_is_one_ = is_one;

    HostZoomMapImpl* host_zoom_map =
        static_cast<HostZoomMapImpl*>(HostZoomMap::GetForWebContents(this));

    if (host_zoom_map) {
      host_zoom_map->SetPageScaleFactorIsOneForView(
          source->GetProcess()->GetID(), source->GetRoutingID(),
          page_scale_factor_is_one_);
    }
  }
#endif  // !defined(OS_ANDROID)

  for (auto& observer : observers_)
    observer.OnPageScaleFactorChanged(page_scale_factor);
}

void WebContentsImpl::OnEnumerateDirectory(RenderViewHostImpl* source,
                                           int request_id,
                                           const base::FilePath& path) {
  if (!delegate_)
    return;

  ChildProcessSecurityPolicyImpl* policy =
      ChildProcessSecurityPolicyImpl::GetInstance();
  if (policy->CanReadFile(source->GetProcess()->GetID(), path)) {
    // TODO(nick): |this| param in the call below ought to be a RenderFrameHost.
    delegate_->EnumerateDirectory(this, request_id, path);
  }
}

void WebContentsImpl::OnRegisterProtocolHandler(RenderFrameHostImpl* source,
                                                const std::string& protocol,
                                                const GURL& url,
                                                const base::string16& title,
                                                bool user_gesture) {
  // TODO(nick): Should we consider |source| here or pass it to the delegate?
  // TODO(nick): Do we need to apply FilterURL to |url|?
  if (!delegate_)
    return;

  ChildProcessSecurityPolicyImpl* policy =
      ChildProcessSecurityPolicyImpl::GetInstance();
  if (policy->IsPseudoScheme(protocol))
    return;

  delegate_->RegisterProtocolHandler(this, protocol, url, user_gesture);
}

void WebContentsImpl::OnUnregisterProtocolHandler(RenderFrameHostImpl* source,
                                                  const std::string& protocol,
                                                  const GURL& url,
                                                  bool user_gesture) {
  // TODO(nick): Should we consider |source| here or pass it to the delegate?
  // TODO(nick): Do we need to apply FilterURL to |url|?
  if (!delegate_)
    return;

  ChildProcessSecurityPolicyImpl* policy =
      ChildProcessSecurityPolicyImpl::GetInstance();
  if (policy->IsPseudoScheme(protocol))
    return;

  delegate_->UnregisterProtocolHandler(this, protocol, url, user_gesture);
}

void WebContentsImpl::OnUpdatePageImportanceSignals(
    RenderFrameHostImpl* source,
    const PageImportanceSignals& signals) {
  // TODO(nick, kouhei): Fix this for oopifs; currently all frames' state gets
  // written to this one field.
  page_importance_signals_ = signals;
}

void WebContentsImpl::OnFindReply(RenderFrameHostImpl* source,
                                  int request_id,
                                  int number_of_matches,
                                  const gfx::Rect& selection_rect,
                                  int active_match_ordinal,
                                  bool final_update) {
  if (active_match_ordinal > 0)
    SetFocusedFrame(source->frame_tree_node(), source->GetSiteInstance());

  // Forward the find reply to the FindRequestManager, along with the
  // RenderFrameHost associated with the frame that the reply came from.
  GetOrCreateFindRequestManager()->OnFindReply(
      source, request_id, number_of_matches, selection_rect,
      active_match_ordinal, final_update);
}

#if defined(OS_ANDROID)
void WebContentsImpl::OnFindMatchRectsReply(
    RenderFrameHostImpl* source,
    int version,
    const std::vector<gfx::RectF>& rects,
    const gfx::RectF& active_rect) {
  GetOrCreateFindRequestManager()->OnFindMatchRectsReply(source, version, rects,
                                                         active_rect);
}

void WebContentsImpl::OnGetNearestFindResultReply(RenderFrameHostImpl* source,
                                                  int request_id,
                                                  float distance) {
  GetOrCreateFindRequestManager()->OnGetNearestFindResultReply(
      source, request_id, distance);
}

void WebContentsImpl::OnOpenDateTimeDialog(
    RenderViewHostImpl* source,
    const ViewHostMsg_DateTimeDialogValue_Params& value) {
  date_time_chooser_->ShowDialog(
      GetTopLevelNativeWindow(), source, value.dialog_type, value.dialog_value,
      value.minimum, value.maximum, value.step, value.suggestions);
}
#endif

void WebContentsImpl::OnDomOperationResponse(RenderFrameHostImpl* source,
                                             const std::string& json_string) {
  // TODO(nick, lukasza): The notification below should probably be updated to
  // include |source|.
  std::string json = json_string;
  NotificationService::current()->Notify(NOTIFICATION_DOM_OPERATION_RESPONSE,
                                         Source<WebContents>(this),
                                         Details<std::string>(&json));
}

void WebContentsImpl::OnAppCacheAccessed(RenderViewHostImpl* source,
                                         const GURL& manifest_url,
                                         bool blocked_by_policy) {
  // TODO(nick): Should we consider |source| here? Should we call FilterURL on
  // |manifest_url|?

  // Notify observers about navigation.
  for (auto& observer : observers_)
    observer.AppCacheAccessed(manifest_url, blocked_by_policy);
}

void WebContentsImpl::OnOpenColorChooser(
    RenderFrameHostImpl* source,
    int color_chooser_id,
    SkColor color,
    const std::vector<ColorSuggestion>& suggestions) {
  ColorChooser* new_color_chooser = delegate_ ?
      delegate_->OpenColorChooser(this, color, suggestions) :
      NULL;
  if (!new_color_chooser)
    return;
  if (color_chooser_info_.get())
    color_chooser_info_->chooser->End();

  color_chooser_info_.reset(new ColorChooserInfo(
      source->GetProcess()->GetID(), source->GetRoutingID(), new_color_chooser,
      color_chooser_id));
}

void WebContentsImpl::OnEndColorChooser(RenderFrameHostImpl* source,
                                        int color_chooser_id) {
  if (color_chooser_info_ &&
      color_chooser_info_->Matches(source, color_chooser_id))
    color_chooser_info_->chooser->End();
}

void WebContentsImpl::OnSetSelectedColorInColorChooser(
    RenderFrameHostImpl* source,
    int color_chooser_id,
    SkColor color) {
  if (color_chooser_info_ &&
      color_chooser_info_->Matches(source, color_chooser_id))
    color_chooser_info_->chooser->SetSelectedColor(color);
}

// This exists for render views that don't have a WebUI, but do have WebUI
// bindings enabled.
void WebContentsImpl::OnWebUISend(RenderViewHostImpl* source,
                                  const GURL& source_url,
                                  const std::string& name,
                                  const base::ListValue& args) {
  // TODO(nick): Should we consider |source| here or pass it to the delegate?
  // TODO(nick): Should FilterURL be applied to |source_url|?
  // TODO(nick): This IPC should be ported to FrameHostMsg_, and |source_url|
  // should be eliminated (use last_committed_url() on the RFH instead).
  if (delegate_)
    delegate_->WebUISend(this, source_url, name, args);
}

#if BUILDFLAG(ENABLE_PLUGINS)
void WebContentsImpl::OnPepperInstanceCreated(RenderFrameHostImpl* source,
                                              int32_t pp_instance) {
  for (auto& observer : observers_)
    observer.PepperInstanceCreated();
  pepper_playback_observer_->PepperInstanceCreated(source, pp_instance);
}

void WebContentsImpl::OnPepperInstanceDeleted(RenderFrameHostImpl* source,
                                              int32_t pp_instance) {
  for (auto& observer : observers_)
    observer.PepperInstanceDeleted();
  pepper_playback_observer_->PepperInstanceDeleted(source, pp_instance);
}

void WebContentsImpl::OnPepperPluginHung(RenderFrameHostImpl* source,
                                         int plugin_child_id,
                                         const base::FilePath& path,
                                         bool is_hung) {
  UMA_HISTOGRAM_COUNTS("Pepper.PluginHung", 1);

  for (auto& observer : observers_)
    observer.PluginHungStatusChanged(plugin_child_id, path, is_hung);
}

void WebContentsImpl::OnPepperStartsPlayback(RenderFrameHostImpl* source,
                                             int32_t pp_instance) {
  pepper_playback_observer_->PepperStartsPlayback(source, pp_instance);
}

void WebContentsImpl::OnPepperStopsPlayback(RenderFrameHostImpl* source,
                                            int32_t pp_instance) {
  pepper_playback_observer_->PepperStopsPlayback(source, pp_instance);
}

void WebContentsImpl::OnPluginCrashed(RenderFrameHostImpl* source,
                                      const base::FilePath& plugin_path,
                                      base::ProcessId plugin_pid) {
  // TODO(nick): Eliminate the |plugin_pid| parameter, which can't be trusted,
  // and is only used by BlinkTestController.
  for (auto& observer : observers_)
    observer.PluginCrashed(plugin_path, plugin_pid);
}

void WebContentsImpl::OnRequestPpapiBrokerPermission(
    RenderViewHostImpl* source,
    int ppb_broker_route_id,
    const GURL& url,
    const base::FilePath& plugin_path) {
  base::Callback<void(bool)> permission_result_callback = base::Bind(
      &WebContentsImpl::SendPpapiBrokerPermissionResult, base::Unretained(this),
      source->GetProcess()->GetID(), ppb_broker_route_id);
  if (!delegate_) {
    permission_result_callback.Run(false);
    return;
  }

  if (!delegate_->RequestPpapiBrokerPermission(this, url, plugin_path,
                                               permission_result_callback)) {
    NOTIMPLEMENTED();
    permission_result_callback.Run(false);
  }
}

void WebContentsImpl::SendPpapiBrokerPermissionResult(int process_id,
                                                      int ppb_broker_route_id,
                                                      bool result) {
  RenderProcessHost* rph = RenderProcessHost::FromID(process_id);
  if (rph) {
    // TODO(nick): Convert this from ViewMsg_ to a Ppapi msg, since it
    // is not routed to a RenderView.
    rph->Send(
        new ViewMsg_PpapiBrokerPermissionResult(ppb_broker_route_id, result));
  }
}

void WebContentsImpl::OnBrowserPluginMessage(RenderFrameHost* render_frame_host,
                                             const IPC::Message& message) {
  CHECK(!browser_plugin_embedder_.get());
  CreateBrowserPluginEmbedderIfNecessary();
  browser_plugin_embedder_->OnMessageReceived(message, render_frame_host);
}
#endif  // BUILDFLAG(ENABLE_PLUGINS)

void WebContentsImpl::OnUpdateFaviconURL(
    RenderFrameHostImpl* source,
    const std::vector<FaviconURL>& candidates) {
  // Ignore favicons for non-main frame.
  if (source->GetParent()) {
    NOTREACHED();
    return;
  }

  // We get updated favicon URLs after the page stops loading. If a cross-site
  // navigation occurs while a page is still loading, the initial page
  // may stop loading and send us updated favicon URLs after the navigation
  // for the new page has committed.
  if (!source->IsCurrent())
    return;

  for (auto& observer : observers_)
    observer.DidUpdateFaviconURL(candidates);
}

void WebContentsImpl::OnPasswordInputShownOnHttp() {
  controller_.ssl_manager()->DidShowPasswordInputOnHttp();
}

void WebContentsImpl::OnAllPasswordInputsHiddenOnHttp() {
  controller_.ssl_manager()->DidHideAllPasswordInputsOnHttp();
}

void WebContentsImpl::OnCreditCardInputShownOnHttp() {
  controller_.ssl_manager()->DidShowCreditCardInputOnHttp();
}

void WebContentsImpl::SetIsOverlayContent(bool is_overlay_content) {
  is_overlay_content_ = is_overlay_content;
}

void WebContentsImpl::OnFirstVisuallyNonEmptyPaint(RenderViewHostImpl* source) {
  // TODO(nick): When this is ported to FrameHostMsg_, we should only listen if
  // |source| is the main frame.
  for (auto& observer : observers_)
    observer.DidFirstVisuallyNonEmptyPaint();

  did_first_visually_non_empty_paint_ = true;

  if (theme_color_ != last_sent_theme_color_) {
    // Theme color should have updated by now if there was one.
    for (auto& observer : observers_)
      observer.DidChangeThemeColor(theme_color_);
    last_sent_theme_color_ = theme_color_;
  }
}

void WebContentsImpl::NotifyBeforeFormRepostWarningShow() {
  for (auto& observer : observers_)
    observer.BeforeFormRepostWarningShow();
}

void WebContentsImpl::ActivateAndShowRepostFormWarningDialog() {
  Activate();
  if (delegate_)
    delegate_->ShowRepostFormWarningDialog(this);
}

bool WebContentsImpl::HasAccessedInitialDocument() {
  return has_accessed_initial_document_;
}

void WebContentsImpl::UpdateTitleForEntry(NavigationEntry* entry,
                                          const base::string16& title) {
  // For file URLs without a title, use the pathname instead. In the case of a
  // synthesized title, we don't want the update to count toward the "one set
  // per page of the title to history."
  base::string16 final_title;
  bool explicit_set;
  if (entry && entry->GetURL().SchemeIsFile() && title.empty()) {
    final_title = base::UTF8ToUTF16(entry->GetURL().ExtractFileName());
    explicit_set = false;  // Don't count synthetic titles toward the set limit.
  } else {
    base::TrimWhitespace(title, base::TRIM_ALL, &final_title);
    explicit_set = true;
  }

  // If a page is created via window.open and never navigated,
  // there will be no navigation entry. In this situation,
  // |page_title_when_no_navigation_entry_| will be used for page title.
  if (entry) {
    if (final_title == entry->GetTitle())
      return;  // Nothing changed, don't bother.

    entry->SetTitle(final_title);
  } else {
    if (page_title_when_no_navigation_entry_ == final_title)
      return;  // Nothing changed, don't bother.

    page_title_when_no_navigation_entry_ = final_title;
  }

  // Lastly, set the title for the view.
  view_->SetPageTitle(final_title);

  for (auto& observer : observers_)
    observer.TitleWasSet(entry, explicit_set);

  // Broadcast notifications when the UI should be updated.
  if (entry == controller_.GetEntryAtOffset(0))
    NotifyNavigationStateChanged(INVALIDATE_TYPE_TITLE);
}

void WebContentsImpl::SendChangeLoadProgress() {
  loading_last_progress_update_ = base::TimeTicks::Now();
  if (delegate_)
    delegate_->LoadProgressChanged(this, frame_tree_.load_progress());
}

void WebContentsImpl::ResetLoadProgressState() {
  frame_tree_.ResetLoadProgress();
  loading_weak_factory_.InvalidateWeakPtrs();
  loading_last_progress_update_ = base::TimeTicks();
}

// Notifies the RenderWidgetHost instance about the fact that the page is
// loading, or done loading.
void WebContentsImpl::LoadingStateChanged(bool to_different_document,
                                          bool due_to_interstitial,
                                          LoadNotificationDetails* details) {
  // Do not send notifications about loading changes in the FrameTree while the
  // interstitial page is pausing the throbber.
  if (ShowingInterstitialPage() &&
      GetRenderManager()->interstitial_page()->pause_throbber() &&
      !due_to_interstitial) {
    return;
  }

  bool is_loading = IsLoading();

  if (!is_loading) {
    load_state_ = net::LoadStateWithParam(net::LOAD_STATE_IDLE,
                                          base::string16());
    load_state_host_.clear();
    upload_size_ = 0;
    upload_position_ = 0;
  }

  GetRenderManager()->SetIsLoading(is_loading);

  waiting_for_response_ = is_loading;
  is_load_to_different_document_ = to_different_document;

  if (delegate_)
    delegate_->LoadingStateChanged(this, to_different_document);
  NotifyNavigationStateChanged(INVALIDATE_TYPE_LOAD);

  std::string url = (details ? details->url.possibly_invalid_spec() : "NULL");
  if (is_loading) {
    TRACE_EVENT_ASYNC_BEGIN2("browser,navigation", "WebContentsImpl Loading",
                             this, "URL", url, "Main FrameTreeNode id",
                             GetFrameTree()->root()->frame_tree_node_id());
    for (auto& observer : observers_)
      observer.DidStartLoading();
  } else {
    TRACE_EVENT_ASYNC_END1("browser,navigation", "WebContentsImpl Loading",
                           this, "URL", url);
    for (auto& observer : observers_)
      observer.DidStopLoading();
  }

  // TODO(avi): Remove. http://crbug.com/170921
  int type = is_loading ? NOTIFICATION_LOAD_START : NOTIFICATION_LOAD_STOP;
  NotificationDetails det = NotificationService::NoDetails();
  if (details)
      det = Details<LoadNotificationDetails>(details);
  NotificationService::current()->Notify(
      type, Source<NavigationController>(&controller_), det);
}

void WebContentsImpl::NotifyViewSwapped(RenderViewHost* old_host,
                                        RenderViewHost* new_host) {
  // After sending out a swap notification, we need to send a disconnect
  // notification so that clients that pick up a pointer to |this| can NULL the
  // pointer.  See Bug 1230284.
  notify_disconnection_ = true;
  for (auto& observer : observers_)
    observer.RenderViewHostChanged(old_host, new_host);

  // Ensure that the associated embedder gets cleared after a RenderViewHost
  // gets swapped, so we don't reuse the same embedder next time a
  // RenderViewHost is attached to this WebContents.
  RemoveBrowserPluginEmbedder();
}

void WebContentsImpl::NotifyFrameSwapped(RenderFrameHost* old_host,
                                         RenderFrameHost* new_host) {
  for (auto& observer : observers_)
    observer.RenderFrameHostChanged(old_host, new_host);
}

// TODO(avi): Remove this entire function because this notification is already
// covered by two observer functions. http://crbug.com/170921
void WebContentsImpl::NotifyDisconnected() {
  if (!notify_disconnection_)
    return;

  notify_disconnection_ = false;
  NotificationService::current()->Notify(
      NOTIFICATION_WEB_CONTENTS_DISCONNECTED,
      Source<WebContents>(this),
      NotificationService::NoDetails());
}

void WebContentsImpl::NotifyNavigationEntryCommitted(
    const LoadCommittedDetails& load_details) {
  for (auto& observer : observers_)
    observer.NavigationEntryCommitted(load_details);
}

void WebContentsImpl::OnAssociatedInterfaceRequest(
    RenderFrameHost* render_frame_host,
    const std::string& interface_name,
    mojo::ScopedInterfaceEndpointHandle handle) {
  auto it = binding_sets_.find(interface_name);
  if (it != binding_sets_.end())
    it->second->OnRequestForFrame(render_frame_host, std::move(handle));
}

const GURL& WebContentsImpl::GetMainFrameLastCommittedURL() const {
  return GetLastCommittedURL();
}

void WebContentsImpl::RenderFrameCreated(RenderFrameHost* render_frame_host) {
  for (auto& observer : observers_)
    observer.RenderFrameCreated(render_frame_host);
  UpdateAccessibilityModeOnFrame(render_frame_host);

  if (!render_frame_host->IsRenderFrameLive() || render_frame_host->GetParent())
    return;

  NavigationEntry* entry = controller_.GetPendingEntry();
  if (entry && entry->IsViewSourceMode()) {
    // Put the renderer in view source mode.
    render_frame_host->Send(
        new FrameMsg_EnableViewSourceMode(render_frame_host->GetRoutingID()));
  }
}

void WebContentsImpl::RenderFrameDeleted(RenderFrameHost* render_frame_host) {
  is_notifying_observers_ = true;
  for (auto& observer : observers_)
    observer.RenderFrameDeleted(render_frame_host);
  is_notifying_observers_ = false;
#if BUILDFLAG(ENABLE_PLUGINS)
  pepper_playback_observer_->RenderFrameDeleted(render_frame_host);
#endif
}

void WebContentsImpl::ShowContextMenu(RenderFrameHost* render_frame_host,
                                      const ContextMenuParams& params) {
  // If a renderer fires off a second command to show a context menu before the
  // first context menu is closed, just ignore it. https://crbug.com/707534
  if (showing_context_menu_)
    return;

  ContextMenuParams context_menu_params(params);
  // Allow WebContentsDelegates to handle the context menu operation first.
  if (delegate_ && delegate_->HandleContextMenu(context_menu_params))
    return;

  render_view_host_delegate_view_->ShowContextMenu(render_frame_host,
                                                   context_menu_params);
}

void WebContentsImpl::RunJavaScriptDialog(RenderFrameHost* render_frame_host,
                                          const base::string16& message,
                                          const base::string16& default_prompt,
                                          const GURL& frame_url,
                                          JavaScriptDialogType dialog_type,
                                          IPC::Message* reply_msg) {
  // Running a dialog causes an exit to webpage-initiated fullscreen.
  // http://crbug.com/728276
  if (IsFullscreenForCurrentTab())
    ExitFullscreen(true);

  // Suppress JavaScript dialogs when requested. Also suppress messages when
  // showing an interstitial as it's shown over the previous page and we don't
  // want the hidden page's dialogs to interfere with the interstitial.
  bool suppress_this_message =
      ShowingInterstitialPage() || !delegate_ ||
      delegate_->ShouldSuppressDialogs(this) ||
      !delegate_->GetJavaScriptDialogManager(this);

  if (!suppress_this_message) {
    is_showing_javascript_dialog_ = true;
    dialog_manager_ = delegate_->GetJavaScriptDialogManager(this);
    dialog_manager_->RunJavaScriptDialog(
        this, frame_url, dialog_type, message, default_prompt,
        base::Bind(&WebContentsImpl::OnDialogClosed, base::Unretained(this),
                   render_frame_host->GetProcess()->GetID(),
                   render_frame_host->GetRoutingID(), reply_msg, false),
        &suppress_this_message);
  }

  if (suppress_this_message) {
    // If we are suppressing messages, just reply as if the user immediately
    // pressed "Cancel", passing true to |dialog_was_suppressed|.
    OnDialogClosed(render_frame_host->GetProcess()->GetID(),
                   render_frame_host->GetRoutingID(), reply_msg,
                   true, false, base::string16());
  }
}

void WebContentsImpl::RunBeforeUnloadConfirm(
    RenderFrameHost* render_frame_host,
    bool is_reload,
    IPC::Message* reply_msg) {
  // Running a dialog causes an exit to webpage-initiated fullscreen.
  // http://crbug.com/728276
  if (IsFullscreenForCurrentTab())
    ExitFullscreen(true);

  RenderFrameHostImpl* rfhi =
      static_cast<RenderFrameHostImpl*>(render_frame_host);
  if (delegate_)
    delegate_->WillRunBeforeUnloadConfirm();

  bool suppress_this_message =
      !rfhi->is_active() ||
      ShowingInterstitialPage() || !delegate_ ||
      delegate_->ShouldSuppressDialogs(this) ||
      !delegate_->GetJavaScriptDialogManager(this);
  if (suppress_this_message) {
    rfhi->JavaScriptDialogClosed(reply_msg, true, base::string16());
    return;
  }

  is_showing_before_unload_dialog_ = true;
  dialog_manager_ = delegate_->GetJavaScriptDialogManager(this);
  dialog_manager_->RunBeforeUnloadDialog(
      this, is_reload,
      base::Bind(&WebContentsImpl::OnDialogClosed, base::Unretained(this),
                 render_frame_host->GetProcess()->GetID(),
                 render_frame_host->GetRoutingID(), reply_msg,
                 false));
}

void WebContentsImpl::RunFileChooser(RenderFrameHost* render_frame_host,
                                     const FileChooserParams& params) {
  if (delegate_)
    delegate_->RunFileChooser(render_frame_host, params);
}

WebContents* WebContentsImpl::GetAsWebContents() {
  return this;
}

#if !defined(OS_ANDROID)
double WebContentsImpl::GetPendingPageZoomLevel() {
  NavigationEntry* pending_entry = GetController().GetPendingEntry();
  if (!pending_entry)
    return HostZoomMap::GetZoomLevel(this);

  GURL url = pending_entry->GetURL();
  return HostZoomMap::GetForWebContents(this)->GetZoomLevelForHostAndScheme(
      url.scheme(), net::GetHostOrSpecFromURL(url));
}
#endif  // !defined(OS_ANDROID)

bool WebContentsImpl::HideDownloadUI() const {
  return is_overlay_content_;
}

bool WebContentsImpl::HasPersistentVideo() const {
  return has_persistent_video_;
}

bool WebContentsImpl::HasActiveEffectivelyFullscreenVideo() const {
  return media_web_contents_observer_->HasActiveEffectivelyFullscreenVideo();
}

bool WebContentsImpl::IsFocusedElementEditable() {
  RenderFrameHostImpl* frame = GetFocusedFrame();
  return frame && frame->has_focused_editable_element();
}

bool WebContentsImpl::IsShowingContextMenu() const {
  return showing_context_menu_;
}

void WebContentsImpl::SetShowingContextMenu(bool showing) {
  DCHECK_NE(showing_context_menu_, showing);
  showing_context_menu_ = showing;

  if (auto* view = GetRenderWidgetHostView()) {
    // Notify the main frame's RWHV to run the platform-specific code, if any.
    static_cast<RenderWidgetHostViewBase*>(view)->SetShowingContextMenu(
        showing);
  }
}

void WebContentsImpl::ClearFocusedElement() {
  if (auto* frame = GetFocusedFrame())
    frame->ClearFocusedElement();
}

bool WebContentsImpl::IsNeverVisible() {
  if (!delegate_)
    return false;
  return delegate_->IsNeverVisible(this);
}

RenderViewHostDelegateView* WebContentsImpl::GetDelegateView() {
  return render_view_host_delegate_view_;
}

RendererPreferences WebContentsImpl::GetRendererPrefs(
    BrowserContext* browser_context) const {
  return renderer_preferences_;
}

void WebContentsImpl::RemoveBrowserPluginEmbedder() {
  if (browser_plugin_embedder_)
    browser_plugin_embedder_.reset();
}

WebContentsImpl* WebContentsImpl::GetOuterWebContents() {
  if (GuestMode::IsCrossProcessFrameGuest(this))
    return node_.outer_web_contents();

  if (browser_plugin_guest_)
    return browser_plugin_guest_->embedder_web_contents();

  return nullptr;
}

WebContentsImpl* WebContentsImpl::GetFocusedWebContents() {
  return GetOutermostWebContents()->node_.focused_web_contents();
}

bool WebContentsImpl::ContainsOrIsFocusedWebContents() {
  for (WebContentsImpl* focused_contents = GetFocusedWebContents();
       focused_contents;
       focused_contents = focused_contents->GetOuterWebContents()) {
    if (focused_contents == this)
      return true;
  }

  return false;
}

WebContentsImpl* WebContentsImpl::GetOutermostWebContents() {
  WebContentsImpl* root = this;
  while (root->GetOuterWebContents())
    root = root->GetOuterWebContents();
  return root;
}

void WebContentsImpl::RenderViewCreated(RenderViewHost* render_view_host) {
  // Don't send notifications if we are just creating a swapped-out RVH for
  // the opener chain.  These won't be used for view-source or WebUI, so it's
  // ok to return early.
  if (!static_cast<RenderViewHostImpl*>(render_view_host)->is_active())
    return;

  if (delegate_)
    view_->SetOverscrollControllerEnabled(CanOverscrollContent());

  NotificationService::current()->Notify(
      NOTIFICATION_WEB_CONTENTS_RENDER_VIEW_HOST_CREATED,
      Source<WebContents>(this),
      Details<RenderViewHost>(render_view_host));

  view_->RenderViewCreated(render_view_host);

  for (auto& observer : observers_)
    observer.RenderViewCreated(render_view_host);
  RenderFrameDevToolsAgentHost::WebContentsCreated(this);
}

void WebContentsImpl::RenderViewReady(RenderViewHost* rvh) {
  if (rvh != GetRenderViewHost()) {
    // Don't notify the world, since this came from a renderer in the
    // background.
    return;
  }

  RenderWidgetHostViewBase* rwhv =
      static_cast<RenderWidgetHostViewBase*>(GetRenderWidgetHostView());
  if (rwhv)
    rwhv->SetMainFrameAXTreeID(GetMainFrame()->GetAXTreeID());

  notify_disconnection_ = true;
  // TODO(avi): Remove. http://crbug.com/170921
  NotificationService::current()->Notify(
      NOTIFICATION_WEB_CONTENTS_CONNECTED,
      Source<WebContents>(this),
      NotificationService::NoDetails());

  bool was_crashed = IsCrashed();
  SetIsCrashed(base::TERMINATION_STATUS_STILL_RUNNING, 0);

  // Restore the focus to the tab (otherwise the focus will be on the top
  // window).
  if (was_crashed && !FocusLocationBarByDefault() &&
      (!delegate_ || delegate_->ShouldFocusPageAfterCrash())) {
    view_->Focus();
  }

  for (auto& observer : observers_)
    observer.RenderViewReady();
}

void WebContentsImpl::RenderViewTerminated(RenderViewHost* rvh,
                                           base::TerminationStatus status,
                                           int error_code) {
  if (rvh != GetRenderViewHost()) {
    // The pending page's RenderViewHost is gone.
    return;
  }

  // Ensure fullscreen mode is exited in the |delegate_| since a crashed
  // renderer may not have made a clean exit.
  if (IsFullscreenForCurrentTab())
    ExitFullscreenMode(false);

  // Cancel any visible dialogs so they are not left dangling over the sad tab.
  CancelActiveAndPendingDialogs();

  if (delegate_)
    delegate_->HideValidationMessage(this);

  audio_stream_monitor_.RenderProcessGone(rvh->GetProcess()->GetID());

  // Reset the loading progress. TODO(avi): What does it mean to have a
  // "renderer crash" when there is more than one renderer process serving a
  // webpage? Once this function is called at a more granular frame level, we
  // probably will need to more granularly reset the state here.
  ResetLoadProgressState();
  NotifyDisconnected();
  SetIsCrashed(status, error_code);

  for (auto& observer : observers_)
    observer.RenderProcessGone(GetCrashedStatus());
}

void WebContentsImpl::RenderViewDeleted(RenderViewHost* rvh) {
  for (auto& observer : observers_)
    observer.RenderViewDeleted(rvh);
}

void WebContentsImpl::UpdateTargetURL(RenderViewHost* render_view_host,
                                      const GURL& url) {
  if (fullscreen_widget_routing_id_ != MSG_ROUTING_NONE) {
    // If we're fullscreen only update the url if it's from the fullscreen
    // renderer.
    RenderWidgetHostView* fs = GetFullscreenRenderWidgetHostView();
    if (fs && fs->GetRenderWidgetHost() != render_view_host->GetWidget())
      return;
  }
  if (delegate_)
    delegate_->UpdateTargetURL(this, url);
}

void WebContentsImpl::Close(RenderViewHost* rvh) {
#if defined(OS_MACOSX)
  // The UI may be in an event-tracking loop, such as between the
  // mouse-down and mouse-up in text selection or a button click.
  // Defer the close until after tracking is complete, so that we
  // don't free objects out from under the UI.
  // TODO(shess): This could get more fine-grained.  For instance,
  // closing a tab in another window while selecting text in the
  // current window's Omnibox should be just fine.
  if (view_->IsEventTracking()) {
    view_->CloseTabAfterEventTracking();
    return;
  }
#endif

  // Ignore this if it comes from a RenderViewHost that we aren't showing.
  if (delegate_ && rvh == GetRenderViewHost())
    delegate_->CloseContents(this);
}

void WebContentsImpl::RequestMove(const gfx::Rect& new_bounds) {
  if (delegate_ && delegate_->IsPopupOrPanel(this))
    delegate_->MoveContents(this, new_bounds);
}

void WebContentsImpl::DidStartLoading(FrameTreeNode* frame_tree_node,
                                      bool to_different_document) {
  LoadingStateChanged(to_different_document, false, nullptr);

  // Notify accessibility that the user is navigating away from the
  // current document.
  //
  // TODO(dmazzoni): do this using a WebContentsObserver.
  BrowserAccessibilityManager* manager =
      frame_tree_node->current_frame_host()->browser_accessibility_manager();
  if (manager)
    manager->UserIsNavigatingAway();
}

void WebContentsImpl::DidStopLoading() {
  std::unique_ptr<LoadNotificationDetails> details;

  // Use the last committed entry rather than the active one, in case a
  // pending entry has been created.
  NavigationEntry* entry = controller_.GetLastCommittedEntry();
  Navigator* navigator = frame_tree_.root()->navigator();

  // An entry may not exist for a stop when loading an initial blank page or
  // if an iframe injected by script into a blank page finishes loading.
  if (entry) {
    base::TimeDelta elapsed =
        base::TimeTicks::Now() - navigator->GetCurrentLoadStart();

    details.reset(new LoadNotificationDetails(
        entry->GetVirtualURL(),
        elapsed,
        &controller_,
        controller_.GetCurrentEntryIndex()));
  }

  LoadingStateChanged(true, false, details.get());
}

void WebContentsImpl::DidChangeLoadProgress() {
  double load_progress = frame_tree_.load_progress();

  // The delegate is notified immediately for the first and last updates. Also,
  // since the message loop may be pretty busy when a page is loaded, it might
  // not execute a posted task in a timely manner so the progress report is sent
  // immediately if enough time has passed.
  base::TimeDelta min_delay =
      base::TimeDelta::FromMilliseconds(kMinimumDelayBetweenLoadingUpdatesMS);
  bool delay_elapsed = loading_last_progress_update_.is_null() ||
      base::TimeTicks::Now() - loading_last_progress_update_ > min_delay;

  if (load_progress == 0.0 || load_progress == 1.0 || delay_elapsed) {
    // If there is a pending task to send progress, it is now obsolete.
    loading_weak_factory_.InvalidateWeakPtrs();

    // Notify the load progress change.
    SendChangeLoadProgress();

    // Clean-up the states if needed.
    if (load_progress == 1.0)
      ResetLoadProgressState();
    return;
  }

  if (loading_weak_factory_.HasWeakPtrs())
    return;

  base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
      FROM_HERE, base::Bind(&WebContentsImpl::SendChangeLoadProgress,
                            loading_weak_factory_.GetWeakPtr()),
      min_delay);
}

std::vector<std::unique_ptr<NavigationThrottle>>
WebContentsImpl::CreateThrottlesForNavigation(
    NavigationHandle* navigation_handle) {
  return GetContentClient()->browser()->CreateThrottlesForNavigation(
      navigation_handle);
}

std::unique_ptr<NavigationUIData> WebContentsImpl::GetNavigationUIData(
    NavigationHandle* navigation_handle) {
  DCHECK(IsBrowserSideNavigationEnabled());
  return GetContentClient()->browser()->GetNavigationUIData(navigation_handle);
}

void WebContentsImpl::DidCancelLoading() {
  controller_.DiscardNonCommittedEntries();

  // Update the URL display.
  NotifyNavigationStateChanged(INVALIDATE_TYPE_URL);
}

void WebContentsImpl::DidAccessInitialDocument() {
  has_accessed_initial_document_ = true;

  // We may have left a failed browser-initiated navigation in the address bar
  // to let the user edit it and try again.  Clear it now that content might
  // show up underneath it.
  if (!IsLoading() && controller_.GetPendingEntry())
    controller_.DiscardPendingEntry(false);

  // Update the URL display.
  NotifyNavigationStateChanged(INVALIDATE_TYPE_URL);
}

void WebContentsImpl::DidChangeName(RenderFrameHost* render_frame_host,
                                    const std::string& name) {
  for (auto& observer : observers_)
    observer.FrameNameChanged(render_frame_host, name);
}

void WebContentsImpl::DocumentOnLoadCompleted(
    RenderFrameHost* render_frame_host) {
  ShowInsecureLocalhostWarningIfNeeded();

  is_notifying_observers_ = true;
  for (auto& observer : observers_)
    observer.DocumentOnLoadCompletedInMainFrame();
  is_notifying_observers_ = false;

  // TODO(avi): Remove. http://crbug.com/170921
  NotificationService::current()->Notify(
      NOTIFICATION_LOAD_COMPLETED_MAIN_FRAME,
      Source<WebContents>(this),
      NotificationService::NoDetails());
}

void WebContentsImpl::UpdateStateForFrame(RenderFrameHost* render_frame_host,
                                          const PageState& page_state) {
  // The state update affects the last NavigationEntry associated with the given
  // |render_frame_host|. This may not be the last committed NavigationEntry (as
  // in the case of an UpdateState from a frame being swapped out). We track
  // which entry this is in the RenderFrameHost's nav_entry_id.
  RenderFrameHostImpl* rfhi =
      static_cast<RenderFrameHostImpl*>(render_frame_host);
  NavigationEntryImpl* entry =
      controller_.GetEntryWithUniqueID(rfhi->nav_entry_id());
  if (!entry)
    return;

  FrameNavigationEntry* frame_entry =
      entry->GetFrameEntry(rfhi->frame_tree_node());
  if (!frame_entry)
    return;

  // The SiteInstance might not match if we do a cross-process navigation with
  // replacement (e.g., auto-subframe), in which case the swap out of the old
  // RenderFrameHost runs in the background after the old FrameNavigationEntry
  // has already been replaced and destroyed.
  if (frame_entry->site_instance() != rfhi->GetSiteInstance())
    return;

  if (page_state == frame_entry->page_state())
    return;  // Nothing to update.

  DCHECK(page_state.IsValid()) << "Shouldn't set an empty PageState.";

  // The document_sequence_number and item_sequence_number recorded in the
  // FrameNavigationEntry should not differ from the one coming with the update,
  // since it must come from the same document. Do not update it if a difference
  // is detected, as this indicates that |frame_entry| is not the correct one.
  ExplodedPageState exploded_state;
  if (!DecodePageState(page_state.ToEncodedData(), &exploded_state))
    return;

  if (exploded_state.top.document_sequence_number !=
          frame_entry->document_sequence_number() ||
      exploded_state.top.item_sequence_number !=
          frame_entry->item_sequence_number()) {
    return;
  }

  frame_entry->SetPageState(page_state);
  controller_.NotifyEntryChanged(entry);
}

void WebContentsImpl::UpdateTitle(RenderFrameHost* render_frame_host,
                                  const base::string16& title,
                                  base::i18n::TextDirection title_direction) {
  // If we have a title, that's a pretty good indication that we've started
  // getting useful data.
  SetNotWaitingForResponse();

  // Try to find the navigation entry, which might not be the current one.
  // For example, it might be from a recently swapped out RFH.
  NavigationEntryImpl* entry = controller_.GetEntryWithUniqueID(
      static_cast<RenderFrameHostImpl*>(render_frame_host)->nav_entry_id());

  // We can handle title updates when we don't have an entry in
  // UpdateTitleForEntry, but only if the update is from the current RVH.
  // TODO(avi): Change to make decisions based on the RenderFrameHost.
  if (!entry && render_frame_host != GetMainFrame())
    return;

  // TODO(evan): make use of title_direction.
  // http://code.google.com/p/chromium/issues/detail?id=27094
  UpdateTitleForEntry(entry, title);
}

void WebContentsImpl::UpdateEncoding(RenderFrameHost* render_frame_host,
                                     const std::string& encoding) {
  SetEncoding(encoding);
}

void WebContentsImpl::DocumentAvailableInMainFrame(
    RenderViewHost* render_view_host) {
  for (auto& observer : observers_)
    observer.DocumentAvailableInMainFrame();
}

void WebContentsImpl::RouteCloseEvent(RenderViewHost* rvh) {
  // Tell the active RenderViewHost to run unload handlers and close, as long
  // as the request came from a RenderViewHost in the same BrowsingInstance.
  // In most cases, we receive this from a swapped out RenderViewHost.
  // It is possible to receive it from one that has just been swapped in,
  // in which case we might as well deliver the message anyway.
  if (rvh->GetSiteInstance()->IsRelatedSiteInstance(GetSiteInstance()))
    ClosePage();
}

bool WebContentsImpl::ShouldRouteMessageEvent(
    RenderFrameHost* target_rfh,
    SiteInstance* source_site_instance) const {
  // Allow the message if this WebContents is dedicated to a browser plugin
  // guest.
  // Note: This check means that an embedder could theoretically receive a
  // postMessage from anyone (not just its own guests). However, this is
  // probably not a risk for apps since other pages won't have references
  // to App windows.
  return GetBrowserPluginGuest() || GetBrowserPluginEmbedder();
}

void WebContentsImpl::EnsureOpenerProxiesExist(RenderFrameHost* source_rfh) {
  WebContentsImpl* source_web_contents = static_cast<WebContentsImpl*>(
      WebContents::FromRenderFrameHost(source_rfh));

  if (source_web_contents) {
    // If this message is going to outer WebContents from inner WebContents,
    // then we should not create a RenderView. AttachToOuterWebContentsFrame()
    // already created a RenderFrameProxyHost for that purpose.
    if (GetBrowserPluginEmbedder() &&
        GuestMode::IsCrossProcessFrameGuest(source_web_contents)) {
      return;
    }

    if (this != source_web_contents && GetBrowserPluginGuest()) {
      // We create a RenderFrameProxyHost for the embedder in the guest's render
      // process but we intentionally do not expose the embedder's opener chain
      // to it.
      source_web_contents->GetRenderManager()->CreateRenderFrameProxy(
          GetSiteInstance());
    } else {
      RenderFrameHostImpl* source_rfhi =
          static_cast<RenderFrameHostImpl*>(source_rfh);
      source_rfhi->frame_tree_node()->render_manager()->CreateOpenerProxies(
          GetSiteInstance(), nullptr);
    }
  }
}

void WebContentsImpl::SetAsFocusedWebContentsIfNecessary() {
  // Only change focus if we are not currently focused.
  WebContentsImpl* old_contents = GetFocusedWebContents();
  if (old_contents == this)
    return;

  GetOutermostWebContents()->node_.SetFocusedWebContents(this);

  if (!GuestMode::IsCrossProcessFrameGuest(this) && browser_plugin_guest_)
    return;

  // Send a page level blur to the old contents so that it displays inactive UI
  // and focus this contents to activate it.
  if (old_contents)
    old_contents->GetMainFrame()->GetRenderWidgetHost()->SetPageFocus(false);

  // Make sure the outer web contents knows our frame is focused. Otherwise, the
  // outer renderer could have the element before or after the frame element
  // focused which would return early without actually advancing focus.
  if (GetRenderManager()->GetProxyToOuterDelegate())
    GetRenderManager()->GetProxyToOuterDelegate()->SetFocusedFrame();

  if (ShowingInterstitialPage()) {
    static_cast<RenderFrameHostImpl*>(
        GetRenderManager()->interstitial_page()->GetMainFrame())
        ->GetRenderWidgetHost()
        ->SetPageFocus(true);
  } else {
    GetMainFrame()->GetRenderWidgetHost()->SetPageFocus(true);
  }
}

void WebContentsImpl::SetFocusedFrame(FrameTreeNode* node,
                                      SiteInstance* source) {
  SetAsFocusedWebContentsIfNecessary();
  frame_tree_.SetFocusedFrame(node, source);
}

void WebContentsImpl::OnFocusedElementChangedInFrame(
    RenderFrameHostImpl* frame,
    const gfx::Rect& bounds_in_root_view) {
  RenderWidgetHostViewBase* root_view =
      static_cast<RenderWidgetHostViewBase*>(GetRenderWidgetHostView());
  if (!root_view || !frame->GetView())
    return;

  // Converting to screen coordinates.
  gfx::Point origin = bounds_in_root_view.origin();
  origin += root_view->GetViewBounds().OffsetFromOrigin();
  gfx::Rect bounds_in_screen(origin, bounds_in_root_view.size());

  root_view->FocusedNodeChanged(frame->has_focused_editable_element(),
                                bounds_in_screen);

  FocusedNodeDetails details = {frame->has_focused_editable_element(),
                                bounds_in_screen};

  // TODO(ekaramad): We should replace this with an observer notification
  // (https://crbug.com/675975).
  NotificationService::current()->Notify(
      NOTIFICATION_FOCUS_CHANGED_IN_PAGE,
      Source<RenderViewHost>(GetRenderViewHost()),
      Details<FocusedNodeDetails>(&details));
}

bool WebContentsImpl::DidAddMessageToConsole(int32_t level,
                                             const base::string16& message,
                                             int32_t line_no,
                                             const base::string16& source_id) {
  if (!delegate_)
    return false;
  return delegate_->DidAddMessageToConsole(this, level, message, line_no,
                                           source_id);
}

void WebContentsImpl::OnUserInteraction(
    RenderWidgetHostImpl* render_widget_host,
    const blink::WebInputEvent::Type type) {
  // Ignore unless the widget is currently in the frame tree.
  if (!HasMatchingWidgetHost(&frame_tree_, render_widget_host))
    return;

  for (auto& observer : observers_)
    observer.DidGetUserInteraction(type);

  ResourceDispatcherHostImpl* rdh = ResourceDispatcherHostImpl::Get();
  // Exclude scroll events as user gestures for resource load dispatches.
  // rdh is NULL in unittests.
  if (rdh && type != blink::WebInputEvent::kMouseWheel)
    rdh->OnUserGesture();
}

void WebContentsImpl::FocusOwningWebContents(
    RenderWidgetHostImpl* render_widget_host) {
  // The PDF plugin still runs as a BrowserPlugin and must go through the
  // input redirection mechanism. It must not become focused direcly.
  if (!GuestMode::IsCrossProcessFrameGuest(this) && browser_plugin_guest_)
    return;

  RenderWidgetHostImpl* focused_widget =
      GetFocusedRenderWidgetHost(render_widget_host);

  if (focused_widget != render_widget_host &&
      (!focused_widget ||
       focused_widget->delegate() != render_widget_host->delegate())) {
    SetAsFocusedWebContentsIfNecessary();
  }
}

void WebContentsImpl::OnIgnoredUIEvent() {
  // Notify observers.
  for (auto& observer : observers_)
    observer.DidGetIgnoredUIEvent();
}

void WebContentsImpl::RendererUnresponsive(
    RenderWidgetHostImpl* render_widget_host) {
  for (auto& observer : observers_)
    observer.OnRendererUnresponsive(render_widget_host);

  // Don't show hung renderer dialog for a swapped out RVH.
  if (render_widget_host != GetRenderViewHost()->GetWidget())
    return;

  if (ShouldIgnoreUnresponsiveRenderer())
    return;

  if (!GetRenderViewHost() || !GetRenderViewHost()->IsRenderViewLive())
    return;

  if (delegate_) {
    WebContentsUnresponsiveState unresponsive_state;
    unresponsive_state.outstanding_ack_count =
        render_widget_host->in_flight_event_count();
    unresponsive_state.outstanding_event_type =
        render_widget_host->hang_monitor_event_type();
    unresponsive_state.last_event_type = render_widget_host->last_event_type();
    delegate_->RendererUnresponsive(this, unresponsive_state);
  }
}

void WebContentsImpl::RendererResponsive(
    RenderWidgetHostImpl* render_widget_host) {
  if (render_widget_host != GetRenderViewHost()->GetWidget())
    return;

  if (delegate_)
    delegate_->RendererResponsive(this);
}

void WebContentsImpl::BeforeUnloadFiredFromRenderManager(
    bool proceed, const base::TimeTicks& proceed_time,
    bool* proceed_to_fire_unload) {
  for (auto& observer : observers_)
    observer.BeforeUnloadFired(proceed_time);
  if (delegate_)
    delegate_->BeforeUnloadFired(this, proceed, proceed_to_fire_unload);
  // Note: |this| might be deleted at this point.
}

void WebContentsImpl::RenderProcessGoneFromRenderManager(
    RenderViewHost* render_view_host) {
  DCHECK(crashed_status_ != base::TERMINATION_STATUS_STILL_RUNNING);
  RenderViewTerminated(render_view_host, crashed_status_, crashed_error_code_);
}

void WebContentsImpl::UpdateRenderViewSizeForRenderManager() {
  // TODO(brettw) this is a hack. See WebContentsView::SizeContents.
  gfx::Size size = GetSizeForNewRenderView();
  // 0x0 isn't a valid window size (minimal window size is 1x1) but it may be
  // here during container initialization and normal window size will be set
  // later. In case of tab duplication this resizing to 0x0 prevents setting
  // normal size later so just ignore it.
  if (!size.IsEmpty())
    view_->SizeContents(size);
}

void WebContentsImpl::CancelModalDialogsForRenderManager() {
  // We need to cancel modal dialogs when doing a process swap, since the load
  // deferrer would prevent us from swapping out. We also clear the state
  // because this is a cross-process navigation, which means that it's a new
  // site that should not have to pay for the sins of its predecessor.
  //
  // Note that we don't bother telling browser_plugin_embedder_ because the
  // cross-process navigation will either destroy the browser plugins or not
  // require their dialogs to close.
  if (dialog_manager_) {
    dialog_manager_->CancelDialogs(this, /*reset_state=*/true);
  }
}

void WebContentsImpl::NotifySwappedFromRenderManager(RenderFrameHost* old_host,
                                                     RenderFrameHost* new_host,
                                                     bool is_main_frame) {
  if (is_main_frame) {
    NotifyViewSwapped(old_host ? old_host->GetRenderViewHost() : nullptr,
                      new_host->GetRenderViewHost());

    // Make sure the visible RVH reflects the new delegate's preferences.
    if (delegate_)
      view_->SetOverscrollControllerEnabled(CanOverscrollContent());

    view_->RenderViewSwappedIn(new_host->GetRenderViewHost());

    RenderWidgetHostViewBase* rwhv =
        static_cast<RenderWidgetHostViewBase*>(GetRenderWidgetHostView());
    if (rwhv)
      rwhv->SetMainFrameAXTreeID(GetMainFrame()->GetAXTreeID());
  }

  NotifyFrameSwapped(old_host, new_host);
}

void WebContentsImpl::NotifyMainFrameSwappedFromRenderManager(
    RenderViewHost* old_host,
    RenderViewHost* new_host) {
  NotifyViewSwapped(old_host, new_host);
}

NavigationControllerImpl& WebContentsImpl::GetControllerForRenderManager() {
  return GetController();
}

std::unique_ptr<WebUIImpl> WebContentsImpl::CreateWebUIForRenderFrameHost(
    const GURL& url) {
  return CreateWebUI(url, std::string());
}

NavigationEntry*
    WebContentsImpl::GetLastCommittedNavigationEntryForRenderManager() {
  return controller_.GetLastCommittedEntry();
}

void WebContentsImpl::CreateRenderWidgetHostViewForRenderManager(
    RenderViewHost* render_view_host) {
  RenderWidgetHostViewBase* rwh_view =
      view_->CreateViewForWidget(render_view_host->GetWidget(), false);

  // Now that the RenderView has been created, we need to tell it its size.
  if (rwh_view)
    rwh_view->SetSize(GetSizeForNewRenderView());
}

bool WebContentsImpl::CreateRenderViewForRenderManager(
    RenderViewHost* render_view_host,
    int opener_frame_routing_id,
    int proxy_routing_id,
    const FrameReplicationState& replicated_frame_state) {
  TRACE_EVENT0("browser,navigation",
               "WebContentsImpl::CreateRenderViewForRenderManager");

  if (proxy_routing_id == MSG_ROUTING_NONE)
    CreateRenderWidgetHostViewForRenderManager(render_view_host);

  if (!static_cast<RenderViewHostImpl*>(render_view_host)
           ->CreateRenderView(opener_frame_routing_id, proxy_routing_id,
                              replicated_frame_state, created_with_opener_)) {
    return false;
  }

  SetHistoryOffsetAndLengthForView(render_view_host,
                                   controller_.GetLastCommittedEntryIndex(),
                                   controller_.GetEntryCount());

#if defined(OS_POSIX) && !defined(OS_MACOSX) && !defined(OS_ANDROID)
  // Force a ViewMsg_Resize to be sent, needed to make plugins show up on
  // linux. See crbug.com/83941.
  RenderWidgetHostView* rwh_view = render_view_host->GetWidget()->GetView();
  if (rwh_view) {
    if (RenderWidgetHost* render_widget_host = rwh_view->GetRenderWidgetHost())
      render_widget_host->WasResized();
  }
#endif

  return true;
}

bool WebContentsImpl::CreateRenderFrameForRenderManager(
    RenderFrameHost* render_frame_host,
    int proxy_routing_id,
    int opener_routing_id,
    int parent_routing_id,
    int previous_sibling_routing_id) {
  TRACE_EVENT0("browser,navigation",
               "WebContentsImpl::CreateRenderFrameForRenderManager");

  RenderFrameHostImpl* rfh =
      static_cast<RenderFrameHostImpl*>(render_frame_host);
  if (!rfh->CreateRenderFrame(proxy_routing_id, opener_routing_id,
                              parent_routing_id, previous_sibling_routing_id))
    return false;

  // TODO(nasko): When RenderWidgetHost is owned by RenderFrameHost, the passed
  // RenderFrameHost will have to be associated with the appropriate
  // RenderWidgetHostView or a new one should be created here.

  return true;
}

#if defined(OS_ANDROID)

base::android::ScopedJavaLocalRef<jobject>
WebContentsImpl::GetJavaWebContents() {
  DCHECK_CURRENTLY_ON(BrowserThread::UI);
  return GetWebContentsAndroid()->GetJavaObject();
}

WebContentsAndroid* WebContentsImpl::GetWebContentsAndroid() {
  WebContentsAndroid* web_contents_android =
      static_cast<WebContentsAndroid*>(GetUserData(kWebContentsAndroidKey));
  if (!web_contents_android) {
    web_contents_android = new WebContentsAndroid(this);
    SetUserData(kWebContentsAndroidKey, base::WrapUnique(web_contents_android));
  }
  return web_contents_android;
}

void WebContentsImpl::ActivateNearestFindResult(float x,
                                                float y) {
  GetOrCreateFindRequestManager()->ActivateNearestFindResult(x, y);
}

void WebContentsImpl::RequestFindMatchRects(int current_version) {
  GetOrCreateFindRequestManager()->RequestFindMatchRects(current_version);
}

bool WebContentsImpl::CreateRenderViewForInitialEmptyDocument() {
  return CreateRenderViewForRenderManager(
      GetRenderViewHost(), MSG_ROUTING_NONE, MSG_ROUTING_NONE,
      frame_tree_.root()->current_replication_state());
}

service_manager::InterfaceProvider* WebContentsImpl::GetJavaInterfaces() {
  if (!java_interfaces_) {
    service_manager::mojom::InterfaceProviderPtr provider;
    BindInterfaceRegistryForWebContents(mojo::MakeRequest(&provider), this);
    java_interfaces_.reset(new service_manager::InterfaceProvider);
    java_interfaces_->Bind(std::move(provider));
  }
  return java_interfaces_.get();
}

#elif defined(OS_MACOSX)

void WebContentsImpl::SetAllowOtherViews(bool allow) {
  view_->SetAllowOtherViews(allow);
}

bool WebContentsImpl::GetAllowOtherViews() {
  return view_->GetAllowOtherViews();
}

#endif

void WebContentsImpl::OnDidDownloadImage(
    const ImageDownloadCallback& callback,
    int id,
    const GURL& image_url,
    int32_t http_status_code,
    const std::vector<SkBitmap>& images,
    const std::vector<gfx::Size>& original_image_sizes) {
  callback.Run(id, http_status_code, image_url, images, original_image_sizes);
}

void WebContentsImpl::OnDialogClosed(int render_process_id,
                                     int render_frame_id,
                                     IPC::Message* reply_msg,
                                     bool dialog_was_suppressed,
                                     bool success,
                                     const base::string16& user_input) {
  RenderFrameHostImpl* rfh = RenderFrameHostImpl::FromID(render_process_id,
                                                         render_frame_id);
  last_dialog_suppressed_ = dialog_was_suppressed;

  if (is_showing_before_unload_dialog_ && !success) {
    // It is possible for the current RenderFrameHost to have changed in the
    // meantime.  Do not reset the navigation state in that case.
    if (rfh && rfh == rfh->frame_tree_node()->current_frame_host()) {
      rfh->frame_tree_node()->BeforeUnloadCanceled();
      controller_.DiscardNonCommittedEntries();
    }

    for (auto& observer : observers_)
      observer.BeforeUnloadDialogCancelled();
  }

  if (rfh) {
    rfh->JavaScriptDialogClosed(reply_msg, success, user_input);
  } else {
    // Don't leak the sync IPC reply if the RFH or process is gone.
    delete reply_msg;
  }

  is_showing_javascript_dialog_ = false;
  is_showing_before_unload_dialog_ = false;
}

void WebContentsImpl::SetEncoding(const std::string& encoding) {
  if (encoding == last_reported_encoding_)
    return;
  last_reported_encoding_ = encoding;

  canonical_encoding_ = base::GetCanonicalEncodingNameByAliasName(encoding);
}

bool WebContentsImpl::IsHidden() {
  return capturer_count_ == 0 && !should_normally_be_visible_;
}

int WebContentsImpl::GetOuterDelegateFrameTreeNodeId() {
  return node_.outer_contents_frame_tree_node_id();
}

RenderWidgetHostImpl* WebContentsImpl::GetFullscreenRenderWidgetHost() const {
  return RenderWidgetHostImpl::FromID(fullscreen_widget_process_id_,
                                      fullscreen_widget_routing_id_);
}

RenderFrameHostManager* WebContentsImpl::GetRenderManager() const {
  return frame_tree_.root()->render_manager();
}

BrowserPluginGuest* WebContentsImpl::GetBrowserPluginGuest() const {
  return browser_plugin_guest_.get();
}

void WebContentsImpl::SetBrowserPluginGuest(BrowserPluginGuest* guest) {
  CHECK(!browser_plugin_guest_);
  CHECK(guest);
  browser_plugin_guest_.reset(guest);
}

BrowserPluginEmbedder* WebContentsImpl::GetBrowserPluginEmbedder() const {
  return browser_plugin_embedder_.get();
}

void WebContentsImpl::CreateBrowserPluginEmbedderIfNecessary() {
  if (browser_plugin_embedder_)
    return;
  browser_plugin_embedder_.reset(BrowserPluginEmbedder::Create(this));
}

gfx::Size WebContentsImpl::GetSizeForNewRenderView() {
  gfx::Size size;
  if (delegate_)
    size = delegate_->GetSizeForNewRenderView(this);
  if (size.IsEmpty())
    size = GetContainerBounds().size();
  return size;
}

void WebContentsImpl::OnFrameRemoved(RenderFrameHost* render_frame_host) {
  for (auto& observer : observers_)
    observer.FrameDeleted(render_frame_host);
}

void WebContentsImpl::OnPreferredSizeChanged(const gfx::Size& old_size) {
  if (!delegate_)
    return;
  const gfx::Size new_size = GetPreferredSize();
  if (new_size != old_size)
    delegate_->UpdatePreferredSize(this, new_size);
}

std::unique_ptr<WebUIImpl> WebContentsImpl::CreateWebUI(
    const GURL& url,
    const std::string& frame_name) {
  std::unique_ptr<WebUIImpl> web_ui =
      base::MakeUnique<WebUIImpl>(this, frame_name);
  WebUIController* controller =
      WebUIControllerFactoryRegistry::GetInstance()
          ->CreateWebUIControllerForURL(web_ui.get(), url);
  if (controller) {
    web_ui->AddMessageHandler(base::MakeUnique<GenericHandler>());
    web_ui->SetController(controller);
    return web_ui;
  }

  return nullptr;
}

FindRequestManager* WebContentsImpl::GetFindRequestManager() {
  for (WebContentsImpl* contents = this; contents;
       contents = contents->GetOuterWebContents()) {
    if (contents->find_request_manager_)
      return contents->find_request_manager_.get();
  }

  return nullptr;
}

FindRequestManager* WebContentsImpl::GetOrCreateFindRequestManager() {
  if (FindRequestManager* manager = GetFindRequestManager())
    return manager;

  // No existing FindRequestManager found, so one must be created.
  find_request_manager_.reset(new FindRequestManager(this));

  // Concurrent find sessions must not overlap, so destroy any existing
  // FindRequestManagers in any inner WebContentses.
  for (WebContentsImpl* contents : GetWebContentsAndAllInner()) {
    if (contents == this)
      continue;
    if (contents->find_request_manager_) {
      contents->find_request_manager_->StopFinding(
          content::STOP_FIND_ACTION_CLEAR_SELECTION);
      contents->find_request_manager_.release();
    }
  }

  return find_request_manager_.get();
}

void WebContentsImpl::NotifyFindReply(int request_id,
                                      int number_of_matches,
                                      const gfx::Rect& selection_rect,
                                      int active_match_ordinal,
                                      bool final_update) {
  if (delegate_ && !is_being_destroyed_) {
    delegate_->FindReply(this, request_id, number_of_matches, selection_rect,
                         active_match_ordinal, final_update);
  }
}

void WebContentsImpl::IncrementBluetoothConnectedDeviceCount() {
  // Trying to invalidate the tab state while being destroyed could result in a
  // use after free.
  if (IsBeingDestroyed()) {
    return;
  }
  // Notify for UI updates if the state changes.
  bluetooth_connected_device_count_++;
  if (bluetooth_connected_device_count_ == 1) {
    NotifyNavigationStateChanged(INVALIDATE_TYPE_TAB);
  }
}

void WebContentsImpl::DecrementBluetoothConnectedDeviceCount() {
  // Trying to invalidate the tab state while being destroyed could result in a
  // use after free.
  if (IsBeingDestroyed()) {
    return;
  }
  // Notify for UI updates if the state changes.
  DCHECK(bluetooth_connected_device_count_ != 0);
  bluetooth_connected_device_count_--;
  if (bluetooth_connected_device_count_ == 0) {
    NotifyNavigationStateChanged(INVALIDATE_TYPE_TAB);
  }
}

void WebContentsImpl::SetHasPersistentVideo(bool has_persistent_video) {
  if (has_persistent_video_ == has_persistent_video)
    return;

  has_persistent_video_ = has_persistent_video;
  NotifyPreferencesChanged();
  media_web_contents_observer()->RequestPersistentVideo(has_persistent_video);
}

void WebContentsImpl::BrowserPluginGuestWillDetach() {
  WebContentsImpl* outermost = GetOutermostWebContents();
  if (this != outermost && ContainsOrIsFocusedWebContents())
    outermost->SetAsFocusedWebContentsIfNecessary();
}

#if defined(OS_ANDROID)
void WebContentsImpl::NotifyFindMatchRectsReply(
    int version,
    const std::vector<gfx::RectF>& rects,
    const gfx::RectF& active_rect) {
  if (delegate_)
    delegate_->FindMatchRectsReply(this, version, rects, active_rect);
}
#endif

void WebContentsImpl::SetForceDisableOverscrollContent(bool force_disable) {
  force_disable_overscroll_content_ = force_disable;
  if (view_)
    view_->SetOverscrollControllerEnabled(CanOverscrollContent());
}

void WebContentsImpl::MediaStartedPlaying(
    const WebContentsObserver::MediaPlayerInfo& media_info,
    const WebContentsObserver::MediaPlayerId& id) {
  if (media_info.has_video)
    currently_playing_video_count_++;

  for (auto& observer : observers_)
    observer.MediaStartedPlaying(media_info, id);
}

void WebContentsImpl::MediaStoppedPlaying(
    const WebContentsObserver::MediaPlayerInfo& media_info,
    const WebContentsObserver::MediaPlayerId& id) {
  if (media_info.has_video)
    currently_playing_video_count_--;

  for (auto& observer : observers_)
    observer.MediaStoppedPlaying(media_info, id);
}

void WebContentsImpl::MediaResized(
    const gfx::Size& size,
    const WebContentsObserver::MediaPlayerId& id) {
  cached_video_sizes_[id] = size;

  for (auto& observer : observers_)
    observer.MediaResized(size, id);
}

const WebContents::VideoSizeMap&
WebContentsImpl::GetCurrentlyPlayingVideoSizes() {
  return cached_video_sizes_;
}

int WebContentsImpl::GetCurrentlyPlayingVideoCount() {
  return currently_playing_video_count_;
}

void WebContentsImpl::UpdateWebContentsVisibility(bool visible) {
  if (!did_first_set_visible_) {
    // If this WebContents has not yet been set to be visible for the first
    // time, ignore any requests to make it hidden, since resources would
    // immediately be destroyed and only re-created after content loaded. In
    // this state the window content is undefined and can show garbage.
    // However, the page load mechanism requires an activation call through a
    // visibility call to (re)load.
    if (visible) {
      did_first_set_visible_ = true;
      WasShown();
    }
    return;
  }
  if (visible == should_normally_be_visible_)
    return;

  if (visible)
    WasShown();
  else
    WasHidden();
}

void WebContentsImpl::UpdateOverridingUserAgent() {
  NotifyPreferencesChanged();
}

void WebContentsImpl::SetJavaScriptDialogManagerForTesting(
    JavaScriptDialogManager* dialog_manager) {
  dialog_manager_ = dialog_manager;
}

void WebContentsImpl::RemoveBindingSet(const std::string& interface_name) {
  auto it = binding_sets_.find(interface_name);
  if (it != binding_sets_.end())
    binding_sets_.erase(it);
}

bool WebContentsImpl::AddDomainInfoToRapporSample(rappor::Sample* sample) {
  // Here we associate this metric to the main frame URL regardless of what
  // caused it.
  sample->SetStringField("Domain", ::rappor::GetDomainAndRegistrySampleFromGURL(
                                       GetLastCommittedURL()));
  return true;
}

void WebContentsImpl::UpdateUrlForUkmSource(ukm::UkmRecorder* service,
                                            ukm::SourceId ukm_source_id) {
  // Here we associate this metric to the main frame URL regardless of what
  // caused it.
  service->UpdateSourceURL(ukm_source_id, GetLastCommittedURL());
}

void WebContentsImpl::FocusedNodeTouched(bool editable) {
#if defined(OS_WIN)
  // We use the cursor position to determine where the touch occurred.
  RenderWidgetHostView* view = GetRenderWidgetHostView();
  if (!view)
    return;
  POINT cursor_pos = {};
  ::GetCursorPos(&cursor_pos);
  float scale = GetScaleFactorForView(view);
  gfx::Point location_dips_screen =
      gfx::ConvertPointToDIP(scale, gfx::Point(cursor_pos));
  view->FocusedNodeTouched(location_dips_screen, editable);
#endif
}

void WebContentsImpl::DidReceiveCompositorFrame() {
  for (auto& observer : observers_)
    observer.DidReceiveCompositorFrame();
}

void WebContentsImpl::ShowInsecureLocalhostWarningIfNeeded() {
  bool allow_localhost = base::CommandLine::ForCurrentProcess()->HasSwitch(
      switches::kAllowInsecureLocalhost);
  if (!allow_localhost)
    return;

  content::NavigationEntry* entry = GetController().GetLastCommittedEntry();
  if (!entry || !net::IsLocalhost(entry->GetURL().host()))
    return;

  content::SSLStatus ssl_status = entry->GetSSL();
  bool is_cert_error = net::IsCertStatusError(ssl_status.cert_status) &&
                       !net::IsCertStatusMinorError(ssl_status.cert_status);
  if (!is_cert_error)
    return;

  GetMainFrame()->AddMessageToConsole(
      content::CONSOLE_MESSAGE_LEVEL_WARNING,
      base::StringPrintf("This site does not have a valid SSL "
                         "certificate! Without SSL, your site's and "
                         "visitors' data is vulnerable to theft and "
                         "tampering. Get a valid SSL certificate before"
                         " releasing your website to the public."));
}

bool WebContentsImpl::IsShowingContextMenuOnPage() const {
  return showing_context_menu_;
}

void WebContentsImpl::NotifyPreferencesChanged() {
  std::set<RenderViewHost*> render_view_host_set;
  for (FrameTreeNode* node : frame_tree_.Nodes()) {
    RenderWidgetHost* render_widget_host =
        node->current_frame_host()->GetRenderWidgetHost();
    if (!render_widget_host)
      continue;
    RenderViewHost* render_view_host = RenderViewHost::From(render_widget_host);
    if (!render_view_host)
      continue;
    render_view_host_set.insert(render_view_host);
  }
  for (RenderViewHost* render_view_host : render_view_host_set)
    render_view_host->OnWebkitPreferencesChanged();
}

void WebContentsImpl::SetOpenerForNewContents(FrameTreeNode* opener,
                                              bool opener_suppressed) {
  if (opener) {
    FrameTreeNode* new_root = GetFrameTree()->root();

    // For the "original opener", track the opener's main frame instead, because
    // if the opener is a subframe, the opener tracking could be easily bypassed
    // by spawning from a subframe and deleting the subframe.
    // https://crbug.com/705316
    new_root->SetOriginalOpener(opener->frame_tree()->root());

    if (!opener_suppressed) {
      new_root->SetOpener(opener);
      created_with_opener_ = true;
    }
  }
}

}  // namespace content
