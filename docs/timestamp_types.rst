.. title:: Timestamp Types

Timestamp Types
===============

Use this page to choose between TSC, system, or custom clock sources and understand their trade-offs.

Timestamp Recording
-------------------

The library records the timestamp of the log statement on the caller thread at the point the call was made.
This approach makes the log statement timestamp more accurate but introduces the possibility of the backend seeing a timestamp in the past, causing an out-of-order timestamp to appear in the log.

Due to the usage of different types of queues, it is possible in rare cases for the timestamp to be recorded but not immediately processed.
For example, the caller thread might block (if a blocking queue is used) or take some time to reallocate a new non-blocking queue.

To mitigate this, there is a configurable :cpp:member:`BackendOptions::log_timestamp_ordering_grace_period` option.
This option delays the reading of timestamps by the backend for a few microseconds (default 5μs), providing additional time for the frontend to push messages and reducing the chance of out-of-order timestamps within that grace window.
A queue head that stays ahead of the wall-clock cutoff becomes eligible once one grace period of steady time has elapsed since its first backend observation. Backward clock corrections therefore do not require waiting for wall time to catch up.
The backend assumes timestamps produced within a single frontend queue are normally non-decreasing.
If a wall clock or user-provided clock moves backwards, :cpp:member:`BackendOptions::ensure_monotonic_output_timestamps` can be enabled to correct regular log and metric output.
Backtrace records preserve their original capture timestamps and may still appear out of timestamp order when flushed later.

Timestamp Methods
-----------------

There are several ways to get the timestamp on the frontend:

- **TSC (Time Stamp Counter)**: This is the fastest method for the frontend, as it simply calls ``__rdtsc`` and pushes the raw TSC counter value. The backend runs an ``RdtscClock`` which periodically syncs with the wall time (default every 500ms). It takes the ``rdtsc`` value from the frontend and converts it to wall time using a lock-free algorithm. \
  Although the ``rdtsc`` clock might drift between resyncs, the difference is generally negligible. However, your processor must support invariant TSC to ensure that TSC counters between different cores remain synchronized. When this timestamp type is used, the backend requires a few seconds for the initial calibration of the ``RdtscClock`` on first usage.

  **Note**: TSC timestamps in rare cases may appear out of chronological order by a few microseconds. This occurs because: (1) frontend threads on different CPU cores capture raw TSC values with potential slight variations, (2) the backend converts these TSC values to wall time using a periodically updated calibration point, and (3) when resync occurs between message captures, different messages may be converted using different calibration points. Since the RdtscClock is not monotonic across resyncs, timestamps from different cores converted with different calibrations can appear out of order even when the actual logging occurred in sequence. This is an inherent characteristic of TSC-based timing across multi-core systems. If strict chronological timestamp ordering is required, use the System clock method instead.

- **System**: This method calls ``std::chrono::system_clock::now()`` on the frontend and pushes the wall time value to the queue. It is slower than TSC but provides the most accurate and immediately usable timestamps. The backend thread does not require any additional initialization time since the ``RdtscClock`` is not used.

- **User**: This method allows the user to provide a custom timestamp in nanoseconds since epoch through a concrete clock type with a ``uint64_t now() const`` member. This is useful, for example, in simulations where you want to display a custom timestamp rather than the current one in the log statements.

Logger Policies
---------------

``quill::LoggerPolicy`` defines per-logger compile-time traits. Its defaults are
``using clock_t = quill::TscClock`` and ``enable_immediate_flush = true``. Derive from
``LoggerPolicy`` and override only the members that change; future defaults are inherited
automatically. Different policies share thread queues when they use the same
``FrontendOptions``. No policy object is stored in a logger.

.. literalinclude:: snippets/quill_docs_logger_policy.cpp
   :language: cpp
   :linenos:

Select a policy with ``Frontend::create_logger<MyPolicy>(...)`` or
``Frontend::create_or_get_logger<MyPolicy>(...)``. The default policy preserves ordinary
TSC logging. The policy type is part of ``LoggerImpl<TFrontendOptions, TLoggerPolicy>``;
use that explicit type or a named alias for the returned logger. ``quill::Logger`` uses
the default frontend options and policy.

All clock types use the same inline logging path. Clock selection is resolved at compile
time; only thread initialization or queue recovery selects the cold fallback.
``ClockSourceType`` and ``get_clock_source_type()`` remain available for introspection
and backend timestamp conversion and ordering.

For lookup, specify the same policy used at creation:

.. code-block:: cpp

   using MyLogger = quill::LoggerImpl<quill::FrontendOptions, MyPolicy>;
   MyLogger* logger = quill::Frontend::get_logger<MyPolicy>("main");

``get_all_loggers<MyPolicy>()`` and ``get_valid_logger<MyPolicy>()`` select only loggers
with that policy and frontend options. Named lookup or ``create_or_get_logger`` reports
an incompatible existing policy, including when RTTI is disabled. Cloning preserves the
policy and copies its clock object or pointer when a new logger is needed.
Without RTTI, checked lookup compares a token for the complete logger specialization.
Hidden shared libraries may have separate tokens for the same C++ type. Perform creation
and typed lookup inside the owning library and expose exported wrapper functions, as in
``examples/shared_library``. RTTI-enabled lookup supports compatible types across libraries.

Setting ``enable_immediate_flush = false`` removes automatic-flush handling from that
logger's hot path. Its ``set_immediate_flush()`` setting has no effect, while explicit
``flush_log()`` still works. Configure this through the policy rather than a global build
setting. The runtime flush threshold remains configurable for policies that enable the feature.

Providing a Custom Timestamp
----------------------------

Set ``MyPolicy::clock_t`` to a custom clock object type or pointer type, and pass its
instance as the clock argument at logger creation. A custom clock supplies ``uint64_t now() const`` returning nanoseconds since epoch. Pass a
copyable or movable clock object by value, or a non-null pointer to an application-owned clock.
Quill stores the object or pointer in the logger. Pointer-based clocks must remain alive until
the logger is removed and no thread can log through it. Cloned loggers copy the clock object
or share the pointed-to clock. Creating a new logger by cloning a move-only clock reports an
error; pass a clock explicitly when creating that logger instead.

Clock reads must be thread-safe when the logger is shared. Successive timestamps within a
frontend queue should normally be non-decreasing; zero is a valid timestamp. Custom clocks
bypass the wall-clock ordering grace check, so simulation time may be in the past or future.
See ``BackendOptions::ensure_monotonic_output_timestamps`` for handling backward clock changes.

.. literalinclude:: ../examples/user_clock_source.cpp
   :language: cpp
   :linenos:

Getting a Synchronized Timestamp with the Backend Thread TSC Clock
------------------------------------------------------------------

In some cases, when using TSC for log statements, you might want to obtain a timestamp that is synchronized with the timestamp seen in the log statements.
To achieve this, you can use the :cpp:class:`BackendTscClock`. See the example below:

.. literalinclude:: snippets/quill_docs_timestamp_types.cpp
   :language: cpp
   :linenos:
