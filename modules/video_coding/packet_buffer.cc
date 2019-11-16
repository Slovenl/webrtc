/*
 *  Copyright (c) 2016 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "modules/video_coding/packet_buffer.h"

#include <string.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

#include "absl/types/variant.h"
#include "api/array_view.h"
#include "api/rtp_packet_info.h"
#include "api/video/encoded_frame.h"
#include "api/video/video_frame_type.h"
#include "common_video/h264/h264_common.h"
#ifndef DISABLE_H265
#include "common_video/h265/h265_common.h"
#endif
#include "modules/rtp_rtcp/source/rtp_header_extensions.h"
#include "modules/rtp_rtcp/source/rtp_packet_received.h"
#include "modules/rtp_rtcp/source/rtp_video_header.h"
#include "modules/rtp_rtcp/source/video_rtp_depacketizer_av1.h"
#include "modules/video_coding/codecs/h264/include/h264_globals.h"
#ifndef DISABLE_H265
#include "modules/video_coding/codecs/h265/include/h265_globals.h"
#endif
#include "modules/video_coding/frame_object.h"
#include "rtc_base/checks.h"
#include "rtc_base/logging.h"
#include "rtc_base/numerics/mod_ops.h"
#include "system_wrappers/include/clock.h"
#include "system_wrappers/include/field_trial.h"

namespace webrtc {
namespace video_coding {

PacketBuffer::Packet::Packet(const RtpPacketReceived& rtp_packet,
                             const RTPVideoHeader& video_header,
                             int64_t ntp_time_ms,
                             int64_t receive_time_ms)
    : marker_bit(rtp_packet.Marker()),
      payload_type(rtp_packet.PayloadType()),
      seq_num(rtp_packet.SequenceNumber()),
      timestamp(rtp_packet.Timestamp()),
      ntp_time_ms(ntp_time_ms),
      times_nacked(-1),
      video_header(video_header),
      packet_info(rtp_packet.Ssrc(),
                  rtp_packet.Csrcs(),
                  rtp_packet.Timestamp(),
                  /*audio_level=*/absl::nullopt,
                  rtp_packet.GetExtension<AbsoluteCaptureTimeExtension>(),
                  receive_time_ms) {}

PacketBuffer::PacketBuffer(Clock* clock,
                           size_t start_buffer_size,
                           size_t max_buffer_size)
    : clock_(clock),
      max_size_(max_buffer_size),
      first_seq_num_(0),
      first_packet_received_(false),
      is_cleared_to_first_seq_num_(false),
      buffer_(start_buffer_size),
      sps_pps_idr_is_h264_keyframe_(
          field_trial::IsEnabled("WebRTC-SpsPpsIdrIsH264Keyframe")) {
  RTC_DCHECK_LE(start_buffer_size, max_buffer_size);
  // Buffer size must always be a power of 2.
  RTC_DCHECK((start_buffer_size & (start_buffer_size - 1)) == 0);
  RTC_DCHECK((max_buffer_size & (max_buffer_size - 1)) == 0);
}

PacketBuffer::~PacketBuffer() {
  Clear();
}

