// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/quic/quic_chromium_alarm_factory.h"
#include "net/quic/test_task_runner.h"
#include "net/test/gtest_util.h"
#include "net/third_party/quic/core/tls_client_handshaker.h"
#include "net/third_party/quic/test_tools/mock_clock.h"
#include "third_party/blink/renderer/modules/peerconnection/adapters/p2p_quic_packet_transport.h"
#include "third_party/blink/renderer/modules/peerconnection/adapters/p2p_quic_transport_factory_impl.h"
#include "third_party/blink/renderer/modules/peerconnection/adapters/p2p_quic_transport_impl.h"
#include "third_party/blink/renderer/modules/peerconnection/adapters/test/mock_p2p_quic_stream_delegate.h"
#include "third_party/blink/renderer/modules/peerconnection/adapters/test/mock_p2p_quic_transport_delegate.h"
#include "third_party/webrtc/rtc_base/rtccertificate.h"
#include "third_party/webrtc/rtc_base/sslfingerprint.h"
#include "third_party/webrtc/rtc_base/sslidentity.h"

namespace blink {

namespace {

using testing::_;
using testing::Invoke;
using testing::InvokeWithoutArgs;
using ::testing::MakePolymorphicAction;
using ::testing::PolymorphicAction;

const std::string kTriggerRemoteStreamPhrase = "open sesame";
const uint32_t kWriteBufferSize = 100 * 1024;

// A custom gmock Action that fires the given callback. This is used in
// conjuction with the CallbackRunLoop in order to drive the TestTaskRunner
// until callbacks are fired. For example:
//   CallbackRunLoop run_loop(runner());
//   EXPECT_CALL(&object, foo())
//       .WillOnce(FireCallback(run_loop.CreateCallback()));
//   run_loop.RunUntilCallbacksFired(task_runner);
class FireCallbackAction {
 public:
  FireCallbackAction(base::RepeatingCallback<void()> callback)
      : callback_(callback) {}

  template <typename Result, typename ArgumentTuple>
  Result Perform(const ArgumentTuple& args) const {
    callback_.Run();
  }

 private:
  base::RepeatingCallback<void()> callback_;
};

// Returns the custom gmock PolymorphicAction created from the
// FireCallbackAction above.
PolymorphicAction<FireCallbackAction> FireCallback(
    base::RepeatingCallback<void()> callback) {
  return MakePolymorphicAction(FireCallbackAction(callback));
}

// A helper object that can drive a TestTaskRunner's tasks, until
// callbacks are fired.
//
// TODO(https://crbug.com/874296): If the test files get moved to the platform
// directory we will run the tests in a different test environment. In that
// case it will make more sense to use the TestCompletionCallback and the
// RunLoop for driving the test.
class CallbackRunLoop {
 public:
  CallbackRunLoop(scoped_refptr<net::test::TestTaskRunner> task_runner)
      : task_runner_(task_runner) {}

  // Drives the run loop until all created callbacks have been fired.
  // This is done using the |task_runner_|, which runs the tasks
  // in the correct order and then advances the quic::MockClock to the time the
  // task is run.
  void RunUntilCallbacksFired() {
    while (callback_counter_ != 0) {
      ASSERT_GT(task_runner_->GetPostedTasks().size(), 0u);
      task_runner_->RunNextTask();
    }
  }

  // Creates a callback and increments the |callback_counter_|. The callback,
  // when fired, will decrement the counter. This callback must only
  // be Run() once (it is a RepeatingCallback because MakePolymorphicAction()
  // requires that the action is COPYABLE).
  base::RepeatingCallback<void()> CreateCallback() {
    callback_counter_++;
    return base::BindRepeating(&CallbackRunLoop::OnCallbackFired,
                               base::Unretained(this));
  }

 private:
  void OnCallbackFired() { callback_counter_--; }

