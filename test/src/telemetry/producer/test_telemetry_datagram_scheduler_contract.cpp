#include "telemetry/datagram_scheduler.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace detail = telemetry::detail;

class ScriptedDatagramIo final : public detail::DatagramIoWork {
  public:
	bool ingress_ready() const noexcept override
	{
		return ingress_is_ready;
	}

	bool egress_ready() const noexcept override
	{
		return egress_is_ready;
	}

	detail::IoStatus try_receive() noexcept override
	{
		calls.push_back(detail::DatagramDirection::Ingress);
		if (clear_ingress_after_receive) {
			ingress_is_ready = false;
		}
		if (enable_egress_after_receive) {
			egress_is_ready = true;
		}
		if (next_receive < receive_results.size()) {
			return receive_results[next_receive++];
		}
		return detail::IoStatus::Complete;
	}

	detail::IoStatus try_send() noexcept override
	{
		calls.push_back(detail::DatagramDirection::Egress);
		if (clear_egress_after_send) {
			egress_is_ready = false;
		}
		if (next_send < send_results.size()) {
			return send_results[next_send++];
		}
		return detail::IoStatus::Complete;
	}

	bool ingress_is_ready = true;
	bool egress_is_ready = true;
	bool clear_ingress_after_receive = false;
	bool enable_egress_after_receive = false;
	bool clear_egress_after_send = false;
	std::vector<detail::IoStatus> receive_results;
	std::vector<detail::IoStatus> send_results;
	std::vector<detail::DatagramDirection> calls;

  private:
	std::size_t next_receive = 0U;
	std::size_t next_send = 0U;
};

static_assert(static_cast<unsigned int>(detail::DatagramPriority::SafetyControl) == 0U);
static_assert(static_cast<unsigned int>(detail::DatagramPriority::ReliableControl) == 1U);
static_assert(static_cast<unsigned int>(detail::DatagramPriority::CommittedState) == 2U);
static_assert(static_cast<unsigned int>(detail::DatagramPriority::Keyframe) == 3U);
static_assert(static_cast<unsigned int>(detail::DatagramPriority::Heartbeat) == 4U);
static_assert(static_cast<unsigned int>(detail::DatagramPriority::ReplaceableDelta) == 5U);
static_assert(static_cast<unsigned int>(detail::DatagramPriority::Count) == 6U,
	"WP04 exposes only the six Phase 1 priorities; video QoS remains a Phase 7 concern.");
static_assert(std::is_polymorphic_v<detail::DatagramIoWork>);
static_assert(std::is_trivially_destructible_v<detail::DatagramPrioritySelector>,
	"The WP04 selector owns cursors only, never candidate or datagram storage.");
static_assert(sizeof(detail::DatagramPrioritySelector) <= 128U,
	"A pure bounded selector must not embed a datagram queue or 1200-byte buffers.");
static_assert(noexcept(std::declval<detail::DatagramTickScheduler&>().run_tick(
	std::declval<std::uint16_t>(), std::declval<detail::DatagramIoWork&>())));
static_assert(noexcept(std::declval<detail::DatagramPrioritySelector&>().select(
	std::declval<const detail::DatagramCandidate*>(), std::declval<std::size_t>())));

TEST(TelemetryDatagramTickSchedulerContract, RejectsInvalidBudgetsWithoutAttemptingIo)
{
	detail::DatagramTickScheduler scheduler;
	ScriptedDatagramIo io;

	const auto zero = scheduler.run_tick(0U, io);
	EXPECT_FALSE(zero.valid_budget);
	EXPECT_EQ(0U, zero.attempts);
	EXPECT_TRUE(io.calls.empty());

	const auto excessive = scheduler.run_tick(257U, io);
	EXPECT_FALSE(excessive.valid_budget);
	EXPECT_EQ(0U, excessive.attempts);
	EXPECT_TRUE(io.calls.empty());
}

