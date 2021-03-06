/*
 *  Copyright (c) 2020 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */
#include "test/pc/e2e/media/media_helper.h"

#include <string>
#include <utility>

#include "api/test/create_frame_generator.h"
#include "api/test/create_peer_connection_quality_test_frame_generator.h"
#include "test/frame_generator_capturer.h"
#include "test/platform_video_capturer.h"
#include "test/testsupport/file_utils.h"

namespace webrtc {
namespace webrtc_pc_e2e {
namespace {

using VideoConfig =
    ::webrtc::webrtc_pc_e2e::PeerConnectionE2EQualityTestFixture::VideoConfig;
using AudioConfig =
    ::webrtc::webrtc_pc_e2e::PeerConnectionE2EQualityTestFixture::AudioConfig;
using VideoGeneratorType = ::webrtc::webrtc_pc_e2e::
    PeerConnectionE2EQualityTestFixture::VideoGeneratorType;

}  // namespace

void MediaHelper::MaybeAddAudio(TestPeer* peer) {
  if (!peer->params()->audio_config) {
    return;
  }
  const AudioConfig& audio_config = peer->params()->audio_config.value();
  rtc::scoped_refptr<webrtc::AudioSourceInterface> source =
      peer->pc_factory()->CreateAudioSource(audio_config.audio_options);
  rtc::scoped_refptr<AudioTrackInterface> track =
      peer->pc_factory()->CreateAudioTrack(*audio_config.stream_label, source);
  std::string sync_group = audio_config.sync_group
                               ? audio_config.sync_group.value()
                               : audio_config.stream_label.value();
  peer->AddTrack(track, {sync_group, *audio_config.stream_label});
}

std::vector<rtc::scoped_refptr<TestVideoCapturerVideoTrackSource>>
MediaHelper::MaybeAddVideo(TestPeer* peer) {
  // Params here valid because of pre-run validation.
  Params* params = peer->params();
  std::vector<rtc::scoped_refptr<TestVideoCapturerVideoTrackSource>> out;
  for (size_t i = 0; i < params->video_configs.size(); ++i) {
    auto video_config = params->video_configs[i];
    // Setup input video source into peer connection.
    std::unique_ptr<test::TestVideoCapturer> capturer = CreateVideoCapturer(
        video_config, peer->ReleaseVideoGenerator(i),
        video_quality_analyzer_injection_helper_->CreateFramePreprocessor(
            video_config));
    rtc::scoped_refptr<TestVideoCapturerVideoTrackSource> source =
        new rtc::RefCountedObject<TestVideoCapturerVideoTrackSource>(
            std::move(capturer),
            /*is_screencast=*/video_config.screen_share_config &&
                video_config.screen_share_config->use_text_content_hint);
    out.push_back(source);
    RTC_LOG(INFO) << "Adding video with video_config.stream_label="
                  << video_config.stream_label.value();
    rtc::scoped_refptr<VideoTrackInterface> track =
        peer->pc_factory()->CreateVideoTrack(video_config.stream_label.value(),
                                             source);
    if (video_config.screen_share_config &&
        video_config.screen_share_config->use_text_content_hint) {
      track->set_content_hint(VideoTrackInterface::ContentHint::kText);
    }
    std::string sync_group = video_config.sync_group
                                 ? video_config.sync_group.value()
                                 : video_config.stream_label.value();
    RTCErrorOr<rtc::scoped_refptr<RtpSenderInterface>> sender =
        peer->AddTrack(track, {sync_group, *video_config.stream_label});
    RTC_CHECK(sender.ok());
    if (video_config.temporal_layers_count) {
      RtpParameters rtp_parameters = sender.value()->GetParameters();
      for (auto& encoding_parameters : rtp_parameters.encodings) {
        encoding_parameters.num_temporal_layers =
            video_config.temporal_layers_count;
      }
      RTCError res = sender.value()->SetParameters(rtp_parameters);
      RTC_CHECK(res.ok()) << "Failed to set RTP parameters";
    }
  }
  return out;
}

std::unique_ptr<test::TestVideoCapturer> MediaHelper::CreateVideoCapturer(
    const VideoConfig& video_config,
    std::unique_ptr<test::FrameGeneratorInterface> generator,
    std::unique_ptr<test::TestVideoCapturer::FramePreprocessor>
        frame_preprocessor) {
  if (video_config.capturing_device_index) {
    std::unique_ptr<test::TestVideoCapturer> capturer =
        test::CreateVideoCapturer(video_config.width, video_config.height,
                                  video_config.fps,
                                  *video_config.capturing_device_index);
    RTC_CHECK(capturer)
        << "Failed to obtain input stream from capturing device #"
        << *video_config.capturing_device_index;
    capturer->SetFramePreprocessor(std::move(frame_preprocessor));
    return capturer;
  }

  std::unique_ptr<test::FrameGeneratorInterface> frame_generator = nullptr;
  if (generator) {
    frame_generator = std::move(generator);
  }

  if (video_config.generator) {
    absl::optional<test::FrameGeneratorInterface::OutputType>
        frame_generator_type = absl::nullopt;
    if (video_config.generator == VideoGeneratorType::kDefault) {
      frame_generator_type = test::FrameGeneratorInterface::OutputType::kI420;
    } else if (video_config.generator == VideoGeneratorType::kI420A) {
      frame_generator_type = test::FrameGeneratorInterface::OutputType::kI420A;
    } else if (video_config.generator == VideoGeneratorType::kI010) {
      frame_generator_type = test::FrameGeneratorInterface::OutputType::kI010;
    }
    frame_generator =
        test::CreateSquareFrameGenerator(static_cast<int>(video_config.width),
                                         static_cast<int>(video_config.height),
                                         frame_generator_type, absl::nullopt);
  }
  if (video_config.input_file_name) {
    frame_generator = test::CreateFromYuvFileFrameGenerator(
        std::vector<std::string>(/*count=*/1,
                                 video_config.input_file_name.value()),
        video_config.width, video_config.height, /*frame_repeat_count=*/1);
  }
  if (video_config.screen_share_config) {
    frame_generator = CreateScreenShareFrameGenerator(
        video_config, *video_config.screen_share_config);
  }
  RTC_CHECK(frame_generator) << "Unsupported video_config input source";

  auto capturer = std::make_unique<test::FrameGeneratorCapturer>(
      clock_, std::move(frame_generator), video_config.fps,
      *task_queue_factory_);
  capturer->SetFramePreprocessor(std::move(frame_preprocessor));
  capturer->Init();
  return capturer;
}

}  // namespace webrtc_pc_e2e
}  // namespace webrtc
