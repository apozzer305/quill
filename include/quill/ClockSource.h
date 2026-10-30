/**
 * @page copyright
 * Copyright(c) 2020-present, Odysseas Georgoudis & quill contributors.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#pragma once

#include "quill/core/Attributes.h"
#include "quill/core/ChronoTimeUtils.h"
#include "quill/core/Rdtsc.h"

QUILL_BEGIN_NAMESPACE

QUILL_BEGIN_EXPORT

/** Records raw TSC ticks for conversion to epoch nanoseconds by the backend. */
struct TscClock
{
  QUILL_NODISCARD static uint64_t now() noexcept { return detail::rdtsc(); }
};

/** Records system time in nanoseconds since epoch on the calling thread. */
struct SystemClock
{
  QUILL_NODISCARD static uint64_t now() noexcept { return detail::get_system_time_ns(); }
};

QUILL_END_EXPORT

QUILL_END_NAMESPACE