PacketBuffer::InsertResult PacketBuffer::InsertPacket(
    std::unique_ptr<PacketBuffer::Packet> packet) {
  PacketBuffer::InsertResult result;
  rtc::CritScope lock(&crit_);

  uint16_t seq_num = packet->seq_num;
  size_t index = seq_num % buffer_.size();

  if (!first_packet_received_) {
    first_seq_num_ = seq_num;
    first_packet_received_ = true;
  } else if (AheadOf(first_seq_num_, seq_num)) {
    // If we have explicitly cleared past this packet then it's old,
    // don't insert it, just silently ignore it.
    if (is_cleared_to_first_seq_num_) {
      return result;
    }

    first_seq_num_ = seq_num;
  }

  if (buffer_[index].used()) {
    // Duplicate packet, just delete the payload.
    if (buffer_[index].seq_num() == packet->seq_num) {
      return result;
    }

    // The packet buffer is full, try to expand the buffer.
    while (ExpandBufferSize() && buffer_[seq_num % buffer_.size()].used()) {
    }
    index = seq_num % buffer_.size();

    // Packet buffer is still full since we were unable to expand the buffer.
    if (buffer_[index].used()) {
      // Clear the buffer, delete payload, and return false to signal that a
      // new keyframe is needed.
      RTC_LOG(LS_WARNING) << "Clear PacketBuffer and request key frame.";
      Clear();
      result.buffer_cleared = true;
      return result;
    }
  }

  int64_t now_ms = clock_->TimeInMilliseconds();
  last_received_packet_ms_ = now_ms;
  if (packet->video_header.frame_type == VideoFrameType::kVideoFrameKey ||
      last_received_keyframe_rtp_timestamp_ == packet->timestamp) {
    last_received_keyframe_packet_ms_ = now_ms;
    last_received_keyframe_rtp_timestamp_ = packet->timestamp;
  }

  StoredPacket& new_entry = buffer_[index];
  new_entry.continuous = false;
  new_entry.packet = std::move(packet);

  UpdateMissingPackets(seq_num);

  result.frames = FindFrames(seq_num);
  return result;
}

void PacketBuffer::ClearTo(uint16_t seq_num) {
  rtc::CritScope lock(&crit_);
  // We have already cleared past this sequence number, no need to do anything.
  if (is_cleared_to_first_seq_num_ &&
      AheadOf<uint16_t>(first_seq_num_, seq_num)) {
    return;
  }

  // If the packet buffer was cleared between a frame was created and returned.
  if (!first_packet_received_)
    return;

  // Avoid iterating over the buffer more than once by capping the number of
  // iterations to the |size_| of the buffer.
  ++seq_num;
  size_t diff = ForwardDiff<uint16_t>(first_seq_num_, seq_num);
  size_t iterations = std::min(diff, buffer_.size());
  for (size_t i = 0; i < iterations; ++i) {
    StoredPacket& stored = buffer_[first_seq_num_ % buffer_.size()];
    if (stored.used() && AheadOf<uint16_t>(seq_num, stored.seq_num())) {
      stored.packet = nullptr;
    }
    ++first_seq_num_;
  }

  // If |diff| is larger than |iterations| it means that we don't increment
  // |first_seq_num_| until we reach |seq_num|, so we set it here.
  first_seq_num_ = seq_num;

  is_cleared_to_first_seq_num_ = true;
  auto clear_to_it = missing_packets_.upper_bound(seq_num);
  if (clear_to_it != missing_packets_.begin()) {
    --clear_to_it;
    missing_packets_.erase(missing_packets_.begin(), clear_to_it);
  }
}

void PacketBuffer::ClearInterval(uint16_t start_seq_num,
                                 uint16_t stop_seq_num) {
  size_t iterations = ForwardDiff<uint16_t>(start_seq_num, stop_seq_num + 1);
  RTC_DCHECK_LE(iterations, buffer_.size());
  uint16_t seq_num = start_seq_num;
  for (size_t i = 0; i < iterations; ++i) {
    size_t index = seq_num % buffer_.size();
    RTC_DCHECK_EQ(buffer_[index].seq_num(), seq_num);
    buffer_[index].packet = nullptr;

    ++seq_num;
  }
}

void PacketBuffer::Clear() {
  rtc::CritScope lock(&crit_);
  for (StoredPacket& entry : buffer_) {
    entry.packet = nullptr;
  }

  first_packet_received_ = false;
  is_cleared_to_first_seq_num_ = false;
  last_received_packet_ms_.reset();
  last_received_keyframe_packet_ms_.reset();
  newest_inserted_seq_num_.reset();
  missing_packets_.clear();
}

PacketBuffer::InsertResult PacketBuffer::InsertPadding(uint16_t seq_num) {
  PacketBuffer::InsertResult result;
  rtc::CritScope lock(&crit_);
  UpdateMissingPackets(seq_num);
  result.frames = FindFrames(static_cast<uint16_t>(seq_num + 1));
  return result;
}

