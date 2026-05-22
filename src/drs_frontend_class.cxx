#include "drs_frontend_class.h"
#include "midas.h"
#include "msystem.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <chrono>
#include <algorithm>
#include <cmath>

/*------------------------------------------------------------------*/

template<typename T>
T DRS4Frontend::odb_get(midas::odb &odb, const std::string &key, T default_val)
{
   try {
      return (T)odb[key];
   } catch (...) {
      return default_val;
   }
}

/*------------------------------------------------------------------*/

DRS4Frontend::DRS4Frontend()
{
   memset(m_boards, 0, sizeof(m_boards));
}

DRS4Frontend::~DRS4Frontend()
{
   if (m_drs) {
      delete m_drs;
      m_drs = nullptr;
   }
}

/*------------------------------------------------------------------*/

void DRS4Frontend::MidasDRSCallback::Progress(int value)
{
   try {
      m_settings["CalibProgress"] = value;
   } catch (...) {}
}

/*------------------------------------------------------------------*/
/*  ODB defaults for a single board                                 */
/*------------------------------------------------------------------*/

void DRS4Frontend::create_board_defaults(int board_index)
{
   char path[256];
   snprintf(path, sizeof(path), "%s/Boards/Board%d", m_settings_path.c_str(), board_index);

   midas::odb board({
      {"SerialNumber",      0},
      {"BoardType",         0},
      {"Connect",           true},
      {"Frequency (GHz)",   5.0},
      {"InputRange",        std::string("-0.5V to +0.5V")},
      {"Options InputRange", std::array<std::string, 2>{"-0.5V to +0.5V", "0V to +1V"}},
      {"TriggerLogic",      std::string("OR")},
      {"Options TriggerLogic", std::array<std::string, 2>{"OR", "AND"}},
      {"EnableCH1",         true},
      {"EnableCH2",         true},
      {"EnableCH3",         true},
      {"EnableCH4",         true},
      {"TriggerLevel_CH0 (V)", 0.05},
      {"TriggerLevel_CH1 (V)", 0.05},
      {"TriggerLevel_CH2 (V)", 0.05},
      {"TriggerLevel_CH3 (V)", 0.05},
      {"TriggerMode",           std::string("Auto")},
      {"Options TriggerMode",   std::array<std::string, 2>{"Auto", "Normal"}},
      {"TriggerPolarity",   std::string("RISING")},
      {"Options TriggerPolarity", std::array<std::string, 2>{"RISING", "FALLING"}},
      {"TriggerDelayNs",    0},
      {"DominoActive",      std::string("AlwaysRunning")},
      {"Options DominoActive", std::array<std::string, 2>{"AlwaysRunning", "StopOnReadout"}},
      {"ReadoutMode",       std::string("FROM_FIRST_BIN")},
      {"Options ReadoutMode", std::array<std::string, 2>{"FROM_FIRST_BIN", "FROM_STOP"}},
      {"TranspMode",        true},
      {"EnableTcal",        false},
      {"NChannels",         4},
      {"CalibrateVoltage",  false},
      {"CalibrateTiming",   false},
   });

   board.connect_and_fix_structure(path);
}

/*------------------------------------------------------------------*/
/*  Scan USB for DRS4 boards                                        */
/*------------------------------------------------------------------*/

void DRS4Frontend::scan_boards()
{
   if (m_drs) {
      delete m_drs;
      m_drs = nullptr;
   }

   m_drs = new DRS();

   char err[256];
   if (m_drs->GetError(err, sizeof(err))) {
      cm_msg(MERROR, "DRS4Frontend", "DRS scan error: %s", err);
   }

   m_num_boards = m_drs->GetNumberOfBoards();
   cm_msg(MINFO, "DRS4Frontend", "Found %d DRS4 board(s)", m_num_boards);

   if (m_num_boards > DRS4_MAX_BOARDS)
      m_num_boards = DRS4_MAX_BOARDS;

   for (int i = 0; i < m_num_boards; i++) {
      m_boards[i] = m_drs->GetBoard(i);
      if (m_boards[i]) {
         m_boards[i]->Init();
         cm_msg(MINFO, "DRS4Frontend", "Board %d: S/N after Init=%d, type=%d",
                i, m_boards[i]->GetBoardSerialNumber(), m_boards[i]->GetBoardType());
         m_board_hw_connected[i] = true;
      }
   }
   for (int i = m_num_boards; i < DRS4_MAX_BOARDS; i++) {
      m_boards[i] = nullptr;
   }
}

/*------------------------------------------------------------------*/
/*  Rescan USB and update ODB                                       */
/*------------------------------------------------------------------*/

void DRS4Frontend::rescan_boards()
{
   m_settings["Status"] = std::string("Scanning USB for boards...");

   for (int i = 0; i < DRS4_MAX_BOARDS; i++) {
      m_board_hw_connected[i] = false;
   }

   scan_boards();

   HNDLE hDB, hKey;
   if (cm_get_experiment_database(&hDB, NULL) == SUCCESS) {
      char board_path[256];
      for (int i = 0; i < DRS4_MAX_BOARDS; i++) {
         snprintf(board_path, sizeof(board_path), "%s/Boards/Board%d", m_settings_path.c_str(), i);
         if (db_find_key(hDB, 0, board_path, &hKey) == SUCCESS) {
            db_delete_key(hDB, hKey, FALSE);
         }
      }
   }

   m_settings.connect(m_settings_path);

   for (int i = 0; i < m_num_boards; i++) {
      char board_path[256];
      snprintf(board_path, sizeof(board_path), "%s/Boards/Board%d", m_settings_path.c_str(), i);
      midas::odb board;
      board.connect_and_fix_structure(board_path);
      create_board_defaults(i);
      read_board_info(i);
   }

   m_settings["Status"] = std::string("Scan complete, " + std::to_string(m_num_boards) + " board(s) found");
   cm_msg(MINFO, "DRS4Frontend", "Rescan complete: %d board(s)", m_num_boards);
}

/*------------------------------------------------------------------*/
/*  Read board info into ODB                                        */
/*------------------------------------------------------------------*/

