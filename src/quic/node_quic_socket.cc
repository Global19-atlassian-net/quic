#include "node_quic_socket-inl.h"  // NOLINT(build/include)
#include "async_wrap-inl.h"
#include "debug_utils.h"
#include "env-inl.h"
#include "memory_tracker-inl.h"
#include "nghttp2/nghttp2.h"
#include "node.h"
#include "node_buffer.h"
#include "node_crypto.h"
#include "node_internals.h"
#include "node_mem-inl.h"
#include "node_quic_crypto.h"
#include "node_quic_session-inl.h"
#include "node_quic_util-inl.h"
#include "node_sockaddr-inl.h"
#include "req_wrap-inl.h"
#include "util.h"
#include "uv.h"
#include "v8.h"

#include <random>

namespace node {

using crypto::EntropySource;
using crypto::SecureContext;

using v8::ArrayBufferView;
using v8::Boolean;
using v8::Context;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::Number;
using v8::Object;
using v8::ObjectTemplate;
using v8::PropertyAttribute;
using v8::String;
using v8::Value;

namespace quic {

namespace {
inline uint32_t GenerateReservedVersion(
    const sockaddr* addr,
    uint32_t version) {
  socklen_t addrlen = SocketAddress::GetLength(addr);
  uint32_t h = 0x811C9DC5u;
  const uint8_t* p = reinterpret_cast<const uint8_t*>(addr);
  const uint8_t* ep = p + addrlen;
  for (; p != ep; ++p) {
    h ^= *p;
    h *= 0x01000193u;
  }
  version = htonl(version);
  p =  reinterpret_cast<const uint8_t*>(&version);
  ep = p + sizeof(version);
  for (; p != ep; ++p) {
    h ^= *p;
    h *= 0x01000193u;
  }
  h &= 0xf0f0f0f0u;
  h |= 0x0a0a0a0au;
  return h;
}

bool IsShortHeader(
    uint32_t version,
    const uint8_t* pscid,
    size_t pscidlen) {
  return version == NGTCP2_PROTO_VER &&
         pscid == nullptr &&
         pscidlen == 0;
}
}  // namespace

QuicPacket::QuicPacket(const char* diagnostic_label, size_t len) :
    data_(len),
    diagnostic_label_(diagnostic_label) {
  CHECK_LE(len, NGTCP2_MAX_PKT_SIZE);
}

QuicPacket::QuicPacket(const QuicPacket& other) :
  QuicPacket(other.diagnostic_label_, other.data_.size()) {
  memcpy(data_.data(), other.data_.data(), other.data_.size());
}

const char* QuicPacket::diagnostic_label() const {
  return diagnostic_label_ != nullptr ?
      diagnostic_label_ : "unspecified";
}

void QuicPacket::MemoryInfo(MemoryTracker* tracker) const {
  tracker->TrackField("data", data_);
}

QuicSocketListener::~QuicSocketListener() {
  if (socket_)
    socket_->RemoveListener(this);
}

void QuicSocketListener::OnError(ssize_t code) {
  if (previous_listener_ != nullptr)
    previous_listener_->OnError(code);
}

void QuicSocketListener::OnError(int code) {
  if (previous_listener_ != nullptr)
    previous_listener_->OnError(code);
}

void QuicSocketListener::OnSessionReady(BaseObjectPtr<QuicSession> session) {
  if (previous_listener_ != nullptr)
    previous_listener_->OnSessionReady(session);
}

void QuicSocketListener::OnServerBusy(bool busy) {
  if (previous_listener_ != nullptr)
    previous_listener_->OnServerBusy(busy);
}

void QuicSocketListener::OnEndpointDone(QuicEndpoint* endpoint) {
  if (previous_listener_ != nullptr)
    previous_listener_->OnEndpointDone(endpoint);
}

void QuicSocketListener::OnDestroy() {
  if (previous_listener_ != nullptr)
    previous_listener_->OnDestroy();
}

void JSQuicSocketListener::OnError(ssize_t code) {
  Environment* env = socket()->env();
  HandleScope scope(env->isolate());
  Context::Scope context_scope(env->context());
  Local<Value> arg = Number::New(env->isolate(), static_cast<double>(code));
  socket()->MakeCallback(env->quic_on_socket_error_function(), 1, &arg);
}

void JSQuicSocketListener::OnError(int code) {
  Environment* env = socket()->env();
  HandleScope scope(env->isolate());
  Context::Scope context_scope(env->context());
  Local<Value> arg = Integer::New(env->isolate(), code);
  socket()->MakeCallback(env->quic_on_socket_error_function(), 1, &arg);
}

void JSQuicSocketListener::OnSessionReady(BaseObjectPtr<QuicSession> session) {
  Environment* env = socket()->env();
  Local<Value> arg = session->object();
  Context::Scope context_scope(env->context());
  socket()->MakeCallback(env->quic_on_session_ready_function(), 1, &arg);
}

void JSQuicSocketListener::OnServerBusy(bool busy) {
  Environment* env = socket()->env();
  HandleScope handle_scope(env->isolate());
  Context::Scope context_scope(env->context());
  Local<Value> arg = Boolean::New(env->isolate(), busy);
  socket()->MakeCallback(env->quic_on_socket_server_busy_function(), 1, &arg);
}

void JSQuicSocketListener::OnEndpointDone(QuicEndpoint* endpoint) {
  Environment* env = socket()->env();
  HandleScope scope(env->isolate());
  Context::Scope context_scope(env->context());
  MakeCallback(
      env->isolate(),
      endpoint->object(),
      env->ondone_string(),
      0, nullptr);
}

void JSQuicSocketListener::OnDestroy() {
  // Do nothing here.
}

QuicEndpoint::QuicEndpoint(
    Environment* env,
    Local<Object> wrap,
    QuicSocket* listener,
    Local<Object> udp_wrap) :
    BaseObject(env, wrap),
    listener_(listener) {
  MakeWeak();
  udp_ = static_cast<UDPWrapBase*>(
      udp_wrap->GetAlignedPointerFromInternalField(
          UDPWrapBase::kUDPWrapBaseField));
  CHECK_NOT_NULL(udp_);
  udp_->set_listener(this);
  strong_ptr_.reset(udp_->GetAsyncWrap());
}

void QuicEndpoint::MemoryInfo(MemoryTracker* tracker) const {}

uv_buf_t QuicEndpoint::OnAlloc(size_t suggested_size) {
  return env()->AllocateManaged(suggested_size).release();
}

void QuicEndpoint::OnRecv(
    ssize_t nread,
    const uv_buf_t& buf_,
    const sockaddr* addr,
    unsigned int flags) {
  AllocatedBuffer buf(env(), buf_);

  if (nread <= 0) {
    if (nread < 0)
      listener_->OnError(this, nread);
    return;
  }

  listener_->OnReceive(
      nread,
      std::move(buf),
      local_address(),
      addr,
      flags);
}

ReqWrap<uv_udp_send_t>* QuicEndpoint::CreateSendWrap(size_t msg_size) {
  return listener_->OnCreateSendWrap(msg_size);
}

void QuicEndpoint::OnSendDone(ReqWrap<uv_udp_send_t>* wrap, int status) {
  DecrementPendingCallbacks();
  listener_->OnSendDone(wrap, status);
  if (!has_pending_callbacks() && waiting_for_callbacks_)
    listener_->OnEndpointDone(this);
}

void QuicEndpoint::OnAfterBind() {
  listener_->OnBind(this);
}

QuicSocket::QuicSocket(
    Environment* env,
    Local<Object> wrap,
    uint64_t retry_token_expiration,
    size_t max_connections_per_host,
    size_t max_stateless_resets_per_host,
    uint32_t options,
    QlogMode qlog,
    const uint8_t* session_reset_secret,
    bool disable_stateless_reset)
  : AsyncWrap(env, wrap, AsyncWrap::PROVIDER_QUICSOCKET),
    alloc_info_(MakeAllocator()),
    options_(options),
    max_connections_per_host_(max_connections_per_host),
    max_stateless_resets_per_host_(max_stateless_resets_per_host),
    retry_token_expiration_(retry_token_expiration),
    qlog_(qlog),
    server_alpn_(NGTCP2_ALPN_H3),
    stats_buffer_(
        env->isolate(),
        sizeof(socket_stats_) / sizeof(uint64_t),
        reinterpret_cast<uint64_t*>(&socket_stats_)) {
  MakeWeak();
  PushListener(&default_listener_);

  Debug(this, "New QuicSocket created.");

  EntropySource(token_secret_, kTokenSecretLen);
  socket_stats_.created_at = uv_hrtime();

  if (disable_stateless_reset)
    set_flag(QUICSOCKET_FLAGS_DISABLE_STATELESS_RESET);

  // Set the session reset secret to the one provided or random.
  // Note that a random secret is going to make it exceedingly
  // difficult for the session reset token to be useful.
  if (session_reset_secret != nullptr) {
    memcpy(reset_token_secret_,
           session_reset_secret,
           NGTCP2_STATELESS_RESET_TOKENLEN);
  } else {
    EntropySource(reset_token_secret_, NGTCP2_STATELESS_RESET_TOKENLEN);
  }

  // TODO(@jasnell): For now, the following is a check rather than properly
  // handled. Before this code moves out of experimental, this should be
  // properly handled.
  wrap->DefineOwnProperty(
      env->context(),
      env->stats_string(),
      stats_buffer_.GetJSArray(),
      PropertyAttribute::ReadOnly).Check();
}

QuicSocket::~QuicSocket() {
  uint64_t now = uv_hrtime();
  Debug(this,
        "QuicSocket destroyed.\n"
        "  Duration: %" PRIu64 "\n"
        "  Bound Duration: %" PRIu64 "\n"
        "  Listen Duration: %" PRIu64 "\n"
        "  Bytes Received: %" PRIu64 "\n"
        "  Bytes Sent: %" PRIu64 "\n"
        "  Packets Received: %" PRIu64 "\n"
        "  Packets Sent: %" PRIu64 "\n"
        "  Packets Ignored: %" PRIu64 "\n"
        "  Server Sessions: %" PRIu64 "\n"
        "  Client Sessions: %" PRIu64 "\n"
        "  Stateless Resets: %" PRIu64 "\n",
        now - socket_stats_.created_at,
        socket_stats_.bound_at > 0 ? now - socket_stats_.bound_at : 0,
        socket_stats_.listen_at > 0 ? now - socket_stats_.listen_at : 0,
        socket_stats_.bytes_received,
        socket_stats_.bytes_sent,
        socket_stats_.packets_received,
        socket_stats_.packets_sent,
        socket_stats_.packets_ignored,
        socket_stats_.server_sessions,
        socket_stats_.client_sessions,
        socket_stats_.stateless_reset_count);
  QuicSocketListener* listener = listener_;
  listener_->OnDestroy();
  // Remove the listener if it didn't remove itself already.
  if (listener == listener_)
    RemoveListener(listener_);
}

void QuicSocket::MemoryInfo(MemoryTracker* tracker) const {
  tracker->TrackField("endpoints", endpoints_);
  tracker->TrackField("sessions", sessions_);
  tracker->TrackField("dcid_to_scid", dcid_to_scid_);
  tracker->TrackField("addr_counts", addr_counts_);
  tracker->TrackField("reset_counts", reset_counts_);
  tracker->TrackField("token_map", token_map_);
  tracker->TrackField("validated_addrs", validated_addrs_);
  tracker->TrackField("stats_buffer", stats_buffer_);
  tracker->TrackFieldWithSize(
      "current_ngtcp2_memory",
      current_ngtcp2_memory_);
}

void QuicSocket::Listen(
    SecureContext* sc,
    const sockaddr* preferred_address,
    const std::string& alpn,
    uint32_t options) {
  CHECK_NOT_NULL(sc);
  CHECK(!server_secure_context_);
  CHECK(!is_flag_set(QUICSOCKET_FLAGS_SERVER_LISTENING));
  Debug(this, "Starting to listen.");
  server_session_config_.Set(env(), preferred_address);
  server_secure_context_.reset(sc);
  server_alpn_ = alpn;
  server_options_ = options;
  set_flag(QUICSOCKET_FLAGS_SERVER_LISTENING);
  socket_stats_.listen_at = uv_hrtime();
  ReceiveStart();
}

void QuicSocket::OnError(QuicEndpoint* endpoint, ssize_t error) {
  Debug(this, "Reading data from UDP socket failed. Error %" PRId64, error);
  listener_->OnError(error);
}

ReqWrap<uv_udp_send_t>* QuicSocket::OnCreateSendWrap(size_t msg_size) {
  HandleScope handle_scope(env()->isolate());
  Local<Object> obj;
  if (!env()->quicsocketsendwrap_constructor_template()
          ->NewInstance(env()->context()).ToLocal(&obj)) return nullptr;
  return last_created_send_wrap_ = new SendWrap(env(), obj, msg_size);
}

void QuicSocket::OnEndpointDone(QuicEndpoint* endpoint) {
  Debug(this, "Endpoint has no pending callbacks.");
  listener_->OnEndpointDone(endpoint);
}

void QuicSocket::OnBind(QuicEndpoint* endpoint) {
  const SocketAddress& local_address = endpoint->local_address();
  Debug(this, "Endpoint %s:%d bound",
        local_address.GetAddress().c_str(),
        local_address.GetPort());
  socket_stats_.bound_at = uv_hrtime();
}

BaseObjectPtr<QuicSession> QuicSocket::FindSession(const QuicCID& cid) {
  BaseObjectPtr<QuicSession> session;
  auto session_it = sessions_.find(cid);
  if (session_it == std::end(sessions_)) {
    auto scid_it = dcid_to_scid_.find(cid);
    if (scid_it != std::end(dcid_to_scid_)) {
      session_it = sessions_.find(scid_it->second);
      CHECK_NE(session_it, std::end(sessions_));
      session = session_it->second;
    }
  } else {
    session = session_it->second;
  }
  return session;
}

// This is the primary entry point for data received for the QuicSocket.
bool QuicSocket::MaybeStatelessReset(
    const QuicCID& dcid,
    const QuicCID& scid,
    ssize_t nread,
    const uint8_t* data,
    const SocketAddress& local_addr,
    const sockaddr* remote_addr,
    unsigned int flags) {
  if (UNLIKELY(is_stateless_reset_disabled() || nread < 16))
    return false;
  StatelessResetToken possible_token(
      data + nread - NGTCP2_STATELESS_RESET_TOKENLEN);
  auto it = token_map_.find(possible_token);
  if (it == token_map_.end())
    return false;
  Debug(this, "Received a stateless reset token");
  return it->second->Receive(nread, data, local_addr, remote_addr, flags);
}

void QuicSocket::OnReceive(
    ssize_t nread,
    AllocatedBuffer buf,
    const SocketAddress& local_addr,
    const sockaddr* remote_addr,
    unsigned int flags) {
  Debug(this, "Receiving %d bytes from the UDP socket.", nread);

  // When diagnostic packet loss is enabled, the packet will be randomly
  // dropped based on the rx_loss_ probability.
  if (UNLIKELY(is_diagnostic_packet_loss(rx_loss_))) {
    Debug(this, "Simulating received packet loss.");
    return;
  }

  IncrementSocketStat(nread, &socket_stats_, &socket_stats::bytes_received);

  const uint8_t* data = reinterpret_cast<const uint8_t*>(buf.data());

  uint32_t pversion;
  const uint8_t* pdcid;
  size_t pdcidlen;
  const uint8_t* pscid;
  size_t pscidlen;

  // This is our first check to see if the received data can be
  // processed as a QUIC packet. If this fails, then the QUIC packet
  // header is invalid and cannot be processed. If this fails, all
  // we can do is ignore it. It's questionable whether we should even
  // increment the packets_ignored statistic here but for now we do.
  if (ngtcp2_pkt_decode_version_cid(
        &pversion,
        &pdcid,
        &pdcidlen,
        &pscid,
        &pscidlen,
        data, nread, kScidLen) < 0) {
    IncrementSocketStat(1, &socket_stats_, &socket_stats::packets_ignored);
    return;
  }

  // QUIC currently requires CID lengths of max NGTCP2_MAX_CIDLEN. The
  // ngtcp2 API allows non-standard lengths, and we may want to allow
  // non-standard lengths later. But for now, we're going to ignore any
  // packet with a non-standard CID length.
  if (pdcidlen > NGTCP2_MAX_CIDLEN || pscidlen > NGTCP2_MAX_CIDLEN) {
    IncrementSocketStat(1, &socket_stats_, &socket_stats::packets_ignored);
    return;
  }

  QuicCID dcid(pdcid, pdcidlen);
  QuicCID scid(pscid, pscidlen);

  std::string dcid_hex = dcid.ToHex();
  Debug(this, "Received a QUIC packet for dcid %s", dcid_hex.c_str());

  BaseObjectPtr<QuicSession> session = FindSession(dcid);

  // If a session is not found, there are three possible reasons:
  // 1. The session has not been created yet
  // 2. The session existed once but we've lost the local state for it
  // 3. This is a malicious or malformed packet.
  //
  // In the case of #1, the packet must be a valid initial packet with
  // a long-form QUIC header. In the case of #2, the packet must have
  // a short-form QUIC header and we should send a stateless reset token.
  // Differentiating between cases 2 and 3 can be difficult, however.
  if (!session) {
    Debug(this, "There is no existing session for dcid %s", dcid_hex.c_str());
    bool is_short_header = IsShortHeader(pversion, pscid, pscidlen);

    // Handle possible reception of a stateless reset token...
    if (is_short_header &&
        MaybeStatelessReset(
            dcid,
            scid,
            nread,
            data,
            local_addr,
            remote_addr,
            flags)) {
      Debug(this, "Handled stateless reset");
      return;
    }

    // AcceptInitialPacket will first validate that the packet can be
    // accepted, then create a new server QuicSession instance if able
    // to do so. If a new instance cannot be created (for any reason),
    // the session shared_ptr will be empty on return.
    session = AcceptInitialPacket(
        pversion,
        dcid,
        scid,
        nread,
        data,
        local_addr,
        remote_addr,
        flags);

    // There are many reasons why a server QuicSession could not be
    // created. The most common will be invalid packets or incorrect
    // QUIC version. In any of these cases, however, to prevent a
    // potential attacker from causing us to consume resources,
    // we're just going to ignore the packet. It is possible that
    // the AcceptInitialPacket sent a version negotiation packet,
    // or (in the future) a CONNECTION_CLOSE packet.
    if (!session) {
      Debug(this, "Unable to create a new server QuicSession.");

      if (is_short_header &&
          SendStatelessReset(dcid, local_addr, remote_addr, nread)) {
        Debug(this, "Sent stateless reset");
        IncrementSocketStat(
            1, &socket_stats_,
            &socket_stats::stateless_reset_count);
        return;
      }
      IncrementSocketStat(1, &socket_stats_, &socket_stats::packets_ignored);
      return;
    }
  }

  CHECK(session);

  // If the packet could not successfully processed for any reason (possibly
  // due to being malformed or malicious in some way) we mark it ignored.
  if (!session->Receive(nread, data, local_addr, remote_addr, flags)) {
    IncrementSocketStat(1, &socket_stats_, &socket_stats::packets_ignored);
    return;
  }

  IncrementSocketStat(1, &socket_stats_, &socket_stats::packets_received);
}

void QuicSocket::SendVersionNegotiation(
      uint32_t version,
      const QuicCID& dcid,
      const QuicCID& scid,
      const SocketAddress& local_addr,
      const sockaddr* remote_addr) {
  uint32_t sv[2];
  sv[0] = GenerateReservedVersion(remote_addr, version);
  sv[1] = NGTCP2_PROTO_VER;

  uint8_t unused_random;
  EntropySource(&unused_random, 1);

  size_t pktlen = dcid.length() + scid.length() + (sizeof(sv)) + 7;

  auto packet = QuicPacket::Create("version negotiation", pktlen);
  ssize_t nwrite = ngtcp2_pkt_write_version_negotiation(
      packet->data(),
      NGTCP2_MAX_PKTLEN_IPV6,
      unused_random,
      dcid.data(),
      dcid.length(),
      scid.data(),
      scid.length(),
      sv,
      arraysize(sv));
  if (nwrite <= 0)
    return;
  packet->set_length(nwrite);
  SocketAddress remote_address(remote_addr);
  SendPacket(local_addr, remote_address, std::move(packet));
}

bool QuicSocket::SendStatelessReset(
    const QuicCID& cid,
    const SocketAddress& local_addr,
    const sockaddr* remote_addr,
    size_t source_len) {
  if (UNLIKELY(is_stateless_reset_disabled()))
    return false;
  constexpr static size_t kRandlen = NGTCP2_MIN_STATELESS_RESET_RANDLEN * 5;
  constexpr static size_t kMinStatelessResetLen = 41;
  uint8_t token[NGTCP2_STATELESS_RESET_TOKENLEN];
  uint8_t random[kRandlen];

  // Per the QUIC spec, we need to protect against sending too
  // many stateless reset tokens to an endpoint to prevent
  // endless looping.
  if (GetCurrentStatelessResetCounter(remote_addr) >=
          max_stateless_resets_per_host_) {
    return false;
  }
  // Per the QUIC spec, a stateless reset token must be strictly
  // smaller than the packet that triggered it. This is one of the
  // mechanisms to prevent infinite looping exchange of stateless
  // tokens with the peer.
  // An endpoint should never send a stateless reset token smaller than
  // 41 bytes per the QUIC spec. The reason is that packets less than
  // 41 bytes may allow an observer to determine that it's a stateless
  // reset.
  size_t pktlen = source_len - 1;
  if (pktlen < kMinStatelessResetLen)
    return false;

  GenerateResetToken(token, reset_token_secret_, cid.cid());
  EntropySource(random, kRandlen);

  auto packet = QuicPacket::Create("stateless reset", pktlen);
  ssize_t nwrite =
      ngtcp2_pkt_write_stateless_reset(
        reinterpret_cast<uint8_t*>(packet->data()),
        NGTCP2_MAX_PKTLEN_IPV4,
        token,
        random,
        kRandlen);
    if (nwrite < static_cast<ssize_t>(kMinStatelessResetLen))
      return false;
    packet->set_length(nwrite);
    SocketAddress remote_address(remote_addr);
    IncrementStatelessResetCounter(remote_address);
    return SendPacket(local_addr, remote_address, std::move(packet)) == 0;
}

bool QuicSocket::SendRetry(
    uint32_t version,
    const QuicCID& dcid,
    const QuicCID& scid,
    const SocketAddress& local_addr,
    const sockaddr* remote_addr) {
  uint8_t token[256];
  size_t tokenlen = sizeof(token);

  if (!GenerateRetryToken(
          token,
          &tokenlen,
          remote_addr,
          dcid.cid(),
          token_secret_)) {
    return false;
  }

  ngtcp2_pkt_hd hd;
  hd.version = version;
  hd.flags = NGTCP2_PKT_FLAG_LONG_FORM;
  hd.type = NGTCP2_PKT_RETRY;
  hd.pkt_num = 0;
  hd.token = nullptr;
  hd.tokenlen = 0;
  hd.len = 0;
  hd.dcid = *scid.cid();
  hd.scid.datalen = kScidLen;

  EntropySource(hd.scid.data, kScidLen);

  size_t pktlen = tokenlen + (2 * NGTCP2_MAX_CIDLEN) + scid.length() + 8;
  CHECK_LE(pktlen, NGTCP2_MAX_PKT_SIZE);

  auto packet = QuicPacket::Create("retry", pktlen);
  ssize_t nwrite =
      ngtcp2_pkt_write_retry(
          reinterpret_cast<uint8_t*>(packet->data()),
          NGTCP2_MAX_PKTLEN_IPV4,
          &hd,
          dcid.cid(),
          token,
          tokenlen);
  if (nwrite <= 0)
    return false;
  packet->set_length(nwrite);
  SocketAddress remote_address(remote_addr);
  return SendPacket(local_addr, remote_address, std::move(packet)) == 0;
}

BaseObjectPtr<QuicSession> QuicSocket::AcceptInitialPacket(
    uint32_t version,
    const QuicCID& dcid,
    const QuicCID& scid,
    ssize_t nread,
    const uint8_t* data,
    const SocketAddress& local_addr,
    const sockaddr* remote_addr,
    unsigned int flags) {
  HandleScope handle_scope(env()->isolate());
  Context::Scope context_scope(env()->context());
  ngtcp2_pkt_hd hd;
  ngtcp2_cid ocid;
  ngtcp2_cid* ocid_ptr = nullptr;
  uint64_t initial_connection_close = NGTCP2_NO_ERROR;

  if (!is_flag_set(QUICSOCKET_FLAGS_SERVER_LISTENING)) {
    Debug(this, "QuicSocket is not listening");
    return {};
  }

  // Perform some initial checks on the packet to see if it is an
  // acceptable initial packet with the right QUIC version.
  switch (QuicSession::Accept(&hd, version, data, nread)) {
    case QuicSession::InitialPacketResult::PACKET_VERSION:
      SendVersionNegotiation(
          version,
          dcid,
          scid,
          local_addr,
          remote_addr);
      return {};
    case QuicSession::InitialPacketResult::PACKET_RETRY:
      Debug(this, "0RTT Packet. Sending retry.");
      SendRetry(version, dcid, scid, local_addr, remote_addr);
      return {};
    case QuicSession::InitialPacketResult::PACKET_IGNORE:
      return {};
    case QuicSession::InitialPacketResult::PACKET_OK:
      break;
  }

  // If the server is busy, new connections will be shut down immediately
  // after the initial keys are installed.
  if (is_flag_set(QUICSOCKET_FLAGS_SERVER_BUSY)) {
    Debug(this, "QuicSocket is busy");
    initial_connection_close = NGTCP2_SERVER_BUSY;
  }

  // Check to see if the number of connections for this peer has been exceeded.
  // If the count has been exceeded, shutdown the connection immediately
  // after the initial keys are installed.
  if (GetCurrentSocketAddressCounter(remote_addr) >=
      max_connections_per_host_) {
    Debug(this, "Connection count for address exceeded");
    initial_connection_close = NGTCP2_SERVER_BUSY;
  }

  // QUIC has address validation built in to the handshake but allows for
  // an additional explicit validation request using RETRY frames. If we
  // are using explicit validation, we check for the existence of a valid
  // retry token in the packet. If one does not exist, we send a retry with
  // a new token. If it does exist, and if it's valid, we grab the original
  // cid and continue.
  //
  // If initial_connection_close is not NGTCP2_NO_ERROR, skip address
  // validation since we're going to reject the connection anyway.
  if (initial_connection_close == NGTCP2_NO_ERROR &&
      is_option_set(QUICSOCKET_OPTIONS_VALIDATE_ADDRESS) &&
      hd.type == NGTCP2_PKT_INITIAL) {
      // If the VALIDATE_ADDRESS_LRU option is set, IsValidatedAddress
      // will check to see if the given address is in the validated_addrs_
      // LRU cache. If it is, we'll skip the validation step entirely.
      // The VALIDATE_ADDRESS_LRU option is disable by default.
    if (!is_validated_address(remote_addr)) {
      Debug(this, "Performing explicit address validation.");
      if (InvalidRetryToken(
              hd.token,
              hd.tokenlen,
              remote_addr,
              &ocid,
              token_secret_,
              retry_token_expiration_)) {
        Debug(this, "A valid retry token was not found. Sending retry.");
        SendRetry(version, dcid, scid, local_addr, remote_addr);
        return {};
      }
      Debug(this, "A valid retry token was found. Continuing.");
      set_validated_address(remote_addr);
      ocid_ptr = &ocid;
    } else {
      Debug(this, "Skipping validation for recently validated address.");
    }
  }

  BaseObjectPtr<QuicSession> session =
      QuicSession::CreateServer(
          this,
          server_session_config_,
          dcid.cid(),
          local_addr,
          remote_addr,
          scid.cid(),
          ocid_ptr,
          version,
          server_alpn_,
          server_options_,
          initial_connection_close,
          qlog_);

  listener_->OnSessionReady(session);

  return session;
}

QuicSocket::SendWrap::SendWrap(
    Environment* env,
    Local<Object> req_wrap_obj,
    size_t total_length)
  : ReqWrap(env, req_wrap_obj, PROVIDER_QUICSOCKET),
    total_length_(total_length) {
}

std::string QuicSocket::SendWrap::MemoryInfoName() const {
  return "QuicSendWrap";
}

void QuicSocket::SendWrap::MemoryInfo(MemoryTracker* tracker) const {
  tracker->TrackField("session", session_);
  tracker->TrackField("packet", packet_);
}

int QuicSocket::SendPacket(
    const SocketAddress& local_addr,
    const SocketAddress& remote_addr,
    std::unique_ptr<QuicPacket> packet,
    BaseObjectPtr<QuicSession> session) {
  // If the packet is empty, there's nothing to do
  if (packet->length() == 0)
    return 0;

  Debug(this, "Sending %" PRIu64 " bytes to %s:%d from %s:%d (label: %s)",
        packet->length(),
        remote_addr.GetAddress().c_str(),
        remote_addr.GetPort(),
        local_addr.GetAddress().c_str(),
        local_addr.GetPort(),
        packet->diagnostic_label());

  // If DiagnosticPacketLoss returns true, it will call Done() internally
  if (UNLIKELY(is_diagnostic_packet_loss(tx_loss_))) {
    Debug(this, "Simulating transmitted packet loss.");
    return 0;
  }

  last_created_send_wrap_ = nullptr;
  uv_buf_t buf = uv_buf_init(
      reinterpret_cast<char*>(packet->data()),
      packet->length());
  CHECK_NOT_NULL(preferred_endpoint_);
  int err = preferred_endpoint_->Send(&buf, 1, remote_addr.data());

  if (err != 0) {
    if (err > 0) err = 0;
    OnSend(err, packet.get());
  } else {
    CHECK_NOT_NULL(last_created_send_wrap_);
    last_created_send_wrap_->set_packet(std::move(packet));
    if (session)
      last_created_send_wrap_->set_session(session);
  }
  return err;
}

void QuicSocket::OnSend(int status, QuicPacket* packet) {
  if (status == 0) {
    Debug(this, "Sent %" PRIu64 " bytes (label: %s)",
          packet->length(),
          packet->diagnostic_label());
    IncrementSocketStat(
        packet->length(),
        &socket_stats_,
        &socket_stats::bytes_sent);
    IncrementSocketStat(
        1,
        &socket_stats_,
        &socket_stats::packets_sent);
  } else {
    Debug(this, "Failed to send %" PRIu64 " bytes (status: %d, label: %s)",
          packet->length(),
          status,
          packet->diagnostic_label());
  }
}

void QuicSocket::OnSendDone(ReqWrap<uv_udp_send_t>* wrap, int status) {
  std::unique_ptr<SendWrap> req_wrap(static_cast<SendWrap*>(wrap));
  OnSend(status, req_wrap->packet());
}

void QuicSocket::CheckAllocatedSize(size_t previous_size) const {
  CHECK_GE(current_ngtcp2_memory_, previous_size);
}

void QuicSocket::IncreaseAllocatedSize(size_t size) {
  current_ngtcp2_memory_ += size;
}

void QuicSocket::DecreaseAllocatedSize(size_t size) {
  current_ngtcp2_memory_ -= size;
}

void QuicSocket::PushListener(QuicSocketListener* listener) {
  CHECK_NOT_NULL(listener);
  CHECK(!listener->socket_);

  listener->previous_listener_ = listener_;
  listener->socket_.reset(this);

  listener_ = listener;
}

void QuicSocket::RemoveListener(QuicSocketListener* listener) {
  CHECK_NOT_NULL(listener);

  QuicSocketListener* previous;
  QuicSocketListener* current;

  for (current = listener_, previous = nullptr;
       /* No loop condition because we want a crash if listener is not found */
       ; previous = current, current = current->previous_listener_) {
    CHECK_NOT_NULL(current);
    if (current == listener) {
      if (previous != nullptr)
        previous->previous_listener_ = current->previous_listener_;
      else
        listener_ = listener->previous_listener_;
      break;
    }
  }

  listener->socket_.reset();
  listener->previous_listener_ = nullptr;
}

// JavaScript API
namespace {
void NewQuicEndpoint(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args.IsConstructCall());
  CHECK(args[0]->IsObject());
  QuicSocket* socket;
  ASSIGN_OR_RETURN_UNWRAP(&socket, args[0].As<Object>());
  CHECK(args[1]->IsObject());
  CHECK_GE(args[1].As<Object>()->InternalFieldCount(),
           UDPWrapBase::kUDPWrapBaseField);
  new QuicEndpoint(env, args.This(), socket, args[1].As<Object>());
}

void NewQuicSocket(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args.IsConstructCall());