TEST(TelemetryDatagramTickSchedulerContract, AlternatesIngressAndEgressWithinTheSingleSharedBudget)
{
	detail::DatagramTickScheduler scheduler;
	ScriptedDatagramIo io;

	const auto result = scheduler.run_tick(5U, io);
	EXPECT_TRUE(result.valid_budget);
	EXPECT_EQ(5U, result.attempts);
	EXPECT_EQ(3U, result.ingress_attempts);
	EXPECT_EQ(2U, result.egress_attempts);
	EXPECT_EQ(detail::IoStatus::Complete, result.terminal_status);
	EXPECT_EQ((std::vector<detail::DatagramDirection>{
			  detail::DatagramDirection::Ingress,
			  detail::DatagramDirection::Egress,
			  detail::DatagramDirection::Ingress,
			  detail::DatagramDirection::Egress,
			  detail::DatagramDirection::Ingress}),
		io.calls);
}

TEST(TelemetryDatagramTickSchedulerContract, PersistentFirstDirectionCursorPreventsStarvationWhenBudgetIsOne)
{
	detail::DatagramTickScheduler scheduler;
	ScriptedDatagramIo io;

	for (std::size_t tick = 0U; tick < 6U; ++tick) {
		const auto result = scheduler.run_tick(1U, io);
		ASSERT_TRUE(result.valid_budget);
		ASSERT_EQ(1U, result.attempts);
	}

	EXPECT_EQ((std::vector<detail::DatagramDirection>{
			  detail::DatagramDirection::Ingress,
			  detail::DatagramDirection::Egress,
			  detail::DatagramDirection::Ingress,
			  detail::DatagramDirection::Egress,
			  detail::DatagramDirection::Ingress,
			  detail::DatagramDirection::Egress}),
		io.calls);

	scheduler.reset();
	io.calls.clear();
	scheduler.run_tick(1U, io);
	EXPECT_EQ((std::vector<detail::DatagramDirection>{detail::DatagramDirection::Ingress}), io.calls);
}

TEST(TelemetryDatagramTickSchedulerContract, WouldBlockConsumesOneAttemptAndStopsOnlyThatDirectionForTheTick)
{
	detail::DatagramTickScheduler scheduler;
	ScriptedDatagramIo io;
	io.receive_results = {detail::IoStatus::WouldBlock};

	const auto result = scheduler.run_tick(4U, io);
	EXPECT_TRUE(result.valid_budget);
	EXPECT_EQ(4U, result.attempts);
	EXPECT_EQ(1U, result.ingress_attempts);
	EXPECT_EQ(3U, result.egress_attempts);
	EXPECT_EQ((std::vector<detail::DatagramDirection>{
			  detail::DatagramDirection::Ingress,
			  detail::DatagramDirection::Egress,
			  detail::DatagramDirection::Egress,
			  detail::DatagramDirection::Egress}),
		io.calls);
}

TEST(TelemetryDatagramTickSchedulerContract, CompleteRelatchesReadinessBeforeAttemptingTheSameDirectionAgain)
{
	detail::DatagramTickScheduler scheduler;
	ScriptedDatagramIo io;
	io.egress_is_ready = false;
	io.clear_ingress_after_receive = true;

	const auto result = scheduler.run_tick(64U, io);
	EXPECT_TRUE(result.valid_budget);
	EXPECT_EQ(1U, result.attempts);
	EXPECT_EQ(1U, result.ingress_attempts);
	EXPECT_EQ(0U, result.egress_attempts);
	EXPECT_EQ((std::vector<detail::DatagramDirection>{detail::DatagramDirection::Ingress}), io.calls)
		<< "A successful dequeue can empty a direction; cached readiness must not cause another syscall.";
}

