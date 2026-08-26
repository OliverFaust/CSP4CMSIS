#include "csp/csp4cmsis.h"

#include <cstdio>

// =================================================================================
// REGRESSION TEST: ALT-vs-ALT rendezvous never completes
// =================================================================================
//
// Bug summary
// -----------
// csp::internal::ChanInGuard::activate() and csp::internal::ChanOutGuard::activate()
// (lib/csp4cmsis/src/alt_channel_sync.cpp) only perform the memcpy + wake-up when
// the *partner* is a plain blocking task (AltChanSyncBase::getWaitingInTask() /
// getWaitingOutTask() != nullptr):
//
//     void ChanOutGuard::activate() {
//         ...
//         TaskHandle_t receiver = parent_channel->getWaitingInTask();
//         if (receiver != nullptr) {
//             memcpy(...);
//             ...
//             xTaskNotifyGive(receiver);
//         } else {
//             // <-- taken when the partner registered via Alternative instead
//             //     of a plain blocking input()/output() call. No memcpy.
//             //     No wakeUp(). Silently does nothing.
//             xSemaphoreGive(parent_channel->getMutex());
//         }
//     }
//
// But Guard::enable() (which decides whether the ALT can fire immediately) is
// satisfied by *either* a waiting task *or* a waiting ALT registration
// (AltChanSyncBase::hasReaderWaiting() / hasWriterWaiting() both check
// `waiting_in_alt.isActive()` / `waiting_out_alt.isActive()`).
//
// Net effect: when BOTH sides of a rendezvous use csp::Alternative (ALT-vs-ALT,
// as opposed to one side calling the plain blocking Chanin<T>::read /
// Chanout<T>::write), the side whose enable() returns true "wins" its select()
// immediately and reports success -- but activate() never actually copies the
// data and never wakes the other side's AltScheduler. The partner is left
// registered and blocked forever (until some other guard, e.g. a timeout,
// fires on its own).
//
// Test design
// -----------
// Two channels, exercised by one Receiver process that uses Alternative on both,
// one after the other:
//
//   1. CONTROL channel: ControlSender writes with a plain blocking call
//      (`out << msg`, NOT via Alternative). Receiver reads it via Alternative.
//      This is the task -> ALT path, which the analysis found to work correctly,
//      and confirms the test harness itself is sound.
//
//   2. BUG channel: AltSender ALSO uses Alternative (with its own timeout guard)
//      to write. Receiver reads it via Alternative (with its own timeout guard).
//      This is the ALT -> ALT path.
//
//        - Expected result if the bug is PRESENT: AltSender's select() reports
//          the write succeeded, but Receiver's select() times out -- no data
//          ever arrives, because activate() silently no-op'd on both sides.
//        - Expected result AFTER A FIX: Receiver's select() reports the
//          channel guard fired and the correct data was delivered.
//
// This file is a drop-in replacement for Core/Src/application.cpp (same
// csp_app_main_init() / MainApp_Task entry points), so it can be swapped in and
// out without touching the rest of the STM32/FreeRTOS project scaffolding.
// =================================================================================

using namespace csp;

#define MAIN_APP_STACK_WORDS 2048

// Generous timeouts: the point of this test is correctness, not speed. If the
// bug is fixed, both phases resolve in well under a second; if the bug is
// present, the BUG-channel wait is expected to run out its full timeout.
#define CONTROL_TIMEOUT_MS 1000
#define BUG_TIMEOUT_MS     1000
#define SENDER_TIMEOUT_MS  500

// How long AltSender waits before touching the BUG channel. This must be long
// enough that the Receiver has already reached its second Alternative and
// registered (blocked in Phase 2) before AltSender's enable() runs, so the
// "already ready" (ALT-vs-ALT) branch is exercised deterministically rather
// than by chance.
#define ALT_SENDER_STARTUP_DELAY_MS 200

struct Message {
  int tag;
  int value;
};

using TestChannel = Channel<Message>;

// ---------------------------------------------------------------------------
// Phase A partner: plain blocking writer (task -> ALT path). Expected to work.
// ---------------------------------------------------------------------------
class ControlSender : public CSProcessStatic<256> {
private:
  Chanout<Message> out;
public:
  explicit ControlSender(Chanout<Message> w) : out(w) {}

  const char* name() const override { return "ControlSender"; }

  void run() override {
    vTaskDelay(pdMS_TO_TICKS(20));
    Message msg{/*tag=*/0, /*value=*/111};
    printf("[ControlSender] Blocking write on CONTROL channel (task->ALT path)...\r\n");
    out << msg;  // Plain blocking output(), NOT via Alternative.
    printf("[ControlSender] Write returned.\r\n");
    while (true) {
      vTaskDelay(portMAX_DELAY);
    }
  }
};

// ---------------------------------------------------------------------------
// Phase B partner: writer that ALSO uses Alternative (ALT -> ALT path). This
// is the side of the rendezvous that triggers the buggy activate() branch.
// ---------------------------------------------------------------------------
class AltSender : public CSProcessStatic<512> {
private:
  Chanout<Message> out;
public:
  explicit AltSender(Chanout<Message> w) : out(w) {}

  const char* name() const override { return "AltSender"; }