  uint32_t options;
  uint32_t retry_token_expiration;
  uint32_t max_connections_per_host;
  uint32_t max_stateless_resets_per_host;

  if (!args[0]->Uint32Value(env->context()).To(&options) ||
      !args[1]->Uint32Value(env->context()).To(&retry_token_expiration) ||
      !args[2]->Uint32Value(env->context()).To(&max_connections_per_host) ||
      !args[3]->Uint32Value(env->context())
          .To(&max_stateless_resets_per_host)) {
    return;
  }
  CHECK_GE(retry_token_expiration, MIN_RETRYTOKEN_EXPIRATION);
  CHECK_LE(retry_token_expiration, MAX_RETRYTOKEN_EXPIRATION);

  const uint8_t* session_reset_secret = nullptr;
  if (args[5]->IsArrayBufferView()) {
    ArrayBufferViewContents<uint8_t> buf(args[5].As<ArrayBufferView>());
    CHECK_EQ(buf.length(), kTokenSecretLen);
    session_reset_secret = buf.data();
  }

  new QuicSocket(
      env,
      args.This(),
      retry_token_expiration,
      max_connections_per_host,
      max_stateless_resets_per_host,
      options,
      args[4]->IsTrue() ? QlogMode::kEnabled : QlogMode::kDisabled,
      session_reset_secret,
      args[5]->IsTrue());
}

void QuicSocketAddEndpoint(const FunctionCallbackInfo<Value>& args) {
  QuicSocket* socket;
  ASSIGN_OR_RETURN_UNWRAP(&socket, args.Holder());
  CHECK(args[0]->IsObject());
  QuicEndpoint* endpoint;
  ASSIGN_OR_RETURN_UNWRAP(&endpoint, args[0].As<Object>());
  socket->AddEndpoint(endpoint, args[1]->IsTrue());
}

// Enabling diagnostic packet loss enables a mode where the QuicSocket
// instance will randomly ignore received packets in order to simulate
// packet loss. This is not an API that should be enabled in production
// but is useful when debugging and diagnosing performance issues.
// Diagnostic packet loss is enabled by setting either the tx or rx
// arguments to a value between 0.0 and 1.0. Setting both values to 0.0
// disables the mechanism.
void QuicSocketSetDiagnosticPacketLoss(
    const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  QuicSocket* socket;
  ASSIGN_OR_RETURN_UNWRAP(&socket, args.Holder());
  double rx, tx;
  if (!args[0]->NumberValue(env->context()).To(&rx) ||
      !args[1]->NumberValue(env->context()).To(&tx)) return;
  CHECK_GE(rx, 0.0f);
  CHECK_GE(tx, 0.0f);
  CHECK_LE(rx, 1.0f);
  CHECK_LE(tx, 1.0f);
  socket->set_diagnostic_packet_loss(rx, tx);
}

void QuicSocketDestroy(const FunctionCallbackInfo<Value>& args) {
  QuicSocket* socket;
  ASSIGN_OR_RETURN_UNWRAP(&socket, args.Holder());
  socket->ReceiveStop();
}

void QuicSocketListen(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  QuicSocket* socket;
  ASSIGN_OR_RETURN_UNWRAP(&socket, args.Holder(),
                          args.GetReturnValue().Set(UV_EBADF));
  CHECK(args[0]->IsObject() &&
        env->secure_context_constructor_template()->HasInstance(args[0]));
  SecureContext* sc;
  ASSIGN_OR_RETURN_UNWRAP(&sc, args[0].As<Object>(),
                          args.GetReturnValue().Set(UV_EBADF));

  sockaddr_storage preferred_address_storage;
  const sockaddr* preferred_address = nullptr;
  if (args[1]->IsString()) {
    node::Utf8Value preferred_address_host(args.GetIsolate(), args[1]);
    int32_t preferred_address_family;
    uint32_t preferred_address_port;
    if (!args[2]->Int32Value(env->context()).To(&preferred_address_family) ||
        !args[3]->Uint32Value(env->context()).To(&preferred_address_port))
      return;
    if (SocketAddress::ToSockAddr(
            preferred_address_family,
            *preferred_address_host,
            preferred_address_port,
            &preferred_address_storage) != nullptr) {
      preferred_address =
          reinterpret_cast<const sockaddr*>(&preferred_address_storage);
    }
  }

  std::string alpn(NGTCP2_ALPN_H3);
  if (args[4]->IsString()) {
    Utf8Value val(env->isolate(), args[4]);
    alpn = val.length();
    alpn += *val;
  }

  uint32_t options = 0;
  if (!args[5]->Uint32Value(env->context()).To(&options)) return;

  socket->Listen(sc, preferred_address, alpn, options);
}

void QuicSocketStopListening(const FunctionCallbackInfo<Value>& args) {
  QuicSocket* socket;
  ASSIGN_OR_RETURN_UNWRAP(&socket, args.Holder());
  socket->StopListening();
}

void QuicSocketset_server_busy(const FunctionCallbackInfo<Value>& args) {
  QuicSocket* socket;
  ASSIGN_OR_RETURN_UNWRAP(&socket, args.Holder());
  CHECK_EQ(args.Length(), 1);
  socket->set_server_busy(args[0]->IsTrue());
}

void QuicSocketToggleStatelessReset(const FunctionCallbackInfo<Value>& args) {
  QuicSocket* socket;
  ASSIGN_OR_RETURN_UNWRAP(&socket, args.Holder());
  args.GetReturnValue().Set(socket->ToggleStatelessReset());
}

void QuicEndpointWaitForPendingCallbacks(
    const FunctionCallbackInfo<Value>& args) {
  QuicEndpoint* endpoint;
  ASSIGN_OR_RETURN_UNWRAP(&endpoint, args.Holder());
  endpoint->WaitForPendingCallbacks();
}

}  // namespace