TEST(TelemetryDatagramTickSchedulerContract, CompleteIngressCanMakeEgressReadyForTheRemainingSharedBudget)
{
	detail::DatagramTickScheduler scheduler;
	ScriptedDatagramIo io;
	io.egress_is_ready = false;
	io.clear_ingress_after_receive = true;
	io.enable_egress_after_receive = true;
	io.clear_egress_after_send = true;

	const auto result = scheduler.run_tick(64U, io);
	EXPECT_TRUE(result.valid_budget);
	EXPECT_EQ(2U, result.attempts);
	EXPECT_EQ(1U, result.ingress_attempts);
	EXPECT_EQ(1U, result.egress_attempts);
	EXPECT_EQ((std::vector<detail::DatagramDirection>{
			  detail::DatagramDirection::Ingress, detail::DatagramDirection::Egress}),
		io.calls)
		<< "Ingress may enqueue a response; dynamic egress readiness must consume the remaining tick budget.";
}

TEST(TelemetryDatagramTickSchedulerContract, TwoWouldBlocksCannotSpinAgainstADeepBacklog)
{
	detail::DatagramTickScheduler scheduler;
	ScriptedDatagramIo io;
	io.receive_results = {detail::IoStatus::WouldBlock};
	io.send_results = {detail::IoStatus::WouldBlock};

	const auto result = scheduler.run_tick(256U, io);
	EXPECT_TRUE(result.valid_budget);
	EXPECT_EQ(2U, result.attempts);
	EXPECT_EQ(1U, result.ingress_attempts);
	EXPECT_EQ(1U, result.egress_attempts);
	EXPECT_EQ((std::vector<detail::DatagramDirection>{
			  detail::DatagramDirection::Ingress, detail::DatagramDirection::Egress}),
		io.calls);
}

TEST(TelemetryDatagramTickSchedulerContract, PermanentTransportStatusStopsTheTickImmediately)
{
	for (const auto failure : {detail::IoStatus::Closed, detail::IoStatus::Error}) {
		SCOPED_TRACE(static_cast<unsigned int>(failure));
		detail::DatagramTickScheduler scheduler;
		ScriptedDatagramIo io;
		io.receive_results = {failure};

		const auto result = scheduler.run_tick(64U, io);
		EXPECT_EQ(1U, result.attempts);
		EXPECT_EQ(failure, result.terminal_status);
		EXPECT_EQ((std::vector<detail::DatagramDirection>{detail::DatagramDirection::Ingress}), io.calls);
	}
}

TEST(TelemetryDatagramPrioritySelectorContract, SelectsTheSixNormativePrioritiesWithoutOwningCandidates)
{
	detail::DatagramPrioritySelector selector;
	std::array<detail::DatagramCandidate, 6> candidates{{
		{detail::DatagramPriority::ReplaceableDelta, 0U, true},
		{detail::DatagramPriority::Heartbeat, 0U, true},
		{detail::DatagramPriority::Keyframe, 0U, true},
		{detail::DatagramPriority::CommittedState, 0U, true},
		{detail::DatagramPriority::ReliableControl, 0U, true},
		{detail::DatagramPriority::SafetyControl, 0U, true},
	}};

	const std::array<std::size_t, 6> expected_indices{{5U, 4U, 3U, 2U, 1U, 0U}};
	for (const auto expected : expected_indices) {
		const auto selected = selector.select(candidates.data(), candidates.size());
		ASSERT_EQ(detail::DatagramSelectionStatus::Selected, selected.status);
		EXPECT_EQ(expected, selected.index);
		candidates[selected.index].ready = false;
	}
	EXPECT_EQ(detail::DatagramSelectionStatus::NoneReady,
		selector.select(candidates.data(), candidates.size()).status);
}

TEST(TelemetryDatagramPrioritySelectorContract, LowerPriorityStateCannotDisplaceReadyControl)
{
	detail::DatagramPrioritySelector selector;
	std::array<detail::DatagramCandidate, 3> candidates{{
		{detail::DatagramPriority::ReplaceableDelta, 0U, true},
		{detail::DatagramPriority::CommittedState, 1U, true},
		{detail::DatagramPriority::ReliableControl, 2U, true},
	}};

	auto selected = selector.select(candidates.data(), candidates.size());
	ASSERT_EQ(detail::DatagramSelectionStatus::Selected, selected.status);
	EXPECT_EQ(2U, selected.index);

	candidates[2].ready = false;
	selected = selector.select(candidates.data(), candidates.size());
	ASSERT_EQ(detail::DatagramSelectionStatus::Selected, selected.status);
	EXPECT_EQ(1U, selected.index);
}

