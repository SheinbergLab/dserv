// test_send_guard.cpp -- unit tests for the two primitives behind the
// send-cycle / slot-exhaustion hardening (2026-08-29 outage):
//
//   1. send_cycle_check (SendGuard.h): the publish-then-walk protocol must
//      refuse 2-hop and transitive cycles, never false-positive on benign
//      sends, and -- the racy part -- two threads entering a mutual cycle
//      simultaneously must NEVER both proceed (Dekker property of the
//      seq_cst store/load pair).  Exercised over many racing iterations.
//
//   2. SharedQueue::wait_pop: delivers immediately when an item is
//      pushed, times out cleanly when nothing arrives, and preserves
//      order under the existing single-consumer contract.
//
// Runs under ctest; exits nonzero on any failure.

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../src/SendGuard.h"
#include "../src/sharedqueue.h"

struct Node {
  std::string name;
  std::atomic<Node*> sending_to{nullptr};
  explicit Node(std::string n) : name(std::move(n)) {}
};

static int failures = 0;
static void check(bool ok, const std::string& what) {
  if (ok) {
    std::cout << "  ok: " << what << std::endl;
  } else {
    std::cout << "  FAIL: " << what << std::endl;
    failures++;
  }
}

int main()
{
  // ---- basic: benign send publishes and passes -----------------------
  {
    Node a("a"), b("b");
    std::string r = send_cycle_check(&a, &b);
    check(r.empty(), "benign send allowed");
    check(a.sending_to.load() == &b, "sender left published while blocked");
    a.sending_to.store(nullptr);  // caller's post-wait clear
  }

  // ---- 2-hop cycle: b is already parked sending to a -----------------
  {
    Node a("a"), b("b");
    b.sending_to.store(&a);       // b's thread is blocked in send to a
    std::string r = send_cycle_check(&a, &b);
    check(r == "a -> b -> a", "2-hop cycle detected with chain: " + r);
    check(a.sending_to.load() == nullptr, "refused sender unpublished");
  }

  // ---- transitive cycle: c -> a with a -> b -> c parked ---------------
  {
    Node a("a"), b("b"), c("c");
    a.sending_to.store(&b);
    b.sending_to.store(&c);
    std::string r = send_cycle_check(&c, &a);
    check(r == "c -> a -> b -> c", "3-hop cycle detected with chain: " + r);
    check(c.sending_to.load() == nullptr, "refused sender unpublished");
  }

  // ---- chain that does NOT come back is allowed ----------------------
  {
    Node a("a"), b("b"), c("c");
    b.sending_to.store(&c);       // b parked sending to c; c idle
    std::string r = send_cycle_check(&a, &b);
    check(r.empty(), "non-cycle chain allowed");
    a.sending_to.store(nullptr);
  }

  // ---- the race: mutual sends started simultaneously ------------------
  // The property that prevents the wedge: it must be IMPOSSIBLE for both
  // sides to proceed.  (Both refusing is acceptable -- loud + retryable.)
  {
    const int iters = 20000;
    int both_proceeded = 0, one_proceeded = 0, both_refused = 0;
    for (int i = 0; i < iters; i++) {
      Node a("a"), b("b");
      std::string ra, rb;
      std::atomic<int> go{0};
      std::thread ta([&] {
        go++; while (go.load() < 2) {}
        ra = send_cycle_check(&a, &b);
      });
      std::thread tb([&] {
        go++; while (go.load() < 2) {}
        rb = send_cycle_check(&b, &a);
      });
      ta.join(); tb.join();
      bool a_ok = ra.empty(), b_ok = rb.empty();
      if (a_ok && b_ok) both_proceeded++;
      else if (a_ok || b_ok) one_proceeded++;
      else both_refused++;
    }
    check(both_proceeded == 0,
          "racing mutual sends never both proceed ("
          + std::to_string(one_proceeded) + " one-side, "
          + std::to_string(both_refused) + " both-refused of "
          + std::to_string(iters) + ")");
  }

  // ---- wait_pop: timeout with nothing queued -------------------------
  {
    SharedQueue<std::string> q;
    std::string out;
    auto t0 = std::chrono::steady_clock::now();
    bool got = q.wait_pop(out, 100);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    check(!got, "wait_pop times out empty");
    check(ms >= 90, "timeout actually waited (~" + std::to_string(ms) + "ms)");
  }

  // ---- wait_pop: delivery wakes it early ------------------------------
  {
    SharedQueue<std::string> q;
    std::string out;
    std::thread producer([&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      q.push_back(std::string("hello"));
    });
    auto t0 = std::chrono::steady_clock::now();
    bool got = q.wait_pop(out, 5000);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    producer.join();
    check(got && out == "hello", "wait_pop delivers pushed item");
    check(ms < 4000, "delivery did not wait for the timeout");
  }

  // ---- wait_pop: order preserved, mixed with timeouts -----------------
  {
    SharedQueue<std::string> q;
    q.push_back(std::string("first"));
    q.push_back(std::string("second"));
    std::string a, b, c;
    bool g1 = q.wait_pop(a, 100);
    bool g2 = q.wait_pop(b, 100);
    bool g3 = q.wait_pop(c, 50);
    check(g1 && a == "first" && g2 && b == "second" && !g3,
          "order preserved; empty pop times out");
  }

  if (failures) {
    std::cout << failures << " FAILURE(S)" << std::endl;
    return 1;
  }
  std::cout << "ALL PASS" << std::endl;
  return 0;
}
