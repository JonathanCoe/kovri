/**                                                                                           //
 * Copyright (c) 2013-2018, The Kovri I2P Router Project                                      //
 *                                                                                            //
 * All rights reserved.                                                                       //
 *                                                                                            //
 * Redistribution and use in source and binary forms, with or without modification, are       //
 * permitted provided that the following conditions are met:                                  //
 *                                                                                            //
 * 1. Redistributions of source code must retain the above copyright notice, this list of     //
 *    conditions and the following disclaimer.                                                //
 *                                                                                            //
 * 2. Redistributions in binary form must reproduce the above copyright notice, this list     //
 *    of conditions and the following disclaimer in the documentation and/or other            //
 *    materials provided with the distribution.                                               //
 *                                                                                            //
 * 3. Neither the name of the copyright holder nor the names of its contributors may be       //
 *    used to endorse or promote products derived from this software without specific         //
 *    prior written permission.                                                               //
 *                                                                                            //
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY        //
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF    //
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL     //
 * THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,       //
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,               //
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS    //
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,          //
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF    //
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.               //
 *                                                                                            //
 * Parts of the project are originally copyright (c) 2013-2015 The PurpleI2P Project          //
 */

#include "core/router/transports/ssu/session.h"

#include <boost/bind.hpp>
#include <boost/endian/conversion.hpp>

#include "core/crypto/diffie_hellman.h"
#include "core/crypto/hash.h"
#include "core/crypto/rand.h"

#include "core/router/context.h"
#include "core/router/transports/ssu/packet.h"
#include "core/router/transports/ssu/server.h"
#include "core/router/transports/impl.h"

#include "core/util/byte_stream.h"
#include "core/util/log.h"
#include "core/util/timestamp.h"