TEST(TelemetryDatagramPrioritySelectorContract, RoundRobinPreventsSamePriorityOwnerStarvation)
{
	detail::DatagramPrioritySelector selector;
	std::array<detail::DatagramCandidate, 4> candidates{{
		{detail::DatagramPriority::ReliableControl, 0U, true},
		{detail::DatagramPriority::ReliableControl, 0U, true},
		{detail::DatagramPriority::ReliableControl, 1U, true},
		{detail::DatagramPriority::ReliableControl, 2U, true},
	}};

	const std::array<std::size_t, 4> expected_indices{{0U, 2U, 3U, 1U}};
	for (const auto expected : expected_indices) {
		const auto selected = selector.select(candidates.data(), candidates.size());
		ASSERT_EQ(detail::DatagramSelectionStatus::Selected, selected.status);
		EXPECT_EQ(expected, selected.index);
		candidates[selected.index].ready = false;
	}
}

TEST(TelemetryDatagramPrioritySelectorContract, SelectionAdvancesRoundRobinEvenUntilExternalOwnerConsumesWork)
{
	detail::DatagramPrioritySelector selector;
	const std::array<detail::DatagramCandidate, 3> candidates{{
		{detail::DatagramPriority::CommittedState, 0U, true},
		{detail::DatagramPriority::CommittedState, 1U, true},
		{detail::DatagramPriority::CommittedState, 2U, true},
	}};

	const std::array<std::size_t, 6> expected_indices{{0U, 1U, 2U, 0U, 1U, 2U}};
	for (const auto expected : expected_indices) {
		const auto selected = selector.select(candidates.data(), candidates.size());
		ASSERT_EQ(detail::DatagramSelectionStatus::Selected, selected.status);
		EXPECT_EQ(expected, selected.index);
	}

	selector.reset();
	EXPECT_EQ(0U, selector.select(candidates.data(), candidates.size()).index);
}

TEST(TelemetryDatagramPrioritySelectorContract, ReadsCurrentExternalReadinessAndNeverCopiesCandidateState)
{
	detail::DatagramPrioritySelector selector;
	std::array<detail::DatagramCandidate, 2> candidates{{
		{detail::DatagramPriority::SafetyControl, 0U, true},
		{detail::DatagramPriority::ReplaceableDelta, 1U, true},
	}};

	EXPECT_EQ(0U, selector.select(candidates.data(), candidates.size()).index);
	candidates[0].ready = false;
	EXPECT_EQ(1U, selector.select(candidates.data(), candidates.size()).index)
		<< "The selector must reread caller-owned candidates instead of retaining a queue or snapshot.";
	candidates[1].ready = false;
	EXPECT_EQ(detail::DatagramSelectionStatus::NoneReady,
		selector.select(candidates.data(), candidates.size()).status);
}

TEST(TelemetryDatagramPrioritySelectorContract, RejectsInvalidOrUnboundedCandidateViewsFailClosed)
{
	detail::DatagramPrioritySelector selector;
	EXPECT_EQ(detail::DatagramSelectionStatus::NoneReady, selector.select(nullptr, 0U).status);
	EXPECT_EQ(detail::DatagramSelectionStatus::InvalidInput, selector.select(nullptr, 1U).status);

	std::array<detail::DatagramCandidate, 1> invalid{{
		{detail::DatagramPriority::Count, 0U, true},
	}};
	EXPECT_EQ(detail::DatagramSelectionStatus::InvalidInput,
		selector.select(invalid.data(), invalid.size()).status);

	std::array<detail::DatagramCandidate, detail::MaximumDatagramCandidates + 1U> excessive{};
	EXPECT_EQ(detail::DatagramSelectionStatus::TooManyCandidates,
		selector.select(excessive.data(), excessive.size()).status);
}

} // namespace