  scoped_refptr<net::test::TestTaskRunner> task_runner_;
  // Incremented when a callback is created and decremented when the returned
  // callback is later Run().
  size_t callback_counter_ = 0;
};

// This is a fake packet transport to be used by the P2PQuicTransportImpl. It
// allows to easily connect two packet transports together. We send packets
// asynchronously, by using the same alarm factory that is being used for the
// underlying QUIC library.
class FakePacketTransport : public P2PQuicPacketTransport,
                            public quic::QuicAlarm::Delegate {
 public:
  FakePacketTransport(quic::QuicAlarmFactory* alarm_factory,
                      quic::MockClock* clock)
      : alarm_(alarm_factory->CreateAlarm(new AlarmDelegate(this))),
        clock_(clock) {}
  ~FakePacketTransport() override {
    // The write observer should be unset when it is destroyed.
    DCHECK(!write_observer_);
  };

  // Called by QUIC for writing data to the other side. The flow for writing a
  // packet is P2PQuicTransportImpl --> quic::QuicConnection -->
  // quic::QuicPacketWriter --> FakePacketTransport. In this case the
  // FakePacketTransport just writes directly to the FakePacketTransport on the
  // other side.
  int WritePacket(const QuicPacket& packet) override {
    // For the test there should always be a peer_packet_transport_ connected at
    // this point.
    if (!peer_packet_transport_) {
      return 0;
    }
    last_packet_num_ = packet.packet_number;
    packet_queue_.emplace_back(packet.buffer, packet.buf_len);
    alarm_->Cancel();
    // We don't want get 0 RTT.
    alarm_->Set(clock_->Now() + quic::QuicTime::Delta::FromMicroseconds(10));

    return packet.buf_len;
  }

  // Sets the P2PQuicTransportImpl as the delegate.
  void SetReceiveDelegate(
      P2PQuicPacketTransport::ReceiveDelegate* delegate) override {
    // We can't set two ReceiveDelegates for one packet transport.
    DCHECK(!delegate_ || !delegate);
    delegate_ = delegate;
  }

  void SetWriteObserver(
      P2PQuicPacketTransport::WriteObserver* write_observer) override {
    // We can't set two WriteObservers for one packet transport.
    DCHECK(!write_observer_ || !write_observer);
    write_observer_ = write_observer;
  }

  bool Writable() override { return true; }

  // Connects the other FakePacketTransport, so we can write to the peer.
  void ConnectPeerTransport(FakePacketTransport* peer_packet_transport) {
    DCHECK(!peer_packet_transport_);
    peer_packet_transport_ = peer_packet_transport;
  }

  // Disconnects the delegate, so we no longer write to it. The test must call
  // this before destructing either of the packet transports!
  void DisconnectPeerTransport(FakePacketTransport* peer_packet_transport) {
    DCHECK(peer_packet_transport_ == peer_packet_transport);
    peer_packet_transport_ = nullptr;
  }

  // The callback used in order for us to communicate between
  // FakePacketTransports.
  void OnDataReceivedFromPeer(const char* data, size_t data_len) {
    DCHECK(delegate_);
    delegate_->OnPacketDataReceived(data, data_len);
  }

  int last_packet_num() { return last_packet_num_; }

 private:
  // Wraps the FakePacketTransport so that we can pass in a raw pointer that can
  // be reference counted when calling CreateAlarm().
  class AlarmDelegate : public quic::QuicAlarm::Delegate {
   public:
    explicit AlarmDelegate(FakePacketTransport* packet_transport)
        : packet_transport_(packet_transport) {}

    void OnAlarm() override { packet_transport_->OnAlarm(); }

   private:
    FakePacketTransport* packet_transport_;
  };

  // Called when we should write any buffered data.
  void OnAlarm() override {
    // Send the data to the peer at this point.
    peer_packet_transport_->OnDataReceivedFromPeer(
        packet_queue_.front().c_str(), packet_queue_.front().length());
    packet_queue_.pop_front();

    // If there's more packets to be sent out, reset the alarm to send it as the
    // next task.
    if (!packet_queue_.empty()) {
      alarm_->Cancel();
      alarm_->Set(clock_->Now());
    }
  }
  // If async, packets are queued here to send.
  quic::QuicDeque<quic::QuicString> packet_queue_;
  // Alarm used to send data asynchronously.
  quic::QuicArenaScopedPtr<quic::QuicAlarm> alarm_;
  // The P2PQuicTransportImpl, which sets itself as the delegate in its
  // constructor. After receiving data it forwards it along to QUIC.
  P2PQuicPacketTransport::ReceiveDelegate* delegate_ = nullptr;

  // The P2PQuicPacketWriter, which sets itself as a write observer
  // during the P2PQuicTransportFactoryImpl::CreateQuicTransport. It is
  // owned by the QuicConnection and will
  P2PQuicPacketTransport::WriteObserver* write_observer_ = nullptr;

  // The other FakePacketTransport that we are writing to. It's the
  // responsibility of the test to disconnect this delegate
  // (set_delegate(nullptr);) before it is destructed.
  FakePacketTransport* peer_packet_transport_ = nullptr;
  quic::QuicPacketNumber last_packet_num_;
  quic::MockClock* clock_;
};

// A helper class to bundle test objects together. It keeps track of the
// P2PQuicTransport, P2PQuicStream and the associated delegate objects. This
// also keeps track of when callbacks are expected on the delegate objects,
// which allows running the TestTaskRunner tasks until they have been fired.
class QuicPeerForTest {
 public:
  QuicPeerForTest(
      std::unique_ptr<FakePacketTransport> packet_transport,
      std::unique_ptr<MockP2PQuicTransportDelegate> quic_transport_delegate,
      std::unique_ptr<P2PQuicTransportImpl> quic_transport,
      rtc::scoped_refptr<rtc::RTCCertificate> certificate)
      : packet_transport_(std::move(packet_transport)),
        quic_transport_delegate_(std::move(quic_transport_delegate)),
        quic_transport_(std::move(quic_transport)),
        certificate_(certificate) {}

  // A helper that creates a stream and creates and attaches a delegate.
  void CreateStreamWithDelegate() {
    stream_ = quic_transport_->CreateStream();
    stream_delegate_ = std::make_unique<MockP2PQuicStreamDelegate>();
    stream_->SetDelegate(stream_delegate_.get());
    stream_id_ = stream_->id();
  }

  // When a remote stream is created via P2PQuicTransport::Delegate::OnStream,
  // this is called to set the stream.
  void SetStreamAndDelegate(
      P2PQuicStreamImpl* stream,
      std::unique_ptr<MockP2PQuicStreamDelegate> stream_delegate) {
    DCHECK(stream);
    stream_ = stream;
    stream_id_ = stream->id();
    stream_delegate_ = std::move(stream_delegate);
  }

  FakePacketTransport* packet_transport() { return packet_transport_.get(); }

  MockP2PQuicTransportDelegate* quic_transport_delegate() {
    return quic_transport_delegate_.get();
  }

  P2PQuicTransportImpl* quic_transport() { return quic_transport_.get(); }

  rtc::scoped_refptr<rtc::RTCCertificate> certificate() { return certificate_; }

  P2PQuicStreamImpl* stream() const { return stream_; }

  MockP2PQuicStreamDelegate* stream_delegate() const {
    return stream_delegate_.get();
  }

  quic::QuicStreamId stream_id() const { return stream_id_; }

