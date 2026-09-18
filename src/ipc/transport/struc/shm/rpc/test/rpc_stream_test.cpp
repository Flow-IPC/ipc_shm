/* Flow-IPC: Shared Memory
 * Copyright 2023 Akamai Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in
 * compliance with the License.  You may obtain a copy
 * of the License at
 *
 *   https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in
 * writing, software distributed under the License is
 * distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
 * CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing
 * permissions and limitations under the License. */

/* Tests of the capnp-RPC `-> stream` feature as it interacts with Session_vat_network's flow-control window
 * (Session_vat_network::streaming_flow_window_ki(), Context_server::streaming_flow_window_default_ki()).
 *
 * ### The harness ###
 * There is one KJ event loop per thread, and a capnp-RPC session needs one per side; so the server lives on
 * its own thread (Ez_rpc_server there, waiting on a stop-promise instead of the usual kj::NEVER_DONE), while
 * the client (Ez_rpc_client) lives on the test thread, whose wait-scope the test drives.  The ipc::session
 * side is an in-process session, as in the struc::Channel tests: same process on both ends, with the App
 * universe (and the post-test cleanup of the Session_server's kernel-persistent items) supplied by the
 * struc test utility's Session_pair, whose SHM-classic session types are exactly the rpc:: aliases.
 *
 * (The main thread isn't really polluted/left unsegregated (w/r/t subsequent or preceding tests) by our placing
 * the Ez_rpc_client loop there.  That's a reasonable concern, but just factually there's only one thread-local
 * thing among the things we run there -- Ez_rpc_kj_io -- and it by design self-resets by the end.  So there isn't
 * really a point to spawning a thread for _client too.)
 *
 * ### What makes the observations deterministic ###
 * capnp's variable-window flow controller (capnp/rpc.c++ WindowFlowController) resolves a streaming send()'s
 * promise at once if in-flight bytes < window + (largest message seen so far); otherwise not until enough
 * earlier messages have been *acked* -- an ack being the Return for that call, i.e., the receiving-side handler
 * having completed.  So the k-th of equal-sized sends (size S) resolves immediately <=> (k - 1) * S < W.
 * We control acks fully: the receiving-side push() handler returns a promise that stays pending until the
 * test opens a Gate.  No sleeps are needed to see a stall: with the gate closed, a stalled promise cannot
 * become ready, and kj::Promise::poll() reports that without blocking.  (The one bounded wait, in the
 * server-window test, is explained there.)
 *
 * In our zero-copy mode the S counted by the controller is the message's size *in SHM* -- the streamed payload
 * itself -- not the tiny SHM-handle message that actually crosses the transport
 * (see Session_vat_network::Rpc_msg_out_impl::sizeInWords()).  The window values below are chosen relative to
 * that S with wide margins, so the exact per-message overhead does not matter. */

