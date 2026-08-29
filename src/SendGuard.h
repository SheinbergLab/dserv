#ifndef SENDGUARD_H
#define SENDGUARD_H

#include <atomic>
#include <string>

/*
 * Send-cycle guard.
 *
 * Every TclServer interp has exactly ONE evaluation thread; a synchronous
 * `send` parks that thread until the target replies.  If the chain of
 * parked senders leads back to the sender (ess -> configs -> ess, the
 * 2026-08-29 outage), every interp in the loop is waiting on the next and
 * none can ever run the queued script: all of them wedge permanently, and
 * every later client of a wedged interp then pins a connection slot
 * forever (see message_client_process).
 *
 * Protocol: the sender PUBLISHES where it is about to block first
 * (self->sending_to = target), then walks the chain starting at the
 * target.  Reaching itself means a cycle: unpublish and refuse.  Both the
 * store and the loads are seq_cst, so two senders racing into a mutual
 * cycle cannot both miss each other's store (the Dekker property): at
 * least one of them walks into the other and errors out loudly instead of
 * deadlocking.  In the worst race BOTH refuse -- loud and retryable, never
 * wedged.  `sending_to` is only ever written by its own interp's
 * evaluation thread.
 *
 * Templated over the server type (needs `std::atomic<S*> sending_to` and
 * `std::string name`) so the identical logic is unit-testable on a
 * minimal struct (tests/test_send_guard.cpp) without a TclServer.
 *
 * Returns an empty string when the send is safe to block on -- with
 * self->sending_to LEFT PUBLISHED (the caller must clear it after its
 * blocking wait completes).  Returns the human-readable cycle chain
 * ("ess -> configs -> ess") when a cycle was detected, with
 * self->sending_to already cleared.
 */
template <typename S>
inline std::string send_cycle_check(S *self, S *target)
{
  self->sending_to.store(target);

  std::string chain = self->name;
  S *t = target;
  for (int hops = 0; t && hops < 16; hops++) {
    chain += " -> " + t->name;
    if (t == self) {
      self->sending_to.store(nullptr);
      return chain;
    }
    t = t->sending_to.load();
  }
  return std::string();
}

#endif
