/*
 * Copyright 2016-2023 ClickHouse, Inc.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


/*
 * This file may have been modified by Bytedance Ltd. and/or its affiliates (“ Bytedance's Modifications”).
 * All Bytedance's Modifications are Copyright (2023) Bytedance Ltd. and/or its affiliates.
 */

#pragma once

#include <common/types.h>
#include <Core/DecimalFunctions.h>
#include <Common/Exception.h>
#include <common/DateLUTImpl.h>
#include <common/DateLUT.h>
#include <Columns/ColumnNullable.h>
#include <Columns/ColumnString.h>
#include <Columns/ColumnVector.h>
#include <Columns/ColumnDecimal.h>
#include <Functions/FunctionHelpers.h>
#include <Functions/IFunction.h>
#include <Functions/FunctionFactory.h>
#include <Functions/extractTimeZoneFromFunctionArguments.h>
#include <DataTypes/DataTypeString.h>
#include <DataTypes/DataTypeDateTime.h>
#include <DataTypes/DataTypeDateTime64.h>
#include <DataTypes/DataTypeNullable.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int ILLEGAL_COLUMN;
}

/** Transformations.
  * Represents two functions - from datetime (UInt32) and from date (UInt16).
  *
  * Also, the "factor transformation" F is defined for the T transformation.
  * This is a transformation of F such that its value identifies the region of monotonicity for T
  *  (for a fixed value of F, the transformation T is monotonic).
  *
  * Or, figuratively, if T is similar to taking the remainder of division, then F is similar to division.
  *
  * Example: for transformation T "get the day number in the month" (2015-02-03 -> 3),
  *  factor-transformation F is "round to the nearest month" (2015-02-03 -> 2015-02-01).
  */

    [[noreturn]] void throwDateIsNotSupported(const char * name);
    [[noreturn]] void throwDateTimeIsNotSupported(const char * name);
    [[noreturn]] void throwDate32IsNotSupported(const char * name);

/// This factor transformation will say that the function is monotone everywhere.
struct ZeroTransform
{
    static inline UInt16 execute(Int64, const DateLUTImpl &) { return 0; }
    static inline UInt16 execute(UInt32, const DateLUTImpl &) { return 0; }
    static inline UInt16 execute(Int32, const DateLUTImpl &) { return 0; }
    static inline UInt16 execute(UInt16, const DateLUTImpl &) { return 0; }
};

struct ToDateImpl
{
    static constexpr auto name = "toDate";

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return t < 0 ? 0 : std::min(Int32(time_zone.toDayNum(t)), Int32(DATE_LUT_MAX_DAY_NUM));
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toDayNum(t);
    }
    static inline UInt16 execute(Int32 t, const DateLUTImpl &)
    {
        return t < 0 ? 0 : std::min(t, Int32(DATE_LUT_MAX_DAY_NUM));
    }
    static inline UInt16 execute(UInt16 d, const DateLUTImpl &)
    {
        return d;
    }

    using FactorTransform = ZeroTransform;
};

struct ToDate32Impl
{
    static constexpr auto name = "toDate32";

    static inline Int32 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return Int32(time_zone.toDayNum(t));
    }
    static inline Int32 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        /// Don't saturate.
        return Int32(time_zone.toDayNum<Int64>(t));
    }
    static inline Int32 execute(Int32 d, const DateLUTImpl &)
    {
        return d;
    }
    static inline Int32 execute(UInt16 d, const DateLUTImpl &)
    {
        return d;
    }

    using FactorTransform = ZeroTransform;
};

struct ToStartOfDayImpl
{
    static constexpr auto name = "toStartOfDay";

    //TODO: right now it is hardcoded to produce DateTime only, needs fixing later. See date_and_time_type_details::ResultDataTypeMap for deduction of result type example.
    static inline UInt32 execute(const DecimalUtils::DecimalComponents<DateTime64> & t, const DateLUTImpl & time_zone)
    {
        if (t.whole < 0 || (t.whole >= 0 && t.fractional < 0))
            return 0;

        return time_zone.toDate(std::min<Int64>(t.whole, Int64(0xffffffff)));
    }
    static inline UInt32 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toDate(t);
    }
    static inline UInt32 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        if (d < 0)
            return 0;

        auto date_time = time_zone.fromDayNum(ExtendedDayNum(d));
        if (date_time <= 0xffffffff)
            return date_time;
        else
            return time_zone.toDate(0xffffffff);
    }
    static inline UInt32 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        auto date_time = time_zone.fromDayNum(ExtendedDayNum(d));
        return date_time < 0xffffffff ? date_time : time_zone.toDate(0xffffffff);
    }
    static inline DecimalUtils::DecimalComponents<DateTime64> executeExtendedResult(const DecimalUtils::DecimalComponents<DateTime64> & t, const DateLUTImpl & time_zone)
    {
        return {time_zone.toDate(t.whole), 0};
    }
    static inline Int64 executeExtendedResult(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.fromDayNum(ExtendedDayNum(d)) * DecimalUtils::scaleMultiplier<DateTime64>(DataTypeDateTime64::default_scale);
    }

    using FactorTransform = ZeroTransform;
};

struct ToMondayImpl
{
    static constexpr auto name = "toMonday";

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return t < 0 ? 0 : time_zone.toFirstDayNumOfWeek(ExtendedDayNum(
                std::min(Int32(time_zone.toDayNum(t)), Int32(DATE_LUT_MAX_DAY_NUM))));
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfWeek(t);
    }
    static inline UInt16 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return d < 0 ? 0 : time_zone.toFirstDayNumOfWeek(ExtendedDayNum(std::min(d, Int32(DATE_LUT_MAX_DAY_NUM))));
    }
    static inline UInt16 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfWeek(DayNum(d));
    }
    static inline Int64 executeExtendedResult(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfWeek(time_zone.toDayNum(t));
    }
    static inline Int32 executeExtendedResult(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfWeek(ExtendedDayNum(d));
    }

    using FactorTransform = ZeroTransform;
};