void DRS4Frontend::read_board_info(int i)
{
   if (i < 0 || i >= m_num_boards || !m_boards[i]) {
      cm_msg(MERROR, "DRS4Frontend", "read_board_info(%d): invalid board", i);
      return;
   }

   int sn = m_boards[i]->GetBoardSerialNumber();
   int bt = m_boards[i]->GetBoardType();

   HNDLE hDB, hKey;
   if (cm_get_experiment_database(&hDB, NULL) != SUCCESS) {
      cm_msg(MERROR, "DRS4Frontend", "Cannot get database handle");
      return;
   }

   char board_path[256];
   snprintf(board_path, sizeof(board_path), "%s/Boards/Board%d", m_settings_path.c_str(), i);
   if (db_find_key(hDB, 0, board_path, &hKey) != SUCCESS) {
      cm_msg(MERROR, "DRS4Frontend", "read_board_info(%d): Board%d ODB path does not exist", i, i);
      return;
   }

   char path[256];
   int sn_val = sn;
   int bt_val = bt;

   snprintf(path, sizeof(path), "%s/Boards/Board%d/SerialNumber", m_settings_path.c_str(), i);
   db_set_value(hDB, 0, path, &sn_val, sizeof(sn_val), 1, TID_INT);

   snprintf(path, sizeof(path), "%s/Boards/Board%d/BoardType", m_settings_path.c_str(), i);
   db_set_value(hDB, 0, path, &bt_val, sizeof(bt_val), 1, TID_INT);

   cm_msg(MINFO, "DRS4Frontend", "Wrote S/N %d and type %d to ODB for Board%d", sn, bt, i);
}

/*------------------------------------------------------------------*/
/*  Setup ODB structure                                             */
/*------------------------------------------------------------------*/

void DRS4Frontend::setup_odb_structure()
{
   midas::odb default_settings({
      {"Status",              std::string("Initialized")},
      {"Refresh",             false},
   });

   default_settings.connect_and_fix_structure(m_settings_path);
   m_settings.connect(m_settings_path);

   for (int i = 0; i < m_num_boards; i++) {
      create_board_defaults(i);
   }
}

/*------------------------------------------------------------------*/
/*  Connect/disconnect                                              */
/*------------------------------------------------------------------*/

void DRS4Frontend::connect_board(int i)
{
   if (i < 0 || i >= m_num_boards || !m_boards[i]) return;
   if (m_board_hw_connected[i]) return;

   int status = m_boards[i]->Init();
   if (status != 1) {
      cm_msg(MERROR, "DRS4Frontend", "Failed to init board %d (serial %d)",
             i, m_boards[i]->GetBoardSerialNumber());
      return;
   }

   m_board_hw_connected[i] = true;
   read_board_info(i);
   cm_msg(MINFO, "DRS4Frontend", "Connected board %d (serial %d, type %d)",
          i, m_boards[i]->GetBoardSerialNumber(), m_boards[i]->GetBoardType());
}

void DRS4Frontend::disconnect_board(int i)
{
   if (i < 0 || i >= DRS4_MAX_BOARDS) return;
   m_board_hw_connected[i] = false;
}

void DRS4Frontend::connect_all()
{
   for (int i = 0; i < m_num_boards; i++) {
      connect_board(i);
   }
}

void DRS4Frontend::disconnect_all()
{
   for (int i = 0; i < DRS4_MAX_BOARDS; i++) {
      disconnect_board(i);
   }
}

void DRS4Frontend::recalc_global_connect_status()
{
   int n_conn = 0, n_total = 0;
   for (int i = 0; i < m_num_boards; i++) {
      try {
         char path[256];
         snprintf(path, sizeof(path), "%s/Boards/Board%d/Connect", m_settings_path.c_str(), i);
         midas::odb c;
         c.connect(path);
         if ((bool)c) n_conn++;
         n_total++;
      } catch (...) {}
   }

   std::string val;
   if (n_conn == 0) val = "ALL_DISCONNECTED";
   else if (n_conn == n_total) val = "ALL_CONNECTED";
   else val = "MIXED";

   m_last_connect_val = val;
   m_settings["ConnectBoards"] = val;
}

/*------------------------------------------------------------------*/
/*  Apply board configuration from ODB                              */
/*------------------------------------------------------------------*/