absl::optional<int64_t> PacketBuffer::LastReceivedPacketMs() const {
  rtc::CritScope lock(&crit_);
  return last_received_packet_ms_;
}

absl::optional<int64_t> PacketBuffer::LastReceivedKeyframePacketMs() const {
  rtc::CritScope lock(&crit_);
  return last_received_keyframe_packet_ms_;
}

bool PacketBuffer::ExpandBufferSize() {
  if (buffer_.size() == max_size_) {
    RTC_LOG(LS_WARNING) << "PacketBuffer is already at max size (" << max_size_
                        << "), failed to increase size.";
    return false;
  }

  size_t new_size = std::min(max_size_, 2 * buffer_.size());
  std::vector<StoredPacket> new_buffer(new_size);
  for (StoredPacket& entry : buffer_) {
    if (entry.used()) {
      new_buffer[entry.seq_num() % new_size] = std::move(entry);
    }
  }
  buffer_ = std::move(new_buffer);
  RTC_LOG(LS_INFO) << "PacketBuffer size expanded to " << new_size;
  return true;
}

bool PacketBuffer::PotentialNewFrame(uint16_t seq_num) const {
  size_t index = seq_num % buffer_.size();
  int prev_index = index > 0 ? index - 1 : buffer_.size() - 1;
  const StoredPacket& entry = buffer_[index];
  const StoredPacket& prev_entry = buffer_[prev_index];

  if (!entry.used())
    return false;
  if (entry.seq_num() != seq_num)
    return false;
  if (entry.frame_begin())
    return true;
  if (!prev_entry.used())
    return false;
  if (prev_entry.seq_num() != static_cast<uint16_t>(entry.seq_num() - 1))
    return false;
  if (prev_entry.packet->timestamp != entry.packet->timestamp)
    return false;
  if (prev_entry.continuous)
    return true;

  return false;
}