namespace kovri {
namespace core {

// TODO(anonimal): bytestream refactor

std::uint8_t* SSUSessionPacket::MAC() const {
  return data;
}

std::uint8_t* SSUSessionPacket::IV() const {
  return data + std::size_t(16);
}

void SSUSessionPacket::PutFlag(
    std::uint8_t flag) const {
  data[32] = flag;
}

void SSUSessionPacket::PutTime(
    std::uint32_t time) const {
  return core::OutputByteStream::Write<std::uint32_t>(&data[33], time);
}

std::uint8_t* SSUSessionPacket::Encrypted() const {
  return data + std::size_t(32);
}

SSUSession::SSUSession(
    SSUServer& server,
    boost::asio::ip::udp::endpoint& remote_endpoint,
    std::shared_ptr<const kovri::core::RouterInfo> router,
    bool peer_test)
    : TransportSession(router),
      m_Server(server),
      m_RemoteEndpoint(remote_endpoint),
      m_Timer(GetService()),
      m_PeerTest(peer_test),
      m_State(SessionState::Unknown),
      m_IsSessionKey(false),
      m_RelayTag(0),
      m_Data(*this),
      m_IsDataReceived(false),
      m_Exception(__func__) {
  m_CreationTime = kovri::core::GetSecondsSinceEpoch();
}

SSUSession::~SSUSession() {}

boost::asio::io_service& SSUSession::GetService() {
  return m_Server.GetService();
}

bool SSUSession::CreateAESandMACKey(
    const std::uint8_t* pub_key) {
  // TODO(anonimal): this try block should be handled entirely by caller
  try {
    kovri::core::DiffieHellman dh;
    std::array<std::uint8_t, 256> shared_key;
    if (!dh.Agree(shared_key.data(), m_DHKeysPair->private_key.data(), pub_key)) {
      LOG(error)
        << "SSUSession:" << GetFormattedSessionInfo()
        << "couldn't create shared key";
      return false;
    }
    std::uint8_t* session_key = m_SessionKey();
    std::uint8_t* mac_key = m_MACKey();
    if (shared_key.at(0) & 0x80) {
      session_key[0] = 0;
      memcpy(session_key + 1, shared_key.data(), 31);
      memcpy(mac_key, shared_key.data() + 31, 32);
    } else if (shared_key.at(0)) {
      memcpy(session_key, shared_key.data(), 32);
      memcpy(mac_key, shared_key.data() + 32, 32);
    } else {
      // find first non-zero byte
      auto non_zero = shared_key.data() + 1;
      while (!*non_zero) {
        non_zero++;
        if (non_zero - shared_key.data() > 32) {
          LOG(warning)
            << "SSUSession:" << GetFormattedSessionInfo()
            << "first 32 bytes of shared key is all zeros. Ignored";
          return false;
        }
      }
      memcpy(session_key, non_zero, 32);
      kovri::core::SHA256().CalculateDigest(
          mac_key,
          non_zero,
          64 - (non_zero - shared_key.data()));
    }
    m_SessionKeyEncryption.SetKey(m_SessionKey);
    m_SessionKeyDecryption.SetKey(m_SessionKey);
  } catch (...) {
    m_Exception.Dispatch(__func__);
    // TODO(anonimal): review if we need to safely break control, ensure exception handling by callers
    throw;
  }
  return m_IsSessionKey = true;
}

/**
 *
 * Process encrypted/decrypted SSU messages
 *
 */

void SSUSession::ProcessNextMessage(
    std::uint8_t* buf,
    std::size_t len,
    const boost::asio::ip::udp::endpoint& sender_endpoint) {
  m_NumReceivedBytes += len;
  LOG(debug)
    << "SSUSession:" << GetFormattedSessionInfo()
    << "--> " << len << " bytes transferred << "
    << GetNumReceivedBytes() << " total bytes received";
  kovri::core::transports.UpdateReceivedBytes(len);
  if (m_State == SessionState::Introduced) {
    // HolePunch received
    LOG(debug) << "SSUSession: SSU HolePunch of " << len << " bytes received";
    m_State = SessionState::Unknown;
    Connect();
  } else {
    if (!len)
      return;  // ignore zero-length packets
    if (m_State == SessionState::Established)
      ScheduleTermination();
    // Try session key first
    if (m_IsSessionKey && Validate(buf, len, m_MACKey))
      {
        DecryptSessionKey(buf, len);
    } else {
      // try intro key depending on side
      auto intro_key = GetIntroKey();
      bool validated = false;
      if (intro_key)
        {
          if (Validate(buf, len, intro_key))
            {
              validated = true;
              Decrypt(buf, len, intro_key);
            }
        }
      if (!validated)
        {
          // try own intro key
          auto address = context.GetRouterInfo().GetSSUAddress();
          if (!address)
            {
              LOG(error) << "SSUSession: " << __func__
                         << ": SSU is not supported";
              return;
            }
          if (!Validate(buf, len, address->key))
            {
              LOG(error) << "SSUSession: MAC verification failed " << len
                         << " bytes from " << sender_endpoint;
              m_Server.DeleteSession(shared_from_this());
              return;
            }
          Decrypt(buf, len, address->key);
        }
    }
    // successfully decrypted
    ProcessDecryptedMessage(buf, len, sender_endpoint);
  }
}

void SSUSession::ProcessDecryptedMessage(
    std::uint8_t* buf,
    std::size_t len,
    const boost::asio::ip::udp::endpoint& sender_endpoint) {
  len -= (len & 0x0F);  // %16, delete extra padding
  SSUPacketParser parser(buf, len);
  std::unique_ptr<SSUPacket> packet;
  try {
    packet = parser.ParsePacket();
  } catch(const std::exception& e) {
    LOG(error)
      << "SSUSession: invalid SSU session packet from " << sender_endpoint
      << " --> " << e.what();
    return;
  } catch (...) {
    LOG(error)
      << "SSUSession: invalid SSU session packet from " << sender_endpoint
      << " --> unknown exception";
    return;
  }
  switch (packet->GetHeader()->GetPayloadType()) {
    case SSUPayloadType::Data:
      ProcessData(packet.get());
      break;
    case SSUPayloadType::SessionRequest:
      ProcessSessionRequest(packet.get(), sender_endpoint);
      break;
    case SSUPayloadType::SessionCreated:
      ProcessSessionCreated(packet.get());
      break;
    case SSUPayloadType::SessionConfirmed:
      ProcessSessionConfirmed(packet.get());
      break;
    case SSUPayloadType::PeerTest:
      LOG(debug) << "SSUSession: PeerTest received";
      ProcessPeerTest(packet.get(), sender_endpoint);
      break;
    case SSUPayloadType::SessionDestroyed:
      LOG(debug) << "SSUSession: SessionDestroy received";
      m_Server.DeleteSession(shared_from_this());
      break;
    case SSUPayloadType::RelayResponse:
      ProcessRelayResponse(packet.get());
      if (m_State != SessionState::Established)
        m_Server.DeleteSession(shared_from_this());
      break;
    case SSUPayloadType::RelayRequest:
      LOG(debug) << "SSUSession: RelayRequest received";
      ProcessRelayRequest(packet.get(), sender_endpoint);
      break;
    case SSUPayloadType::RelayIntro:
      LOG(debug) << "SSUSession: RelayIntro received";
      ProcessRelayIntro(packet.get());
      break;
    default:
      LOG(warning)
        << "SSUSession: unexpected payload type: "
        << static_cast<int>(packet->GetHeader()->GetPayloadType());
  }
}

/**
 * SSU messages (payload types)
 * ------------------------
 *
 *  There are 10 defined SSU messages:
 *
 *  0 SessionRequest
 *  1 SessionCreated
 *  2 SessionConfirmed
 *  3 RelayRequest
 *  4 RelayResponse
 *  5 RelayIntro
 *  6 Data
 *  7 PeerTest
 *  8 SessionDestroyed (implemented as of 0.8.9)
 *  n/a HolePunch
 */

/**
 *
 * Payload type 0: SessionRequest
 *
 */

void SSUSession::ProcessSessionRequest(
    SSUPacket* pkt,
    const boost::asio::ip::udp::endpoint& sender_endpoint) {
  // We cannot handle session request if we are outbound
  if (IsOutbound()) {
    return;
  }
  LOG(debug) << "SSUSession: SessionRequest received";
  auto packet = static_cast<SSUSessionRequestPacket*>(pkt);
  SetRemoteEndpoint(sender_endpoint);
  if (!m_DHKeysPair)
    m_DHKeysPair = transports.GetNextDHKeysPair();
  if (!CreateAESandMACKey(packet->GetDhX())) {
    LOG(error)
      << "SSUSession:" << GetFormattedSessionInfo()
      << "invalid DH-X, not sending SessionCreated";
    return;
  }
  SendSessionCreated(packet->GetDhX());
}

void SSUSession::SendSessionRequest() {
  LOG(debug)
    << "SSUSession:" << GetFormattedSessionInfo() << "sending SessionRequest";
  auto intro_key = GetIntroKey();
  if (!intro_key) {
    LOG(error)
      << "SSUSession:" << GetFormattedSessionInfo()
      << __func__ << ": SSU is not supported";
    return;
  }
  SSUSessionRequestPacket packet;
  packet.SetHeader(std::make_unique<SSUHeader>(SSUPayloadType::SessionRequest));
  std::array<std::uint8_t, SSUSize::IV> iv;
  kovri::core::RandBytes(iv.data(), iv.size());
  packet.GetHeader()->SetIV(iv.data());
  packet.SetDhX(m_DHKeysPair->public_key.data());
  // Fill extended options
  std::array<std::uint8_t, 2> extended_data {{ 0x00, 0x00 }};
  if (context.GetState() == RouterState::OK) {  // we don't need relays
    packet.GetHeader()->SetExtendedOptions(true);
    packet.GetHeader()->SetExtendedOptionsData(extended_data.data(), 2);
  }
  auto const remote_ip(core::AddressToByteVector(GetRemoteEndpoint().address()));
  // TODO(unassigned): remove const_cast, see bytestream TODO
  packet.SetIPAddress(const_cast<std::uint8_t*>(remote_ip.data()), remote_ip.size());
  const std::size_t packet_size = SSUPacketBuilder::GetPaddedSize(packet.GetSize());
  const std::size_t buffer_size = packet_size + SSUSize::BufferMargin;
  // Buffer has SSUSize::BufferMargin extra bytes for computing the HMAC
  auto buffer = std::make_unique<std::uint8_t[]>(buffer_size);
  WriteAndEncrypt(&packet, buffer.get(), buffer_size, intro_key, intro_key);
  m_Server.Send(buffer.get(), packet_size, GetRemoteEndpoint());
}

/**
 *
 * Payload type 1: SessionCreated
 *
 */

void SSUSession::ProcessSessionCreated(
    SSUPacket* pkt) {
  if (!m_RemoteRouter || !m_DHKeysPair) {
    LOG(warning)
      << "SSUSession:" << GetFormattedSessionInfo()
      << "unsolicited SessionCreated message";
    return;
  }
  LOG(debug)
    << "SSUSession:" << GetFormattedSessionInfo()
    << "SessionCreated received";
  m_Timer.cancel();  // connect timer
  auto packet = static_cast<SSUSessionCreatedPacket*>(pkt);
  // x, y, our IP, our port, remote IP, remote port, relay tag, signed on time
  SignedData s;
  if (!CreateAESandMACKey(packet->GetDhY())) {
    LOG(error)
      << "SSUSession:" << GetFormattedSessionInfo()
      << "invalid DH-Y, not sending SessionConfirmed";
    return;
  }
  s.Insert(m_DHKeysPair->public_key.data(), 256);  // x
  s.Insert(packet->GetDhY(), 256);  // y
  boost::asio::ip::address our_IP;
  if (packet->GetIPAddressSize() == 4) {  // v4
    boost::asio::ip::address_v4::bytes_type bytes;
    memcpy(bytes.data(), packet->GetIPAddress(), 4);
    our_IP = boost::asio::ip::address_v4(bytes);
  } else {  // v6
    boost::asio::ip::address_v6::bytes_type bytes;
    memcpy(bytes.data(), packet->GetIPAddress(), 16);
    our_IP = boost::asio::ip::address_v6(bytes);
  }
  s.Insert(packet->GetIPAddress(), packet->GetIPAddressSize());  // our IP
  s.Insert<std::uint16_t>(boost::endian::native_to_big(packet->GetPort()));  // our port
  LOG(debug)
    << "SSUSession:" << GetFormattedSessionInfo()
    << __func__ << ": our external address is "
    << our_IP.to_string() << ":" << packet->GetPort();
  context.UpdateAddress(our_IP);
  if (GetRemoteEndpoint().address().is_v4()) {
    // remote IP v4
    s.Insert(GetRemoteEndpoint().address().to_v4().to_bytes().data(), 4);
  } else {
    // remote IP v6
    s.Insert(GetRemoteEndpoint().address().to_v6().to_bytes().data(), 16);
  }
  s.Insert<std::uint16_t>(boost::endian::native_to_big(GetRemoteEndpoint().port()));  // remote port
  m_RelayTag = packet->GetRelayTag();
  s.Insert<std::uint32_t>(boost::endian::native_to_big(m_RelayTag));  // relay tag
  s.Insert<std::uint32_t>(boost::endian::native_to_big(packet->GetSignedOnTime()));  // signed on time
  // decrypt signature
  auto signature_len = m_RemoteIdentity.GetSignatureLen();
  auto padding_size = signature_len & 0x0F;  // %16
  if (padding_size > 0)
    signature_len += (16 - padding_size);
  // TODO(anonimal): this try block should be larger or handled entirely by caller
  try {
    m_SessionKeyDecryption.SetIV(packet->GetHeader()->GetIV());
    m_SessionKeyDecryption.Decrypt(
        packet->GetSignature(),
        signature_len,
        packet->GetSignature());
  } catch (...) {
    m_Exception.Dispatch(__func__);
    // TODO(anonimal): review if we need to safely break control, ensure exception handling by callers
    throw;
  }
  // verify
  if (s.Verify(m_RemoteIdentity, packet->GetSignature())) {
    SendSessionConfirmed(
        packet->GetDhY(),
        packet->GetIPAddress(),
        packet->GetIPAddressSize(),
        packet->GetPort());
  } else {  // invalid signature
    LOG(error)
      << "SSUSession:" << GetFormattedSessionInfo()
      << "SessionCreated signature verification failed";
    // Reset the session key, Java routers might resent the message if it
    //  failed the first time
    m_IsSessionKey = false;
  }
}

void SSUSession::SendSessionCreated(
    const std::uint8_t* x) {
  auto intro_key = GetIntroKey();
  auto address = IsV6() ?
    context.GetRouterInfo().GetSSUAddress(true) :
    context.GetRouterInfo().GetSSUAddress();  // v4 only
  if (!intro_key || !address) {
    LOG(error)
      << "SSUSession:" << GetFormattedSessionInfo()
      << "SendSessionCreated(): SSU is not supported";
    return;
  }
  SSUSessionCreatedPacket packet;
  packet.SetHeader(std::make_unique<SSUHeader>(SSUPayloadType::SessionCreated));
  std::array<std::uint8_t, SSUSize::IV> iv;
  kovri::core::RandBytes(iv.data(), iv.size());
  packet.GetHeader()->SetIV(iv.data());
  packet.SetDhY(m_DHKeysPair->public_key.data());
  packet.SetPort(GetRemoteEndpoint().port());
  // signature
  // x,y, remote IP, remote port, our IP, our port, relay tag, signed on time
  SignedData s;
  s.Insert(x, 256);  // x
  s.Insert(packet.GetDhY(), 256);  // y
  auto const remote_ip(core::AddressToByteVector(GetRemoteEndpoint().address()));
  // TODO(unassigned): remove const_cast, see bytestream TODO
  packet.SetIPAddress(const_cast<std::uint8_t*>(remote_ip.data()), remote_ip.size());
  s.Insert(remote_ip.data(), remote_ip.size());  // remote ip
  s.Insert<std::uint16_t>(boost::endian::native_to_big(packet.GetPort()));  // remote port
  if (address->host.is_v4())
    s.Insert(address->host.to_v4().to_bytes().data(), 4);  // our IP V4
  else
    s.Insert(address->host.to_v6().to_bytes().data(), 16);  // our IP V6
  s.Insert<std::uint16_t> (boost::endian::native_to_big(address->port));  // our port

  std::uint32_t relay_tag = 0;
  if (context.GetRouterInfo().HasCap(RouterInfo::Cap::SSUIntroducer)) {
    relay_tag = kovri::core::Rand<std::uint32_t>();
    if (!relay_tag)
      relay_tag = 1;
    m_Server.AddRelay(relay_tag, GetRemoteEndpoint());
  }
  packet.SetRelayTag(relay_tag);
  packet.SetSignedOnTime(kovri::core::GetSecondsSinceEpoch());
  s.Insert<std::uint32_t>(boost::endian::native_to_big(relay_tag));
  s.Insert<std::uint32_t>(boost::endian::native_to_big(packet.GetSignedOnTime()));
  // store for session confirmation
  m_SessionConfirmData = std::make_unique<SignedData>(s);

  // Set signature size to compute the required padding size 
  std::size_t signature_size = context.GetIdentity().GetSignatureLen();
  packet.SetSignature(nullptr, signature_size);
  const std::size_t sig_padding = SSUPacketBuilder::GetPaddingSize(
      packet.GetSize());
  // Set signature with correct size and fill the padding
  auto signature_buf = std::make_unique<std::uint8_t[]>(
      signature_size + sig_padding);
  s.Sign(context.GetPrivateKeys(), signature_buf.get());
  kovri::core::RandBytes(signature_buf.get() + signature_size, sig_padding);
  packet.SetSignature(signature_buf.get(), signature_size + sig_padding);

  // Encrypt signature and padding with newly created session key
  // TODO(anonimal): this try block should be larger or handled entirely by caller
  try {
    m_SessionKeyEncryption.SetIV(packet.GetHeader()->GetIV());
    m_SessionKeyEncryption.Encrypt(
        packet.GetSignature(),
        packet.GetSignatureSize(),
        packet.GetSignature());
  } catch (...) {
    m_Exception.Dispatch(__func__);
    // TODO(anonimal): review if we need to safely break control, ensure exception handling by callers
    throw;
  }
  const std::size_t packet_size = SSUPacketBuilder::GetPaddedSize(packet.GetSize());
  const std::size_t buffer_size = packet_size + SSUSize::BufferMargin;
  // TODO(EinMByte): Deal with large messages in a better way
  if (packet_size <= SSUSize::MTUv4) {
    auto buffer = std::make_unique<std::uint8_t[]>(buffer_size);
    WriteAndEncrypt(&packet, buffer.get(), buffer_size, intro_key, intro_key);
    Send(buffer.get(), packet_size);
  }
}

/**
 *
 * Payload type 2: SessionConfirmed
 *
 */

void SSUSession::ProcessSessionConfirmed(SSUPacket* pkt) {
  if (m_SessionConfirmData == nullptr) {
    // No session confirm data
    LOG(error)
      << "SSUSession:" << GetFormattedSessionInfo() << "unsolicited SessionConfirmed";
    return;
  }
  LOG(debug)
    << "SSUSession:" << GetFormattedSessionInfo() << "SessionConfirmed received";
  auto packet = static_cast<SSUSessionConfirmedPacket*>(pkt);
  m_RemoteIdentity = packet->GetRemoteRouterIdentity();
  m_Data.UpdatePacketSize(m_RemoteIdentity.GetIdentHash());
  // signature time : replace with last value
  m_SessionConfirmData->Insert<std::uint32_t>(
      std::ios_base::end, -4, boost::endian::native_to_big(packet->GetSignedOnTime()));
  if (m_SessionConfirmData->Verify(m_RemoteIdentity, packet->GetSignature())) {
    // verified
    Established();
    return;
  }
  // bad state or verification failed
  LOG(error)
    << "SSUSession:" << GetFormattedSessionInfo() << "SessionConfirmed Failed";
}

void SSUSession::SendSessionConfirmed(
    const std::uint8_t* y,
    const std::uint8_t* our_address,
    std::size_t our_address_len,
    std::uint16_t our_port) {
  SSUSessionConfirmedPacket packet;
  packet.SetHeader(std::make_unique<SSUHeader>(SSUPayloadType::SessionConfirmed));
  std::array<std::uint8_t, SSUSize::IV> iv;
  kovri::core::RandBytes(iv.data(), iv.size());
  packet.GetHeader()->SetIV(iv.data());
  packet.SetRemoteRouterIdentity(context.GetIdentity());
  packet.SetSignedOnTime(kovri::core::GetSecondsSinceEpoch());
  auto signature_buf = std::make_unique<std::uint8_t[]>(
      context.GetIdentity().GetSignatureLen());
  // signature
  // x,y, our IP, our port, remote IP, remote port,
  // relay_tag, our signed on time
  SignedData s;
  s.Insert(m_DHKeysPair->public_key.data(), 256);  // x
  s.Insert(y, 256);  // y
  s.Insert(our_address, our_address_len);
  s.Insert<std::uint16_t>(boost::endian::native_to_big(our_port));
  auto const address = GetRemoteEndpoint().address();
  if (address.is_v4())  // remote IP V4
    s.Insert(address.to_v4().to_bytes().data(), 4);
  else  // remote IP V6
    s.Insert(address.to_v6().to_bytes().data(), 16);
  s.Insert<std::uint16_t>(boost::endian::native_to_big(GetRemoteEndpoint().port()));  // remote port
  s.Insert(boost::endian::native_to_big(m_RelayTag));
  s.Insert(boost::endian::native_to_big(packet.GetSignedOnTime()));
  s.Sign(context.GetPrivateKeys(), signature_buf.get());
  packet.SetSignature(signature_buf.get());
  const std::size_t packet_size = SSUPacketBuilder::GetPaddedSize(packet.GetSize());
  const std::size_t buffer_size = packet_size + SSUSize::BufferMargin;
  auto buffer = std::make_unique<std::uint8_t[]>(buffer_size);
  WriteAndEncrypt(&packet, buffer.get(), buffer_size, m_SessionKey, m_MACKey);
  Send(buffer.get(), packet_size);
}

/**
 *
 * Payload type 3: RelayRequest
 *
 */

void SSUSession::ProcessRelayRequest(
    SSUPacket* pkt,
    const boost::asio::ip::udp::endpoint& from) {
  auto packet = static_cast<SSURelayRequestPacket*>(pkt);
  auto session = m_Server.FindRelaySession(packet->GetRelayTag());
  if (!session)
    return;
  SendRelayResponse(
      packet->GetNonce(),
      from,
      packet->GetIntroKey(),
      session->GetRemoteEndpoint());
  SendRelayIntro(session.get(), from);
}

void SSUSession::SendRelayRequest(
    const std::uint32_t introducer_tag,
    const std::uint8_t* introducer_key)
{
  auto* const address = context.GetRouterInfo().GetSSUAddress();
  if (!address)
    {
      LOG(error) << "SSUSession:" << GetFormattedSessionInfo() << __func__
                 << ": SSU is not supported";
      return;
    }

  // Create message  // TODO(anonimal): move to packet writer
  // TODO(unassigned): size if we include Alice's IP (see SSU spec, unimplemented)
  core::OutputByteStream message(
      SSUSize::RelayRequestBuffer + SSUSize::BufferMargin);  // TODO(anonimal): review buffer margin

  // TODO(unassigned): Endianness is not spec-defined, assuming BE

  // Skip header (written later)
  message.SkipBytes(SSUSize::HeaderMin);

  // Intro tag
  message.Write<std::uint32_t>(introducer_tag);

  // Address, port, and challenge (see SSU spec)
  message.SkipBytes(4);

  // Key
  message.Write<std::uint32_t>(
      core::InputByteStream::Read<std::uint32_t>(address->key));

  // Nonce
  message.Write<std::uint32_t>(core::Rand<std::uint32_t>());

  // Create header IV
  // TODO(anonimal): should be done internally during header creation
  std::array<std::uint8_t, SSUSize::IV> IV;
  kovri::core::RandBytes(IV.data(), IV.size());

  // Write header and send
  if (m_State == SessionState::Established)
    {
      // Use Alice/Bob session key if session is established
      FillHeaderAndEncrypt(
          SSUPayloadType::RelayRequest,
          message.Data(),
          SSUSize::RelayRequestBuffer,
          m_SessionKey,
          IV.data(),
          m_MACKey);
    }
  else
    {
      FillHeaderAndEncrypt(
          SSUPayloadType::RelayRequest,
          message.Data(),
          SSUSize::RelayRequestBuffer,
          introducer_key,
          IV.data(),
          introducer_key);
    }

  m_Server.Send(
      message.Data(), SSUSize::RelayRequestBuffer, GetRemoteEndpoint());
}

/**
 *
 * Payload type 4: RelayResponse
 *
 */

void SSUSession::ProcessRelayResponse(SSUPacket* pkt) {
  LOG(debug)
    << "SSUSession:" << GetFormattedSessionInfo() << "RelayResponse received";
  auto packet = static_cast<SSURelayResponsePacket*>(pkt);
  // TODO(EinMByte): Check remote (charlie) address
  boost::asio::ip::address our_IP;
  if (packet->GetIPAddressAliceSize() == 4) {
    boost::asio::ip::address_v4::bytes_type bytes;
    memcpy(bytes.data(), packet->GetIPAddressAlice(), 4);
    our_IP = boost::asio::ip::address_v4(bytes);
  } else {
    boost::asio::ip::address_v6::bytes_type bytes;
    memcpy(bytes.data(), packet->GetIPAddressAlice(), 16);
    our_IP = boost::asio::ip::address_v6(bytes);
  }
  LOG(debug)
    << "SSUSession:" << GetFormattedSessionInfo()
    << __func__ << ": our external address is "
    << our_IP.to_string() << ":" << packet->GetPortAlice();
  context.UpdateAddress(our_IP);
}

void SSUSession::SendRelayResponse(
    const std::uint32_t nonce,
    const boost::asio::ip::udp::endpoint& from,
    const std::uint8_t* intro_key,
    const boost::asio::ip::udp::endpoint& to)
{
  // Charlie's address must always be IPv4
  if (!to.address().is_v4())
    {
      LOG(error) << "SSUSession:" << GetFormattedSessionInfo() << __func__
                 << ": Charlie's must use IPv4";
      // TODO(anonimal): don't throw?...
      return;
    }

  // Create message  // TODO(anonimal): move to packet writer
  core::OutputByteStream message(
      SSUSize::RelayResponseBuffer + SSUSize::BufferMargin);  // TODO(anonimal): review buffer margin

  // Skip header (written later)
  message.SkipBytes(SSUSize::HeaderMin);

  // Charlie's IPv4 size
  message.Write<std::uint8_t>(4);

  // Charlie's address
  message.Write<std::uint32_t>(to.address().to_v4().to_ulong());

  // Charlie's port
  message.Write<std::uint16_t>(to.port());

  // Alice's IP address
  bool const is_IPv4 = from.address().is_v4();
  if (is_IPv4)
    {
      message.Write<std::uint8_t>(4);
      message.WriteData(from.address().to_v4().to_bytes().data(), 4);
    }
  else  // TODO(anonimal): *assumes* IPv6?
    {
      message.Write<std::uint8_t>(16);
      message.WriteData(from.address().to_v6().to_bytes().data(), 16);
    }

  // Alice's port
  message.Write<std::uint16_t>(from.port());

  // Nonce
  message.Write<std::uint32_t>(nonce);

  // Write header and send
  std::uint8_t const message_size = is_IPv4 ? SSUSize::RelayResponseBuffer - 16
                                             : SSUSize::RelayResponseBuffer;
  if (m_State == SessionState::Established)
    {
      // Uses session key if established
      FillHeaderAndEncrypt(
          SSUPayloadType::RelayResponse, message.Data(), message_size);

      Send(message.Data(), message_size);
    }
  else
    {
      // Encrypt with Alice's intro key
      // TODO(anonimal): should be done internally during header creation
      std::array<std::uint8_t, SSUSize::IV> IV;
      kovri::core::RandBytes(IV.data(), IV.size());

      FillHeaderAndEncrypt(
          SSUPayloadType::RelayResponse,
          message.Data(),
          message_size,
          intro_key,
          IV.data(),
          intro_key);

      m_Server.Send(message.Data(), message_size, from);
    }

  LOG(debug) << "SSUSession: RelayResponse sent";
}

/**
 *
 * Payload type 5: RelayIntro
 *
 */

void SSUSession::ProcessRelayIntro(SSUPacket* pkt) {
  auto packet = static_cast<SSURelayIntroPacket*>(pkt);
  std::uint32_t const size = packet->GetIPAddressSize();
  if (size == 4) {
    std::uint32_t const addr =
        core::InputByteStream::Read<std::uint32_t>(packet->GetIPAddress());
    boost::asio::ip::address_v4 address(addr);
    // send hole punch of 1 byte
    m_Server.Send(
        {},
        0,
        boost::asio::ip::udp::endpoint(
            address,
            packet->GetPort()));
  } else {
    LOG(warning)
      << "SSUSession:" << GetFormattedSessionInfo()
      << __func__ << ": address size " << size << " is not supported";
  }
}

// TODO(anonimal): bytestream refactor
void SSUSession::SendRelayIntro(
    SSUSession* session,
    const boost::asio::ip::udp::endpoint& from) {
  if (!session)
    return;
  // Alice's address always v4
  if (!from.address().is_v4()) {
    LOG(error)
      << "SSUSession:" << GetFormattedSessionInfo()
      << __func__ << ": Alice's IP must be V4";
    return;
  }
  std::array<std::uint8_t, 48 + SSUSize::BufferMargin> buf{{}};
  auto payload = buf.data() + SSUSize::HeaderMin;
  *payload = 4;
  payload++;  // size
  core::OutputByteStream::Write<std::uint32_t>(
      payload, from.address().to_v4().to_ulong());  // Alice's IP
  payload += 4;  // address
  core::OutputByteStream::Write<std::uint16_t>(payload, from.port());  // Alice's port
  payload += 2;  // port
  *payload = 0;  // challenge size
  std::array<std::uint8_t, SSUSize::IV> iv;
  kovri::core::RandBytes(iv.data(), iv.size());  // random iv
  FillHeaderAndEncrypt(
      SSUPayloadType::RelayIntro,
      buf.data(),
      48,
      session->m_SessionKey,
      iv.data(),
      session->m_MACKey);
  m_Server.Send(
      buf.data(),
      48,
      session->GetRemoteEndpoint());
  LOG(debug) << "SSUSession: " << GetFormattedSessionInfo() << "RelayIntro sent";
}

/**
 *
 * Payload type 6: Data
 *
 */

void SSUSession::ProcessData(SSUPacket* pkt) {
  auto packet = static_cast<SSUDataPacket*>(pkt);
  // TODO(EinMByte): Don't use raw data
  m_Data.ProcessMessage(packet->m_RawData, packet->m_RawDataLength);
  m_IsDataReceived = true;
}

void SSUSession::FlushData() {
  if (m_IsDataReceived) {
    m_Data.FlushReceivedMessage();
    m_IsDataReceived = false;
  }
}

/**
 *
 * Payload type 7: PeerTest
 *
 */

void SSUSession::ProcessPeerTest(
    SSUPacket* pkt,
    const boost::asio::ip::udp::endpoint& sender_endpoint) {
  auto packet = static_cast<SSUPeerTestPacket*>(pkt);
  if (packet->GetPort() && !packet->GetIPAddress()) {
    LOG(warning)
      << "SSUSession:" << GetFormattedSessionInfo() << "address size "
      << " bytes not supported";
    return;
  }
  auto peer_test = SSUPayloadType::PeerTest;
  switch (m_Server.GetPeerTestParticipant(packet->GetNonce())) {
    // existing test
    case PeerTestParticipant::Alice1: {
      if (m_Server.GetPeerTestSession(packet->GetNonce()) == shared_from_this()) {
        LOG(debug)
          << "SSUSession:" << GetFormattedSessionInfo()
          << "PeerTest from Bob. We are Alice";
        if (context.GetState() == RouterState::Testing)  // still not OK
          context.SetState(RouterState::Firewalled);
      } else {
        LOG(debug)
          << "SSUSession:" << GetFormattedSessionInfo()
          << "first PeerTest from Charlie. We are Alice";
        context.SetState(RouterState::OK);
        m_Server.UpdatePeerTest(
            packet->GetNonce(),
            PeerTestParticipant::Alice2);
        SendPeerTest(
            packet->GetNonce(),
            sender_endpoint.address().to_v4().to_ulong(),
            sender_endpoint.port(),
            packet->GetIntroKey(),
            true,
            false);  // to Charlie
      }
      break;
    }
    case PeerTestParticipant::Alice2: {
      if (m_Server.GetPeerTestSession(packet->GetNonce()) == shared_from_this()) {
        LOG(debug)
          << "SSUSession:" << GetFormattedSessionInfo()
          << "PeerTest from Bob. We are Alice";
      } else {
        // PeerTest successive
        LOG(debug)
          << "SSUSession:" << GetFormattedSessionInfo()
          << "second PeerTest from Charlie. We are Alice";
        context.SetState(RouterState::OK);
      }
      break;
    }
    case PeerTestParticipant::Bob: {
      LOG(debug)
        << "SSUSession:" << GetFormattedSessionInfo()
        << "PeerTest from Charlie. We are Bob";
      // session with Alice from PeerTest
      auto session = m_Server.GetPeerTestSession(packet->GetNonce());
      if (session && session->m_State == SessionState::Established)
        session->Send(  // back to Alice
            peer_test,
            packet->m_RawData,
            packet->m_RawDataLength);
      m_Server.RemovePeerTest(packet->GetNonce());  // nonce has been used
      break;
    }
    case PeerTestParticipant::Charlie: {
      LOG(debug)
        << "SSUSession:" << GetFormattedSessionInfo()
        << "PeerTest from Alice. We are Charlie";
      SendPeerTest(
          packet->GetNonce(),
          sender_endpoint.address().to_v4().to_ulong(),
          sender_endpoint.port(),
          packet->GetIntroKey());  // to Alice with her actual address
      m_Server.RemovePeerTest(packet->GetNonce());  // nonce has been used
      break;
    }
    // test not found
    case PeerTestParticipant::Unknown: {
      if (m_State == SessionState::Established) {
        // new test
        if (packet->GetPort()) {
          LOG(debug)
            << "SSUSession:" << GetFormattedSessionInfo()
            << "PeerTest from Bob. We are Charlie";
          m_Server.NewPeerTest(packet->GetNonce(), PeerTestParticipant::Charlie);
          Send(  // back to Bob
              peer_test,
              packet->m_RawData,
              packet->m_RawDataLength);
          SendPeerTest(  // to Alice with her address received from Bob
              packet->GetNonce(),
              boost::endian::big_to_native(packet->GetIPAddress()),  // TODO(anonimal): native / big endian?
              packet->GetPort(),  // TODO(anonimal): native / big endian?
              packet->GetIntroKey());
        } else {
          LOG(debug)
            << "SSUSession:" << GetFormattedSessionInfo()
            << "PeerTest from Alice. We are Bob";
          // Charlie
          auto session = m_Server.GetRandomEstablishedSession(shared_from_this());
          if (session) {
            m_Server.NewPeerTest(
                packet->GetNonce(),
                PeerTestParticipant::Bob,
                shared_from_this());
            session->SendPeerTest(
                packet->GetNonce(),
                sender_endpoint.address().to_v4().to_ulong(),
                sender_endpoint.port(),
                packet->GetIntroKey(),
                false);  // to Charlie with Alice's actual address
          }
        }
      } else {
        LOG(error)
          << "SSUSession:" << GetFormattedSessionInfo() << "unexpected PeerTest";
      }
    }
  }
}

// TODO(anonimal): bytestream refactor
// TODO(anonimal): pass reference to structure, not SIX arguments!
void SSUSession::SendPeerTest(
    std::uint32_t nonce,
    std::uint32_t address,
    std::uint16_t port,
    const std::uint8_t* intro_key,
    bool to_address,  // is true for Alice<->Charlie communications only
    bool send_address) {  // is false if message comes from Alice
  std::array<std::uint8_t, 80 + SSUSize::BufferMargin> buf{{}};
  auto payload = buf.data() + SSUSize::HeaderMin;
  core::OutputByteStream::Write<std::uint32_t>(payload, nonce);
  payload += 4;  // nonce
  // address and port
  if (send_address && address) {
    *payload = 4;
    payload++;  // size
    core::OutputByteStream::Write<std::uint32_t>(payload, address);
    payload += 4;  // address
  } else {
    *payload = 0;
    payload++;  // size
  }
  core::OutputByteStream::Write<std::uint16_t>(payload, port);
  payload += 2;  // port
  // intro key
  if (to_address) {
    // send our intro key to address instead it's own
    auto addr = context.GetRouterInfo().GetSSUAddress();
    if (addr)
      memcpy(payload, addr->key, 32);  // intro key
    else
      LOG(error)
        << "SSUSession:" << GetFormattedSessionInfo()
        << "SSU is not supported, can't send PeerTest";
  } else {
    memcpy(payload, intro_key, 32);  // intro key
  }
  // send
  std::array<std::uint8_t, SSUSize::IV> iv;
  kovri::core::RandBytes(iv.data(), iv.size());
  auto peer_test = SSUPayloadType::PeerTest;
  if (to_address) {
    // encrypt message with specified intro key
    FillHeaderAndEncrypt(
        peer_test,
        buf.data(),
        80,
        intro_key,
        iv.data(),
        intro_key);
    boost::asio::ip::udp::endpoint ep(
        boost::asio::ip::address_v4(address),
        port);
    m_Server.Send(buf.data(), 80, ep);
  } else {
    // encrypt message with session key
    FillHeaderAndEncrypt(
        peer_test,
        buf.data(),
        80);
    Send(buf.data(), 80);
  }
}

void SSUSession::SendPeerTest() {
  // we are Alice
  LOG(debug) << "SSUSession: <--" << GetFormattedSessionInfo() << "sending PeerTest";
  auto address = context.GetRouterInfo().GetSSUAddress();
  if (!address) {
    LOG(error)
      << "SSUSession:" << GetFormattedSessionInfo()
      << "SSU is not supported, can't send PeerTest";
    return;
  }
  auto nonce = kovri::core::Rand<std::uint32_t>();
  if (!nonce)
    nonce = 1;
  m_PeerTest = false;
  m_Server.NewPeerTest(nonce, PeerTestParticipant::Alice1, shared_from_this());
  SendPeerTest(
      nonce,
      0,  // address and port always zero for Alice
      0,  // ^
      address->key,
      false,
      false);
}

/**
 *
 * Payload type 8: SessionDestroyed
 *
 */

void SSUSession::SendSesionDestroyed() {
  if (m_IsSessionKey) {
    std::array<std::uint8_t, 48 + SSUSize::BufferMargin> buf {{}};
    // encrypt message with session key
    FillHeaderAndEncrypt(
        SSUPayloadType::SessionDestroyed,
        buf.data(),
        48);
    try {
      Send(buf.data(), 48);
    } catch(const std::exception& ex) {
      LOG(error)
        << "SSUSession:" << GetFormattedSessionInfo()
        << __func__ << ": '" << ex.what() << "'";
    }
    LOG(debug)
      << "SSUSession:" << GetFormattedSessionInfo() << "SessionDestroyed sent";
  }
}

void SSUSession::SendKeepAlive() {
  if (m_State == SessionState::Established) {
    std::array<std::uint8_t, 48 + SSUSize::BufferMargin> buf {{}};  // TODO(unassigned): document values
    auto payload = buf.data() + SSUSize::HeaderMin;
    *payload = 0;  // flags
    payload++;
    *payload = 0;  // num fragments
    // encrypt message with session key
    FillHeaderAndEncrypt(
        SSUPayloadType::Data,
        buf.data(),
        48);
    Send(buf.data(), 48);
    LOG(debug)
      << "SSUSession:" << GetFormattedSessionInfo() << "keep-alive sent";
    ScheduleTermination();
  }
}

// TODO(anonimal): pass reference to structure, not SEVEN arguments!
void SSUSession::FillHeaderAndEncrypt(
    std::uint8_t payload_type,
    std::uint8_t* buf,
    std::size_t len,
    const std::uint8_t* aes_key,
    const std::uint8_t* iv,
    const std::uint8_t* mac_key,
    std::uint8_t flag) {
  // TODO(anonimal): this try block should be handled entirely by caller
  try {
    if (len < SSUSize::HeaderMin) {
      LOG(error)
        << "SSUSession:" << GetFormattedSessionInfo()
        << "unexpected SSU packet length " << len;
      return;
    }
    SSUSessionPacket pkt(buf, len);
    memcpy(pkt.IV(), iv, SSUSize::IV);
    pkt.PutFlag(flag | (payload_type << 4));  // MSB is 0
    pkt.PutTime(kovri::core::GetSecondsSinceEpoch());
    auto encrypted = pkt.Encrypted();
    auto encrypted_len = len - (encrypted - buf);
    kovri::core::CBCEncryption encryption(aes_key, iv);
    encryption.Encrypt(
        encrypted,
        encrypted_len,
        encrypted);
    // assume actual buffer size is 18 (16 + 2) bytes more
    // TODO(unassigned): ^ this is stupid and dangerous to assume that caller is responsible
    memcpy(buf + len, iv, SSUSize::IV);
    core::OutputByteStream::Write<std::uint16_t>(
        buf + len + SSUSize::IV, encrypted_len);
    kovri::core::HMACMD5Digest(
        encrypted,
        encrypted_len + SSUSize::BufferMargin,
        mac_key,
        pkt.MAC());
  } catch (...) {
    m_Exception.Dispatch(__func__);
    // TODO(anonimal): review if we need to safely break control, ensure exception handling by callers
    throw;
  }
}

void SSUSession::WriteAndEncrypt(
    SSUPacket* packet,
    std::uint8_t* buffer,
    std::size_t buffer_size,
    const std::uint8_t* aes_key,
    const std::uint8_t* mac_key) {
  // TODO(anonimal): this try block should be handled entirely by caller
  try {
    packet->GetHeader()->SetTime(kovri::core::GetSecondsSinceEpoch());
    SSUPacketBuilder builder(buffer, buffer_size);
    // Write header (excluding MAC)
    builder.WriteHeader(packet->GetHeader());
    // Write packet body
    builder.WritePacket(packet);
    // Encrypt everything after the MAC and IV
    std::uint8_t* encrypted =
      buffer
      + SSUSize::IV
      + SSUSize::MAC;
    auto encrypted_len = builder.Tellp() - encrypted;
    // Add padding
    std::size_t const padding_size =
        SSUPacketBuilder::GetPaddingSize(encrypted_len);
    if (padding_size)
      {
        std::vector<std::uint8_t> padding(padding_size);
        core::RandBytes(padding.data(), padding.size());
        builder.WriteData(padding.data(), padding.size());
        encrypted_len += padding.size();
      }
    kovri::core::CBCEncryption encryption(aes_key, packet->GetHeader()->GetIV());
    encryption.Encrypt(encrypted, encrypted_len, encrypted);
    // Compute HMAC of encryptedPayload + IV + (payloadLength ^ protocolVersion)
    // Currently, protocolVersion == 0
    kovri::core::OutputByteStream stream(
        encrypted + encrypted_len, buffer_size - (encrypted - buffer));
    stream.WriteData(
        packet->GetHeader()->GetIV(),
        SSUSize::IV);
    stream.Write<std::uint16_t>(encrypted_len);
    kovri::core::HMACMD5Digest(
        encrypted,
        encrypted_len + SSUSize::BufferMargin,
        mac_key,
        buffer);
  } catch (...) {
    m_Exception.Dispatch(__func__);
    // TODO(anonimal): review if we need to safely break control, ensure exception handling by callers
    throw;
  }
}

void SSUSession::FillHeaderAndEncrypt(
    std::uint8_t payload_type,
    std::uint8_t* buf,
    std::size_t len) {
  // TODO(anonimal): this try block should be handled entirely by caller
  try {
    if (len < SSUSize::HeaderMin) {
      LOG(error)
        << "SSUSession:" << GetFormattedSessionInfo()
        << "unexpected SSU packet length " << len;
      return;
    }
    SSUSessionPacket pkt(buf, len);
    kovri::core::RandBytes(pkt.IV(), SSUSize::IV);  // random iv
    m_SessionKeyEncryption.SetIV(pkt.IV());
    pkt.PutFlag(payload_type << 4);  // MSB is 0
    pkt.PutTime(kovri::core::GetSecondsSinceEpoch());
    auto encrypted = pkt.Encrypted();
    auto encrypted_len = len - (encrypted - buf);
    m_SessionKeyEncryption.Encrypt(
        encrypted,
        encrypted_len,
        encrypted);
    // assume actual buffer size is 18 (16 + 2) bytes more
    // TODO(unassigned): ^ this is stupid and dangerous to assume that caller is responsible
    memcpy(buf + len, pkt.IV(), SSUSize::IV);
    core::OutputByteStream::Write<std::uint16_t>(
        buf + len + SSUSize::IV, encrypted_len);
    kovri::core::HMACMD5Digest(
        encrypted,
        encrypted_len + SSUSize::BufferMargin,
        m_MACKey,
        pkt.MAC());
  } catch (...) {
    m_Exception.Dispatch(__func__);
    // TODO(anonimal): review if we need to safely break control, ensure exception handling by callers
    throw;
  }
}

void SSUSession::Decrypt(
    std::uint8_t* buf,
    std::size_t len,
    const std::uint8_t* aes_key) {
  // TODO(anonimal): this try block should be handled entirely by caller
  try {
    if (len < SSUSize::HeaderMin) {
      LOG(error)
        << "SSUSession:" << GetFormattedSessionInfo()
        << __func__ << ": unexpected SSU packet length " << len;
      return;
    }
    SSUSessionPacket pkt(buf, len);
    auto encrypted = pkt.Encrypted();
    auto encrypted_len = len - (encrypted - buf);
    kovri::core::CBCDecryption decryption;
    decryption.SetKey(aes_key);
    decryption.SetIV(pkt.IV());
    decryption.Decrypt(
        encrypted,
        encrypted_len,
        encrypted);
  } catch (...) {
    m_Exception.Dispatch(__func__);
    // TODO(anonimal): review if we need to safely break control, ensure exception handling by callers
    throw;
  }
}

void SSUSession::DecryptSessionKey(
    std::uint8_t* buf,
    std::size_t len) {
  if (len < SSUSize::HeaderMin) {
    LOG(error)
      << "SSUSession:" << GetFormattedSessionInfo()
      << __func__ << ": unexpected SSU packet length " << len;
    return;
  }
  SSUSessionPacket pkt(buf, len);
  auto encrypted = pkt.Encrypted();
  auto encrypted_len = len - (encrypted - buf);
  if (encrypted_len > 0) {
    // TODO(anonimal): this try block should be larger or handled entirely by caller
    try {
      m_SessionKeyDecryption.SetIV(pkt.IV());
      m_SessionKeyDecryption.Decrypt(encrypted, encrypted_len, encrypted);
    } catch (...) {
      m_Exception.Dispatch(__func__);
      // TODO(anonimal): review if we need to safely break control, ensure exception handling by callers
      throw;
    }
  }
}

bool SSUSession::Validate(
    std::uint8_t* buf,
    std::size_t len,
    const std::uint8_t* mac_key) {
  if (len < SSUSize::HeaderMin) {
    LOG(error)
      << "SSUSession:" << GetFormattedSessionInfo()
      << __func__ << ": unexpected SSU packet length " << len;
    return false;
  }
  SSUSessionPacket pkt(buf, len);
  auto encrypted = pkt.Encrypted();
  auto encrypted_len = len - (encrypted - buf);
  // assume actual buffer size is 18 (16 + 2) bytes more (SSUSize::RawPacketBuffer)
  memcpy(buf + len, pkt.IV(), SSUSize::IV);
  core::OutputByteStream::Write<std::uint16_t>(
      buf + len + SSUSize::IV, encrypted_len);
  std::array<std::uint8_t, 16> digest;
  kovri::core::HMACMD5Digest(
      encrypted,
      encrypted_len + SSUSize::BufferMargin,
      mac_key,
      digest.data());
  return !memcmp(pkt.MAC(), digest.data(), digest.size());
}

void SSUSession::Connect() {
  if (m_State == SessionState::Unknown) {
    // set connect timer
    ScheduleConnectTimer();
    m_DHKeysPair = transports.GetNextDHKeysPair();
    SendSessionRequest();
  }
}

void SSUSession::WaitForConnect() {
  if (IsOutbound())
    LOG(warning)
      << "SSUSession:" << GetFormattedSessionInfo()
      << __func__ << " for outgoing session";  // TODO(anonimal): message
  else
    ScheduleConnectTimer();
}

void SSUSession::ScheduleConnectTimer() {
  m_Timer.cancel();
  m_Timer.expires_from_now(
      boost::posix_time::seconds(
          SSUDuration::ConnectTimeout));
  m_Timer.async_wait(
      std::bind(
          &SSUSession::HandleConnectTimer,
          shared_from_this(),
          std::placeholders::_1));
}

void SSUSession::HandleConnectTimer(
    const boost::system::error_code& ecode) {
  if (!ecode) {
    // timeout expired
    LOG(error)
      << "SSUSession:" << GetFormattedSessionInfo()
      << "session was not established after "
      << static_cast<std::size_t>(SSUDuration::ConnectTimeout) << " seconds";
    Failed();
  }
}

void SSUSession::Introduce(
    std::uint32_t introducer_tag,
    const std::uint8_t* introducer_key) {
  if (m_State == SessionState::Unknown) {
    // set connect timer
    m_Timer.expires_from_now(
        boost::posix_time::seconds(
            SSUDuration::ConnectTimeout));
    m_Timer.async_wait(
        std::bind(
          &SSUSession::HandleConnectTimer,
          shared_from_this(),
          std::placeholders::_1));
  }
  SendRelayRequest(introducer_tag, introducer_key);
}

void SSUSession::WaitForIntroduction() {
  m_State = SessionState::Introduced;
  // set connect timer
  m_Timer.expires_from_now(
      boost::posix_time::seconds(
          SSUDuration::ConnectTimeout));
  m_Timer.async_wait(
      std::bind(
        &SSUSession::HandleConnectTimer,
        shared_from_this(),
        std::placeholders::_1));
}

void SSUSession::Close() {
  m_State = SessionState::Closed;
  SendSesionDestroyed();
  transports.PeerDisconnected(shared_from_this());
  m_Data.Stop();
  m_Timer.cancel();
}

void SSUSession::Done() {
  GetService().post(
      std::bind(
        &SSUSession::Failed,
        shared_from_this()));
}

void SSUSession::Established() {
  // clear out session confirmation data
  m_SessionConfirmData.reset(nullptr);
  m_State = SessionState::Established;
  if (m_DHKeysPair) {
    m_DHKeysPair.reset(nullptr);
  }
  m_Data.Start();
  // send delivery status
  m_Data.Send(CreateDeliveryStatusMsg(0));
  // send database store
  m_Data.Send(CreateDatabaseStoreMsg());
  transports.PeerConnected(shared_from_this());
  if (m_PeerTest && (m_RemoteRouter && m_RemoteRouter->HasCap(RouterInfo::Cap::SSUTesting)))
    SendPeerTest();
  ScheduleTermination();
}

void SSUSession::Failed() {
  if (m_State != SessionState::Failed) {
    m_State = SessionState::Failed;
    m_Server.DeleteSession(shared_from_this());
  }
}

void SSUSession::ScheduleTermination() {
  m_Timer.cancel();
  m_Timer.expires_from_now(
      boost::posix_time::seconds(
          SSUDuration::TerminationTimeout));
  m_Timer.async_wait(
      std::bind(
          &SSUSession::HandleTerminationTimer,
          shared_from_this(),
          std::placeholders::_1));
}

void SSUSession::HandleTerminationTimer(
    const boost::system::error_code& ecode) {
  if (ecode != boost::asio::error::operation_aborted) {
    LOG(error)
      << "SSUSession:" << GetFormattedSessionInfo() << "no activity for "
      << static_cast<std::size_t>(SSUDuration::TerminationTimeout) << " seconds";
    Failed();
  }
}

const std::uint8_t* SSUSession::GetIntroKey() const {
  if (m_RemoteRouter) {
    // we are client
    auto address = m_RemoteRouter->GetSSUAddress();
    return address ? (const std::uint8_t *)address->key : nullptr;
  } else {
    // we are server
    auto address = context.GetRouterInfo().GetSSUAddress();
    return address ? (const std::uint8_t *)address->key : nullptr;
  }
}

void SSUSession::SendI2NPMessages(
    const std::vector<std::shared_ptr<I2NPMessage>>& msgs) {
  GetService().post(
      std::bind(
          &SSUSession::PostI2NPMessages,
          shared_from_this(),
          msgs));
}

void SSUSession::PostI2NPMessages(
    std::vector<std::shared_ptr<I2NPMessage>> msgs) {
  if (m_State == SessionState::Established) {
    for (auto it : msgs)
      if (it)
        m_Data.Send(it);
  }
}

void SSUSession::Send(
    std::uint8_t type,
    const std::uint8_t* payload,
    std::size_t len) {
  std::array<std::uint8_t, SSUSize::RawPacketBuffer> buf{{}};
  auto msg_size = len + SSUSize::HeaderMin;
  auto padding_size = msg_size & 0x0F;  // %16
  if (padding_size > 0)
    msg_size += (16 - padding_size);
  if (msg_size > SSUSize::MTUv4) {
    LOG(warning)
      << "SSUSession:" << GetFormattedSessionInfo()
      << "<-- payload size " << msg_size << " exceeds MTU";
    return;
  }
  memcpy(buf.data() + SSUSize::HeaderMin, payload, len);
  // encrypt message with session key
  FillHeaderAndEncrypt(type, buf.data(), msg_size);
  Send(buf.data(), msg_size);
}

void SSUSession::Send(
    const std::uint8_t* buf,
    std::size_t size) {
  m_NumSentBytes += size;
  LOG(debug)
    << "SSUSession:" << GetFormattedSessionInfo()
    << "<-- " << size << " bytes transferred, "
    << GetNumSentBytes() << " total bytes sent";
  kovri::core::transports.UpdateSentBytes(size);
  m_Server.Send(buf, size, GetRemoteEndpoint());
}

}  // namespace core
}  // namespace kovri