void DRS4Frontend::apply_board_config(int i)
{
   if (!m_boards[i] || !m_board_hw_connected[i]) return;

   char path[256];
   snprintf(path, sizeof(path), "%s/Boards/Board%d", m_settings_path.c_str(), i);

   try {
      midas::odb b;
      b.connect(path);

      // Input range: ODB stores string like "-0.5V to +0.5V" or "0V to +1V"
      // SetInputRange expects center voltage: -0.5V range -> 0.0, 0V..+1V range -> 0.5
      std::string input_range_str = odb_get<std::string>(b, "InputRange", "-0.5V to +0.5V");
      double input_range_center = 0.0;
      if (input_range_str.find("0V to +1V") != std::string::npos) {
         input_range_center = 0.5;
      }
      m_boards[i]->SetInputRange(input_range_center);

      // ---- REG_CTRL writes first (official DRS trigger setup order) ----
      // For board type 8/9 (USB Eval 4.0), BIT_ENABLE_TRIGGER1 (bit 22) is the
      // general trigger enable.  BIT_ENABLE_TRIGGER2 (bit 31) is for older boards.
      // Official code: Osci::SetTriggerSource/SetTriggerConfig use EnableTrigger(1,0).

      int boardType = m_boards[i]->GetBoardType();

      // Decimation: turn off (match official software)
      m_boards[i]->SetDecimation(0);

      // Transparent mode (required for analog trigger, firmware >= 21699)
      bool transp = odb_get<bool>(b, "TranspMode", true);
      int fw = m_boards[i]->GetFirmwareVersion();
      if (fw >= 21699) {
         m_boards[i]->SetTranspMode(transp ? 1 : 0);
      } else {
         m_boards[i]->SetTranspMode(0);
      }

      // DominoActive: AlwaysRunning keeps domino rolling
      std::string domino_active = odb_get<std::string>(b, "DominoActive", "AlwaysRunning");
      m_boards[i]->SetDominoActive(domino_active == "AlwaysRunning" ? 1 : 0);

      // Trigger polarity
      std::string pol = odb_get<std::string>(b, "TriggerPolarity", "RISING");
      m_boards[i]->SetTriggerPolarity(pol == "FALLING");

      // Readout mode
      std::string ro_mode = odb_get<std::string>(b, "ReadoutMode", "FROM_FIRST_BIN");
      m_boards[i]->SetReadoutMode(ro_mode == "FROM_STOP" ? 1 : 0);

      // Timing calibration signal
      bool tcal = odb_get<bool>(b, "EnableTcal", false);
      m_boards[i]->EnableTcal(tcal ? 1 : 0);

      // Trigger enable — board-type-dependent (must match official software).
      // Board type 8/9: EnableTrigger(1,0) sets BIT_ENABLE_TRIGGER1 (bit 22),
      // the general trigger enable.  The trigger source (analog vs. ext) is
      // determined by the REG_TRG_CONFIG mask below.
      // Board type 5/7: EnableTrigger(0,1) sets BIT_ENABLE_TRIGGER2 (bit 31)
      // for the analog threshold comparator.
      if (boardType == 8 || boardType == 9) {
         m_boards[i]->EnableTrigger(1, 0);
      } else {
         m_boards[i]->EnableTrigger(0, 1);
      }

      // ---- REG_TRG_CONFIG after all REG_CTRL bits are set ----

      // Trigger source mask for REG_TRG_CONFIG.
      // OR  path (bits 0-3): fires when ANY enabled channel exceeds threshold.
      // AND path (bits 8-11): fires when ALL enabled channels exceed threshold.
      // These are independent — setting both defeats AND (OR always fires first).
      bool enable_ch[4];
      enable_ch[0] = odb_get<bool>(b, "EnableCH1", true);
      enable_ch[1] = odb_get<bool>(b, "EnableCH2", true);
      enable_ch[2] = odb_get<bool>(b, "EnableCH3", true);
      enable_ch[3] = odb_get<bool>(b, "EnableCH4", true);
      std::string trig_logic = odb_get<std::string>(b, "TriggerLogic", "OR");

      // Direct ODB read for debug comparison
      std::string trig_logic_direct = "UNREADABLE";
      try {
         trig_logic_direct = (std::string)b["TriggerLogic"];
      } catch (std::exception &e) {
         trig_logic_direct = std::string("EXCEPTION: ") + e.what();
      }

      unsigned int src_mask = 0;
      int bit_offset = (trig_logic == "AND") ? 8 : 0;  // OR=bits 0-3, AND=bits 8-11
      for (int ch = 0; ch < 4; ch++) {
         if (enable_ch[ch]) {
            src_mask |= (1 << (ch + bit_offset));
         }
      }
      cm_msg(MINFO, "DRS4Frontend", "apply_board_config[%d]: trig_logic='%s' direct='%s' -> bit_offset=%d -> src_mask=0x%04x",
             i, trig_logic.c_str(), trig_logic_direct.c_str(), bit_offset, src_mask);
      m_boards[i]->SetTriggerSource(src_mask);

      // ---- REG_CONFIG, REG_TRG_DELAY, DAC levels ----

      // Frequency FIRST — SetFrequency internally starts domino,
      // waits for PLL lock, then calls SoftTrigger. Do NOT call
      // StartDomino() again until after SoftTrigger in the capture flow.
      double freq = odb_get<double>(b, "Frequency (GHz)", 5.0);
      m_boards[i]->SetFrequency(freq, true);

      // DominoMode=1 (continuous) for live preview.  Must be set AFTER
      // SetFrequency so the config register gets the right domino mode.
      m_boards[i]->SetDominoMode(1);

      // Trigger delay (ns)
      int delay = odb_get<int>(b, "TriggerDelayNs", 0);
      m_boards[i]->SetTriggerDelayNs(delay);

      // Individual trigger levels for channels 0-3
      m_boards[i]->SetIndividualTriggerLevel(0, odb_get<double>(b, "TriggerLevel_CH0 (V)", 0.05));
      m_boards[i]->SetIndividualTriggerLevel(1, odb_get<double>(b, "TriggerLevel_CH1 (V)", 0.05));
      m_boards[i]->SetIndividualTriggerLevel(2, odb_get<double>(b, "TriggerLevel_CH2 (V)", 0.05));
      m_boards[i]->SetIndividualTriggerLevel(3, odb_get<double>(b, "TriggerLevel_CH3 (V)", 0.05));

      // Disable analog calibration (match official software init)
      m_boards[i]->EnableAcal(0, 0);

      cm_msg(MINFO, "DRS4Frontend", "Applied config to board %d", i);

   } catch (std::exception &e) {
      cm_msg(MERROR, "DRS4Frontend", "Error applying config to board %d: %s", i, e.what());
   }
}

void DRS4Frontend::apply_all_configs()
{
   for (int i = 0; i < m_num_boards; i++) {
      apply_board_config(i);
   }
}

/*------------------------------------------------------------------*/
/*  Calibration                                                     */
/*------------------------------------------------------------------*/

void DRS4Frontend::do_calibrate_all()
{
   MidasDRSCallback cb(m_settings);
   char msg[256];

   for (int i = 0; i < m_num_boards; i++) {
      if (!m_boards[i]) continue;
      snprintf(msg, sizeof(msg), "Board%d: Starting voltage calibration...", i);
      m_settings["Status"] = std::string(msg);
      cm_msg(MINFO, "DRS4Frontend", "%s", msg);
      int rc = m_boards[i]->CalibrateVolt(&cb);
      if (rc == 1) {
         snprintf(msg, sizeof(msg), "Board%d: Voltage calibration done", i);
         cm_msg(MINFO, "DRS4Frontend", "%s", msg);
      } else {
         snprintf(msg, sizeof(msg), "Board%d: Voltage calibration FAILED", i);
         cm_msg(MERROR, "DRS4Frontend", "%s", msg);
      }
      m_settings["Status"] = std::string(msg);
   }

   for (int i = 0; i < m_num_boards; i++) {
      if (!m_boards[i]) continue;
      snprintf(msg, sizeof(msg), "Board%d: Starting timing calibration...", i);
      m_settings["Status"] = std::string(msg);
      cm_msg(MINFO, "DRS4Frontend", "%s", msg);
      int rc = m_boards[i]->CalibrateTiming(&cb);
      if (rc == 1) {
         snprintf(msg, sizeof(msg), "Board%d: Timing calibration done", i);
         cm_msg(MINFO, "DRS4Frontend", "%s", msg);
      } else {
         snprintf(msg, sizeof(msg), "Board%d: Timing calibration FAILED", i);
         cm_msg(MERROR, "DRS4Frontend", "%s", msg);
      }
      m_settings["Status"] = std::string(msg);
   }

   m_settings["Status"] = std::string("All calibrations complete");
}

void DRS4Frontend::do_calibrate_board(int board_idx)
{
   if (board_idx < 0 || board_idx >= m_num_boards || !m_board_hw_connected[board_idx]) return;

   MidasDRSCallback cb(m_settings);

   char msg[256];
   snprintf(msg, sizeof(msg), "Calibrating voltage board %d...", board_idx);
   m_settings["Status"] = std::string(msg);
   int rc = m_boards[board_idx]->CalibrateVolt(&cb);
   if (rc == 1)
      cm_msg(MINFO, "DRS4Frontend", "Voltage calibration done for board %d", board_idx);
   else
      cm_msg(MERROR, "DRS4Frontend", "Voltage calibration failed for board %d", board_idx);
   m_settings["CalibProgress"] = 50;

   snprintf(msg, sizeof(msg), "Calibrating timing board %d...", board_idx);
   m_settings["Status"] = std::string(msg);
   rc = m_boards[board_idx]->CalibrateTiming(&cb);
   if (rc == 1)
      cm_msg(MINFO, "DRS4Frontend", "Timing calibration done for board %d", board_idx);
   else
      cm_msg(MERROR, "DRS4Frontend", "Timing calibration failed for board %d", board_idx);
   m_settings["CalibProgress"] = 100;
}