std::vector<std::unique_ptr<RtpFrameObject>> PacketBuffer::FindFrames(
    uint16_t seq_num) {
  std::vector<std::unique_ptr<RtpFrameObject>> found_frames;
  for (size_t i = 0; i < buffer_.size() && PotentialNewFrame(seq_num); ++i) {
    size_t index = seq_num % buffer_.size();
    buffer_[index].continuous = true;

    // If all packets of the frame is continuous, find the first packet of the
    // frame and create an RtpFrameObject.
    if (buffer_[index].frame_end()) {
      uint16_t start_seq_num = seq_num;

      // Find the start index by searching backward until the packet with
      // the |frame_begin| flag is set.
      int start_index = index;
      size_t tested_packets = 0;
      int64_t frame_timestamp = buffer_[start_index].packet->timestamp;

      // Identify H.264 keyframes by means of SPS, PPS, and IDR.
      bool is_h264 = buffer_[start_index].packet->codec() == kVideoCodecH264;
      bool has_h264_sps = false;
      bool has_h264_pps = false;
      bool has_h264_idr = false;
      bool is_h264_keyframe = false;

      bool is_h265 = false;
#ifndef DISABLE_H265
      is_h265 = buffer_[start_index].packet->codec() == kVideoCodecH265;
      bool has_h265_sps = false;
      bool has_h265_pps = false;
      bool has_h265_idr = false;
      bool is_h265_keyframe = false;
#endif

      int idr_width = -1;
      int idr_height = -1;
      while (true) {
        ++tested_packets;

        if (!is_h264 && !is_h265 && buffer_[start_index].frame_begin())
          break;

        if (is_h264) {
          const auto* h264_header = absl::get_if<RTPVideoHeaderH264>(
              &buffer_[start_index].packet->video_header.video_type_header);
          if (!h264_header || h264_header->nalus_length >= kMaxNalusPerPacket)
            return found_frames;

          for (size_t j = 0; j < h264_header->nalus_length; ++j) {
            if (h264_header->nalus[j].type == H264::NaluType::kSps) {
              has_h264_sps = true;
            } else if (h264_header->nalus[j].type == H264::NaluType::kPps) {
              has_h264_pps = true;
            } else if (h264_header->nalus[j].type == H264::NaluType::kIdr) {
              has_h264_idr = true;
            }
          }
          if ((sps_pps_idr_is_h264_keyframe_ && has_h264_idr && has_h264_sps &&
               has_h264_pps) ||
              (!sps_pps_idr_is_h264_keyframe_ && has_h264_idr)) {
            is_h264_keyframe = true;
            // Store the resolution of key frame which is the packet with
            // smallest index and valid resolution; typically its IDR or SPS
            // packet; there may be packet preceeding this packet, IDR's
            // resolution will be applied to them.
            if (buffer_[start_index].packet->width() > 0 &&
                buffer_[start_index].packet->height() > 0) {
              idr_width = buffer_[start_index].packet->width();
              idr_height = buffer_[start_index].packet->height();
            }
          }
        }

#ifndef DISABLE_H265
        if (is_h265 && !is_h265_keyframe) {
          const auto* h265_header = absl::get_if<RTPVideoHeaderH265>(
              &buffer_[start_index].packet->video_header.video_type_header);
          if (!h265_header || h265_header->nalus_length >= kMaxNalusPerPacket)
            return found_frames;
          for (size_t j = 0; j < h265_header->nalus_length; ++j) {
            if (h265_header->nalus[j].type == H265::NaluType::kSps) {
              has_h265_sps = true;
            } else if (h265_header->nalus[j].type == H265::NaluType::kPps) {
              has_h265_pps = true;
            } else if (h265_header->nalus[j].type == H265::NaluType::kIdrWRadl
                       || h265_header->nalus[j].type == H265::NaluType::kIdrNLp
                       || h265_header->nalus[j].type == H265::NaluType::kCra) {
              has_h265_idr = true;
            }
          }
          if ((has_h265_sps && has_h265_pps) || has_h265_idr) {
            is_h265_keyframe = true;
            // Store the resolution of key frame which is the packet with
            // smallest index and valid resolution; typically its IDR or SPS
            // packet; there may be packet preceeding this packet, IDR's
            // resolution will be applied to them.
            if (buffer_[start_index].packet->width() > 0 &&
                buffer_[start_index].packet->height() > 0) {
              idr_width = buffer_[start_index].packet->width();
              idr_height = buffer_[start_index].packet->height();
            }
          }
        }
#endif

        if (tested_packets == buffer_.size())
          break;

        start_index = start_index > 0 ? start_index - 1 : buffer_.size() - 1;

        // In the case of H264 we don't have a frame_begin bit (yes,
        // |frame_begin| might be set to true but that is a lie). So instead
        // we traverese backwards as long as we have a previous packet and
        // the timestamp of that packet is the same as this one. This may cause
        // the PacketBuffer to hand out incomplete frames.
        // See: https://bugs.chromium.org/p/webrtc/issues/detail?id=7106
        if ((is_h264 || is_h265) &&
            (!buffer_[start_index].used() ||
             buffer_[start_index].packet->timestamp != frame_timestamp)) {
          break;
        }

        --start_seq_num;
      }

      if (is_h264) {
        // Warn if this is an unsafe frame.
        if (has_h264_idr && (!has_h264_sps || !has_h264_pps)) {
          RTC_LOG(LS_WARNING)
              << "Received H.264-IDR frame "
                 "(SPS: "
              << has_h264_sps << ", PPS: " << has_h264_pps << "). Treating as "
              << (sps_pps_idr_is_h264_keyframe_ ? "delta" : "key")
              << " frame since WebRTC-SpsPpsIdrIsH264Keyframe is "
              << (sps_pps_idr_is_h264_keyframe_ ? "enabled." : "disabled");
        }

        // Now that we have decided whether to treat this frame as a key frame
        // or delta frame in the frame buffer, we update the field that
        // determines if the RtpFrameObject is a key frame or delta frame.
        const size_t first_packet_index = start_seq_num % buffer_.size();
        if (is_h264_keyframe) {
          buffer_[first_packet_index].packet->video_header.frame_type =
              VideoFrameType::kVideoFrameKey;
          if (idr_width > 0 && idr_height > 0) {
            // IDR frame was finalized and we have the correct resolution for
            // IDR; update first packet to have same resolution as IDR.
            buffer_[first_packet_index].packet->video_header.width = idr_width;
            buffer_[first_packet_index].packet->video_header.height =
                idr_height;
          }
        } else {
          buffer_[first_packet_index].packet->video_header.frame_type =
              VideoFrameType::kVideoFrameDelta;
        }

        // With IPPP, if this is not a keyframe, make sure there are no gaps
        // in the packet sequence numbers up until this point.
        const uint8_t h264tid =
            buffer_[start_index].used()
                ? buffer_[start_index]
                      .packet->video_header.frame_marking.temporal_id
                : kNoTemporalIdx;
        if (h264tid == kNoTemporalIdx && !is_h264_keyframe &&
            missing_packets_.upper_bound(start_seq_num) !=
                missing_packets_.begin()) {
          return found_frames;
        }
      }

#ifndef DISABLE_H265
      if (is_h265) {
        // Warn if this is an unsafe frame.
        if (has_h265_idr && (!has_h265_sps || !has_h265_pps)) {
          RTC_LOG(LS_WARNING)
              << "Received H.265-IDR frame "
              << "(SPS: " << has_h265_sps << ", PPS: " << has_h265_pps
              << "). Treating as delta frame since "
              <<  "WebRTC-SpsPpsIdrIsH265Keyframe is always enabled.";
        }

        // Now that we have decided whether to treat this frame as a key frame
        // or delta frame in the frame buffer, we update the field that
        // determines if the RtpFrameObject is a key frame or delta frame.
        const size_t first_packet_index = start_seq_num % buffer_.size();
        if (is_h265_keyframe) {
          buffer_[first_packet_index].packet->video_header.frame_type =
              VideoFrameType::kVideoFrameKey;
          if (idr_width > 0 && idr_height > 0) {
            // IDR frame was finalized and we have the correct resolution for
            // IDR; update first packet to have same resolution as IDR.
            buffer_[first_packet_index].packet->video_header.width = idr_width;
            buffer_[first_packet_index].packet->video_header.height =
                idr_height;
          }
        } else {
          buffer_[first_packet_index].packet->video_header.frame_type =
              VideoFrameType::kVideoFrameDelta;
        }

        // If this is not a key frame, make sure there are no gaps in the
        // packet sequence numbers up until this point.
        if (!is_h265_keyframe && missing_packets_.upper_bound(start_seq_num) !=
                                     missing_packets_.begin()) {
          return found_frames;
        }
      }
#endif

      if (auto frame = AssembleFrame(start_seq_num, seq_num)) {
        found_frames.push_back(std::move(frame));
      } else {
        RTC_LOG(LS_ERROR) << "Failed to assemble frame from packets "
                          << start_seq_num << "-" << seq_num;
      }

      missing_packets_.erase(missing_packets_.begin(),
                             missing_packets_.upper_bound(seq_num));
      ClearInterval(start_seq_num, seq_num);
    }
    ++seq_num;
  }
  return found_frames;
}