 private:
  std::unique_ptr<FakePacketTransport> packet_transport_;
  std::unique_ptr<MockP2PQuicTransportDelegate> quic_transport_delegate_;
  // The corresponding delegate to |stream_|.
  std::unique_ptr<MockP2PQuicStreamDelegate> stream_delegate_ = nullptr;
  // Created as a result of CreateStreamWithDelegate() or RemoteStreamCreated().
  // Owned by the |quic_transport_|.
  P2PQuicStreamImpl* stream_ = nullptr;
  // The corresponding ID for |stream_|. This can be used to check if the stream
  // is closed at the transport level (after the stream object could be
  // deleted).
  quic::QuicStreamId stream_id_;
  std::unique_ptr<P2PQuicTransportImpl> quic_transport_;
  rtc::scoped_refptr<rtc::RTCCertificate> certificate_;
};

rtc::scoped_refptr<rtc::RTCCertificate> CreateTestCertificate() {
  rtc::KeyParams params;
  rtc::SSLIdentity* ssl_identity =
      rtc::SSLIdentity::Generate("dummy_certificate", params);
  return rtc::RTCCertificate::Create(
      std::unique_ptr<rtc::SSLIdentity>(ssl_identity));
}

// Allows faking a failing handshake.
class FailingProofVerifier : public quic::ProofVerifier {
 public:
  FailingProofVerifier() {}
  ~FailingProofVerifier() override {}

  // ProofVerifier override.
  quic::QuicAsyncStatus VerifyProof(
      const quic::QuicString& hostname,
      const uint16_t port,
      const quic::QuicString& server_config,
      quic::QuicTransportVersion transport_version,
      quic::QuicStringPiece chlo_hash,
      const std::vector<quic::QuicString>& certs,
      const quic::QuicString& cert_sct,
      const quic::QuicString& signature,
      const quic::ProofVerifyContext* context,
      quic::QuicString* error_details,
      std::unique_ptr<quic::ProofVerifyDetails>* verify_details,
      std::unique_ptr<quic::ProofVerifierCallback> callback) override {
    return quic::QUIC_FAILURE;
  }

  quic::QuicAsyncStatus VerifyCertChain(
      const quic::QuicString& hostname,
      const std::vector<quic::QuicString>& certs,
      const quic::ProofVerifyContext* context,
      quic::QuicString* error_details,
      std::unique_ptr<quic::ProofVerifyDetails>* details,
      std::unique_ptr<quic::ProofVerifierCallback> callback) override {
    return quic::QUIC_FAILURE;
  }

  std::unique_ptr<quic::ProofVerifyContext> CreateDefaultContext() override {
    return nullptr;
  }
};
}  // namespace

// Unit tests for the P2PQuicTransport, using an underlying fake packet
// transport that sends packets directly between endpoints. This also tests
// P2PQuicStreams for test cases that involve two streams connected between
// separate endpoints. This is because the P2PQuicStream is highly coupled to
// the P2PQuicSession for communicating between endpoints, so we would like to
// test it with the real session object.
//
// The test is driven using the quic::TestTaskRunner to run posted tasks until
// callbacks have been fired.
class P2PQuicTransportTest : public testing::Test {
 public:
  P2PQuicTransportTest() {}

  ~P2PQuicTransportTest() override {
    // This must be done before desctructing the transports so that we don't
    // have any dangling pointers.
    client_peer_->packet_transport()->DisconnectPeerTransport(
        server_peer_->packet_transport());
    server_peer_->packet_transport()->DisconnectPeerTransport(
        client_peer_->packet_transport());
  }

  // Connects both peer's underlying transports and creates both
  // P2PQuicTransportImpls.
  void Initialize(bool can_respond_to_crypto_handshake = true) {
    // Quic crashes if packets are sent at time 0, and the clock defaults to 0.
    clock_.AdvanceTime(quic::QuicTime::Delta::FromMilliseconds(1000));
    runner_ = new net::test::TestTaskRunner(&clock_);
    net::QuicChromiumAlarmFactory* alarm_factory =
        new net::QuicChromiumAlarmFactory(runner_.get(), &clock_);
    quic_transport_factory_ = std::make_unique<P2PQuicTransportFactoryImpl>(
        &clock_, std::unique_ptr<net::QuicChromiumAlarmFactory>(alarm_factory));

    auto client_packet_transport =
        std::make_unique<FakePacketTransport>(alarm_factory, &clock_);
    auto server_packet_transport =
        std::make_unique<FakePacketTransport>(alarm_factory, &clock_);
    // Connect the transports so that they can speak to each other.
    client_packet_transport->ConnectPeerTransport(
        server_packet_transport.get());
    server_packet_transport->ConnectPeerTransport(
        client_packet_transport.get());
    rtc::scoped_refptr<rtc::RTCCertificate> client_cert =
        CreateTestCertificate();

    auto client_quic_transport_delegate =
        std::make_unique<MockP2PQuicTransportDelegate>();
    std::vector<rtc::scoped_refptr<rtc::RTCCertificate>> client_certificates;
    client_certificates.push_back(client_cert);
    P2PQuicTransportConfig client_config(client_quic_transport_delegate.get(),
                                         client_packet_transport.get(),
                                         client_certificates, kWriteBufferSize);
    client_config.is_server = false;
    client_config.can_respond_to_crypto_handshake =
        can_respond_to_crypto_handshake;
    // We can't downcast a unique_ptr to an object, so we have to release, cast
    // it, then create a unique_ptr of the downcasted pointer.
    P2PQuicTransportImpl* client_quic_transport_ptr =
        static_cast<P2PQuicTransportImpl*>(
            quic_transport_factory_
                ->CreateQuicTransport(std::move(client_config))
                .release());
    std::unique_ptr<P2PQuicTransportImpl> client_quic_transport =
        std::unique_ptr<P2PQuicTransportImpl>(client_quic_transport_ptr);
    client_peer_ = std::make_unique<QuicPeerForTest>(
        std::move(client_packet_transport),
        std::move(client_quic_transport_delegate),
        std::move(client_quic_transport), client_cert);

    auto server_quic_transport_delegate =
        std::make_unique<MockP2PQuicTransportDelegate>();

    rtc::scoped_refptr<rtc::RTCCertificate> server_cert =
        CreateTestCertificate();
    std::vector<rtc::scoped_refptr<rtc::RTCCertificate>> server_certificates;
    server_certificates.push_back(server_cert);
    P2PQuicTransportConfig server_config(server_quic_transport_delegate.get(),
                                         server_packet_transport.get(),
                                         server_certificates, kWriteBufferSize);
    server_config.is_server = true;
    server_config.can_respond_to_crypto_handshake =
        can_respond_to_crypto_handshake;
    P2PQuicTransportImpl* server_quic_transport_ptr =
        static_cast<P2PQuicTransportImpl*>(
            quic_transport_factory_
                ->CreateQuicTransport(std::move(server_config))
                .release());
    std::unique_ptr<P2PQuicTransportImpl> server_quic_transport =
        std::unique_ptr<P2PQuicTransportImpl>(server_quic_transport_ptr);
    server_peer_ = std::make_unique<QuicPeerForTest>(
        std::move(server_packet_transport),
        std::move(server_quic_transport_delegate),
        std::move(server_quic_transport), server_cert);
  }