void QuicEndpoint::Initialize(
    Environment* env,
    Local<Object> target,
    Local<Context> context) {
  Isolate* isolate = env->isolate();
  Local<String> class_name = FIXED_ONE_BYTE_STRING(isolate, "QuicEndpoint");
  Local<FunctionTemplate> endpoint = env->NewFunctionTemplate(NewQuicEndpoint);
  endpoint->SetClassName(class_name);
  endpoint->InstanceTemplate()->SetInternalFieldCount(1);
  env->SetProtoMethod(endpoint,
                      "waitForPendingCallbacks",
                      QuicEndpointWaitForPendingCallbacks);
  endpoint->InstanceTemplate()->Set(env->owner_symbol(), Null(isolate));

  target->Set(
      context,
      class_name,
      endpoint->GetFunction(context).ToLocalChecked())
          .FromJust();
}

void QuicSocket::Initialize(
    Environment* env,
    Local<Object> target,
    Local<Context> context) {
  Isolate* isolate = env->isolate();
  Local<String> class_name = FIXED_ONE_BYTE_STRING(isolate, "QuicSocket");
  Local<FunctionTemplate> socket = env->NewFunctionTemplate(NewQuicSocket);
  socket->SetClassName(class_name);
  socket->InstanceTemplate()->SetInternalFieldCount(1);
  socket->InstanceTemplate()->Set(env->owner_symbol(), Null(isolate));
  env->SetProtoMethod(socket,
                      "addEndpoint",
                      QuicSocketAddEndpoint);
  env->SetProtoMethod(socket,
                      "destroy",
                      QuicSocketDestroy);
  env->SetProtoMethod(socket,
                      "listen",
                      QuicSocketListen);
  env->SetProtoMethod(socket,
                      "setDiagnosticPacketLoss",
                      QuicSocketSetDiagnosticPacketLoss);
  env->SetProtoMethod(socket,
                      "setServerBusy",
                      QuicSocketset_server_busy);
  env->SetProtoMethod(socket,
                      "stopListening",
                      QuicSocketStopListening);
  env->SetProtoMethod(socket,
                      "toggleStatelessReset",
                      QuicSocketToggleStatelessReset);
  socket->Inherit(HandleWrap::GetConstructorTemplate(env));
  target->Set(context, class_name,
              socket->GetFunction(env->context()).ToLocalChecked()).FromJust();

  // TODO(addaleax): None of these templates actually are constructor templates.
  Local<ObjectTemplate> sendwrap_template = ObjectTemplate::New(isolate);
  sendwrap_template->SetInternalFieldCount(1);
  env->set_quicsocketsendwrap_constructor_template(sendwrap_template);
}

}  // namespace quic
}  // namespace node