std::unique_ptr<RtpFrameObject> PacketBuffer::AssembleFrame(
    uint16_t first_seq_num,
    uint16_t last_seq_num) {
  const uint16_t end_seq_num = last_seq_num + 1;
  const uint16_t num_packets = end_seq_num - first_seq_num;
  int max_nack_count = -1;
  int64_t min_recv_time = std::numeric_limits<int64_t>::max();
  int64_t max_recv_time = std::numeric_limits<int64_t>::min();
  size_t frame_size = 0;

  std::vector<rtc::ArrayView<const uint8_t>> payloads;
  RtpPacketInfos::vector_type packet_infos;
  payloads.reserve(num_packets);
  packet_infos.reserve(num_packets);

  for (uint16_t seq_num = first_seq_num; seq_num != end_seq_num; ++seq_num) {
    const Packet& packet = GetPacket(seq_num);

    max_nack_count = std::max(max_nack_count, packet.times_nacked);
    min_recv_time =
        std::min(min_recv_time, packet.packet_info.receive_time_ms());
    max_recv_time =
        std::max(max_recv_time, packet.packet_info.receive_time_ms());
    frame_size += packet.video_payload.size();
    payloads.emplace_back(packet.video_payload);
    packet_infos.push_back(packet.packet_info);
  }

  const Packet& first_packet = GetPacket(first_seq_num);
  rtc::scoped_refptr<EncodedImageBuffer> bitstream;
  // TODO(danilchap): Hide codec-specific code paths behind an interface.
  if (first_packet.codec() == VideoCodecType::kVideoCodecAV1) {
    bitstream = VideoRtpDepacketizerAv1::AssembleFrame(payloads);
    if (!bitstream) {
      // Failed to assemble a frame. Discard and continue.
      return nullptr;
    }
  } else {
    bitstream = EncodedImageBuffer::Create(frame_size);

    uint8_t* write_at = bitstream->data();
    for (rtc::ArrayView<const uint8_t> payload : payloads) {
      memcpy(write_at, payload.data(), payload.size());
      write_at += payload.size();
    }
    RTC_DCHECK_EQ(write_at - bitstream->data(), bitstream->size());
  }
  const Packet& last_packet = GetPacket(last_seq_num);
  return std::make_unique<RtpFrameObject>(
      first_seq_num,                            //
      last_seq_num,                             //
      last_packet.marker_bit,                   //
      max_nack_count,                           //
      min_recv_time,                            //
      max_recv_time,                            //
      first_packet.timestamp,                   //
      first_packet.ntp_time_ms,                 //
      last_packet.video_header.video_timing,    //
      first_packet.payload_type,                //
      first_packet.codec(),                     //
      last_packet.video_header.rotation,        //
      last_packet.video_header.content_type,    //
      first_packet.video_header,                //
      last_packet.video_header.color_space,     //
      first_packet.generic_descriptor,          //
      RtpPacketInfos(std::move(packet_infos)),  //
      std::move(bitstream));
}