  // Sets a FailingProofVerifier to the client transport before initializing
  // the its crypto stream. This allows the client to fail the proof
  // verification step during the crypto handshake.
  void InitializeWithFailingProofVerification() {
    // Allows us to initialize the crypto streams after constructing the
    // objects.
    Initialize(false);
    // Create the client crypto config and insert it into the client transport.
    std::unique_ptr<quic::ProofVerifier> proof_verifier(
        new FailingProofVerifier);
    std::unique_ptr<quic::QuicCryptoClientConfig> crypto_client_config =
        std::make_unique<quic::QuicCryptoClientConfig>(
            std::move(proof_verifier),
            quic::TlsClientHandshaker::CreateSslCtx());
    client_peer_->quic_transport()->set_crypto_client_config(
        std::move(crypto_client_config));
    // Now initialize the crypto streams.
    client_peer_->quic_transport()->InitializeCryptoStream();
    server_peer_->quic_transport()->InitializeCryptoStream();
  }

  // Drives the test by running the current tasks that are posted.
  void RunCurrentTasks() {
    size_t posted_tasks_size = runner_->GetPostedTasks().size();
    for (size_t i = 0; i < posted_tasks_size; ++i) {
      runner_->RunNextTask();
    }
  }

  // Starts the handshake, by setting the remote fingerprints and kicking off
  // the handshake from the client.
  void StartHandshake() {
    std::vector<std::unique_ptr<rtc::SSLFingerprint>> server_fingerprints;
    server_fingerprints.emplace_back(rtc::SSLFingerprint::Create(
        "sha-256", server_peer_->certificate()->identity()));
    // The server side doesn't currently need call this to set the remote
    // fingerprints, but once P2P certificate verification is supported in the
    // TLS 1.3 handshake this will ben necessary.
    server_peer_->quic_transport()->Start(std::move(server_fingerprints));

    std::vector<std::unique_ptr<rtc::SSLFingerprint>> client_fingerprints;
    client_fingerprints.emplace_back(rtc::SSLFingerprint::Create(
        "sha-256", client_peer_->certificate()->identity()));
    client_peer_->quic_transport()->Start(std::move(client_fingerprints));
  }

  // Sets up an initial handshake and connection between peers.
  void Connect() {
    CallbackRunLoop run_loop(runner());

    EXPECT_CALL(*client_peer_->quic_transport_delegate(), OnConnected())
        .WillOnce(FireCallback(run_loop.CreateCallback()));
    EXPECT_CALL(*server_peer_->quic_transport_delegate(), OnConnected())
        .WillOnce(FireCallback(run_loop.CreateCallback()));

    StartHandshake();
    run_loop.RunUntilCallbacksFired();
  }

  // Creates a P2PQuicStreamImpl on both the client and server side that are
  // connected to each other. The client's stream is created with
  // P2PQuicTransport::CreateStream, while the server's stream is initiated from
  // the remote (client) side, with P2PQuicStream::Delegate::OnStream. This
  // allows us to test at an integration level with connected streams.
  void SetupConnectedStreams() {
    CallbackRunLoop run_loop(runner());
    // We must already have a secure connection before streams are created.
    ASSERT_TRUE(client_peer_->quic_transport()->IsEncryptionEstablished());
    ASSERT_TRUE(server_peer_->quic_transport()->IsEncryptionEstablished());

    client_peer_->CreateStreamWithDelegate();
    ASSERT_TRUE(client_peer_->stream());
    ASSERT_TRUE(client_peer_->stream_delegate());

    // Send some data to trigger the remote side (server side) to get an
    // incoming stream. We capture the stream and set it's delegate when
    // OnStream gets called on the mock object.
    base::RepeatingCallback<void()> callback = run_loop.CreateCallback();
    QuicPeerForTest* server_peer_ptr = server_peer_.get();
    MockP2PQuicStreamDelegate* stream_delegate =
        new MockP2PQuicStreamDelegate();
    P2PQuicStream* server_stream;
    EXPECT_CALL(*server_peer_->quic_transport_delegate(), OnStream(_))
        .WillOnce(Invoke([&callback, &server_stream,
                          &stream_delegate](P2PQuicStream* stream) {
          stream->SetDelegate(stream_delegate);
          server_stream = stream;
          callback.Run();
        }));

    client_peer_->stream()->WriteData(
        std::vector<uint8_t>(kTriggerRemoteStreamPhrase.begin(),
                             kTriggerRemoteStreamPhrase.end()),
        /*fin=*/false);
    run_loop.RunUntilCallbacksFired();
    // Set the stream and delegate to the |server_peer_|, so that it can be
    // accessed by tests later.
    server_peer_ptr->SetStreamAndDelegate(
        static_cast<P2PQuicStreamImpl*>(server_stream),
        std::unique_ptr<MockP2PQuicStreamDelegate>(stream_delegate));
    ASSERT_TRUE(client_peer_->stream());
    ASSERT_TRUE(client_peer_->stream_delegate());
  }

