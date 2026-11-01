/**
 * @page copyright
 * Copyright(c) 2020-present, Odysseas Georgoudis & quill contributors.
 * Distributed under the MIT License (http://opensource.org/licenses/MIT)
 */

#pragma once

#include "quill/ClockSource.h"

#if defined(QUILL_ENABLE_IMMEDIATE_FLUSH)
  #error "Replace QUILL_ENABLE_IMMEDIATE_FLUSH with LoggerPolicy::enable_immediate_flush"
#endif

QUILL_BEGIN_NAMESPACE

QUILL_BEGIN_EXPORT

/**
 * Per-logger compile-time policy. Derive from LoggerPolicy and override only the members
 * that change, so future defaults are inherited automatically. Different policies may share
 * the same frontend options and thread queues. Policy traits do not occupy logger storage.
 */
struct LoggerPolicy
{
  /** Clock object or non-owned pointer with uint64_t now() const; custom clocks return epoch nanoseconds. */
  using clock_t = TscClock;

  /** Compile in automatic flushing configured by set_immediate_flush(). Explicit flush_log() always works. */
  static constexpr bool enable_immediate_flush = true;
};

QUILL_END_EXPORT

QUILL_END_NAMESPACE