const PacketBuffer::Packet& PacketBuffer::GetPacket(uint16_t seq_num) const {
  const StoredPacket& entry = buffer_[seq_num % buffer_.size()];
  RTC_DCHECK(entry.used());
  RTC_DCHECK_EQ(seq_num, entry.seq_num());
  return *entry.packet;
}

void PacketBuffer::UpdateMissingPackets(uint16_t seq_num) {
  if (!newest_inserted_seq_num_)
    newest_inserted_seq_num_ = seq_num;

  const int kMaxPaddingAge = 1000;
  if (AheadOf(seq_num, *newest_inserted_seq_num_)) {
    uint16_t old_seq_num = seq_num - kMaxPaddingAge;
    auto erase_to = missing_packets_.lower_bound(old_seq_num);
    missing_packets_.erase(missing_packets_.begin(), erase_to);

    // Guard against inserting a large amount of missing packets if there is a
    // jump in the sequence number.
    if (AheadOf(old_seq_num, *newest_inserted_seq_num_))
      *newest_inserted_seq_num_ = old_seq_num;

    ++*newest_inserted_seq_num_;
    while (AheadOf(seq_num, *newest_inserted_seq_num_)) {
      missing_packets_.insert(*newest_inserted_seq_num_);
      ++*newest_inserted_seq_num_;
    }
  } else {
    missing_packets_.erase(seq_num);
  }
}

}  // namespace video_coding
}  // namespace webrtc