struct ToStartOfMonthImpl
{
    static constexpr auto name = "toStartOfMonth";

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return t < 0 ? 0 : time_zone.toFirstDayNumOfMonth(ExtendedDayNum(std::min(Int32(time_zone.toDayNum(t)), Int32(DATE_LUT_MAX_DAY_NUM))));
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfMonth(ExtendedDayNum(time_zone.toDayNum(t)));
    }
    static inline UInt16 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return d < 0 ? 0 : time_zone.toFirstDayNumOfMonth(ExtendedDayNum(std::min(d, Int32(DATE_LUT_MAX_DAY_NUM))));
    }
    static inline UInt16 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfMonth(DayNum(d));
    }
    static inline Int64 executeExtendedResult(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfMonth(time_zone.toDayNum(t));
    }
    static inline Int32 executeExtendedResult(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfMonth(ExtendedDayNum(d));
    }

    using FactorTransform = ZeroTransform;
};

struct ToStartOfBiMonthImpl
{
    static constexpr auto name = "toStartOfBiMonth";

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return t < 0 ? 0 : time_zone.toFirstDayNumOfBiMonth(ExtendedDayNum(std::min(Int32(time_zone.toDayNum(t)), Int32(DATE_LUT_MAX_DAY_NUM))));
    }

    static inline UInt16 execute(UInt32 t, const DateLUTImpl & timezone)
    {
        return timezone.toFirstDayNumOfBiMonth(ExtendedDayNum(timezone.toDayNum(t)));
    }
    static inline UInt16 execute(Int32 d, const DateLUTImpl & timezone)
    {
        return d < 0 ? 0 : timezone.toFirstDayNumOfBiMonth(ExtendedDayNum(std::min(d, Int32(DATE_LUT_MAX_DAY_NUM))));
    }
    static inline UInt16 execute(UInt16 d, const DateLUTImpl & timezone)
    {
        return timezone.toFirstDayNumOfBiMonth(DayNum(d));
    }
    static inline Int64 executeExtendedResult(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfBiMonth(time_zone.toDayNum(t));
    }
    static inline Int32 executeExtendedResult(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfBiMonth(ExtendedDayNum(d));
    }

    using FactorTransform = ZeroTransform;
};

struct ToStartOfQuarterImpl
{
    static constexpr auto name = "toStartOfQuarter";

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return t < 0 ? 0 : time_zone.toFirstDayNumOfQuarter(ExtendedDayNum(std::min<Int64>(Int64(time_zone.toDayNum(t)), Int64(DATE_LUT_MAX_DAY_NUM))));
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfQuarter(time_zone.toDayNum(t));
    }
    static inline UInt16 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return d < 0 ? 0 : time_zone.toFirstDayNumOfQuarter(ExtendedDayNum(std::min<Int32>(d, Int32(DATE_LUT_MAX_DAY_NUM))));
    }
    static inline UInt16 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfQuarter(DayNum(d));
    }
    static inline Int64 executeExtendedResult(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfQuarter(time_zone.toDayNum(t));
    }
    static inline Int32 executeExtendedResult(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfQuarter(ExtendedDayNum(d));
    }

    using FactorTransform = ZeroTransform;
};

struct ToStartOfYearImpl
{
    static constexpr auto name = "toStartOfYear";

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return t < 0 ? 0 : time_zone.toFirstDayNumOfYear(ExtendedDayNum(std::min(Int32(time_zone.toDayNum(t)), Int32(DATE_LUT_MAX_DAY_NUM))));
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfYear(time_zone.toDayNum(t));
    }
    static inline UInt16 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return d < 0 ? 0 : time_zone.toFirstDayNumOfYear(ExtendedDayNum(std::min(d, Int32(DATE_LUT_MAX_DAY_NUM))));
    }
    static inline UInt16 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfYear(DayNum(d));
    }
    static inline Int64 executeExtendedResult(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfYear(time_zone.toDayNum(t));
    }
    static inline Int32 executeExtendedResult(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfYear(ExtendedDayNum(d));
    }

    using FactorTransform = ZeroTransform;
};


struct ToTimeImpl
{
    /// When transforming to time, the date will be equated to 1970-01-02.
    static constexpr auto name = "toTime";

    static UInt32 execute(const DecimalUtils::DecimalComponents<DateTime64> & t, const DateLUTImpl & time_zone)
    {
        return time_zone.toTime(t.whole) + 86400;
    }
    static inline UInt32 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toTime(t) + 86400;
    }
    static inline UInt32 execute(Int32, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }
    static inline UInt32 execute(UInt16, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }

    using FactorTransform = ToDateImpl;
};

struct ToStartOfMinuteImpl
{
    static constexpr auto name = "toStartOfMinute";

    static inline UInt32 execute(const DecimalUtils::DecimalComponents<DateTime64> & t, const DateLUTImpl & time_zone)
    {
        if (t.whole < 0 || (t.whole >= 0 && t.fractional < 0))
            return 0;

        return time_zone.toStartOfMinute(std::min<Int64>(t.whole, Int64(0xffffffff)));
    }
    static inline UInt32 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toStartOfMinute(t);
    }
    static inline UInt32 execute(Int32, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }
    static inline UInt32 execute(UInt16, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }
    static inline DecimalUtils::DecimalComponents<DateTime64> executeExtendedResult(const DecimalUtils::DecimalComponents<DateTime64> & t, const DateLUTImpl & time_zone)
    {
        return {time_zone.toStartOfMinute(t.whole), 0};
    }
    static inline Int64 executeExtendedResult(Int32, const DateLUTImpl &)
    {
        throwDate32IsNotSupported(name);
    }

    using FactorTransform = ZeroTransform;
};

