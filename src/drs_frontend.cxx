#include "midas.h"
#include "mfe.h"
#include "drs_frontend_class.h"

#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <ucontext.h>
#include <pthread.h>
#include <sys/syscall.h>

/*------------------------------------------------------------------*/
/*  Crash handler: log signal + the actual faulting instruction       */
/*  pointer, plus a backtrace anchored at the crash site (not the    */
/*  handler).  Uses write(2) — async-signal-safe.                    */
/*------------------------------------------------------------------*/

static void drs4_crash_handler(int sig, siginfo_t *info, void *ucontext)
{
   const char *name =
      sig == SIGSEGV ? "SIGSEGV" :
      sig == SIGBUS  ? "SIGBUS"  :
      sig == SIGABRT ? "SIGABRT" :
      sig == SIGFPE  ? "SIGFPE"  : "SIGNAL";

   void *fault_addr = info ? info->si_addr : nullptr;
   void *caller = nullptr;
#if defined(__x86_64__)
   if (ucontext) {
      ucontext_t *uc = (ucontext_t *)ucontext;
      /* RIP is at a well-known offset in gregs on Linux/x86_64 */
      caller = (void *)uc->uc_mcontext.gregs[REG_RIP];
   }
#endif

   char hdr[256];
   int n = snprintf(hdr, sizeof(hdr),
                    "\n[DRS4,ERROR] *** %s ktid=%d ptid=%lu at addr=%p, crash PC=%p ***\n",
                    name, (int)syscall(SYS_gettid), (unsigned long)pthread_self(),
                    fault_addr, caller);
   (void)!write(STDERR_FILENO, hdr, n);
   cm_msg(MERROR, "DRS4Frontend",
          "*** %s ktid=%d ptid=%lu at addr=%p, crash PC=%p ***",
          name, (int)syscall(SYS_gettid), (unsigned long)pthread_self(),
          fault_addr, caller);

   /* Backtrace anchored at the crash site: skip this handler frame. */
   void *array[32];
   int depth = backtrace(array, 32);
   if (depth > 2) {
      char **syms = backtrace_symbols(array + 2, depth - 2);
      if (syms) {
         for (int i = 0; i < depth - 2; i++) {
            n = snprintf(hdr, sizeof(hdr), "  %s\n", syms[i]);
            (void)!write(STDERR_FILENO, hdr, n);
         }
         free(syms);
      }
   }

   signal(sig, SIG_DFL);
   raise(sig);
}

static void drs4_install_crash_handler()
{
   struct sigaction sa{};
   sa.sa_sigaction = drs4_crash_handler;
   sigemptyset(&sa.sa_mask);
   sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
   sigaction(SIGSEGV, &sa, nullptr);
   sigaction(SIGBUS,  &sa, nullptr);
   sigaction(SIGABRT, &sa, nullptr);
   sigaction(SIGFPE,  &sa, nullptr);
}

// Required MIDAS frontend globals
const char *frontend_name = "DRS4";
const char *frontend_file_name = __FILE__;
BOOL frontend_call_loop = FALSE;
INT display_period = 0;
INT max_event_size = 5 * 1024 * 1024;
INT max_event_size_frag = 5 * 1024 * 1024;
INT event_buffer_size = 10 * 1024 * 1024;
BOOL equipment_common_overwrite = TRUE;

// Forward declarations for EQUIPMENT readout pointer
INT read_trigger_event(char *pevent);

// Global frontend instance
static DRS4Frontend *gFe = nullptr;

// Equipment list
EQUIPMENT equipment[] = {
   {
      "DRS4",              // equipment name (single instance, no %02d)
      {
         1002, 0,           // event ID, trigger mask
         "SYSTEM",          // event buffer
         EQ_POLLED,          // equipment type
         0,                  // event source
         "MIDAS",            // format
         TRUE,               // enabled
         RO_RUNNING,          // read when running; do NOT write banks to /Equipment/DRS4/Variables/
         100,                // poll period (ms)
         0,                  // event limit
         0,                  // subevent limit
         0,                  // history period (0=disabled)
         "", "", "",          // log, foreground, background history
      },
      (INT(*)(char *, INT))read_trigger_event, // readout routine
      NULL,                                       // class driver (not used)
      NULL,                                       // device driver list (not used)
      NULL,                                       // init string (not used)
      NULL,                                       // cd_info (not used)
   },
   {""}
};

INT frontend_init()
{
   drs4_install_crash_handler();
   try {
      gFe = new DRS4Frontend();
      INT status = gFe->init("DRS4", __FILE__, get_frontend_index());
      return status;
   } catch (std::exception &e) {
      cm_msg(MERROR, "DRS400", "frontend_init exception: %s", e.what());
      return FE_ERR_HW;
   } catch (...) {
      cm_msg(MERROR, "DRS400", "frontend_init unknown exception");
      return FE_ERR_HW;
   }
}

INT frontend_exit()
{
   delete gFe;
   gFe = nullptr;
   return SUCCESS;
}

INT begin_of_run(INT run_number, char *error)
{
   if (gFe) return gFe->begin_of_run(run_number, error);
   return SUCCESS;
}

INT end_of_run(INT run_number, char *error)
{
   if (gFe) return gFe->end_of_run();
   return SUCCESS;
}

INT pause_run(INT run_number, char *error) { return SUCCESS; }
INT resume_run(INT run_number, char *error) { return SUCCESS; }

INT poll_event(INT source, INT count, BOOL test)
{
   if (!gFe) return 0;
   if (test) return gFe->is_data_available();
   return gFe->is_data_available();
}

INT read_trigger_event(char *pevent)
{
   if (gFe) return gFe->fill_midas_banks(pevent);
   return 0;
}

INT frontend_loop() {
   return SUCCESS;
}

INT interrupt_configure(INT cmd, INT source, POINTER_T adr) {
   return SUCCESS;
}