  void ExpectConnectionNotEstablished() {
    EXPECT_FALSE(client_peer_->quic_transport()->IsEncryptionEstablished());
    EXPECT_FALSE(client_peer_->quic_transport()->IsCryptoHandshakeConfirmed());
    EXPECT_FALSE(server_peer_->quic_transport()->IsCryptoHandshakeConfirmed());
    EXPECT_FALSE(server_peer_->quic_transport()->IsEncryptionEstablished());
  }

  void ExpectTransportsClosed() {
    EXPECT_TRUE(client_peer_->quic_transport()->IsClosed());
    EXPECT_TRUE(server_peer_->quic_transport()->IsClosed());
  }

  // Expects that streams of both the server and client transports are
  // closed.
  void ExpectStreamsClosed() {
    EXPECT_EQ(0u, client_peer_->quic_transport()->GetNumActiveStreams());
    EXPECT_TRUE(client_peer_->quic_transport()->IsClosedStream(
        client_peer()->stream_id()));

    EXPECT_EQ(0u, server_peer_->quic_transport()->GetNumActiveStreams());
    EXPECT_TRUE(server_peer()->quic_transport()->IsClosedStream(
        server_peer()->stream_id()));
  }

  // Exposes these private functions to the test.
  bool IsClientClosed() { return client_peer_->quic_transport()->IsClosed(); }
  bool IsServerClosed() { return server_peer_->quic_transport()->IsClosed(); }

  QuicPeerForTest* client_peer() { return client_peer_.get(); }

  quic::QuicConnection* client_connection() {
    return client_peer_->quic_transport()->connection();
  }

  QuicPeerForTest* server_peer() { return server_peer_.get(); }

  quic::QuicConnection* server_connection() {
    return server_peer_->quic_transport()->connection();
  }

  scoped_refptr<net::test::TestTaskRunner> runner() { return runner_; }

 private:
  quic::MockClock clock_;
  // The TestTaskRunner is used by the QUIC library for setting/firing alarms.
  // We are able to explicitly run these tasks ourselves with the
  // TestTaskRunner.
  scoped_refptr<net::test::TestTaskRunner> runner_;

  std::unique_ptr<P2PQuicTransportFactoryImpl> quic_transport_factory_;
  std::unique_ptr<QuicPeerForTest> client_peer_;
  std::unique_ptr<QuicPeerForTest> server_peer_;
};

// Tests that we can connect two quic transports.
TEST_F(P2PQuicTransportTest, HandshakeConnectsPeers) {
  Initialize();
  Connect();

  EXPECT_TRUE(client_peer()->quic_transport()->IsEncryptionEstablished());
  EXPECT_TRUE(client_peer()->quic_transport()->IsCryptoHandshakeConfirmed());
  EXPECT_TRUE(server_peer()->quic_transport()->IsCryptoHandshakeConfirmed());
  EXPECT_TRUE(server_peer()->quic_transport()->IsEncryptionEstablished());
}

// Tests the standard case for the server side closing the connection.
TEST_F(P2PQuicTransportTest, ServerStops) {
  Initialize();
  Connect();
  CallbackRunLoop run_loop(runner());
  EXPECT_CALL(*client_peer()->quic_transport_delegate(), OnRemoteStopped())
      .WillOnce(FireCallback(run_loop.CreateCallback()));
  EXPECT_CALL(*server_peer()->quic_transport_delegate(), OnRemoteStopped())
      .Times(0);

  server_peer()->quic_transport()->Stop();
  run_loop.RunUntilCallbacksFired();

  ExpectTransportsClosed();
}

// Tests the standard case for the client side closing the connection.
TEST_F(P2PQuicTransportTest, ClientStops) {
  Initialize();
  Connect();
  CallbackRunLoop run_loop(runner());
  EXPECT_CALL(*server_peer()->quic_transport_delegate(), OnRemoteStopped())
      .WillOnce(FireCallback(run_loop.CreateCallback()));
  EXPECT_CALL(*client_peer()->quic_transport_delegate(), OnRemoteStopped())
      .Times(0);

  client_peer()->quic_transport()->Stop();
  run_loop.RunUntilCallbacksFired();

  ExpectTransportsClosed();
}

// Tests that if either side tries to close the connection a second time, it
// will be ignored because the connection has already been closed.
TEST_F(P2PQuicTransportTest, StopAfterStopped) {
  Initialize();
  Connect();
  CallbackRunLoop run_loop(runner());
  EXPECT_CALL(*server_peer()->quic_transport_delegate(), OnRemoteStopped())
      .WillOnce(FireCallback(run_loop.CreateCallback()));
  client_peer()->quic_transport()->Stop();
  run_loop.RunUntilCallbacksFired();

  EXPECT_CALL(*server_peer()->quic_transport_delegate(), OnRemoteStopped())
      .Times(0);
  EXPECT_CALL(*client_peer()->quic_transport_delegate(), OnRemoteStopped())
      .Times(0);

  client_peer()->quic_transport()->Stop();
  server_peer()->quic_transport()->Stop();
  RunCurrentTasks();

  ExpectTransportsClosed();
}

// Tests that when the client closes the connection the subsequent call to
// StartHandshake() will be ignored.
TEST_F(P2PQuicTransportTest, ClientStopsBeforeClientStarts) {
  Initialize();
  CallbackRunLoop run_loop(runner());
  EXPECT_CALL(*server_peer()->quic_transport_delegate(), OnRemoteStopped())
      .WillOnce(FireCallback(run_loop.CreateCallback()));
  client_peer()->quic_transport()->Stop();
  StartHandshake();
  run_loop.RunUntilCallbacksFired();

  ExpectConnectionNotEstablished();
  ExpectTransportsClosed();
}

// Tests that if the server closes the connection before the client starts the
// handshake, the client side will already be closed and Start() will be
// ignored.
TEST_F(P2PQuicTransportTest, ServerStopsBeforeClientStarts) {
  Initialize();
  CallbackRunLoop run_loop(runner());
  EXPECT_CALL(*client_peer()->quic_transport_delegate(), OnRemoteStopped())
      .WillOnce(FireCallback(run_loop.CreateCallback()));
  EXPECT_CALL(*server_peer()->quic_transport_delegate(), OnRemoteStopped())
      .Times(0);

  server_peer()->quic_transport()->Stop();
  StartHandshake();
  run_loop.RunUntilCallbacksFired();

  ExpectConnectionNotEstablished();
  ExpectTransportsClosed();
}

// Tests that when the server's connection fails and then a handshake is
// attempted the transports will not become connected.
TEST_F(P2PQuicTransportTest, ClientConnectionClosesBeforeHandshake) {
  Initialize();
  CallbackRunLoop run_loop(runner());
  EXPECT_CALL(*client_peer()->quic_transport_delegate(),
              OnConnectionFailed(_, _))
      .WillOnce(FireCallback(run_loop.CreateCallback()));
  EXPECT_CALL(*server_peer()->quic_transport_delegate(),
              OnConnectionFailed(_, _))
      .WillOnce(FireCallback(run_loop.CreateCallback()));

  client_connection()->CloseConnection(
      quic::QuicErrorCode::QUIC_INTERNAL_ERROR, "internal error",
      quic::ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
  StartHandshake();
  run_loop.RunUntilCallbacksFired();

  ExpectConnectionNotEstablished();
}

// Tests that when the server's connection fails and then a handshake is
// attempted the transports will not become connected.
TEST_F(P2PQuicTransportTest, ServerConnectionClosesBeforeHandshake) {
  Initialize();
  CallbackRunLoop run_loop(runner());
  EXPECT_CALL(*client_peer()->quic_transport_delegate(),
              OnConnectionFailed(_, _))
      .WillOnce(FireCallback(run_loop.CreateCallback()));
  EXPECT_CALL(*server_peer()->quic_transport_delegate(),
              OnConnectionFailed(_, _))
      .WillOnce(FireCallback(run_loop.CreateCallback()));

  server_connection()->CloseConnection(
      quic::QuicErrorCode::QUIC_INTERNAL_ERROR, "internal error",
      quic::ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
  StartHandshake();
  run_loop.RunUntilCallbacksFired();

  ExpectConnectionNotEstablished();
}

// Tests that the appropriate callbacks are fired when the handshake fails.
TEST_F(P2PQuicTransportTest, HandshakeFailure) {
  InitializeWithFailingProofVerification();
  CallbackRunLoop run_loop(runner());
  EXPECT_CALL(*client_peer()->quic_transport_delegate(),
              OnConnectionFailed(_, _))
      .WillOnce(FireCallback(run_loop.CreateCallback()));
  EXPECT_CALL(*server_peer()->quic_transport_delegate(),
              OnConnectionFailed(_, _))
      .WillOnce(FireCallback(run_loop.CreateCallback()));

  StartHandshake();
  run_loop.RunUntilCallbacksFired();

  ExpectConnectionNotEstablished();
  ExpectTransportsClosed();
}

// Tests that the appropriate callbacks are fired when the client's connection
// fails after the transports have connected.
TEST_F(P2PQuicTransportTest, ClientConnectionFailureAfterConnected) {
  Initialize();
  Connect();
  CallbackRunLoop run_loop(runner());
  EXPECT_CALL(*client_peer()->quic_transport_delegate(),
              OnConnectionFailed(_, /*from_remote=*/false))
      .WillOnce(FireCallback(run_loop.CreateCallback()));
  EXPECT_CALL(*server_peer()->quic_transport_delegate(),
              OnConnectionFailed(_, /*from_remote=*/true))
      .WillOnce(FireCallback(run_loop.CreateCallback()));

  // Close the connection with an internal QUIC error.
  client_connection()->CloseConnection(
      quic::QuicErrorCode::QUIC_INTERNAL_ERROR, "internal error",
      quic::ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
  run_loop.RunUntilCallbacksFired();

  ExpectTransportsClosed();
}

// Tests that the appropriate callbacks are fired when the server's connection
// fails after the transports have connected.
TEST_F(P2PQuicTransportTest, ServerConnectionFailureAfterConnected) {
  Initialize();
  Connect();
  CallbackRunLoop run_loop(runner());
  EXPECT_CALL(*client_peer()->quic_transport_delegate(),
              OnConnectionFailed(_, /*from_remote=*/true))
      .WillOnce(FireCallback(run_loop.CreateCallback()));
  EXPECT_CALL(*server_peer()->quic_transport_delegate(),
              OnConnectionFailed(_, /*from_remote=*/false))
      .WillOnce(FireCallback(run_loop.CreateCallback()));

  server_connection()->CloseConnection(
      quic::QuicErrorCode::QUIC_INTERNAL_ERROR, "internal error",
      quic::ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET);
  run_loop.RunUntilCallbacksFired();

  ExpectTransportsClosed();
}

// Tests that closing the connection with no ACK frame does not make any
// difference in the closing procedure.
TEST_F(P2PQuicTransportTest, ConnectionFailureNoAckFrame) {
  Initialize();
  Connect();
  CallbackRunLoop run_loop(runner());
  EXPECT_CALL(*client_peer()->quic_transport_delegate(),
              OnConnectionFailed(_, _))
      .WillOnce(FireCallback(run_loop.CreateCallback()));
  EXPECT_CALL(*server_peer()->quic_transport_delegate(),
              OnConnectionFailed(_, _))
      .WillOnce(FireCallback(run_loop.CreateCallback()));

  client_connection()->CloseConnection(
      quic::QuicErrorCode::QUIC_INTERNAL_ERROR, "internal error",
      quic::ConnectionCloseBehavior::SEND_CONNECTION_CLOSE_PACKET_WITH_NO_ACK);
  run_loop.RunUntilCallbacksFired();

  ExpectTransportsClosed();
}

// Tests that a silent failure will only close on one side.
TEST_F(P2PQuicTransportTest, ConnectionSilentFailure) {
  Initialize();
  Connect();
  CallbackRunLoop run_loop(runner());
  EXPECT_CALL(*client_peer()->quic_transport_delegate(),
              OnConnectionFailed(_, _))
      .WillOnce(FireCallback(run_loop.CreateCallback()));
  EXPECT_CALL(*server_peer()->quic_transport_delegate(),
              OnConnectionFailed(_, _))
      .Times(0);

  client_connection()->CloseConnection(
      quic::QuicErrorCode::QUIC_INTERNAL_ERROR, "internal error",
      quic::ConnectionCloseBehavior::SILENT_CLOSE);
  run_loop.RunUntilCallbacksFired();

  EXPECT_TRUE(IsClientClosed());
  EXPECT_FALSE(IsServerClosed());
}

// Tests that the client transport can create a stream and an incoming stream
// will be created on the remote server.
TEST_F(P2PQuicTransportTest, ClientCreatesStream) {
  Initialize();
  Connect();
  CallbackRunLoop run_loop(runner());
  client_peer()->CreateStreamWithDelegate();
  ASSERT_TRUE(client_peer()->stream());

  RunCurrentTasks();

  EXPECT_TRUE(client_peer()->quic_transport()->HasOpenDynamicStreams());
  EXPECT_FALSE(server_peer()->quic_transport()->HasOpenDynamicStreams());

  // After sending data across it will trigger a stream to be created on the
  // server side.
  MockP2PQuicStreamDelegate server_stream_delegate;
  base::RepeatingCallback<void()> callback = run_loop.CreateCallback();
  EXPECT_CALL(*server_peer()->quic_transport_delegate(), OnStream(_))
      .WillOnce(
          Invoke([&callback, &server_stream_delegate](P2PQuicStream* stream) {
            ASSERT_TRUE(stream);
            // The Delegate must get immediately set to a new incoming stream.
            stream->SetDelegate(&server_stream_delegate);
            // Allows the run loop to run until this is fired.
            callback.Run();
          }));

  client_peer()->stream()->WriteData(
      std::vector<uint8_t>(kTriggerRemoteStreamPhrase.begin(),
                           kTriggerRemoteStreamPhrase.end()),
      /*fin=*/false);
  run_loop.RunUntilCallbacksFired();

  EXPECT_TRUE(server_peer()->quic_transport()->HasOpenDynamicStreams());
}

// Tests that the server transport can create a stream and an incoming stream
// will be created on the remote client.
TEST_F(P2PQuicTransportTest, ServerCreatesStream) {
  Initialize();
  Connect();
  CallbackRunLoop run_loop(runner());
  server_peer()->CreateStreamWithDelegate();
  ASSERT_TRUE(server_peer()->stream());

  RunCurrentTasks();

  EXPECT_TRUE(server_peer()->quic_transport()->HasOpenDynamicStreams());
  EXPECT_FALSE(client_peer()->quic_transport()->HasOpenDynamicStreams());

  // After sending data across it will trigger a stream to be created on the
  // server side.
  MockP2PQuicStreamDelegate client_stream_delegate;
  base::RepeatingCallback<void()> callback = run_loop.CreateCallback();
  EXPECT_CALL(*client_peer()->quic_transport_delegate(), OnStream(_))
      .WillOnce(
          Invoke([&callback, &client_stream_delegate](P2PQuicStream* stream) {
            ASSERT_TRUE(stream);
            // The Delegate must get immediately set to a new incoming stream.
            stream->SetDelegate(&client_stream_delegate);
            // Allows the run loop to run until this is fired.
            callback.Run();
          }));

  server_peer()->stream()->WriteData(
      std::vector<uint8_t>(kTriggerRemoteStreamPhrase.begin(),
                           kTriggerRemoteStreamPhrase.end()),
      /*fin=*/false);
  run_loop.RunUntilCallbacksFired();

  EXPECT_TRUE(client_peer()->quic_transport()->HasOpenDynamicStreams());
}

// Tests that when the client transport calls Stop() it closes its outgoing
// stream, which, in turn closes the incoming stream on the server quic
// transport.
TEST_F(P2PQuicTransportTest, ClientClosingConnectionClosesStreams) {
  Initialize();
  Connect();
  SetupConnectedStreams();

  client_peer()->quic_transport()->Stop();
  RunCurrentTasks();

  ExpectTransportsClosed();
  ExpectStreamsClosed();
}

// Tests that when the server transport calls Stop() it closes its incoming
// stream, which, in turn closes the outgoing stream on the client quic
// transport.
TEST_F(P2PQuicTransportTest, ServerClosingConnectionClosesStreams) {
  Initialize();
  Connect();
  SetupConnectedStreams();

  server_peer()->quic_transport()->Stop();
  RunCurrentTasks();

  ExpectTransportsClosed();
  ExpectStreamsClosed();
}

// Tests that calling Reset() will close both side's streams for reading and
// writing.
TEST_F(P2PQuicTransportTest, ClientStreamReset) {
  Initialize();
  Connect();
  SetupConnectedStreams();
  CallbackRunLoop run_loop(runner());

  EXPECT_CALL(*server_peer()->stream_delegate(), OnRemoteReset())
      .WillOnce(FireCallback(run_loop.CreateCallback()));

  client_peer()->stream()->Reset();
  run_loop.RunUntilCallbacksFired();
  ExpectStreamsClosed();
}

// Tests that calling Reset() will close both side's streams for reading and
// writing.
TEST_F(P2PQuicTransportTest, ServerStreamReset) {
  Initialize();
  Connect();
  SetupConnectedStreams();
  CallbackRunLoop run_loop(runner());

  EXPECT_CALL(*client_peer()->stream_delegate(), OnRemoteReset())
      .WillOnce(FireCallback(run_loop.CreateCallback()));

  server_peer()->stream()->Reset();
  run_loop.RunUntilCallbacksFired();

  ExpectStreamsClosed();
}

// Tests the basic case for sending a FIN bit on both sides.
TEST_F(P2PQuicTransportTest, StreamClosedAfterSendingAndReceivingFin) {
  Initialize();
  Connect();
  SetupConnectedStreams();
  CallbackRunLoop run_loop(runner());

  EXPECT_CALL(*server_peer()->stream_delegate(), OnRemoteFinish())
      .WillOnce(FireCallback(run_loop.CreateCallback()));

  client_peer()->stream()->WriteData({}, /*fin=*/true);
  run_loop.RunUntilCallbacksFired();

  ASSERT_EQ(1u, server_peer()->quic_transport()->GetNumActiveStreams());
  ASSERT_EQ(1u, client_peer()->quic_transport()->GetNumActiveStreams());
  EXPECT_TRUE(client_peer()->stream()->write_side_closed());
  EXPECT_FALSE(client_peer()->stream()->reading_stopped());
  EXPECT_FALSE(server_peer()->stream()->write_side_closed());
  EXPECT_TRUE(server_peer()->stream()->reading_stopped());
  EXPECT_FALSE(server_peer()->quic_transport()->IsClosedStream(
      server_peer()->stream_id()));
  EXPECT_FALSE(client_peer()->quic_transport()->IsClosedStream(
      client_peer()->stream_id()));

  EXPECT_CALL(*client_peer()->stream_delegate(), OnRemoteFinish())
      .WillOnce(FireCallback(run_loop.CreateCallback()));

  server_peer()->stream()->WriteData({}, /*fin=*/true);
  run_loop.RunUntilCallbacksFired();

  // This is required so that the client acks the FIN back to the server side
  // and the server side removes its zombie streams.
  RunCurrentTasks();

  ASSERT_EQ(0u, server_peer()->quic_transport()->GetNumActiveStreams());
  ASSERT_EQ(0u, client_peer()->quic_transport()->GetNumActiveStreams());
  EXPECT_TRUE(server_peer()->quic_transport()->IsClosedStream(
      server_peer()->stream_id()));
  EXPECT_TRUE(client_peer()->quic_transport()->IsClosedStream(
      client_peer()->stream_id()));
}

// Tests that if a Reset() is called after sending a FIN bit, both sides close
// down properly.
TEST_F(P2PQuicTransportTest, StreamResetAfterSendingFin) {
  Initialize();
  Connect();
  SetupConnectedStreams();
  CallbackRunLoop run_loop(runner());

  EXPECT_CALL(*server_peer()->stream_delegate(), OnRemoteFinish())
      .WillOnce(FireCallback(run_loop.CreateCallback()));

  client_peer()->stream()->WriteData({}, /*fin=*/true);
  run_loop.RunUntilCallbacksFired();

  EXPECT_CALL(*server_peer()->stream_delegate(), OnRemoteReset())
      .WillOnce(FireCallback(run_loop.CreateCallback()));
  EXPECT_CALL(*client_peer()->stream_delegate(), OnRemoteReset()).Times(0);

  client_peer()->stream()->Reset();
  run_loop.RunUntilCallbacksFired();

  ExpectStreamsClosed();
}

// Tests that if a Reset() is called after receiving a stream frame with the FIN
// bit set from the remote side, both sides close down properly.
TEST_F(P2PQuicTransportTest, StreamResetAfterReceivingFin) {
  Initialize();
  Connect();
  SetupConnectedStreams();
  CallbackRunLoop run_loop(runner());

  EXPECT_CALL(*server_peer()->stream_delegate(), OnRemoteFinish())
      .WillOnce(FireCallback(run_loop.CreateCallback()));

  client_peer()->stream()->WriteData({}, /*fin=*/true);
  run_loop.RunUntilCallbacksFired();

  EXPECT_CALL(*client_peer()->stream_delegate(), OnRemoteReset())
      .WillOnce(FireCallback(run_loop.CreateCallback()));
  EXPECT_CALL(*server_peer()->stream_delegate(), OnRemoteReset()).Times(0);

  // The server stream has received its FIN bit from the remote side, and
  // responds with a Reset() to close everything down.
  server_peer()->stream()->Reset();
  run_loop.RunUntilCallbacksFired();

  ExpectStreamsClosed();
}

}  // namespace blink