// Rounding towards negative infinity.
// 1.01 => 1.00
// -1.01 => -2
struct ToStartOfSecondImpl
{
    static constexpr auto name = "toStartOfSecond";

    static inline DateTime64 execute(const DateTime64 & datetime64, Int64 scale_multiplier, const DateLUTImpl &)
    {
        auto fractional_with_sign = DecimalUtils::getFractionalPartWithScaleMultiplier<DateTime64, true>(datetime64, scale_multiplier);

        // given that scale is 3, scale_multiplier is 1000
        // for DateTime64 value of 123.456:
        // 123456 - 456 = 123000
        // for DateTime64 value of -123.456:
        // -123456 - (1000 + (-456)) = -124000

        if (fractional_with_sign < 0)
            fractional_with_sign += scale_multiplier;

        return datetime64 - fractional_with_sign;
    }

    static inline UInt32 execute(UInt32, const DateLUTImpl &)
    {
        throw Exception("Illegal type DateTime of argument for function " + std::string(name), ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT);
    }
    static inline UInt32 execute(Int32, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }
    static inline UInt32 execute(UInt16, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }

    using FactorTransform = ZeroTransform;
};

struct ToStartOfFiveMinuteImpl
{
    static constexpr auto name = "toStartOfFiveMinute";

    static inline UInt32 execute(const DecimalUtils::DecimalComponents<DateTime64> & t, const DateLUTImpl & time_zone)
    {
        return time_zone.toStartOfFiveMinutes(t.whole);
    }
    static inline UInt32 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toStartOfFiveMinutes(t);
    }
    static inline UInt32 execute(Int32, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }
    static inline UInt32 execute(UInt16, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }
    static inline DecimalUtils::DecimalComponents<DateTime64> executeExtendedResult(const DecimalUtils::DecimalComponents<DateTime64> & t, const DateLUTImpl & time_zone)
    {
        return {time_zone.toStartOfFiveMinutes(t.whole), 0};
    }
    static inline Int64 executeExtendedResult(Int32, const DateLUTImpl &)
    {
        throwDate32IsNotSupported(name);
    }

    using FactorTransform = ZeroTransform;
};

struct ToStartOfTenMinutesImpl
{
    static constexpr auto name = "toStartOfTenMinutes";

    static inline UInt32 execute(const DecimalUtils::DecimalComponents<DateTime64> & t, const DateLUTImpl & time_zone)
    {
        return time_zone.toStartOfTenMinutes(t.whole);
    }
    static inline UInt32 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toStartOfTenMinutes(t);
    }
    static inline UInt32 execute(Int32, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }
    static inline UInt32 execute(UInt16, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }
    static inline DecimalUtils::DecimalComponents<DateTime64> executeExtendedResult(const DecimalUtils::DecimalComponents<DateTime64> & t, const DateLUTImpl & time_zone)
    {
        return {time_zone.toStartOfTenMinutes(t.whole), 0};
    }
    static inline Int64 executeExtendedResult(Int32, const DateLUTImpl &)
    {
        throwDate32IsNotSupported(name);
    }

    using FactorTransform = ZeroTransform;
};

struct ToStartOfFifteenMinutesImpl
{
    static constexpr auto name = "toStartOfFifteenMinutes";

    static inline UInt32 execute(const DecimalUtils::DecimalComponents<DateTime64> & t, const DateLUTImpl & time_zone)
    {
        return time_zone.toStartOfFifteenMinutes(t.whole);
    }
    static inline UInt32 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toStartOfFifteenMinutes(t);
    }
    static inline UInt32 execute(Int32, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }
    static inline UInt32 execute(UInt16, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }
    static inline DecimalUtils::DecimalComponents<DateTime64> executeExtendedResult(const DecimalUtils::DecimalComponents<DateTime64> & t, const DateLUTImpl & time_zone)
    {
        return {time_zone.toStartOfFifteenMinutes(t.whole), 0};
    }
    static inline Int64 executeExtendedResult(Int32, const DateLUTImpl &)
    {
        throwDate32IsNotSupported(name);
    }

    using FactorTransform = ZeroTransform;
};

/// Round to start of half-an-hour length interval with unspecified offset. This transform is specific for Yandex.Metrica.
struct TimeSlotImpl
{
    static constexpr auto name = "timeSlot";

    //static inline DecimalUtils::DecimalComponents<DateTime64> execute(const DecimalUtils::DecimalComponents<DateTime64> & t, const DateLUTImpl &)
    static inline UInt32 execute(const DecimalUtils::DecimalComponents<DateTime64> & t, const DateLUTImpl &)
    {
        return t.whole / 1800 * 1800;
    }

    static inline UInt32 execute(UInt32 t, const DateLUTImpl &)
    {
        return t / 1800 * 1800;
    }

    static inline UInt32 execute(Int32, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }

    static inline UInt32 execute(UInt16, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }

    static inline DecimalUtils::DecimalComponents<DateTime64>  executeExtendedResult(const DecimalUtils::DecimalComponents<DateTime64> & t, const DateLUTImpl &)
    {
        if (likely(t.whole >= 0))
            return {t.whole / 1800 * 1800, 0};
        return {(t.whole + 1 - 1800) / 1800 * 1800, 0};
    }

    static inline Int64 executeExtendedResult(Int32, const DateLUTImpl &)
    {
        throwDate32IsNotSupported(name);
    }

    using FactorTransform = ZeroTransform;
};

struct ToStartOfHourImpl
{
    static constexpr auto name = "toStartOfHour";

    static inline UInt32 execute(const DecimalUtils::DecimalComponents<DateTime64> & t, const DateLUTImpl & time_zone)
    {
        if (t.whole < 0 || (t.whole >= 0 && t.fractional < 0))
            return 0;

        return time_zone.toStartOfHour(std::min<Int64>(t.whole, Int64(0xffffffff)));
    }

