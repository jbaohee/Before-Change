// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/modules/peerconnection/adapters/quic_transport_host.h"

#include <utility>

#include "third_party/blink/renderer/modules/peerconnection/adapters/ice_transport_host.h"
#include "third_party/blink/renderer/modules/peerconnection/adapters/p2p_quic_transport_factory.h"
#include "third_party/blink/renderer/modules/peerconnection/adapters/quic_stream_host.h"
#include "third_party/blink/renderer/modules/peerconnection/adapters/quic_stream_proxy.h"
#include "third_party/blink/renderer/modules/peerconnection/adapters/quic_transport_proxy.h"
#include "third_party/blink/renderer/modules/peerconnection/adapters/web_rtc_cross_thread_copier.h"
#include "third_party/blink/renderer/platform/cross_thread_functional.h"
#include "third_party/blink/renderer/platform/web_task_runner.h"

namespace blink {

QuicTransportHost::QuicTransportHost(
    base::WeakPtr<QuicTransportProxy> proxy,
    std::unique_ptr<P2PQuicTransportFactory> quic_transport_factory)
    : quic_transport_factory_(std::move(quic_transport_factory)),
      proxy_(std::move(proxy)) {
  DETACH_FROM_THREAD(thread_checker_);
  DCHECK(quic_transport_factory_);
  DCHECK(proxy_);
}

QuicTransportHost::~QuicTransportHost() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  // If the TaskRunner this is getting initialized on is destroyed before
  // Initialize is called then |ice_transport_host_| may still be null.
  if (ice_transport_host_) {
    ice_transport_host_->DisconnectConsumer(this);
  }
}

void QuicTransportHost::Initialize(
    IceTransportHost* ice_transport_host,
    quic::Perspective perspective,
    const std::vector<rtc::scoped_refptr<rtc::RTCCertificate>>& certificates) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(ice_transport_host);
  DCHECK(!ice_transport_host_);
  ice_transport_host_ = ice_transport_host;
  // TODO(https://crbug.com/874296): Pass through values for read and write
  // stream buffer sizes in the P2PQuicTransportConfig. Currently this is just
  // set to the same size as the QUIC receive window size (24 MB).
  uint32_t stream_buffer_size = 24 * 1024 * 1024;
  P2PQuicTransportConfig config(
      this, ice_transport_host->ConnectConsumer(this)->packet_transport(),
      certificates, stream_buffer_size);
  config.is_server = (perspective == quic::Perspective::IS_SERVER);
  quic_transport_ =
      quic_transport_factory_->CreateQuicTransport(std::move(config));
}

scoped_refptr<base::SingleThreadTaskRunner> QuicTransportHost::proxy_thread()
    const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return ice_transport_host_->proxy_thread();
}

scoped_refptr<base::SingleThreadTaskRunner> QuicTransportHost::host_thread()
    const {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  return ice_transport_host_->host_thread();
}

void QuicTransportHost::Start(
    std::vector<std::unique_ptr<rtc::SSLFingerprint>> remote_fingerprints) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  quic_transport_->Start(std::move(remote_fingerprints));
}

void QuicTransportHost::Stop() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  quic_transport_->Stop();
}

void QuicTransportHost::CreateStream(
    std::unique_ptr<QuicStreamHost> stream_host) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  P2PQuicStream* p2p_stream = quic_transport_->CreateStream();
  stream_host->Initialize(this, p2p_stream);
  stream_hosts_.insert(
      std::make_pair(stream_host.get(), std::move(stream_host)));
}

void QuicTransportHost::OnRemoveStream(QuicStreamHost* stream_host_to_remove) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);

  auto it = stream_hosts_.find(stream_host_to_remove);
  DCHECK(it != stream_hosts_.end());
  stream_hosts_.erase(it);
}

void QuicTransportHost::OnRemoteStopped() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  stream_hosts_.clear();
  PostCrossThreadTask(
      *proxy_thread(), FROM_HERE,
      CrossThreadBind(&QuicTransportProxy::OnRemoteStopped, proxy_));
}

void QuicTransportHost::OnConnectionFailed(const std::string& error_details,
                                           bool from_remote) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  stream_hosts_.clear();
  PostCrossThreadTask(*proxy_thread(), FROM_HERE,
                      CrossThreadBind(&QuicTransportProxy::OnConnectionFailed,
                                      proxy_, error_details, from_remote));
}

void QuicTransportHost::OnConnected() {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  PostCrossThreadTask(
      *proxy_thread(), FROM_HERE,
      CrossThreadBind(&QuicTransportProxy::OnConnected, proxy_));
}

void QuicTransportHost::OnStream(P2PQuicStream* p2p_stream) {
  DCHECK_CALLED_ON_VALID_THREAD(thread_checker_);
  DCHECK(p2p_stream);

  auto stream_proxy = std::make_unique<QuicStreamProxy>();
  auto stream_host = std::make_unique<QuicStreamHost>();
  stream_proxy->set_host(stream_host->AsWeakPtr());
  stream_host->set_proxy(stream_proxy->AsWeakPtr());

  stream_host->Initialize(this, p2p_stream);

  stream_hosts_.insert(
      std::make_pair(stream_host.get(), std::move(stream_host)));

  PostCrossThreadTask(*proxy_thread(), FROM_HERE,
                      CrossThreadBind(&QuicTransportProxy::OnStream, proxy_,
                                      WTF::Passed(std::move(stream_proxy))));
}

}  // namespace blink