void DRS4Frontend::do_calibrate_voltage_board(int board_idx)
{
   if (board_idx < 0 || board_idx >= m_num_boards || !m_boards[board_idx]) return;

   MidasDRSCallback cb(m_settings);
   char msg[256];
   snprintf(msg, sizeof(msg), "Board%d: Starting voltage calibration...", board_idx);
   m_settings["Status"] = std::string(msg);
   cm_msg(MINFO, "DRS4Frontend", "%s", msg);
   int rc = m_boards[board_idx]->CalibrateVolt(&cb);
   if (rc == 1) {
      snprintf(msg, sizeof(msg), "Board%d: Voltage calibration done", board_idx);
      cm_msg(MINFO, "DRS4Frontend", "%s", msg);
   } else {
      snprintf(msg, sizeof(msg), "Board%d: Voltage calibration FAILED", board_idx);
      cm_msg(MERROR, "DRS4Frontend", "%s", msg);
   }
   m_settings["Status"] = std::string(msg);
}

void DRS4Frontend::do_calibrate_timing_board(int board_idx)
{
   if (board_idx < 0 || board_idx >= m_num_boards || !m_boards[board_idx]) return;

   MidasDRSCallback cb(m_settings);
   char msg[256];
   snprintf(msg, sizeof(msg), "Board%d: Starting timing calibration...", board_idx);
   m_settings["Status"] = std::string(msg);
   cm_msg(MINFO, "DRS4Frontend", "%s", msg);
   int rc = m_boards[board_idx]->CalibrateTiming(&cb);
   if (rc == 1) {
      snprintf(msg, sizeof(msg), "Board%d: Timing calibration done", board_idx);
      cm_msg(MINFO, "DRS4Frontend", "%s", msg);
   } else {
      snprintf(msg, sizeof(msg), "Board%d: Timing calibration FAILED", board_idx);
      cm_msg(MERROR, "DRS4Frontend", "%s", msg);
   }
   m_settings["Status"] = std::string(msg);
}

void DRS4Frontend::do_calibrate_voltage()
{
   MidasDRSCallback cb(m_settings);

   for (int i = 0; i < m_num_boards; i++) {
      if (!m_board_hw_connected[i]) continue;
      cm_msg(MINFO, "DRS4Frontend", "Starting voltage calibration for board %d", i);
      m_settings["Status"] = std::string("Calibrating voltage board " + std::to_string(i));
      int rc = m_boards[i]->CalibrateVolt(&cb);
      if (rc == 1)
         cm_msg(MINFO, "DRS4Frontend", "Voltage calibration done for board %d", i);
      else
         cm_msg(MERROR, "DRS4Frontend", "Voltage calibration failed for board %d", i);
   }
   m_settings["CalibrateVoltage"] = false;
   m_settings["Status"] = std::string("Voltage calibration complete");
}

void DRS4Frontend::do_calibrate_timing()
{
   MidasDRSCallback cb(m_settings);

   for (int i = 0; i < m_num_boards; i++) {
      if (!m_board_hw_connected[i]) continue;
      cm_msg(MINFO, "DRS4Frontend", "Starting timing calibration for board %d", i);
      m_settings["Status"] = std::string("Calibrating timing board " + std::to_string(i));
      int rc = m_boards[i]->CalibrateTiming(&cb);
      if (rc == 1)
         cm_msg(MINFO, "DRS4Frontend", "Timing calibration done for board %d", i);
      else
         cm_msg(MERROR, "DRS4Frontend", "Timing calibration failed for board %d", i);
   }
   m_settings["CalibrateTiming"] = false;
   m_settings["Status"] = std::string("Timing calibration complete");
}

/*------------------------------------------------------------------*/
/*  Ring buffer                                                     */
/*------------------------------------------------------------------*/

void DRS4Frontend::setup_ring_buffer()
{
   INT status = rb_create(128 * 1024 * 1024,
                          DRS4_NCHANNELS * DRS4_NSAMPLES * sizeof(float) * 2 + 1024,
                          &m_rb_handle);
   if (status != SUCCESS) {
      cm_msg(MERROR, "DRS4Frontend", "Failed to create ring buffer");
      return;
   }
   rb_set_nonblocking();
}

/*------------------------------------------------------------------*/
/*  ODB watches                                                     */
/*------------------------------------------------------------------*/