    static inline UInt32 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toStartOfHour(t);
    }

    static inline UInt32 execute(Int32, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }

    static inline UInt32 execute(UInt16, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }

    static inline DecimalUtils::DecimalComponents<DateTime64> executeExtendedResult(const DecimalUtils::DecimalComponents<DateTime64> & t, const DateLUTImpl & time_zone)
    {
        return {time_zone.toStartOfHour(t.whole), 0};
    }

    static inline Int64 executeExtendedResult(Int32, const DateLUTImpl &)
    {
        throwDate32IsNotSupported(name);
    }

    using FactorTransform = ZeroTransform;
};

struct ToYearImpl
{
    static constexpr auto name = "toYear";

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toYear(t);
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toYear(t);
    }
    static inline UInt16 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toYear(ExtendedDayNum(d));
    }
    static inline UInt16 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toYear(DayNum(d));
    }

    using FactorTransform = ZeroTransform;
};

struct ToQuarterImpl
{
    static constexpr auto name = "toQuarter";

    static inline UInt8 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toQuarter(t);
    }
    static inline UInt8 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toQuarter(t);
    }
    static inline UInt8 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toQuarter(ExtendedDayNum(d));
    }
    static inline UInt8 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toQuarter(DayNum(d));
    }

    using FactorTransform = ToStartOfYearImpl;
};

struct ToMonthImpl
{
    static constexpr auto name = "toMonth";

    static inline UInt8 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toMonth(t);
    }
    static inline UInt8 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toMonth(t);
    }
    static inline UInt8 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toMonth(ExtendedDayNum(d));
    }
    static inline UInt8 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toMonth(DayNum(d));
    }

    using FactorTransform = ToStartOfYearImpl;
};

struct ToYearMonthImpl
{
    static constexpr auto name = "toYearMonth";

    static ToMonthImpl toMonth;
    static ToYearImpl toYear;

    static inline UInt32 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return toYear.execute(t, time_zone) * 100 + toMonth.execute(t, time_zone);
    }
    static inline UInt32 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return toYear.execute(t, time_zone) * 100 + toMonth.execute(t, time_zone);
    }
    static inline UInt32 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return toYear.execute(d, time_zone) * 100 + toMonth.execute(d, time_zone);
    }
    static inline UInt32 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return toYear.execute(d, time_zone) * 100 + toMonth.execute(d, time_zone);
    }

    using FactorTransform = ZeroTransform; // Todo: check this
};

struct ToDayOfMonthImpl
{
    static constexpr auto name = "toDayOfMonth";

    static inline UInt8 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toDayOfMonth(t);
    }
    static inline UInt8 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toDayOfMonth(t);
    }
    static inline UInt8 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toDayOfMonth(ExtendedDayNum(d));
    }
    static inline UInt8 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toDayOfMonth(DayNum(d));
    }

    using FactorTransform = ToStartOfMonthImpl;
};

struct ToDayOfWeekImpl
{
    static constexpr auto name = "toDayOfWeek";

    static inline UInt8 execute(Int64 t, UInt8 mode, const DateLUTImpl & time_zone) { return time_zone.toDayOfWeek(t, mode); }
    static inline UInt8 execute(UInt32 t, UInt8 mode, const DateLUTImpl & time_zone) { return time_zone.toDayOfWeek(t, mode); }
    static inline UInt8 execute(Int32 d, UInt8 mode, const DateLUTImpl & time_zone)
    {
        return time_zone.toDayOfWeek(ExtendedDayNum(d), mode);
    }
    static inline UInt8 execute(UInt16 d, UInt8 mode, const DateLUTImpl & time_zone) { return time_zone.toDayOfWeek(DayNum(d), mode); }

    using FactorTransform = ToMondayImpl;
};

struct ToDayOfWeekMySQLImpl
{
    static constexpr auto name = "toDayOfWeekMySQL";

    static inline UInt8 execute(Int64 t, UInt8 mode, const DateLUTImpl & time_zone) { return time_zone.toDayOfWeek(t, mode); }
    static inline UInt8 execute(UInt32 t, UInt8 mode, const DateLUTImpl & time_zone) { return time_zone.toDayOfWeek(t, mode); }
    static inline UInt8 execute(Int32 d, UInt8 mode, const DateLUTImpl & time_zone)
    {
        return time_zone.toDayOfWeek(ExtendedDayNum(d), mode);
    }
    static inline UInt8 execute(UInt16 d, UInt8 mode, const DateLUTImpl & time_zone) { return time_zone.toDayOfWeek(DayNum(d), mode); }

    using FactorTransform = ToMondayImpl;
};

struct ToDayOfYearImpl
{
    static constexpr auto name = "toDayOfYear";

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toDayOfYear(t);
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toDayOfYear(t);
    }
    static inline UInt16 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toDayOfYear(ExtendedDayNum(d));
    }
    static inline UInt16 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toDayOfYear(DayNum(d));
    }

    using FactorTransform = ToStartOfYearImpl;
};

struct ToHourImpl
{
    static constexpr auto name = "toHour";

    static inline UInt8 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toHour(t);
    }

    static inline UInt8 executeTime(Decimal64 t, UInt32 scale_multiplier, const DateLUTImpl &)
    {
        auto components = DecimalUtils::splitWithScaleMultiplier(t, scale_multiplier);
        return components.whole / 3600;
    }

    static inline UInt8 executeTime(Int64 t, const DateLUTImpl &)
    {
        return t / 3600;
    }

    static inline UInt8 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toHour(t);
    }

    static inline UInt8 execute(Int32, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }

    static inline UInt8 execute(UInt16, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }

    using FactorTransform = ToDateImpl;
};

