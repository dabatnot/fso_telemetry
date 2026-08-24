#include "telemetry/communication_view_producer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

namespace {

using telemetry::CommunicationViewSample;
using telemetry::detail::CommunicationAsset;
using telemetry::detail::CommunicationBundle;
using telemetry::detail::CommunicationMapping;
using telemetry::detail::CommunicationViewProducer;
using telemetry::detail::PendingCommunicationEvent;
using namespace telemetry::protocol;

CommunicationBundle make_bundle()
{
	CommunicationBundle bundle;
	CommunicationAsset asset;
	asset.asset_id = 0x1020304050607080ULL;
	asset.duration_us = 2'000'000U;
	asset.source_format = SourceFormat::Ani;
	asset.delivered_format = DeliveredFormat::Apng;
	bundle.assets.push_back(asset);
	bundle.mappings.push_back(CommunicationMapping{"command.ani", SourceFormat::Ani, asset.asset_id});
	return bundle;
}

CommunicationViewSample make_sample(const char* name = "command.ani")
{
	CommunicationViewSample sample;
	std::copy_n(name, std::min(std::strlen(name), sample.generic_anim_name.size() - 1U),
		sample.generic_anim_name.begin());
	sample.generic_anim_type = SourceFormat::Ani;
	sample.engine_message_id = 42U;
	sample.sender_object_signature = 77U;
	sample.animation_time_us = 250'000U;
	sample.duration_us = 2'000'000U;
	sample.playback_mode = CommPlaybackMode::Once;
	sample.color_mode = CommColorMode::HudTint;
	sample.playback_rate = 1.0F;
	return sample;
}

class CommunicationViewProducerTest : public testing::Test {
  protected:
	void SetUp() override
	{
		telemetry::communication_view_bridge().purge();
		telemetry::communication_view_bridge().set_enabled(true);
	}
	void TearDown() override { telemetry::communication_view_bridge().set_enabled(false); }
};

TEST_F(CommunicationViewProducerTest, PublishesAuthoritativeStartStateAndStop)
{
	auto bundle = make_bundle();
	CommunicationViewProducer producer;
	producer.configure(&bundle);
	ASSERT_TRUE(telemetry::communication_view_bridge().start(make_sample()));
	producer.tick(1'000'000U, 1U, false, 1.0F);

	ASSERT_TRUE(producer.active());
	const auto state = producer.state(1234U);
	EXPECT_EQ(bundle.assets[0].asset_id, state.head_asset_id);
	EXPECT_EQ(1234U, state.sender_entity_id);
	EXPECT_EQ(250'000U, state.animation_time_us);
	EXPECT_EQ(42U, state.engine_message_id);

	PendingCommunicationEvent event;
	ASSERT_TRUE(producer.pop_event(event));
	EXPECT_EQ(CommEventKind::Start, event.payload.event_kind);
	EXPECT_EQ(77U, event.sender_object_signature);
	EXPECT_EQ(state.playback_id, event.payload.playback_id);

	ASSERT_TRUE(telemetry::communication_view_bridge().stop(CommStopReason::Interrupted));
	producer.tick(1'010'000U, 1U, false, 1.0F);
	ASSERT_TRUE(producer.pop_event(event));
	EXPECT_EQ(CommEventKind::Stop, event.payload.event_kind);
	EXPECT_EQ(CommStopReason::Interrupted, event.payload.stop_reason);
	EXPECT_FALSE(producer.active());
}

TEST_F(CommunicationViewProducerTest, ReplacementIsStrictlyStopThenStart)
{
	auto bundle = make_bundle();
	CommunicationViewProducer producer;
	producer.configure(&bundle);
	ASSERT_TRUE(telemetry::communication_view_bridge().start(make_sample()));
	producer.tick(100U, 1U, false, 1.0F);
	PendingCommunicationEvent event;
	ASSERT_TRUE(producer.pop_event(event));
	const auto first_playback = event.payload.playback_id;

	ASSERT_TRUE(telemetry::communication_view_bridge().start(make_sample()));
	producer.tick(200U, 1U, false, 1.0F);
	ASSERT_TRUE(producer.pop_event(event));
	EXPECT_EQ(CommEventKind::Stop, event.payload.event_kind);
	EXPECT_EQ(CommStopReason::Replaced, event.payload.stop_reason);
	EXPECT_EQ(first_playback, event.payload.playback_id);
	ASSERT_TRUE(producer.pop_event(event));
	EXPECT_EQ(CommEventKind::Start, event.payload.event_kind);
	EXPECT_GT(event.payload.playback_id, first_playback);
}

TEST_F(CommunicationViewProducerTest, UnknownAssetNeverCreatesAnActiveState)
{
	auto bundle = make_bundle();
	CommunicationViewProducer producer;
	producer.configure(&bundle);
	ASSERT_TRUE(telemetry::communication_view_bridge().start(make_sample("missing.ani")));
	producer.tick(100U, 1U, false, 1.0F);
	EXPECT_FALSE(producer.active());
	EXPECT_EQ(1U, producer.unknown_asset_count());
	PendingCommunicationEvent event;
	EXPECT_FALSE(producer.pop_event(event));
}

TEST_F(CommunicationViewProducerTest, MissionChangeStopsAndPurgesOldSamples)
{
	auto bundle = make_bundle();
	CommunicationViewProducer producer;
	producer.configure(&bundle);
	ASSERT_TRUE(telemetry::communication_view_bridge().start(make_sample()));
	producer.tick(100U, 1U, false, 1.0F);
	PendingCommunicationEvent event;
	ASSERT_TRUE(producer.pop_event(event));

	producer.mission_changed(200U);
	ASSERT_TRUE(producer.pop_event(event));
	EXPECT_EQ(CommStopReason::MissionChanged, event.payload.stop_reason);
	EXPECT_FALSE(producer.active());
	producer.tick(300U, 2U, false, 1.0F);
	EXPECT_FALSE(producer.pop_event(event));
}

} // namespace
