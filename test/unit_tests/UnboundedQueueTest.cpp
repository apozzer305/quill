#include "doctest/doctest.h"
#include "quill/core/FrontendOptions.h"

#include "quill/core/UnboundedSPSCQueue.h"
#include <cstring>
#include <thread>
#include <vector>

TEST_SUITE_BEGIN("UnboundedQueue");

using namespace quill::detail;

TEST_CASE("unbounded_queue_shrink")
{
  constexpr size_t CHUNK{256};
  constexpr size_t INITIAL_SIZE{1024};

  UnboundedSPSCQueue buffer{INITIAL_SIZE, quill::FrontendOptions::unbounded_queue_max_capacity};
  REQUIRE_EQ(buffer.producer_capacity(), INITIAL_SIZE);

  // This queue will grow as we request 5 * 256
  for (uint32_t i = 0; i < 5; ++i)
  {
    auto* write_buffer = buffer.prepare_write(CHUNK);

    REQUIRE(write_buffer);
    buffer.finish_write(CHUNK);
    buffer.commit_write();
  }

  {
    UnboundedSPSCQueue::ReadResult res = buffer.prepare_read();
    REQUIRE(res.read_pos);
    buffer.finish_read(INITIAL_SIZE);
    buffer.commit_read();
  }

  {
    // Now read again the next part from the allocated queue
    UnboundedSPSCQueue::ReadResult res = buffer.prepare_read();
    REQUIRE(res.read_pos);
    REQUIRE(res.allocation);
    REQUIRE_EQ(res.previous_capacity, INITIAL_SIZE);
    REQUIRE_EQ(res.new_capacity, INITIAL_SIZE * 2);
    buffer.finish_read(CHUNK);
    buffer.commit_read();
  }

  // Shrink the queue
  REQUIRE_EQ(buffer.producer_capacity(), INITIAL_SIZE * 2);
  buffer.shrink(INITIAL_SIZE);
  REQUIRE_EQ(buffer.producer_capacity(), INITIAL_SIZE);

  {
    // On next read we should see the new queue - old is gone
    UnboundedSPSCQueue::ReadResult res = buffer.prepare_read();
    REQUIRE_EQ(res.read_pos, nullptr);
    REQUIRE(res.allocation);
    REQUIRE_EQ(res.previous_capacity, INITIAL_SIZE * 2);
    REQUIRE_EQ(res.new_capacity, INITIAL_SIZE);
    // Note: finish_read()/commit_read() must not be called after a prepare_read() that returned
    // no data; it would publish a reader position ahead of the writer position
  }
}

TEST_CASE("unbounded_queue_allocation_within_limit")
{
  UnboundedSPSCQueue buffer{1024, std::numeric_limits<uint32_t>::max()};

  static constexpr size_t two_mb = 2u * 1024u * 1024u;

  // Attempt to allocate a buffer size that exceeds the default limit,
  // ensuring that allocation within configurable bounds does not throw.
  auto* write_buffer = buffer.prepare_write(2 * two_mb);
  REQUIRE(write_buffer);
  REQUIRE_EQ(buffer.producer_capacity(), 2 * two_mb);
}

TEST_CASE("unbounded_queue_rounds_max_capacity")
{
  UnboundedSPSCQueue buffer{1024, 3000};

  auto* write_buffer = buffer.prepare_write(2500);
  REQUIRE(write_buffer);
  REQUIRE_EQ(buffer.producer_capacity(), 4096);
}

TEST_CASE("unbounded_queue_allocation_exceeds_limit")
{
  constexpr static uint64_t two_mb = 2u * 1024u * 1024u;

  UnboundedSPSCQueue buffer{1024, two_mb};

#if !defined(QUILL_NO_EXCEPTIONS)
  // Attempt to allocate a buffer size that exceeds the specified capacity,
  // which should trigger an exception.
  auto func = [&buffer]()
  {
    auto* write_buffer_z = buffer.prepare_write(2 * two_mb);
    return write_buffer_z;
  };

  REQUIRE_THROWS_AS(func(), quill::QuillError);
#endif
  REQUIRE_EQ(buffer.producer_capacity(), 1024);
}

TEST_CASE("unbounded_queue_read_write_multithreaded_plain_ints")
{
  UnboundedSPSCQueue buffer{1024, quill::FrontendOptions::unbounded_queue_max_capacity};

  std::thread producer_thread(
    [&buffer]()
    {
      for (uint32_t wrap_cnt = 0; wrap_cnt < 20; ++wrap_cnt)
      {
        for (uint32_t i = 0; i < 8192; ++i)
        {
          auto* write_buffer = buffer.prepare_write(sizeof(uint32_t));

          while (!write_buffer)
          {
            std::this_thread::sleep_for(std::chrono::microseconds{2});

            write_buffer = buffer.prepare_write(sizeof(uint32_t));
          }

          std::memcpy(write_buffer, &i, sizeof(uint32_t));
          buffer.finish_write(sizeof(uint32_t));
          buffer.commit_write();
        }
      }
    });

  // Delay creating the consumer thread
  std::this_thread::sleep_for(std::chrono::milliseconds{300});

  std::thread consumer_thread(
    [&buffer]()
    {
      for (uint32_t wrap_cnt = 0; wrap_cnt < 20; ++wrap_cnt)
      {
        for (uint32_t i = 0; i < 8192; ++i)
        {
          auto read_result = buffer.prepare_read();
          while (!read_result.read_pos)
          {
            std::this_thread::sleep_for(std::chrono::microseconds{2});
            read_result = buffer.prepare_read();
          }

          auto const value = reinterpret_cast<uint32_t const*>(read_result.read_pos);
          REQUIRE_EQ(*value, i);
          buffer.finish_read(sizeof(uint32_t));
          buffer.commit_read();
        }
      }
    });

  producer_thread.join();
  consumer_thread.join();
  REQUIRE(buffer.empty());
}

TEST_CASE("unbounded_queue_recovered_write_reservation")
{
  UnboundedSPSCQueue queue{1024, 2048};
  auto first = queue.prepare_write_reserve(1024);
  REQUIRE(first.write_buffer);
  first.bounded_queue->finish_and_commit_write_reservation(first.writer_pos + 1024);
  CHECK_FALSE(queue.prepare_write_reserve_cached(8).write_buffer);

  // Growth changes the producer. Commit through the returned queue, not the old one.
  auto recovered = queue.prepare_write_reserve(8);
  REQUIRE(recovered.write_buffer);
  CHECK_NE(recovered.bounded_queue, first.bounded_queue);
  CHECK_EQ(recovered.writer_pos, 0);
  recovered.write_buffer[0] = std::byte{42};
  recovered.bounded_queue->finish_and_commit_write_reservation(recovered.writer_pos + 8);

  REQUIRE(queue.prepare_read().read_pos);
  queue.finish_read(1024);
  queue.commit_read();
  auto const next = queue.prepare_read();
  REQUIRE(next.read_pos);
  CHECK(next.allocation);
  CHECK_EQ(next.read_pos[0], std::byte{42});
  queue.finish_read(8);
  queue.commit_read();
  CHECK_FALSE(queue.prepare_read().read_pos);

  // A failed reservation must remain empty when the configured capacity limit is reached.
  auto const full = queue.prepare_write_reserve(2048);
  REQUIRE(full.write_buffer);
  full.bounded_queue->finish_and_commit_write_reservation(full.writer_pos + 2048);
  auto const failed = queue.prepare_write_reserve(1);
  CHECK_FALSE(failed.write_buffer);
}

TEST_SUITE_END();