struct TimezoneOffsetImpl
{
    static constexpr auto name = "timezoneOffset";

    static inline time_t execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.timezoneOffset(t);
    }

    static inline time_t execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.timezoneOffset(t);
    }

    static inline time_t execute(Int32, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }

    static inline time_t execute(UInt16, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }

    using FactorTransform = ToTimeImpl;
};

struct ToMinuteImpl
{
    static constexpr auto name = "toMinute";

    static inline UInt8 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toMinute(t);
    }
    static inline UInt8 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toMinute(t);
    }
    static inline UInt8 execute(Int32, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }
    static inline UInt8 execute(UInt16, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }
    static inline UInt8 executeTime(Decimal64 t, UInt32 scale_multiplier, const DateLUTImpl &)
    {
        auto components = DecimalUtils::splitWithScaleMultiplier(t, scale_multiplier);
        return (components.whole / 60) % 60;
    }

    static inline UInt8 executeTime(Int64 t, const DateLUTImpl &)
    {
        return (t / 60) % 60;
    }

    using FactorTransform = ToStartOfHourImpl;
};

struct ToSecondImpl
{
    static constexpr auto name = "toSecond";

    static inline UInt8 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toSecond(t);
    }
    static inline UInt8 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toSecond(t);
    }
    static inline UInt8 execute(Int32, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }
    static inline UInt8 execute(UInt16, const DateLUTImpl &)
    {
        throwDateTimeIsNotSupported(name);
    }
    static inline UInt8 executeTime(Decimal64 t, UInt32 scale_multiplier, const DateLUTImpl &)
    {
        auto components = DecimalUtils::splitWithScaleMultiplier(t, scale_multiplier);
        return components.whole % 60;
    }

    static inline UInt8 executeTime(Int64 t, const DateLUTImpl &)
    {
        return t % 60;
    }

    using FactorTransform = ToStartOfMinuteImpl;
};

struct ToMinuteSecondImpl
{
    static constexpr auto name = "toMinuteSecond";

    static ToMinuteImpl toMinute;
    static ToSecondImpl toSecond;

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return toMinute.execute(t, time_zone) * 100 + toSecond.execute(t, time_zone);
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return toMinute.execute(t, time_zone) * 100 + toSecond.execute(t, time_zone);
    }
    static inline UInt16 execute(Int32 t, const DateLUTImpl & time_zone)
    {
        return toMinute.execute(t, time_zone) * 100 + toSecond.execute(t, time_zone);
    }
    static inline UInt16 execute(UInt16 t, const DateLUTImpl & time_zone)
    {
        return toMinute.execute(t, time_zone) * 100 + toSecond.execute(t, time_zone);
    }
    static inline UInt16 executeTime(Decimal64 t, UInt32 scale_multiplier, const DateLUTImpl & date_lut)
    {
        return toMinute.executeTime(t, scale_multiplier, date_lut) * 100 + toSecond.executeTime(t, scale_multiplier, date_lut);
    }

    using FactorTransform = ZeroTransform; // Todo: check this
};

struct ToHourMinuteImpl
{
    static constexpr auto name = "toHourMinute";

    static ToHourImpl toHour;
    static ToMinuteImpl toMinute;

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return toHour.execute(t, time_zone) * 100 + toMinute.execute(t, time_zone);
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return toHour.execute(t, time_zone) * 100 + toMinute.execute(t, time_zone);
    }
    static inline UInt16 execute(Int32 t, const DateLUTImpl & time_zone)
    {
        return toHour.execute(t, time_zone) * 100 + toMinute.execute(t, time_zone);
    }
    static inline UInt16 execute(UInt16 t, const DateLUTImpl & time_zone)
    {
        return toHour.execute(t, time_zone) * 100 + toMinute.execute(t, time_zone);
    }
    static inline UInt16 executeTime(Decimal64 t, UInt32 scale_multiplier, const DateLUTImpl & date_lut)
    {
        return toHour.executeTime(t, scale_multiplier, date_lut) * 100 + toMinute.executeTime(t, scale_multiplier, date_lut);
    }

    using FactorTransform = ZeroTransform; // Todo: check this
};

struct ToHourSecondImpl
{
    static constexpr auto name = "toHourSecond";

    static ToHourImpl toHour;
    static ToMinuteSecondImpl toMinuteSecond;

    static inline UInt32 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return toHour.execute(t, time_zone) * 10000 + toMinuteSecond.execute(t, time_zone);
    }
    static inline UInt32 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return toHour.execute(t, time_zone) * 10000 + toMinuteSecond.execute(t, time_zone);
    }
    static inline UInt32 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return toHour.execute(d, time_zone) * 10000 + toMinuteSecond.execute(d, time_zone);
    }
    static inline UInt32 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return toHour.execute(d, time_zone) * 10000 + toMinuteSecond.execute(d, time_zone);
    }
    static inline UInt32 executeTime(Decimal64 t, UInt32 scale_multiplier, const DateLUTImpl & date_lut)
    {
        return toHour.executeTime(t, scale_multiplier, date_lut) * 10000 + toMinuteSecond.executeTime(t, scale_multiplier, date_lut);
    }

    using FactorTransform = ZeroTransform; // Todo: check this
};

struct ToDaySecondImpl
{
    static constexpr auto name = "toDaySecond";

    static ToDayOfMonthImpl toDay;
    static ToHourSecondImpl toHourSecond;

    static inline UInt32 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return toDay.execute(t, time_zone) * 1000000 + toHourSecond.execute(t, time_zone);
    }
    static inline UInt32 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return toDay.execute(t, time_zone) * 1000000 + toHourSecond.execute(t, time_zone);
    }
    static inline UInt32 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return toDay.execute(d, time_zone) * 1000000 + toHourSecond.execute(d, time_zone);
    }
    static inline UInt32 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return toDay.execute(d, time_zone) * 1000000 + toHourSecond.execute(d, time_zone);
    }

    /* Commented because ToDayOfMonth does not have executeTime
    static inline UInt32 executeTime(Decimal64 t, UInt32 scale_multiplier, const DateLUTImpl & date_lut)
    {
        return toHour.executeTime(t, scale_multiplier, date_lut) * 10000 + toMinuteSecond.executeTime(t, scale_multiplier, date_lut);
    }
    */

    using FactorTransform = ZeroTransform; // Todo: check this
};

struct ToDayMinuteImpl
{
    static constexpr auto name = "toDayMinute";

    static ToDayOfMonthImpl toDay;
    static ToHourMinuteImpl toHourMinute;

    static inline UInt32 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return toDay.execute(t, time_zone) * 10000 + toHourMinute.execute(t, time_zone);
    }
    static inline UInt32 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return toDay.execute(t, time_zone) * 10000 + toHourMinute.execute(t, time_zone);
    }
    static inline UInt32 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return toDay.execute(d, time_zone) * 10000 + toHourMinute.execute(d, time_zone);
    }
    static inline UInt32 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return toDay.execute(d, time_zone) * 10000 + toHourMinute.execute(d, time_zone);
    }

    /* Commented because ToDayOfMonth does not have executeTime
    // Todo: check return type
    static inline UInt8 executeTime(Decimal64 t, UInt32 scale_multiplier, const DateLUTImpl & date_lut)
    {
        return toHour.executeTime(t, scale_multiplier, date_lut) * 10000 + toMinuteSecond.executeTime(t, scale_multiplier, date_lut);
    }
    */

    using FactorTransform = ZeroTransform; // Todo: check this
};

struct ToDayHourImpl
{
    static constexpr auto name = "toDayHour";

    static ToDayOfMonthImpl toDay;
    static ToHourImpl toHour;

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return toDay.execute(t, time_zone) * 100 + toHour.execute(t, time_zone);
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return toDay.execute(t, time_zone) * 100 + toHour.execute(t, time_zone);
    }
    static inline UInt16 execute(Int32 t, const DateLUTImpl & time_zone)
    {
        return toDay.execute(t, time_zone) * 100 + toHour.execute(t, time_zone);
    }
    static inline UInt16 execute(UInt16 t, const DateLUTImpl & time_zone)
    {
        return toDay.execute(t, time_zone) * 100 + toHour.execute(t, time_zone);
    }

    /* Commented because ToDayOfMonth does not have executeTime
    // Todo: check return type
    static inline UInt8 executeTime(Decimal64 t, UInt32 scale_multiplier, const DateLUTImpl & date_lut)
    {
        return toHour.executeTime(t, scale_multiplier, date_lut) * 10000 + toMinuteSecond.executeTime(t, scale_multiplier, date_lut);
    }
    */

    using FactorTransform = ZeroTransform; // Todo: check this
};

struct ToISOYearImpl
{
    static constexpr auto name = "toISOYear";

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toISOYear(time_zone.toDayNum(t));
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toISOYear(time_zone.toDayNum(t));
    }
    static inline UInt16 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toISOYear(ExtendedDayNum(d));
    }
    static inline UInt16 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toISOYear(DayNum(d));
    }

    using FactorTransform = ZeroTransform;
};

struct ToStartOfISOYearImpl
{
    static constexpr auto name = "toStartOfISOYear";

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return t < 0 ? 0 : time_zone.toFirstDayNumOfISOYear(ExtendedDayNum(std::min(Int32(time_zone.toDayNum(t)), Int32(DATE_LUT_MAX_DAY_NUM))));
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfISOYear(time_zone.toDayNum(t));
    }
    static inline UInt16 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return d < 0 ? 0 : time_zone.toFirstDayNumOfISOYear(ExtendedDayNum(std::min(d, Int32(DATE_LUT_MAX_DAY_NUM))));
    }
    static inline UInt16 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfISOYear(DayNum(d));
    }
    static inline Int64 executeExtendedResult(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfISOYear(time_zone.toDayNum(t));
    }
    static inline Int32 executeExtendedResult(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toFirstDayNumOfISOYear(ExtendedDayNum(d));
    }

    using FactorTransform = ZeroTransform;
};

struct ToISOWeekImpl
{
    static constexpr auto name = "toISOWeek";

    static inline UInt8 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toISOWeek(time_zone.toDayNum(t));
    }
    static inline UInt8 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toISOWeek(time_zone.toDayNum(t));
    }
    static inline UInt8 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toISOWeek(ExtendedDayNum(d));
    }
    static inline UInt8 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toISOWeek(DayNum(d));
    }

    using FactorTransform = ToISOYearImpl;
};

struct ToRelativeYearNumImpl
{
    static constexpr auto name = "toRelativeYearNum";

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toYear(t);
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toYear(static_cast<time_t>(t));
    }
    static inline UInt16 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toYear(ExtendedDayNum(d));
    }
    static inline UInt16 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toYear(DayNum(d));
    }

    using FactorTransform = ZeroTransform;
};

struct ToRelativeQuarterNumImpl
{
    static constexpr auto name = "toRelativeQuarterNum";

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeQuarterNum(t);
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeQuarterNum(static_cast<time_t>(t));
    }
    static inline UInt16 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeQuarterNum(ExtendedDayNum(d));
    }
    static inline UInt16 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeQuarterNum(DayNum(d));
    }

    using FactorTransform = ZeroTransform;
};

struct ToRelativeMonthNumImpl
{
    static constexpr auto name = "toRelativeMonthNum";

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeMonthNum(t);
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeMonthNum(static_cast<time_t>(t));
    }
    static inline UInt16 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeMonthNum(ExtendedDayNum(d));
    }
    static inline UInt16 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeMonthNum(DayNum(d));
    }

    using FactorTransform = ZeroTransform;
};