void DRS4Frontend::setup_watches()
{
   m_settings["Refresh"].watch([this](midas::odb &key) {
      if (!(bool)key) return;
      m_settings["Refresh"] = false;
      rescan_boards();
   });

   for (int i = 0; i < m_num_boards; i++) {
      char board_path[256];
      snprintf(board_path, sizeof(board_path), "%s/Boards/Board%d", m_settings_path.c_str(), i);

      midas::odb b;
      b.connect(board_path);

      b["CalibrateVoltage"].watch([this, i](midas::odb &key) {
         if (!(bool)key) return;
         char msg[256];
         snprintf(msg, sizeof(msg), "Board%d: Starting voltage calibration...", i);
         m_settings["Status"] = std::string(msg);
         do_calibrate_voltage_board(i);
         snprintf(msg, sizeof(msg), "%s/Boards/Board%d/CalibrateVoltage", m_settings_path.c_str(), i);
         HNDLE hDB2;
         if (cm_get_experiment_database(&hDB2, NULL) == SUCCESS) {
            int zero = 0;
            db_set_value(hDB2, 0, msg, &zero, sizeof(zero), 1, TID_BOOL);
         }
         snprintf(msg, sizeof(msg), "Board%d: Voltage calibration done", i);
         m_settings["Status"] = std::string(msg);
      });

      b["CalibrateTiming"].watch([this, i](midas::odb &key) {
         if (!(bool)key) return;
         char msg[256];
         snprintf(msg, sizeof(msg), "Board%d: Starting timing calibration...", i);
         m_settings["Status"] = std::string(msg);
         do_calibrate_timing_board(i);
         snprintf(msg, sizeof(msg), "%s/Boards/Board%d/CalibrateTiming", m_settings_path.c_str(), i);
         HNDLE hDB2;
         if (cm_get_experiment_database(&hDB2, NULL) == SUCCESS) {
            int zero = 0;
            db_set_value(hDB2, 0, msg, &zero, sizeof(zero), 1, TID_BOOL);
         }
         snprintf(msg, sizeof(msg), "Board%d: Timing calibration done", i);
         m_settings["Status"] = std::string(msg);
      });

      b["Frequency (GHz)"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["TriggerLevel_CH0 (V)"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["TriggerLevel_CH1 (V)"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["TriggerLevel_CH2 (V)"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["TriggerLevel_CH3 (V)"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["TriggerPolarity"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["TriggerDelayNs"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["TriggerMode"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["DominoActive"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["ReadoutMode"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["TranspMode"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["EnableTcal"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["EnableCH1"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["EnableCH2"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["EnableCH3"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["EnableCH4"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["TriggerLogic"].watch([this, i](midas::odb &key) { apply_board_config(i); });
      b["InputRange"].watch([this, i](midas::odb &key) { apply_board_config(i); });
   }

   m_settings["SnapshotWaveform"].watch([this](midas::odb &key) {
      if (!(bool)key) return;
      m_snapshot_wanted = true;
   });
}

/*------------------------------------------------------------------*/
/*  Waveform snapshot for web UI                                    */
/*------------------------------------------------------------------*/

void DRS4Frontend::create_snapshot_odb()
{
   m_settings["SnapshotWaveform"] = false;
   m_settings["SnapshotBoard"] = 0;
   m_settings["SnapshotReady"] = false;
   m_settings["SnapshotPath"] = std::string("/home/muon/midasExp/drs4/web/drs4_snapshot.json");
}

void DRS4Frontend::do_snapshot()
{
   std::string snapPath = m_settings["SnapshotPath"];

   // Get current timestamp for debugging
   time_t now = time(NULL);
   struct tm tbuf;
   struct tm *t = localtime_r(&now, &tbuf);
   char timestamp[64];
   strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);

   std::string triggered = (m_snapshot_trigger_cell >= 0) ? "\"yes\"" : "\"no\"";
   std::string trig_mode = (m_snapshot_trigger_cell >= 0) ? "\"triggered\"" : "\"no_trigger\"";
   std::string json = "{\"timestamp\":\"" + std::string(timestamp) + "\"," +
                      "\"freq\":" + std::to_string(m_snapshot_freq) +
                      ",\"trigger_cell\":" + std::to_string(m_snapshot_trigger_cell) +
                      ",\"triggered\":" + triggered +
                      ",\"trig_mode\":" + trig_mode +
                      ",\"board\":" + std::to_string(m_snapshot_board) +
                      ",\"channels\":[";
   for (int ch = 0; ch < DRS4_NCHANNELS; ch++) {
      json += "{\"time\":[";
      for (int s = 0; s < DRS4_NSAMPLES; s++) {
         if (s > 0) json += ",";
         json += std::to_string(m_snapshot_time[ch][s]);
      }
      json += "],\"wave\":[";
      for (int s = 0; s < DRS4_NSAMPLES; s++) {
         if (s > 0) json += ",";
         json += std::to_string(m_snapshot_wave[ch][s]);
      }
      json += "]}";
      if (ch < DRS4_NCHANNELS - 1) json += ",";
   }
   json += "]}";

   FILE *f = fopen(snapPath.c_str(), "w");
   if (f) {
      fprintf(f, "%s", json.c_str());
      fclose(f);
   }

   try {
      m_settings["SnapshotWaveform"] = false;
      m_settings["SnapshotReady"] = true;
   } catch (...) {}
}

/*------------------------------------------------------------------*/
/*  Init                                                            */
/*------------------------------------------------------------------*/

INT DRS4Frontend::init(const char *eq_name, const char *eq_filename, int index)
{
   m_eq_name = eq_name;
   m_fe_index = index;

   char settings_path[256];
   snprintf(settings_path, sizeof(settings_path), "/Equipment/%s/Settings", eq_name);
   m_settings_path = settings_path;

   scan_boards();

   HNDLE hDB, hKey;
   if (cm_get_experiment_database(&hDB, NULL) == SUCCESS) {
      char stale_path[256];
      for (int try_idx = 0; try_idx < 10; try_idx++) {
         snprintf(stale_path, sizeof(stale_path), "/Equipment/%s%02d/Settings", eq_name, try_idx);
         if (db_find_key(hDB, 0, stale_path, &hKey) == SUCCESS) {
            db_delete_key(hDB, hKey, TRUE);
         }
         snprintf(stale_path, sizeof(stale_path), "/Equipment/%sHistory%02d/Settings", eq_name, try_idx);
         if (db_find_key(hDB, 0, stale_path, &hKey) == SUCCESS) {
            db_delete_key(hDB, hKey, TRUE);
         }
      }
      snprintf(stale_path, sizeof(stale_path), "/Equipment/%s/Settings", eq_name);
      if (db_find_key(hDB, 0, stale_path, &hKey) == SUCCESS) {
         db_delete_key(hDB, hKey, TRUE);
         cm_msg(MINFO, "DRS4Frontend", "Removed stale ODB: %s", stale_path);
      }

      if (db_find_key(hDB, 0, "/Custom/DRS4", &hKey) == SUCCESS) {
         db_delete_key(hDB, hKey, FALSE);
      }
      if (db_find_key(hDB, 0, "/Custom", &hKey) != SUCCESS) {
         db_create_key(hDB, 0, "/Custom", TID_KEY);
      } else {
         KEY key;
         if (db_get_key(hDB, hKey, &key) == SUCCESS && key.type == TID_STRING) {
            db_delete_key(hDB, hKey, FALSE);
            db_create_key(hDB, 0, "/Custom", TID_KEY);
         }
      }
      if (db_find_key(hDB, 0, "/Custom/Path", &hKey) == SUCCESS) {
         KEY key;
         if (db_get_key(hDB, hKey, &key) == SUCCESS && key.type == TID_KEY) {
            db_delete_key(hDB, hKey, FALSE);
         }
      }
   }

   setup_odb_structure();

   for (int i = 0; i < m_num_boards; i++) {
      read_board_info(i);
   }

   setup_ring_buffer();
   create_snapshot_odb();

   if (cm_get_experiment_database(&hDB, NULL) == SUCCESS) {
      if (db_find_key(hDB, 0, "/Custom", &hKey) != SUCCESS) {
         db_create_key(hDB, 0, "/Custom", TID_KEY);
      }
      if (db_find_key(hDB, 0, "/Custom/DRS4&", &hKey) == SUCCESS) {
         db_delete_key(hDB, hKey, FALSE);
      }
      db_set_value(hDB, 0, "/Custom/DRS4&", "/home/muon/midasExp/drs4/web/drs4.html", 256, 1, TID_STRING);
      cm_msg(MINFO, "DRS4Frontend", "Custom page registered at /Custom/DRS4&");
   }

   setup_watches();

   m_settings["Status"] = std::string("Initialized, " + std::to_string(m_num_boards) + " board(s) found");

   m_readout_thread = new std::thread(&DRS4Frontend::live_preview_loop, this);

   cm_msg(MINFO, "DRS4Frontend", "Init complete, %d board(s) found", m_num_boards);
   return SUCCESS;
}

/*------------------------------------------------------------------*/
/*  Begin of run                                                    */
/*------------------------------------------------------------------*/

INT DRS4Frontend::begin_of_run(int run_number, char *error)
{
   m_in_end_of_run = false;
   m_run_active = false;

   memset(m_prev_event_cnt, 0, sizeof(m_prev_event_cnt));
   m_prev_time = 0;

   bool any_connected = false;
   for (int i = 0; i < m_num_boards; i++) {
      if (m_board_hw_connected[i]) { any_connected = true; break; }
   }

   if (!any_connected) {
      connect_all();
      any_connected = false;
      for (int i = 0; i < m_num_boards; i++) {
         if (m_board_hw_connected[i]) { any_connected = true; break; }
      }
      if (!any_connected) {
         snprintf(error, 256, "No DRS4 boards connected");
         return FE_ERR_HW;
      }
   }

   apply_all_configs();

   for (int i = 0; i < m_num_boards; i++) {
      if (!m_board_hw_connected[i]) continue;
      m_boards[i]->StartDomino();
   }

   if (m_readout_thread) {
      m_readout_thread->join();
      delete m_readout_thread;
      m_readout_thread = nullptr;
   }

   m_run_active = true;

   m_readout_thread = new std::thread(&DRS4Frontend::readout_loop, this);

   m_settings["Status"] = std::string("Running");
   cm_msg(MINFO, "DRS4Frontend", "Begin of run %d", run_number);

   return SUCCESS;
}

/*------------------------------------------------------------------*/
/*  End of run                                                      */
/*------------------------------------------------------------------*/

INT DRS4Frontend::end_of_run()
{
   m_in_end_of_run = true;

   if (m_readout_thread) {
      m_readout_thread->join();
      delete m_readout_thread;
      m_readout_thread = nullptr;
   }

   m_run_active = false;

   m_in_end_of_run = false;
   m_readout_thread = new std::thread(&DRS4Frontend::live_preview_loop, this);

   m_settings["Status"] = std::string("Stopped");
   cm_msg(MINFO, "DRS4Frontend", "End of run");

   return SUCCESS;
}

/*------------------------------------------------------------------*/
/*  Live preview loop (runs in separate thread, independent of DAQ)  */
/*------------------------------------------------------------------*/

void DRS4Frontend::live_preview_loop()
{
   apply_all_configs();

   std::string prev_domino_mode;
   std::string prev_trig_logic;
   bool prev_enable_ch[4] = {true, true, true, true};
   double prev_trigger_level[4] = {0.05, 0.05, 0.05, 0.05};

   int master_init = -1;
   for (int i = 0; i < m_num_boards; i++) {
      if (m_board_hw_connected[i]) { master_init = i; break; }
   }
   if (master_init >= 0) {
      char path[256];
      snprintf(path, sizeof(path), "%s/Boards/Board%d/TriggerMode", m_settings_path.c_str(), master_init);
      char trig_mode_str[64] = "Auto";
      HNDLE hDB_init;
      if (cm_get_experiment_database(&hDB_init, NULL) == SUCCESS) {
         int size = sizeof(trig_mode_str);
         db_get_value(hDB_init, 0, path, trig_mode_str, &size, TID_STRING, 0);
      }
      prev_domino_mode = trig_mode_str;

      snprintf(path, sizeof(path), "%s/Boards/Board%d/TriggerLogic", m_settings_path.c_str(), master_init);
      char trig_str[64] = "OR";
      if (cm_get_experiment_database(&hDB_init, NULL) == SUCCESS) {
         int size = sizeof(trig_str);
         db_get_value(hDB_init, 0, path, trig_str, &size, TID_STRING, 0);
      }
      prev_trig_logic = trig_str;

      for (int ch = 0; ch < 4; ch++) {
         snprintf(path, sizeof(path), "%s/Boards/Board%d/EnableCH%d", m_settings_path.c_str(), master_init, ch+1);
         int val = 1;
         if (cm_get_experiment_database(&hDB_init, NULL) == SUCCESS) {
            int size = sizeof(val);
            db_get_value(hDB_init, 0, path, &val, &size, TID_BOOL, 0);
            prev_enable_ch[ch] = (bool)val;
         }
         snprintf(path, sizeof(path), "%s/Boards/Board%d/TriggerLevel_CH%d (V)", m_settings_path.c_str(), master_init, ch);
         double lvl = 0.05;
         if (cm_get_experiment_database(&hDB_init, NULL) == SUCCESS) {
            int size = sizeof(lvl);
            db_get_value(hDB_init, 0, path, &lvl, &size, TID_DOUBLE, 0);
            prev_trigger_level[ch] = lvl;
         }
      }
   }

   // Initialize trigger rate measurement
   m_prev_trg_time = 0;
   for (int i = 0; i < DRS4_MAX_BOARDS; i++) {
      m_prev_trg_scaler[i] = 0;
   }

   while (!m_in_end_of_run) {
      // Measure trigger rate from scaler registers every ~1 second
      auto now = std::chrono::system_clock::now();
      double now_s = std::chrono::duration<double>(now.time_since_epoch()).count();
      if (m_prev_trg_time > 0) {
         double dt = now_s - m_prev_trg_time;
         if (dt >= 1.0) {
            for (int i = 0; i < m_num_boards; i++) {
               if (!m_board_hw_connected[i]) continue;
               unsigned int scaler = m_boards[i]->GetScaler(0);
               if (dt > 0) {
                  double rate = (scaler - m_prev_trg_scaler[i]) / dt;
                  if (rate < 0) rate = scaler / dt;
                  char key[64];
                  snprintf(key, sizeof(key), "TrgRate_B%d", i);
                  try { m_settings[key] = (float)rate; } catch (...) {}
               }
               m_prev_trg_scaler[i] = scaler;
            }
            m_prev_trg_time = now_s;
         }
      } else {
         m_prev_trg_time = now_s;
         for (int i = 0; i < m_num_boards; i++) {
            if (m_board_hw_connected[i]) {
               m_prev_trg_scaler[i] = m_boards[i]->GetScaler(0);
            }
         }
      }

      if (!m_run_active) {
         bool any_board = false;
         for (int i = 0; i < m_num_boards; i++) {
            if (m_board_hw_connected[i]) {
               any_board = true;
            }
         }

         if (any_board) {
            int master = -1;
            for (int i = 0; i < m_num_boards; i++) {
               if (m_board_hw_connected[i]) { master = i; break; }
            }

            if (master >= 0) {
               int masterType = m_boards[master]->GetBoardType();
               int masterSerial = m_boards[master]->GetBoardSerialNumber();
               if (masterType == 0 || masterSerial == 0) {
                  usleep(100000);
                  continue;
               }

               char path[256];
               snprintf(path, sizeof(path), "%s/Boards/Board%d/TriggerMode", m_settings_path.c_str(), master);
               char mode_str[64] = "Auto";
               HNDLE hDB3;
               if (cm_get_experiment_database(&hDB3, NULL) == SUCCESS) {
                  int size = sizeof(mode_str);
                  db_get_value(hDB3, 0, path, mode_str, &size, TID_STRING, 0);
               }
               bool single_mode = (strcmp(mode_str, "Normal") == 0);

               snprintf(path, sizeof(path), "%s/Boards/Board%d/TriggerLogic", m_settings_path.c_str(), master);
               char trig_str[64] = "OR";
               if (cm_get_experiment_database(&hDB3, NULL) == SUCCESS) {
                  int size = sizeof(trig_str);
                  db_get_value(hDB3, 0, path, trig_str, &size, TID_STRING, 0);
               }
               std::string current_trig = trig_str;

               bool trig_level_changed = false;
               for (int ch = 0; ch < 4; ch++) {
                  snprintf(path, sizeof(path), "%s/Boards/Board%d/TriggerLevel_CH%d (V)", m_settings_path.c_str(), master, ch);
                  double lvl = 0.05;
                  if (cm_get_experiment_database(&hDB3, NULL) == SUCCESS) {
                     int size = sizeof(lvl);
                     db_get_value(hDB3, 0, path, &lvl, &size, TID_DOUBLE, 0);
                  }
                  double diff = lvl - prev_trigger_level[ch];
                  if (diff < 0) diff = -diff;
                  if (diff > 0.001) {
                     trig_level_changed = true;
                     prev_trigger_level[ch] = lvl;
                  }
               }

               bool enable_ch_changed = false;
               bool current_enable_ch[4] = {true, true, true, true};
               for (int ch = 0; ch < 4; ch++) {
                  snprintf(path, sizeof(path), "%s/Boards/Board%d/EnableCH%d", m_settings_path.c_str(), master, ch+1);
                  int val = 1;
                  if (cm_get_experiment_database(&hDB3, NULL) == SUCCESS) {
                     int size = sizeof(val);
                     db_get_value(hDB3, 0, path, &val, &size, TID_BOOL, 0);
                  }
                  current_enable_ch[ch] = (bool)val;
                  if (current_enable_ch[ch] != prev_enable_ch[ch]) {
                     enable_ch_changed = true;
                     prev_enable_ch[ch] = current_enable_ch[ch];
                  }
               }

               bool settings_changed = (std::string(mode_str) != prev_domino_mode) || (current_trig != prev_trig_logic) || trig_level_changed || enable_ch_changed;
               if (settings_changed) {
                  apply_all_configs();
                  prev_domino_mode = mode_str;
                  prev_trig_logic = current_trig;
               }

               if (single_mode) {
                  // Normal (triggered) mode with DominoMode=1 (continuous).
                  // StartDomino keeps the domino rolling.  A hardware
                  // trigger stops it (IsBusy→0).  We wait for that.

                  if (!m_in_end_of_run && !m_run_active) {
                     m_boards[master]->StartDomino();
                     // Wait for hardware trigger to stop the sweep
                     int trig_timeout = 10000;
                     while (m_boards[master]->IsBusy()
                            && !m_in_end_of_run && !m_run_active) {
                        usleep(100);
                        if (--trig_timeout <= 0) break;
                     }
                     if (!m_boards[master]->IsBusy())
                        capture_and_snapshot(false);
                  }
               } else {
                  // Auto (free-run) mode: continuous domino + SoftTrigger
                  capture_and_snapshot(true);
               }
            }
         }
      }

      usleep(100000);
   }
}

/*------------------------------------------------------------------*/
/*  Capture & snapshot helper                                        */
/*------------------------------------------------------------------*/

void DRS4Frontend::capture_and_snapshot(bool auto_mode)
{
   for (int i = 0; i < m_num_boards; i++) {
      if (!m_board_hw_connected[i]) continue;

      int boardType = m_boards[i]->GetBoardType();
      int boardSerial = m_boards[i]->GetBoardSerialNumber();
      if (boardType == 0 || boardSerial == 0) continue;

      // DominoMode=0 (single sweep).  StartDomino begins a single pass
      // through all 1024 cells.  The sweep completes in ~200 ns at
      // 5 GHz, then the FPGA reads the analog data into its digital RAM.
      // BIT_RUNNING (IsBusy) goes 1 during readout, then 0 when done.
      //
      // In auto_mode we start a fresh sweep.  In Normal (triggered) mode
      // the sweep was already started and stopped by the hardware trigger
      // (or completed naturally if no trigger hit).

      static int snap_cnt = 0;
      if (auto_mode) {
         // DominoMode=1 (continuous).  StartDomino keeps the domino
         // rolling.  SoftTrigger stops it and latches the current
         // position, then the FPGA reads out analog data to RAM.
         // Wait for !IsBusy() so the readout (and stop cell) is valid.
         m_boards[i]->StartDomino();
         usleep(200);

         m_boards[i]->SoftTrigger();

         int loops = 0;
         while (m_boards[i]->IsBusy() && loops < 10000) {
            usleep(10);
            loops++;
         }
      } else {
         // Normal mode: hardware trigger already fired, wait for readout
         for (int j = 0; j < 10000 && m_boards[i]->IsBusy(); j++)
            usleep(10);
      }

      m_boards[i]->TransferWaves(0, 8);
      int trigger_cell = m_boards[i]->GetTriggerCell(0);

      // Firmware v30000 may always report tc=0. Detect the true trigger
      // position from waveform discontinuities: the largest inter-sample
      // step marks where the circular buffer wraps (data starts fresh).
      if (trigger_cell == 0) {
         float wf[DRS4_NSAMPLES];
         m_boards[i]->GetWave(0, 0, wf, false, 0);
         float prev = wf[DRS4_NSAMPLES - 1];
         float max_diff = 0;
         int max_idx = 0;
         for (int s = 0; s < DRS4_NSAMPLES; s++) {
            float diff = fabsf(wf[s] - prev);
            if (diff > max_diff) {
               max_diff = diff;
               max_idx = s;
            }
            prev = wf[s];
         }
         if (max_diff > 10.0f)  // require >10mV step to confirm real trigger
            trigger_cell = max_idx;
      }

      float freq = (float)m_boards[i]->GetTrueFrequency();
      bool vcal = m_boards[i]->IsVoltageCalibrationValid();
      bool tcal = m_boards[i]->IsTimingCalibrationValid();
      int fw_ver = m_boards[i]->GetFirmwareVersion();

      if (snap_cnt++ < 5 || snap_cnt % 100 == 0)
         cm_msg(MINFO, "DRS4Frontend",
                "Snapshot #%d: board=%d tc=%d fw=%d freq=%.3f vcal=%d tcal=%d (auto=%d)",
                snap_cnt, i, trigger_cell, fw_ver, freq, vcal, tcal, auto_mode);

      std::lock_guard<std::mutex> lock(m_snapshot_mutex);
      m_snapshot_trigger_cell = trigger_cell;
      m_snapshot_freq = freq;
      m_snapshot_board = i;

      for (int ch = 0; ch < DRS4_NCHANNELS; ch++) {
         int drs_ch = ch * 2;
         m_boards[i]->GetTime(0, drs_ch, trigger_cell, m_snapshot_time[ch]);
         m_boards[i]->GetWave(0, drs_ch, m_snapshot_wave[ch], true, trigger_cell);
      }

      do_snapshot();
   }
}

/*------------------------------------------------------------------*/
/*  Readout loop (runs in separate thread)                          */
/*------------------------------------------------------------------*/

void DRS4Frontend::readout_loop()
{
   const int header_size = 16;
   const int ch_data_size = 4 + DRS4_NSAMPLES * sizeof(float) * 2;
   const int event_size = header_size + DRS4_NCHANNELS * ch_data_size;

   while (!m_in_end_of_run) {
      for (int i = m_num_boards - 1; i >= 0; i--) {
         if (m_board_hw_connected[i]) {
            m_boards[i]->StartDomino();
         }
      }

      int master = -1;
      for (int i = 0; i < m_num_boards; i++) {
         if (m_board_hw_connected[i]) { master = i; break; }
      }
      if (master < 0) break;

      int timeout = 100000;
      while (m_boards[master]->IsBusy() && !m_in_end_of_run) {
         usleep(10);
         if (--timeout <= 0) break;
      }

      if (m_in_end_of_run) break;
      if (m_boards[master]->IsBusy()) continue;

      for (int i = 0; i < m_num_boards; i++) {
         if (!m_board_hw_connected[i]) continue;

         if (m_boards[i]->IsBusy()) continue;

         m_boards[i]->TransferWaves(0, 8);
         int trigger_cell = m_boards[i]->GetTriggerCell(0);

         void *wp;
         INT rv = rb_get_wp(m_rb_handle, &wp, 1000);
         if (rv != SUCCESS) continue;

         char *pbuf = (char *)wp;

         uint32_t board_id = (uint32_t)i;
         uint32_t n_channels = DRS4_NCHANNELS;
         float freq = (float)m_boards[i]->GetTrueFrequency();

         memcpy(pbuf, &board_id, 4);
         memcpy(pbuf + 4, &trigger_cell, 4);
         memcpy(pbuf + 8, &n_channels, 4);
         memcpy(pbuf + 12, &freq, 4);

         for (int ch = 0; ch < DRS4_NCHANNELS; ch++) {
            int drs_ch = ch * 2;
            float time_array[DRS4_NSAMPLES];
            float wave_array[DRS4_NSAMPLES];

            m_boards[i]->GetTime(0, drs_ch, trigger_cell, time_array);
            m_boards[i]->GetWave(0, drs_ch, wave_array, true, trigger_cell);

            char *pch = pbuf + header_size + ch * ch_data_size;
            uint32_t channel_id = (uint32_t)ch;
            memcpy(pch, &channel_id, 4);
            memcpy(pch + 4, time_array, DRS4_NSAMPLES * sizeof(float));
            memcpy(pch + 4 + DRS4_NSAMPLES * sizeof(float), wave_array, DRS4_NSAMPLES * sizeof(float));
         }

         rb_increment_wp(m_rb_handle, event_size);
      }
   }
}

/*------------------------------------------------------------------*/
/*  Is data available (poll)                                        */
/*------------------------------------------------------------------*/

INT DRS4Frontend::is_data_available()
{
   if (m_rb_handle < 0) return 0;

   INT buf_level;
   if (rb_get_buffer_level(m_rb_handle, &buf_level) != SUCCESS)
      return 0;

   return (buf_level > 0) ? 1 : 0;
}

/*------------------------------------------------------------------*/
/*  Fill MIDAS banks from ring buffer                               */
/*------------------------------------------------------------------*/

INT DRS4Frontend::fill_midas_banks(char *pevent)
{
   if (m_rb_handle < 0) return 0;

   INT buf_level;
   if (rb_get_buffer_level(m_rb_handle, &buf_level) != SUCCESS || buf_level == 0)
      return 0;

   void *rp;
   if (rb_get_rp(m_rb_handle, &rp, 10) != SUCCESS || rp == nullptr)
      return 0;

   char *pbuf = (char *)rp;

   uint32_t board_id, trigger_cell, n_channels;
   float freq;
   memcpy(&board_id, pbuf, 4);
   memcpy(&trigger_cell, pbuf + 4, 4);
   memcpy(&n_channels, pbuf + 8, 4);
   memcpy(&freq, pbuf + 12, 4);

   const int header_size = 16;
   const int ch_data_size = 4 + DRS4_NSAMPLES * sizeof(float) * 2;
   const int event_size = header_size + n_channels * ch_data_size;

   char bank_name[5];
   snprintf(bank_name, sizeof(bank_name), "DR%02d", board_id);

   char *pbk;
   bk_init32(pevent);
   bk_create(pevent, bank_name, TID_BYTE, (void **)&pbk);

   memcpy(pbk, pbuf, event_size);
   pbk += event_size;

   bk_close(pevent, pbk);

   rb_increment_rp(m_rb_handle, event_size);

   return bk_size(pevent);
}