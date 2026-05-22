#include "midas.h"
#include "mfe.h"
#include "drs_frontend_class.h"

// Required MIDAS frontend globals
const char *frontend_name = "DRS4";
const char *frontend_file_name = __FILE__;
BOOL frontend_call_loop = FALSE;
INT display_period = 0;
INT max_event_size = 5 * 1024 * 1024;
INT max_event_size_frag = 5 * 1024 * 1024;
INT event_buffer_size = 10 * 1024 * 1024;
BOOL equipment_common_overwrite = TRUE;

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
         RO_RUNNING | RO_ODB, // read when running + update ODB
         100,                // poll period (ms)
         0,                  // event limit
         0,                  // subevent limit
         0,                  // history period (0=disabled)
         "", "", "",          // log, foreground, background history
      },
      NULL, NULL, NULL,     // readout, class, init
      NULL, NULL,           // bank list
   },
   {""}
};

INT frontend_init()
{
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