struct ToRelativeWeekNumImpl
{
    static constexpr auto name = "toRelativeWeekNum";

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeWeekNum(t);
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeWeekNum(static_cast<time_t>(t));
    }
    static inline UInt16 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeWeekNum(ExtendedDayNum(d));
    }
    static inline UInt16 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeWeekNum(DayNum(d));
    }

    using FactorTransform = ZeroTransform;
};

struct ToRelativeDayNumImpl
{
    static constexpr auto name = "toRelativeDayNum";

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toDayNum(t);
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toDayNum(static_cast<time_t>(t));
    }
    static inline UInt16 execute(Int32 d, const DateLUTImpl &)
    {
        return static_cast<DayNum>(d);
    }
    static inline UInt16 execute(UInt16 d, const DateLUTImpl &)
    {
        return static_cast<DayNum>(d);
    }

    using FactorTransform = ZeroTransform;
};


struct ToRelativeHourNumImpl
{
    static constexpr auto name = "toRelativeHourNum";

    static inline UInt32 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeHourNum(t);
    }
    static inline UInt32 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeHourNum(static_cast<time_t>(t));
    }
    static inline UInt32 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeHourNum(ExtendedDayNum(d));
    }
    static inline UInt32 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeHourNum(DayNum(d));
    }

    using FactorTransform = ZeroTransform;
};

struct ToRelativeMinuteNumImpl
{
    static constexpr auto name = "toRelativeMinuteNum";

    static inline UInt32 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeMinuteNum(t);
    }
    static inline UInt32 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeMinuteNum(static_cast<time_t>(t));
    }
    static inline UInt32 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeMinuteNum(ExtendedDayNum(d));
    }
    static inline UInt32 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toRelativeMinuteNum(DayNum(d));
    }

    using FactorTransform = ZeroTransform;
};

struct ToRelativeSecondNumImpl
{
    static constexpr auto name = "toRelativeSecondNum";

    static inline Int64 execute(Int64 t, const DateLUTImpl &)
    {
        return t;
    }
    static inline UInt32 execute(UInt32 t, const DateLUTImpl &)
    {
        return t;
    }
    static inline UInt32 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.fromDayNum(ExtendedDayNum(d));
    }
    static inline UInt32 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.fromDayNum(ExtendedDayNum(d));
    }

    using FactorTransform = ZeroTransform;
};

struct ToYYYYMMImpl
{
    static constexpr auto name = "toYYYYMM";

    static inline UInt32 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toNumYYYYMM(t);
    }
    static inline UInt32 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toNumYYYYMM(t);
    }
    static inline UInt32 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toNumYYYYMM(ExtendedDayNum(d));
    }
    static inline UInt32 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toNumYYYYMM(DayNum(d));
    }

    using FactorTransform = ZeroTransform;
};

struct ToYYYYMMDDImpl
{
    static constexpr auto name = "toYYYYMMDD";

    static inline UInt32 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toNumYYYYMMDD(t);
    }
    static inline UInt32 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toNumYYYYMMDD(t);
    }
    static inline UInt32 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toNumYYYYMMDD(ExtendedDayNum(d));
    }
    static inline UInt32 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toNumYYYYMMDD(DayNum(d));
    }

    using FactorTransform = ZeroTransform;
};

struct ToYYYYMMDDhhmmssImpl
{
    static constexpr auto name = "toYYYYMMDDhhmmss";

    static inline UInt64 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toNumYYYYMMDDhhmmss(t);
    }
    static inline UInt64 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toNumYYYYMMDDhhmmss(t);
    }
    static inline UInt64 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toNumYYYYMMDDhhmmss(time_zone.toDate(ExtendedDayNum(d)));
    }
    static inline UInt64 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toNumYYYYMMDDhhmmss(time_zone.toDate(DayNum(d)));
    }

    using FactorTransform = ZeroTransform;
};

struct ToLastDayOfMonthImpl
{
    static constexpr auto name = "toLastDayOfMonth";

    static inline UInt16 execute(Int64 t, const DateLUTImpl & time_zone)
    {
        if (t < 0)
            return 0;

        /// 0xFFF9 is Int value for 2149-05-31 -- the last day where we can actually find LastDayOfMonth. This will also be the return value.
        return time_zone.toLastDayNumOfMonth(ExtendedDayNum(std::min(Int32(time_zone.toDayNum(t)), Int32(0xFFF9))));
    }
    static inline UInt16 execute(UInt32 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toLastDayNumOfMonth(t);
    }
    static inline UInt16 execute(Int32 d, const DateLUTImpl & time_zone)
    {
        if (d < 0)
            return 0;

        /// 0xFFF9 is Int value for 2149-05-31 -- the last day where we can actually find LastDayOfMonth. This will also be the return value.
        return time_zone.toLastDayNumOfMonth(ExtendedDayNum(std::min(d, Int32(0xFFF9))));
    }
    static inline UInt16 execute(UInt16 d, const DateLUTImpl & time_zone)
    {
        /// 0xFFF9 is Int value for 2149-05-31 -- the last day where we can actually find LastDayOfMonth. This will also be the return value.
        return time_zone.toLastDayNumOfMonth(DayNum(std::min(d, UInt16(0xFFF9))));
    }
    static inline Int64 executeExtendedResult(Int64 t, const DateLUTImpl & time_zone)
    {
        return time_zone.toLastDayNumOfMonth(time_zone.toDayNum(t));
    }
    static inline Int32 executeExtendedResult(Int32 d, const DateLUTImpl & time_zone)
    {
        return time_zone.toLastDayNumOfMonth(ExtendedDayNum(d));
    }

    using FactorTransform = ZeroTransform;
};