  void run() override {
    // Let the Receiver's second Alternative register on the BUG channel
    // (and start blocking) before we attempt the write.
    vTaskDelay(pdMS_TO_TICKS(ALT_SENDER_STARTUP_DELAY_MS));

    Message msg{/*tag=*/1, /*value=*/222};
    RelTimeoutGuard timeout(Milliseconds(SENDER_TIMEOUT_MS));

    // NOTE: deliberately built via the initializer_list<internal::Guard*>
    // constructor rather than the variadic pipe-syntax constructor
    // (`Alternative(out | msg, timeout)`). The variadic constructor takes
    // its Bindings by value, which would copy this stack-local
    // RelTimeoutGuard (and the FreeRTOS timer handle owned by its
    // internal::TimerGuard member) -- an unrelated, separate footgun this
    // test intentionally avoids so it isolates just the bug under test.
    Alternative alt({out.getGuard(msg), timeout.internal_guard_ptr});

    printf("[AltSender] Selecting write on BUG channel (ALT->ALT path)...\r\n");
    int selected = alt.priSelect();

    if (selected == 0) {
      printf("[AltSender] select() reports the write SUCCEEDED (channel guard fired).\r\n");
    } else {
      printf("[AltSender] select() timed out trying to write (unexpected -- a reader "
             "was known to be waiting).\r\n");
    }

    while (true) {
      vTaskDelay(portMAX_DELAY);
    }
  }
};

// ---------------------------------------------------------------------------
// Receiver: runs Phase A (control/sanity check) then Phase B (bug repro),
// each as its own short-lived Alternative with a timeout guard, and prints a
// verdict at the end.
// ---------------------------------------------------------------------------
class Receiver : public CSProcessStatic<512> {
private:
  Chanin<Message> inControl;
  Chanin<Message> inBug;
public:
  Receiver(Chanin<Message> control, Chanin<Message> bug)
    : inControl(control), inBug(bug) {}

  const char* name() const override { return "Receiver"; }

  void run() override {
    vTaskDelay(pdMS_TO_TICKS(10));

    bool control_ok = false;
    bool bug_present = false;

    // --- Phase A: control check (task -> ALT). Expected to succeed quickly. ---
    {
      Message msg{};
      RelTimeoutGuard timeout(Milliseconds(CONTROL_TIMEOUT_MS));
      Alternative alt({inControl.getGuard(msg), timeout.internal_guard_ptr});

      printf("[Receiver] Waiting on CONTROL channel (task->ALT path)...\r\n");
      int selected = alt.priSelect();

      if (selected == 0 && msg.tag == 0 && msg.value == 111) {
        control_ok = true;
        printf("[Receiver] CONTROL OK: got tag=%d value=%d\r\n", msg.tag, msg.value);
      } else {
        printf("[Receiver] CONTROL FAILED: selected=%d. The harness itself is broken -- "
               "the BUG-channel result below is not meaningful.\r\n", selected);
      }
    }

    // --- Phase B: bug repro (ALT -> ALT). Expected (buggy) result: timeout. ---
    {
      Message msg{};
      RelTimeoutGuard timeout(Milliseconds(BUG_TIMEOUT_MS));
      Alternative alt({inBug.getGuard(msg), timeout.internal_guard_ptr});

      printf("[Receiver] Waiting on BUG channel (ALT->ALT path)...\r\n");
      int selected = alt.priSelect();

      if (selected == 0) {
        printf("[Receiver] BUG channel delivered tag=%d value=%d -- rendezvous completed "
               "normally.\r\n", msg.tag, msg.value);
      } else {
        bug_present = true;
        printf("[Receiver] BUG channel TIMED OUT after %d ms, waiting for data that "
               "AltSender's select() already reported as delivered.\r\n", BUG_TIMEOUT_MS);
      }
    }

    printf("\r\n--- RESULT ---\r\n");
    if (!control_ok) {
      printf("INCONCLUSIVE: the CONTROL phase itself failed. Fix the test harness before "
             "trusting the BUG-channel result.\r\n");
    } else if (bug_present) {
      printf("BUG CONFIRMED: ALT-vs-ALT rendezvous never transferred data and never woke "
             "the waiting side, even though the sender's Alternative reported success.\r\n");
    } else {
      printf("BUG NOT REPRODUCED: ALT-vs-ALT rendezvous completed correctly. If this test "
             "was run against a fix for the activate() gap, this is the expected result.\r\n");
    }

    while (true) {
      vTaskDelay(portMAX_DELAY);
    }
  }
};

static TaskHandle_t s_main_app_task_handle = NULL;

void MainApp_Task(void* params) {
  vTaskDelay(pdMS_TO_TICKS(10));

  printf("\r\n--- CSP4CMSIS Regression Test: ALT-vs-ALT rendezvous (Guard::activate) ---\r\n");

  // Static storage (.data segment) -- no dynamic allocation, matching the
  // zero-heap style of the original application.cpp.
  static TestChannel chan_control;
  static TestChannel chan_bug;

  static ControlSender control_sender(chan_control.writer());
  static AltSender      alt_sender(chan_bug.writer());
  static Receiver        receiver(chan_control.reader(), chan_bug.reader());

  Run(InParallel(control_sender, alt_sender, receiver), ExecutionMode::StaticNetwork);

  printf("*** MainApp_Task: Run() returned, network finished. Terminating. ***\r\n");
  vTaskDelete(NULL);
}

void RunProcessingChainTest(void) {
    // Note: Task creation is the only 'dynamic' part remaining, standard for FreeRTOS
    BaseType_t status = xTaskCreate(MainApp_Task, "MainApp", MAIN_APP_STACK_WORDS, NULL,
                                     tskIDLE_PRIORITY + 3, &s_main_app_task_handle);
    if (status != pdPASS) {
        printf("ERROR: MainApp_Task creation failed!\r\n");
    }
}
