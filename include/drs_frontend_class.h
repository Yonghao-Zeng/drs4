#ifndef DRS_FRONTEND_CLASS_H
#define DRS_FRONTEND_CLASS_H

#include "midas.h"
#include "odbxx.h"
#include "DRS.h"

#include <string>
#include <thread>
#include <atomic>
#include <array>
#include <mutex>

constexpr int DRS4_MAX_BOARDS = 8;
constexpr int DRS4_NCHANNELS = 4;        // physical inputs per board
constexpr int DRS4_NSAMPLES = 1024;      // samples per waveform

class DRS4Frontend {
public:
   DRS4Frontend();
   ~DRS4Frontend();

   INT init(const char *eq_name, const char *eq_filename, int index);
   INT begin_of_run(int run_number, char *error);
   INT end_of_run();
   INT is_data_available();
   INT fill_midas_banks(char *pevent);

private:
   // ODB
   midas::odb m_settings;
   std::string m_settings_path;
   std::string m_eq_name;
   int m_fe_index;

   // Hardware
   DRS *m_drs{nullptr};
   int m_num_boards{0};

   // Board state
   bool m_board_hw_connected[DRS4_MAX_BOARDS]{};
   DRSBoard *m_boards[DRS4_MAX_BOARDS]{};

   // Readout
   std::thread *m_readout_thread{nullptr};
   std::atomic<bool> m_in_end_of_run{false};
   std::atomic<bool> m_run_active{false};
   std::atomic<bool> m_single_mode{false};  // true=Single (triggered), false=Normal (free-run)
   int m_rb_handle{-1};

   // Trigger rate measurement in live preview (also shown in custom page)
   uint64_t m_prev_event_cnt[DRS4_MAX_BOARDS]{};
   double m_prev_time{0};
   double m_prev_trg_scaler[DRS4_MAX_BOARDS]{};
   double m_prev_trg_time{0};

   // Connect re-entry guard
   std::string m_last_connect_val;

   // Waveform snapshot for web display
   std::mutex m_snapshot_mutex;
   std::atomic<bool> m_snapshot_wanted{false};
   float m_snapshot_time[DRS4_NCHANNELS][DRS4_NSAMPLES]{};
   float m_snapshot_wave[DRS4_NCHANNELS][DRS4_NSAMPLES]{};
   int m_snapshot_trigger_cell{0};
   float m_snapshot_freq{0};
   int m_snapshot_board{0};

   // Calibration
   class MidasDRSCallback : public DRSCallback {
   public:
      MidasDRSCallback(midas::odb &settings) : m_settings(settings) {}
      void Progress(int value) override;
   private:
      midas::odb &m_settings;
   };

   // Methods
   void scan_boards();
   void create_board_defaults(int board_index);
   void setup_odb_structure();
   void setup_watches();
   void setup_ring_buffer();

   void connect_board(int i);
   void disconnect_board(int i);
   void connect_all();
   void disconnect_all();
   void recalc_global_connect_status();

   void apply_board_config(int i);
   void apply_all_configs();
   void read_board_info(int i);

   void do_calibrate_voltage();
   void do_calibrate_timing();
   void do_calibrate_all();
   void do_calibrate_board(int board_idx);
   void do_calibrate_voltage_board(int board_idx);
   void do_calibrate_timing_board(int board_idx);
   void rescan_boards();
   void create_snapshot_odb();
   void do_snapshot();

   void readout_loop();
   void live_preview_loop();
   void capture_and_snapshot(bool auto_mode);

   // ODB helpers
   template<typename T>
   T odb_get(midas::odb &odb, const std::string &key, T default_val);
};

#endif // DRS_FRONTEND_CLASS_H