#if __has_include("telemetry/capture_scheduler.h")
#include "telemetry/capture_scheduler.h"
#define FSO_HAS_TELEMETRY_CAPTURE_SCHEDULER 1
#else
#define FSO_HAS_TELEMETRY_CAPTURE_SCHEDULER 0
#endif

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>

namespace {

constexpr const char* MissingScheduler =
	"WP07-D1 requires telemetry/capture_scheduler.h with the tracker-frozen local cadence contract.";

#if !FSO_HAS_TELEMETRY_CAPTURE_SCHEDULER

#define WP07_D1_MISSING_TEST(name) \
	TEST(TelemetryCaptureSchedulerContract, name) \
	{ \
		FAIL() << MissingScheduler; \
	}

WP07_D1_MISSING_TEST(StatusDefaultsAndExactNoexceptApiExist)
WP07_D1_MISSING_TEST(ConfigureAcceptsOnlyOneToSixtyAndUsesCeilingPeriods)
WP07_D1_MISSING_TEST(FirstActivePollIsImmediateAndExactBoundaryIsSingleDue)
WP07_D1_MISSING_TEST(HitchSkipsMissedPeriodsAndAdvancesOnlyFromNow)
WP07_D1_MISSING_TEST(InactiveStopAndResetHaveDistinctReactivationSemantics)
WP07_D1_MISSING_TEST(EveryPollDetectsClockRegressionBeforeScheduling)
WP07_D1_MISSING_TEST(DeadlineOverflowFailsClosedWithoutDueCapture)

#undef WP07_D1_MISSING_TEST

#else

namespace detail = telemetry::detail;

using detail::Capture30Hz;
using detail::CaptureCadenceResult;
using detail::CaptureCadenceStatus;

static_assert(std::is_same_v<std::underlying_type_t<CaptureCadenceStatus>, std::uint8_t>);
static_assert(static_cast<std::uint8_t>(CaptureCadenceStatus::Inactive) == 0U);
static_assert(static_cast<std::uint8_t>(CaptureCadenceStatus::NotDue) == 1U);
static_assert(static_cast<std::uint8_t>(CaptureCadenceStatus::Due) == 2U);
static_assert(static_cast<std::uint8_t>(CaptureCadenceStatus::InvalidRate) == 3U);
static_assert(static_cast<std::uint8_t>(CaptureCadenceStatus::ClockRegression) == 4U);
static_assert(static_cast<std::uint8_t>(CaptureCadenceStatus::DeadlineOverflow) == 5U);
static_assert(static_cast<std::uint8_t>(CaptureCadenceStatus::Count) == 6U);
static_assert(std::is_same_v<decltype(CaptureCadenceResult::status), CaptureCadenceStatus>);
static_assert(std::is_same_v<decltype(CaptureCadenceResult::next_due_us), std::uint64_t>);
static_assert(std::is_final_v<Capture30Hz> && std::is_nothrow_default_constructible_v<Capture30Hz>);
using ConfigureSignature = bool (Capture30Hz::*)(std::uint32_t) noexcept;
static_assert(std::is_same_v<decltype(static_cast<ConfigureSignature>(&Capture30Hz::configure)),
	ConfigureSignature>);
static_assert(std::is_same_v<decltype(std::declval<Capture30Hz&>().poll(std::uint64_t{}, bool{})),
	CaptureCadenceResult>);
static_assert(std::is_same_v<decltype(std::declval<Capture30Hz&>().stop()), void>);
static_assert(std::is_same_v<decltype(std::declval<Capture30Hz&>().reset()), void>);
static_assert(std::is_same_v<decltype(std::declval<const Capture30Hz&>().period_us()), std::uint64_t>);
static_assert(std::is_same_v<decltype(std::declval<const Capture30Hz&>().armed()), bool>);
static_assert(std::is_same_v<decltype(std::declval<const Capture30Hz&>().next_due_us()), std::uint64_t>);
static_assert(noexcept(std::declval<Capture30Hz&>().configure(std::uint32_t{})));
static_assert(noexcept(std::declval<Capture30Hz&>().poll(std::uint64_t{}, bool{})));
static_assert(noexcept(std::declval<Capture30Hz&>().stop()));
static_assert(noexcept(std::declval<Capture30Hz&>().reset()));
static_assert(noexcept(std::declval<const Capture30Hz&>().period_us()));
static_assert(noexcept(std::declval<const Capture30Hz&>().armed()));
static_assert(noexcept(std::declval<const Capture30Hz&>().next_due_us()));

void expect_result(const CaptureCadenceResult& result,
	CaptureCadenceStatus status,
	std::uint64_t next_due_us)
{
	EXPECT_EQ(status, result.status);
	EXPECT_EQ(next_due_us, result.next_due_us);
}

TEST(TelemetryCaptureSchedulerContract, StatusDefaultsAndExactNoexceptApiExist)
{
	const CaptureCadenceResult defaults{};
	expect_result(defaults, CaptureCadenceStatus::InvalidRate, 0U);
	const Capture30Hz scheduler;
	EXPECT_EQ(0U, scheduler.period_us());
	EXPECT_FALSE(scheduler.armed());
	EXPECT_EQ(0U, scheduler.next_due_us());
}

TEST(TelemetryCaptureSchedulerContract, ConfigureAcceptsOnlyOneToSixtyAndUsesCeilingPeriods)
{
	Capture30Hz scheduler;
	for (std::uint32_t rate = 1U; rate <= 60U; ++rate) {
		const auto quotient = std::uint64_t{1'000'000U} / rate;
		const auto remainder = std::uint64_t{1'000'000U} % rate;
		const auto independent_ceiling = quotient + (remainder == 0U ? 0U : 1U);
		ASSERT_TRUE(scheduler.configure(rate)) << "rate=" << rate;
		EXPECT_EQ(independent_ceiling, scheduler.period_us()) << "rate=" << rate;
		EXPECT_FALSE(scheduler.armed());
		EXPECT_EQ(0U, scheduler.next_due_us());
	}
	ASSERT_TRUE(scheduler.configure(std::uint32_t{1U}));
	EXPECT_EQ(1'000'000U, scheduler.period_us());
	ASSERT_TRUE(scheduler.configure(std::uint32_t{30U}));
	EXPECT_EQ(33'334U, scheduler.period_us());
	ASSERT_TRUE(scheduler.configure(std::uint32_t{60U}));
	EXPECT_EQ(16'667U, scheduler.period_us());

	ASSERT_TRUE(scheduler.configure(std::uint32_t{30U}));
	expect_result(scheduler.poll(100U, true), CaptureCadenceStatus::Due, 33'434U);
	EXPECT_FALSE(scheduler.configure(std::uint32_t{0U}));
	EXPECT_EQ(0U, scheduler.period_us());
	EXPECT_FALSE(scheduler.armed());
	EXPECT_EQ(0U, scheduler.next_due_us());
	expect_result(scheduler.poll(101U, true), CaptureCadenceStatus::InvalidRate, 0U);

	ASSERT_TRUE(scheduler.configure(std::uint32_t{30U}));
	expect_result(scheduler.poll(1'000U, true), CaptureCadenceStatus::Due, 34'334U);
	EXPECT_FALSE(scheduler.configure(std::uint32_t{61U}));
	EXPECT_EQ(0U, scheduler.period_us());
	EXPECT_FALSE(scheduler.armed());
	EXPECT_EQ(0U, scheduler.next_due_us());
	expect_result(scheduler.poll(1'001U, false), CaptureCadenceStatus::InvalidRate, 0U);
	ASSERT_TRUE(scheduler.configure(std::uint32_t{30U}));
	expect_result(scheduler.poll(1U, true), CaptureCadenceStatus::Due, 33'335U);
}

TEST(TelemetryCaptureSchedulerContract, FirstActivePollIsImmediateAndExactBoundaryIsSingleDue)
{
	Capture30Hz scheduler;
	ASSERT_TRUE(scheduler.configure(std::uint32_t{30U}));
	expect_result(scheduler.poll(100U, true), CaptureCadenceStatus::Due, 33'434U);
	EXPECT_TRUE(scheduler.armed());
	EXPECT_EQ(33'434U, scheduler.next_due_us());
	expect_result(scheduler.poll(33'433U, true), CaptureCadenceStatus::NotDue, 33'434U);
	expect_result(scheduler.poll(33'434U, true), CaptureCadenceStatus::Due, 66'768U);
	EXPECT_EQ(66'768U, scheduler.next_due_us());
}

TEST(TelemetryCaptureSchedulerContract, HitchSkipsMissedPeriodsAndAdvancesOnlyFromNow)
{
	Capture30Hz scheduler;
	ASSERT_TRUE(scheduler.configure(std::uint32_t{30U}));
	const auto period = scheduler.period_us();
	expect_result(scheduler.poll(0U, true), CaptureCadenceStatus::Due, period);
	const auto hitch_now = period * 10U + 17U;
	expect_result(scheduler.poll(hitch_now, true), CaptureCadenceStatus::Due, hitch_now + period);
	expect_result(scheduler.poll(hitch_now, true), CaptureCadenceStatus::NotDue, hitch_now + period);
	EXPECT_EQ(hitch_now + period, scheduler.next_due_us());
}

TEST(TelemetryCaptureSchedulerContract, InactiveStopAndResetHaveDistinctReactivationSemantics)
{
	Capture30Hz scheduler;
	ASSERT_TRUE(scheduler.configure(std::uint32_t{30U}));
	expect_result(scheduler.poll(100U, true), CaptureCadenceStatus::Due, 33'434U);
	expect_result(scheduler.poll(200U, false), CaptureCadenceStatus::Inactive, 0U);
	EXPECT_FALSE(scheduler.armed());
	EXPECT_EQ(0U, scheduler.next_due_us());
	expect_result(scheduler.poll(300U, true), CaptureCadenceStatus::Due, 33'634U);
	expect_result(scheduler.poll(400U, false), CaptureCadenceStatus::Inactive, 0U);
	expect_result(scheduler.poll(399U, true), CaptureCadenceStatus::ClockRegression, 0U);
	EXPECT_FALSE(scheduler.armed());

	scheduler.stop();
	EXPECT_EQ(33'334U, scheduler.period_us());
	EXPECT_FALSE(scheduler.armed());
	EXPECT_EQ(0U, scheduler.next_due_us());
	expect_result(scheduler.poll(10U, true), CaptureCadenceStatus::Due, 33'344U);

	scheduler.reset();
	EXPECT_EQ(0U, scheduler.period_us());
	EXPECT_FALSE(scheduler.armed());
	EXPECT_EQ(0U, scheduler.next_due_us());
	expect_result(scheduler.poll(0U, true), CaptureCadenceStatus::InvalidRate, 0U);
}

TEST(TelemetryCaptureSchedulerContract, EveryPollDetectsClockRegressionBeforeScheduling)
{
	Capture30Hz active;
	ASSERT_TRUE(active.configure(std::uint32_t{30U}));
	expect_result(active.poll(500U, true), CaptureCadenceStatus::Due, 33'834U);
	expect_result(active.poll(499U, true), CaptureCadenceStatus::ClockRegression, 0U);
	EXPECT_FALSE(active.armed());
	EXPECT_EQ(0U, active.next_due_us());

	Capture30Hz inactive;
	ASSERT_TRUE(inactive.configure(std::uint32_t{30U}));
	expect_result(inactive.poll(500U, false), CaptureCadenceStatus::Inactive, 0U);
	expect_result(inactive.poll(499U, false), CaptureCadenceStatus::ClockRegression, 0U);
	EXPECT_FALSE(inactive.armed());
}

TEST(TelemetryCaptureSchedulerContract, DeadlineOverflowFailsClosedWithoutDueCapture)
{
	const auto maximum = std::numeric_limits<std::uint64_t>::max();
	for (const auto rate : {std::uint32_t{1U}, std::uint32_t{30U}, std::uint32_t{60U}}) {
		Capture30Hz exact;
		ASSERT_TRUE(exact.configure(rate));
		const auto period = exact.period_us();
		expect_result(exact.poll(maximum - period, true), CaptureCadenceStatus::Due, maximum);
		expect_result(exact.poll(maximum - period, true), CaptureCadenceStatus::NotDue, maximum);

		Capture30Hz overflow;
		ASSERT_TRUE(overflow.configure(rate));
		expect_result(overflow.poll(maximum - period + 1U, true),
			CaptureCadenceStatus::DeadlineOverflow,
			0U);
		EXPECT_FALSE(overflow.armed());
		EXPECT_EQ(0U, overflow.next_due_us());
	}

	Capture30Hz hitch_overflow;
	ASSERT_TRUE(hitch_overflow.configure(std::uint32_t{30U}));
	expect_result(hitch_overflow.poll(0U, true), CaptureCadenceStatus::Due, 33'334U);
	expect_result(hitch_overflow.poll(maximum, true), CaptureCadenceStatus::DeadlineOverflow, 0U);
	EXPECT_FALSE(hitch_overflow.armed());
}

#endif

} // namespace