template <typename FromType, typename ToType, typename Transform, bool is_extended_result = false>
struct Transformer
{
    template <typename FromTypeVector, typename ToTypeVector>
    static void vector(const FromTypeVector & vec_from, ToTypeVector & vec_to, const DateLUTImpl & time_zone, const Transform & transform)
    {
        size_t size = vec_from.size();
        vec_to.resize(size);

        for (size_t i = 0; i < size; ++i)
            if constexpr (is_extended_result)
                vec_to[i] = transform.executeExtendedResult(vec_from[i], time_zone);
            else
                // narrowing conversions for time_t has a transformer to handle it
                // coverity[store_truncates_time_t]
                vec_to[i] = transform.execute(vec_from[i], time_zone);
    }
};


template <typename FromDataType, typename ToDataType, typename Transform, bool is_extended_result = false>
struct DateTimeTransformImpl
{
    static ColumnPtr execute(
        const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count, const Transform & transform = {})
    {
        using Op = Transformer<typename FromDataType::FieldType, typename ToDataType::FieldType, Transform, is_extended_result>;

        ColumnPtr source_col = arguments[0].column;

        if (checkAndGetColumn<ColumnString>(source_col.get()))
        {
            auto function_overload = FunctionFactory::instance().tryGet("toDateTime64", nullptr);

            if (!function_overload)
                throw Exception("Couldn't convert ColumnString to ColumnData since can't get function toDateTime64", ErrorCodes::BAD_ARGUMENTS);

            const auto scale_type = std::make_shared<DataTypeUInt8>();
            const auto scale_col = scale_type->createColumnConst(1, Field(0));
            ColumnWithTypeAndName scale_arg {std::move(scale_col), std::move(scale_type), "scale"};
            ColumnsWithTypeAndName args {std::move(arguments[0]), std::move(scale_arg)};
            auto func_base = function_overload->build(args);
            source_col = func_base->execute(args, func_base->getResultType(), input_rows_count);
        }

        if (const auto * sources = checkAndGetColumn<typename FromDataType::ColumnType>(source_col.get()))
        {
            auto mutable_result_col = result_type->createColumn();
            auto * col_to = assert_cast<typename ToDataType::ColumnType *>(mutable_result_col.get());

            WhichDataType result_data_type(result_type);
            if (result_data_type.isDateTime() || result_data_type.isDateTime64())
            {
                const auto & time_zone = dynamic_cast<const TimezoneMixin &>(*result_type).getTimeZone();
                Op::vector(sources->getData(), col_to->getData(), time_zone, transform);
            }
            else
            {
                size_t time_zone_argument_position = 1;
                if constexpr (std::is_same_v<ToDataType, DataTypeDateTime64> || std::is_same_v<ToDataType, DataTypeTime>)
                    time_zone_argument_position = 2;

                const DateLUTImpl & time_zone = extractTimeZoneFromFunctionArguments(arguments, time_zone_argument_position, 0);
                Op::vector(sources->getData(), col_to->getData(), time_zone, transform);
            }

            return mutable_result_col;
        }
        else
        {
            throw Exception("Illegal column " + arguments[0].column->getName()
                + " of first argument of function " + Transform::name,
                ErrorCodes::ILLEGAL_COLUMN);
        }
    }
};

template <typename FromDataType, typename ToDataType, typename Transform>
struct DateTimeTransformForNullImpl
{
    static ColumnPtr execute(
        const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count, const Transform & transform = {})
    {
        using ToColumnType = typename ToDataType::ColumnType;
        auto nested_result_type = removeNullable(result_type);

        using Op = Transformer<typename FromDataType::FieldType, typename ToDataType::FieldType, Transform>;
        
        ColumnPtr source_col = arguments[0].column;

        if (checkAndGetColumn<ColumnString>(source_col.get()))
        {
            auto function_overload = FunctionFactory::instance().tryGet("toDate", nullptr);

            if (!function_overload)
                throw Exception("Couldn't convert ColumnString to ColumnData since can't get function toDate", ErrorCodes::BAD_ARGUMENTS);

            auto func_base = function_overload->build({arguments[0]});
            source_col = func_base->execute(arguments, func_base->getResultType(), input_rows_count);
        }

        if (const auto * sources = checkAndGetColumn<typename FromDataType::ColumnType>(source_col.get()))
        {
            ToColumnType * col_to;
            auto mutable_result_col = result_type->createColumn();
            if (mutable_result_col->isNullable())
            {
                auto * nullable_column = assert_cast<ColumnNullable *>(mutable_result_col.get());
                col_to = &assert_cast<typename ToDataType::ColumnType &>(nullable_column->getNestedColumn());
                ColumnUInt8 & null_map = nullable_column->getNullMapColumn();
                null_map.getData().resize_fill(sources->getData().size(), 0);     
            }
            else
                col_to = assert_cast<typename ToDataType::ColumnType *>(mutable_result_col.get());

            WhichDataType result_data_type(nested_result_type);
            if (result_data_type.isDateTime() || result_data_type.isDateTime64())
            {
                const auto & time_zone = dynamic_cast<const TimezoneMixin &>(*nested_result_type).getTimeZone();
                Op::vector(sources->getData(), col_to->getData(), time_zone, transform);
            }
            else
            {
                size_t time_zone_argument_position = 1;
                if constexpr (std::is_same_v<ToDataType, DataTypeDateTime64>)
                    time_zone_argument_position = 2;

                const DateLUTImpl & time_zone = extractTimeZoneFromFunctionArguments(arguments, time_zone_argument_position, 0);
                Op::vector(sources->getData(), col_to->getData(), time_zone, transform);
            }

            return mutable_result_col;
        }
        else
        {
            throw Exception("Illegal column " + arguments[0].column->getName()
                + " of first argument of function " + Transform::name,
                ErrorCodes::ILLEGAL_COLUMN);
        }
    }
};

}