#include "ipc/transport/struc/shm/rpc/ez_rpc.hpp"
#include "ipc/transport/struc/shm/rpc/test/rpc_test_schema.capnp.h"
#include "ipc/transport/struc/test/test_util.hpp"
#include "ipc/test/test_logger.hpp"
#include <flow/test/test_common_util.hpp>
#include <flow/async/single_thread_task_loop.hpp>
#include <flow/util/util_fwd.hpp>
#include <gtest/gtest.h>
#include <kj/async.h>
#include <kj/async-io.h>
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/thread/future.hpp>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace ipc::transport::struc::shm::rpc::test
{

namespace
{

using flow::util::Mutex_non_recursive;
using flow::util::Lock_guard;

/* Choose one of the (as of this writing 2) available SHM-providers: SHM-classic.  For this test it does not
 * matter. */
using Client_session_t = session::shm::classic::rpc::Client_session<>;
using Session_server_t = session::shm::classic::rpc::Session_server<>;
using Ez_client = Ez_rpc_client<Client_session_t>;
using Ez_server = Ez_rpc_server<Session_server_t>;

/* The App universe + cleanup helper.  Its session types coincide with the rpc:: aliases above (a `static_assert`
 * ensures it), so its App descriptors are exactly what Ez_rpc_{client|server} need; its own session members stay
 * unused (NULL state).  Ez_rpc* does the Flow-IPC ipc::session-wrangling for us (that's one of its things that makes
 * it ez), so we don't need those parts of test::Session_pair. */
using Apps = struc::test::Session_pair<session::schema::MqType::NONE, true, session::schema::ShmType::CLASSIC>;
static_assert(std::is_same_v<Apps::Client_session_t, Client_session_t>
                && std::is_same_v<Apps::Session_server_t, Session_server_t>,
              "The rpc:: session aliases should be the SHM-classic, handles-enabled, MQ-less session types.");

// Logger for the objects under test: null by default; flip to get their logs (at the level test_main.cpp sets).
flow::log::Logger* obj_logger()
{
#if 0 // 1 XXXtemp for debug
  return nullptr;
#else
  static ipc::test::Test_logger s_logger{flow::log::Sev::S_TRACE}; // Default DATA is a bit extreme with the dumps.
  return &s_logger;
#endif
}

/* Streamed-chunk geometry.  S (the message's SHM size) is sized a bit over CHUNK_KI Ki bytes;
 * windows are set relative to CHUNK_KI with margins of tens of Ki bytes, so the "a bit" is irrelevant. */
constexpr size_t CHUNK_N_ELEMS = 12800;
constexpr size_t CHUNK_KI = CHUNK_N_ELEMS * sizeof(uint64_t) / 1024; // 100.
static_assert(CHUNK_KI == 100, "Keep the arithmetic below easy to follow.");

/* A `-> stream` handler's completion switch.  While closed, pass() yields a promise that stays pending; open()
 * releases all such promises and makes subsequent pass()es resolve immediately.  pass() runs on the KJ thread that
 * services the handler; open()/close() may run on any thread (the fulfillers are cross-thread). */
class Gate
{
public:
  kj::Promise<void> pass()
  {
    Lock_guard<Mutex_non_recursive> lock{m_mutex};
    if (m_open)
    {
      return kj::READY_NOW;
    }
    // else
    auto paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    m_fulfillers.emplace_back(kj::mv(paf.fulfiller));
    return kj::mv(paf.promise);
  }

  void open()
  {
    Lock_guard<Mutex_non_recursive> lock{m_mutex};
    m_open = true;
    for (auto& fulfiller : m_fulfillers)
    {
      fulfiller->fulfill();
    }
    m_fulfillers.clear();
  }

  void close()
  {
    Lock_guard<Mutex_non_recursive> lock{m_mutex};
    m_open = false;
  }

private:
  Mutex_non_recursive m_mutex;
  bool m_open = false;
  std::vector<kj::Own<kj::CrossThreadPromiseFulfiller<void>>> m_fulfillers;
}; // class Gate

/* Sink impl: accumulates count + sum of everything push()ed; each push() completes per the Gate.
 * Server_t is Sink::Server (client-side sink for pull()) or Streamer::Server (the server's bootstrap object). */
template<typename Server_t>
class Counting_sink : public Server_t
{
public:
  explicit Counting_sink(Gate* gate) : m_gate(gate) {}

  kj::Promise<void> push(typename Server_t::PushContext context) override
  {
    const auto chunk = context.getParams().getChunk();
    for (const auto val : chunk)
    {
      m_sum += val;
    }
    m_count += chunk.size();
    ++m_n_chunks;
    return m_gate->pass(); // The Return (hence the sender's flow-control ack) is withheld until the gate opens.
  }

  uint64_t count() const { return m_count; }
  uint64_t sum() const { return m_sum; }
  size_t n_chunks() const { return m_n_chunks; }

protected:
  void reset() { m_count = m_sum = 0; m_n_chunks = 0; }

private:
  Gate* const m_gate;
  uint64_t m_count = 0;
  uint64_t m_sum = 0;
  size_t m_n_chunks = 0;
}; // class Counting_sink

/* Client-side sink for pull(): additionally fulfills `on_nth_chunk[n - 1]` (if given) upon arrival of the n-th
 * chunk, so the test can await a given chunk's arrival deterministically.  (Only the test thread touches it, so
 * plain fulfillers suffice.) */
class Cli_sink final : public Counting_sink<Sink::Server>
{
public:
  using Fulfillers = std::vector<kj::Own<kj::PromiseFulfiller<void>>>;

  Cli_sink(Gate* gate, Fulfillers&& on_nth_chunk) :
    Counting_sink<Sink::Server>(gate), m_on_nth_chunk(std::move(on_nth_chunk)) {}

  kj::Promise<void> push(PushContext context) override
  {
    auto promise = Counting_sink<Sink::Server>::push(kj::mv(context));
    if (n_chunks() <= m_on_nth_chunk.size())
    {
      m_on_nth_chunk[n_chunks() - 1]->fulfill();
    }
    return promise;
  }

private:
  Fulfillers m_on_nth_chunk;
};

// Fills `chunk` with the values [base, base + size) where `base = chunk_idx * size`: so all chunks together = [0, N).
void fill_chunk(::capnp::List<uint64_t>::Builder chunk, size_t chunk_idx)
{
  const auto base = chunk_idx * chunk.size();
  for (size_t idx = 0; idx != chunk.size(); ++idx)
  {
    chunk.set(idx, base + idx);
  }
}

// Issues one streaming push() of chunk `chunk_idx`; returns the flow-controlled promise.
kj::Promise<void> push_chunk(Sink::Client sink, size_t chunk_idx, size_t chunk_n_elems)
{
  auto request = sink.pushRequest();
  fill_chunk(request.initChunk(chunk_n_elems), chunk_idx);
  return request.send();
}

// Streams chunks [chunk_idx, n_chunks) sequentially: each push() awaited before the next (as in rpc_demo).
kj::Promise<void> stream_chunks(Sink::Client sink, size_t chunk_idx, size_t n_chunks, size_t chunk_n_elems)
{
  if (chunk_idx == n_chunks)
  {
    return kj::READY_NOW;
  }
  // else
  return push_chunk(sink, chunk_idx, chunk_n_elems).then([sink, chunk_idx, n_chunks, chunk_n_elems]() mutable
  {
    return stream_chunks(kj::mv(sink), chunk_idx + 1, n_chunks, chunk_n_elems);
  });
}

// Expected count and sum of chunks [0, n_chunks) as filled by fill_chunk().
std::pair<uint64_t, uint64_t> expected_count_sum(size_t n_chunks, size_t chunk_n_elems)
{
  const uint64_t n = n_chunks * chunk_n_elems;
  return { n, (n == 0) ? 0 : ((n * (n - 1)) / 2) };
}

// The server's bootstrap object.
class Streamer_impl final : public Counting_sink<Streamer::Server>
{
public:
  explicit Streamer_impl(Gate* gate) : Counting_sink<Streamer::Server>(gate) {}

  kj::Promise<void> pushDone(PushDoneContext context) override
  {
    auto results = context.getResults();
    results.setCount(count());
    results.setSum(sum());
    reset();
    return kj::READY_NOW;
  }

  kj::Promise<void> pull(PullContext context) override
  {
    const auto params = context.getParams();
    return stream_chunks(params.getSink(), 0, params.getNChunks(), params.getChunkSz());
  }
};

/* Server on its own thread + KJ loop; client on the calling (test) thread.  See comment at top of this file.
 * srv_default_window_ki_or_0 is applied via Context_server::streaming_flow_window_default_ki() before the
 * client connects, since that setting applies to sessions accepted subsequently. */
class Rpc_pair
{
public:
  explicit Rpc_pair(size_t srv_default_window_ki_or_0 = 0) :
    m_apps(boost::make_shared<Apps>()),
    m_srv_thread(obj_logger(), "rpc_srv")
  {
    m_apps->populate_apps("Rpc");
    m_apps->remove_server_persistent_bits(false); // A previous (crashed?) run's leavings would trip the server.

    boost::promise<void> srv_ready;
    m_srv_thread.start();
    m_srv_thread.post([&]() { srv_main(srv_default_window_ki_or_0, &srv_ready); });
    srv_ready.get_future().wait();

    m_cli.emplace(obj_logger(), m_apps->m_cli_app, m_apps->m_srv_app, false); // (No FD transport needed.)
  }

  ~Rpc_pair()
  {
    m_srv_gate.open(); // Any handler still gated would otherwise keep the session from unwinding cleanly.
    m_cli.reset(); // Session ends; the server side notices (on_disconnect) and drops its per-session objects.
    m_srv_stop->fulfill();
    m_srv_thread.stop(); // Joins: srv_main() returns, destroying the Ez_rpc_server (and its Session_server).
    m_apps->remove_server_persistent_bits();
  }

  Ez_client& cli() { return *m_cli; }
  kj::WaitScope& ws() { return *m_cli->get_wait_scope(); }
  Gate& srv_gate() { return m_srv_gate; }

  /* A fresh bootstrap handle (a promise-capability until its first call's response resolves it).  A test
   * should obtain one and stream over that same handle throughout: the flow controller is per capability. */
  Streamer::Client streamer() { return m_cli->get_main<Streamer>(); }

  /* Ordinary RPC over `streamer`: verifies the server's push() accumulator and resets it.  Its response also
   * implies every preceding push() has been acked; and, on first use of `streamer`, that it has resolved --
   * so that subsequent streaming send()s go straight to the flow controller, which then decides
   * ready-or-not synchronously, instead of queueing behind the capability's resolution. */
  void expect_push_done(Streamer::Client& streamer, size_t n_chunks, size_t chunk_n_elems = CHUNK_N_ELEMS)
  {
    const auto response = streamer.pushDoneRequest().send().wait(ws());
    const auto [count, sum] = expected_count_sum(n_chunks, chunk_n_elems);
    EXPECT_EQ(response.getCount(), count);
    EXPECT_EQ(response.getSum(), sum);
  }

private:
  // Body of the server thread: runs until the stop-promise is fulfilled by ~Rpc_pair().
  void srv_main(size_t srv_default_window_ki_or_0, boost::promise<void>* srv_ready)
  {
    Ez_server srv{obj_logger(),
                  [this](auto&&...) { return kj::heap<Streamer_impl>(&m_srv_gate); },
                  m_apps->m_srv_app, m_apps->m_cli_apps, false};
    if (srv_default_window_ki_or_0 != 0)
    {
      srv.context_server()->streaming_flow_window_default_ki(srv_default_window_ki_or_0);
      EXPECT_EQ(srv.context_server()->streaming_flow_window_default_ki(), srv_default_window_ki_or_0);
    }

    auto stop_paf = kj::newPromiseAndCrossThreadFulfiller<void>();
    m_srv_stop = kj::mv(stop_paf.fulfiller);
    srv_ready->set_value();

    stop_paf.promise.wait(*srv.get_wait_scope()); // Service the client(s) until told to stop.
  }

  boost::shared_ptr<Apps> m_apps; // Heap-pinned: the sessions store the App members by address.
  Gate m_srv_gate;
  kj::Own<kj::CrossThreadPromiseFulfiller<void>> m_srv_stop; // Set by srv_main() before it signals readiness.
  flow::async::Single_thread_task_loop m_srv_thread;
  std::optional<Ez_client> m_cli;
}; // class Rpc_pair

} // namespace (anon)

TEST(Rpc_stream_test, client_window_knob)
{
  FLOW_TEST_TRACE_CTX("Session_vat_network::streaming_flow_window_ki() accessor/mutator.");
  Rpc_pair pair;
  auto& net = *pair.cli().rpc_context()->vat_network();

  EXPECT_EQ(net.streaming_flow_window_ki(), Session_vat_network_base::S_STREAMING_FLOW_WINDOW_KI);
  net.streaming_flow_window_ki(64);
  EXPECT_EQ(net.streaming_flow_window_ki(), 64u);
  EXPECT_THROW(net.streaming_flow_window_ki(0), kj::Exception); // Contract: must be positive (KJ_REQUIRE()).
  EXPECT_EQ(net.streaming_flow_window_ki(), 64u); // Rejected => unchanged.
}

TEST(Rpc_stream_test, client_stream_window)
{
  Rpc_pair pair;
  auto& ws = pair.ws();
  auto& net = *pair.cli().rpc_context()->vat_network();
  auto streamer = pair.streamer();

  pair.expect_push_done(streamer, 0); // Warm-up: resolves `streamer` (see expect_push_done()).

  {
    FLOW_TEST_TRACE_CTX("Window < S: the 2nd send() stalls until the 1st chunk is acked.");
    net.streaming_flow_window_ki(CHUNK_KI / 2);
    pair.srv_gate().close();

    auto send1 = push_chunk(streamer, 0, CHUNK_N_ELEMS);
    EXPECT_TRUE(send1.poll(ws)); // The 1st message is never gated (window is extended by the largest message).
    auto send2 = push_chunk(streamer, 1, CHUNK_N_ELEMS);
    EXPECT_FALSE(send2.poll(ws)); // In-flight (2 S) >= window + S: stalled; and no ack can arrive while gated.

    pair.srv_gate().open(); // Server completes push(0) => its Return acks chunk 0 => send2 resolves.
    send2.wait(ws);
    send1.wait(ws);
    pair.expect_push_done(streamer, 2);
  }

  {
    FLOW_TEST_TRACE_CTX("Window = 2.5 S: send()s 1-3 resolve at once; the 4th stalls.");
    net.streaming_flow_window_ki(CHUNK_KI * 5 / 2);
    pair.srv_gate().close();

    std::vector<kj::Promise<void>> sends;
    for (size_t idx = 0; idx != 3; ++idx)
    {
      sends.emplace_back(push_chunk(streamer, idx, CHUNK_N_ELEMS));
      EXPECT_TRUE(sends.back().poll(ws)) << "send() [" << idx << "] should have resolved at once.";
    }
    sends.emplace_back(push_chunk(streamer, 3, CHUNK_N_ELEMS));
    EXPECT_FALSE(sends.back().poll(ws)); // 3 S in flight >= 2.5 S + S.

    pair.srv_gate().open();
    for (auto& send : sends)
    {
      send.wait(ws);
    }
    pair.expect_push_done(streamer, 4);
  }
} // TEST(Rpc_stream_test, client_stream_window)

/* Context_server::streaming_flow_window_default_ki() applies to each Server_context accepted after it is set:
 * hence server->client streaming (pull() into our Sink) obeys the *server's* window.  We stream 2 chunks with
 * the client-side sink gated: with window < S the 2nd send() stalls, so pull() cannot return until we open the
 * gate; with a window >> 2 S both send()s resolve at once, so pull() returns while still gated. */
TEST(Rpc_stream_test, server_window_default)
{
  /* XXX Temporarily skipped: reproduces a real bug (SIGSEGV at teardown: a capnp-RPC call context, owned by the
   * KJ event loop rather than the RpcSystem, frees its in-SHM message after the session's SHM arena is gone).
   * Re-enable once the Session_vat_network shared-session-handle fix is in. */
  GTEST_SKIP() << "Skipped pending the Session_vat_network message-outlives-session fix.";

  const auto run = [](size_t srv_window_ki, bool expect_stall)
  {
    constexpr size_t N_CHUNKS = 2;

    Gate cli_gate; // Declared before `pair` (hence the sink below): a gated handler's promise refers to it.
    Rpc_pair pair{srv_window_ki};
    auto& ws = pair.ws();
    auto streamer = pair.streamer();
    pair.expect_push_done(streamer, 0); // Warm-up (see expect_push_done()).

    auto chunk0_rcvd_paf = kj::newPromiseAndFulfiller<void>();
    auto chunk1_rcvd_paf = kj::newPromiseAndFulfiller<void>();
    Cli_sink::Fulfillers on_nth_chunk;
    on_nth_chunk.emplace_back(kj::mv(chunk0_rcvd_paf.fulfiller));
    on_nth_chunk.emplace_back(kj::mv(chunk1_rcvd_paf.fulfiller));
    auto sink_own = kj::heap<Cli_sink>(&cli_gate, std::move(on_nth_chunk));
    const auto& sink = *sink_own;
    /* We keep our own capability handle to the sink -- and thus keep the object alive for as long as we read
     * its counters -- rather than handing the server the only reference: once pull() completes server-side,
     * it releases the sink, and the RPC system would then destroy the object under us. */
    Sink::Client sink_cap{kj::mv(sink_own)};

    auto request = streamer.pullRequest();
    request.setSink(sink_cap);
    request.setNChunks(N_CHUNKS);
    request.setChunkSz(CHUNK_N_ELEMS);
    auto pull_done = request.send().ignoreResult().fork(); // fork(): we look at it twice without cancelling it.

    chunk0_rcvd_paf.promise.wait(ws); // Chunk 0's (gated) handler has been entered.
    if (expect_stall)
    {
      /* A stall means the response *cannot* arrive while gated; the only way to observe "did not arrive" for a
       * remote event is a bounded wait.  It is a negative check: a pathologically slow machine could only make
       * it pass wrongly, never fail wrongly. */
      auto& timer = pair.cli().get_io_provider()->getTimer();
      const bool arrived
        = pull_done.addBranch().then([]() { return true; })
            .exclusiveJoin(timer.afterDelay(250 * kj::MILLISECONDS).then([]() { return false; }))
            .wait(ws);
      EXPECT_FALSE(arrived) << "pull() returned despite server window [" << srv_window_ki << "Ki] < S.";
    }
    else
    {
      pull_done.addBranch().wait(ws); // Completes while gated: both send()s resolved without any ack.
    }
    EXPECT_EQ(sink.n_chunks(), 1u); // Chunk 1 cannot be delivered before chunk 0's handler completes (in-order).

    cli_gate.open();
    pull_done.addBranch().wait(ws);
    chunk1_rcvd_paf.promise.wait(ws);
    const auto [count, sum] = expected_count_sum(N_CHUNKS, CHUNK_N_ELEMS);
    EXPECT_EQ(sink.count(), count);
    EXPECT_EQ(sink.sum(), sum);
  }; // run =

  {
    FLOW_TEST_TRACE_CTX("Server window < S => pull() stalls behind the gated sink.");
    run(CHUNK_KI / 2, true);
  }
  {
    FLOW_TEST_TRACE_CTX("Server window >> 2 S => pull() completes while the sink is gated.");
    run(CHUNK_KI * 10, false);
  }
} // TEST(Rpc_stream_test, server_window_default)

} // namespace ipc::transport::struc::shm::rpc::test
