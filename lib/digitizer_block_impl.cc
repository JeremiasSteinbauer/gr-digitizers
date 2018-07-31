/* -*- c++ -*- */
/* Copyright (C) 2018 GSI Darmstadt, Germany - All Rights Reserved
 * co-developed with: Cosylab, Ljubljana, Slovenia and CERN, Geneva, Switzerland
 * You may use, distribute and modify this code under the terms of the GPL v.3  license.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "digitizer_block_impl.h"
#include <thread>
#include <chrono>
#include <boost/lexical_cast.hpp>
#include <digitizers/tags.h>
#include <gnuradio/thread/thread.h> // set_name

namespace gr {
  namespace digitizers {


   /**********************************************************************
    * Error codes
    *********************************************************************/

    struct digitizer_block_err_category : std::error_category
    {
      const char* name() const noexcept override;
      std::string message(int ev) const override;
    };

    const char*
    digitizer_block_err_category::name() const noexcept
    {
     return "digitizer_block";
    }

    std::string
    digitizer_block_err_category::message(int ev) const
    {
      switch (static_cast<digitizer_block_errc>(ev))
      {
       case digitizer_block_errc::Interrupted:
        return "Wit interrupted";

       default:
        return "(unrecognized error)";
      }
   }

   const digitizer_block_err_category __digitizer_block_category {};

   std::error_code
   make_error_code(digitizer_block_errc e)
   {
     return {static_cast<int>(e), __digitizer_block_category};
   }


   /**********************************************************************
    * Structors
    *********************************************************************/

   static const int AVERAGE_HISTORY_LENGTH = 100000;

   digitizer_block_impl::digitizer_block_impl(int ai_channels, int di_ports, bool auto_arm) :
       d_samp_rate(10000),
       d_actual_samp_rate(d_samp_rate),
       d_samples(10000),
       d_pre_samples(1000),
       d_nr_captures(1),
       d_buffer_size(8192),
       d_nr_buffers(100),
       d_driver_buffer_size(100000),
       d_acquisition_mode(acquisition_mode_t::STREAMING),
       d_poll_rate(0.001),
       d_downsampling_mode(downsampling_mode_t::DOWNSAMPLING_MODE_NONE),
       d_downsampling_factor(1),
       d_ai_channels(ai_channels),
       d_ports(di_ports),
       d_channel_settings(),
       d_port_settings(),
       d_trigger_settings(),
       d_status(ai_channels),
       d_app_buffer(),
       d_was_last_callback_timestamp_taken(false),
       d_estimated_sample_rate(AVERAGE_HISTORY_LENGTH),
       d_initialized(false),
       d_armed(false),
       d_auto_arm(auto_arm),
       d_trigger_once(false),
       d_was_triggered_once(false),
       d_timebase_published(false),
       ai_buffers(ai_channels),
       ai_error_buffers(ai_channels),
       port_buffers(di_ports),
       d_data_rdy(false),
       d_trigger_state(0),
       d_read_idx(0),
       d_buffer_samples(0),
       d_errors(128),
       d_poller_state(poller_state_t::IDLE)
   {
     d_ai_buffers = std::vector<std::vector<float>>(d_ai_channels);
     d_ai_error_buffers = std::vector<std::vector<float>>(d_ai_channels);

     if (di_ports) {
       d_port_buffers = std::vector<std::vector<uint8_t>>(di_ports);
     }

     assert(d_ai_channels < MAX_SUPPORTED_AI_CHANNELS);
     assert(d_ports < MAX_SUPPORTED_PORTS);
   }

   digitizer_block_impl::~digitizer_block_impl()
   {
   }

   /**********************************************************************
    * Helpers
    **********************************************************************/

   uint32_t
   digitizer_block_impl::get_pre_trigger_samples_with_downsampling() const
   {
     auto count = d_pre_samples;
     if (d_downsampling_mode != downsampling_mode_t::DOWNSAMPLING_MODE_NONE) {
       count = count / d_downsampling_factor;
     }
     return count;
   }

   uint32_t
   digitizer_block_impl::get_post_trigger_samples_with_downsampling() const
   {
     auto count = d_samples;
     if (d_downsampling_mode != downsampling_mode_t::DOWNSAMPLING_MODE_NONE) {
       count = count / d_downsampling_factor;
     }
     return count;
   }

   uint32_t
   digitizer_block_impl::get_block_size() const
   {
     return d_samples + d_pre_samples;
   }

   uint32_t
   digitizer_block_impl::get_block_size_with_downsampling() const
   {
       auto count = get_pre_trigger_samples_with_downsampling()
               + get_post_trigger_samples_with_downsampling();
       return count;
   }

   double
   digitizer_block_impl::get_timebase_with_downsampling() const
   {
     if (d_downsampling_mode == downsampling_mode_t::DOWNSAMPLING_MODE_NONE) {
       return 1.0 / d_actual_samp_rate;
     }
     else {
       return d_downsampling_factor / d_actual_samp_rate;
     }
   }

   void
   digitizer_block_impl::add_error_code(std::error_code ec)
   {
     d_errors.push(ec);
   }

   std::vector<int>
   digitizer_block_impl::find_analog_triggers(float const * const samples, int nsamples)
   {
     std::vector<int> trigger_offsets; // relative offset of detected triggers

     assert(nsamples >= 0);

     if (!d_trigger_settings.is_enabled() || nsamples == 0) {
       return trigger_offsets;
     }

     assert(d_trigger_settings.is_analog());

     auto aichan = convert_to_aichan_idx(d_trigger_settings.source);

     if (d_trigger_settings.direction == TRIGGER_DIRECTION_RISING
             || d_trigger_settings.direction == TRIGGER_DIRECTION_HIGH) {

       float band = d_channel_settings[aichan].actual_range / 100.0;
       float lo = static_cast<float>(d_trigger_settings.threshold - band);

       for(auto i = 0; i < nsamples; i++) {
         if(!d_trigger_state && samples[i] >= d_trigger_settings.threshold) {
           d_trigger_state = 1;
           trigger_offsets.push_back(i);
         }
         else if(d_trigger_state && samples[i] <= lo) {
           d_trigger_state = 0;
         }
       }
     }
     else if (d_trigger_settings.direction == TRIGGER_DIRECTION_FALLING
             || d_trigger_settings.direction == TRIGGER_DIRECTION_LOW) {

       float band = d_channel_settings[aichan].actual_range / 100.0;
       float hi = static_cast<float>(d_trigger_settings.threshold + band);

       for(auto i = 0; i < nsamples; i++) {
         if(d_trigger_state && samples[i] <= d_trigger_settings.threshold) {
           d_trigger_state = 0;
           trigger_offsets.push_back(i);
         }
         else if(!d_trigger_state && samples[i] >= hi) {
           d_trigger_state = 1;
         }
       }
     }

     return trigger_offsets;
   }

   std::vector<int>
   digitizer_block_impl::find_digital_triggers(uint8_t const * const samples, int nsamples, uint8_t mask)
   {
     std::vector<int> trigger_offsets;

     if (d_trigger_settings.direction == TRIGGER_DIRECTION_RISING
            || d_trigger_settings.direction == TRIGGER_DIRECTION_HIGH) {

       for(auto i = 0; i < nsamples; i++) {
         if(!d_trigger_state && (samples[i] & mask)) {
           d_trigger_state = 1;
           trigger_offsets.push_back(i);
         }
         else if(d_trigger_state && !(samples[i] & mask)) {
           d_trigger_state = 0;
         }
       }
     }
     else if (d_trigger_settings.direction == TRIGGER_DIRECTION_FALLING
             || d_trigger_settings.direction == TRIGGER_DIRECTION_LOW) {

       for(auto i = 0; i < nsamples; i++) {
         if(d_trigger_state && !(samples[i] & mask)) {
           d_trigger_state = 0;
           trigger_offsets.push_back(i);
         }
         else if(!d_trigger_state && (samples[i] & mask)) {
           d_trigger_state = 1;
         }
       }
     }

     return trigger_offsets;
   }

   /**********************************************************************
    * Public API
    **********************************************************************/

   acquisition_mode_t
   digitizer_block_impl::get_acquisition_mode()
   {
     return d_acquisition_mode;
   }

   void
   digitizer_block_impl::set_samples(int samples, int pre_samples)
   {
     if (samples < 1) {
       throw std::invalid_argument("post-trigger samples can't be less than one");
     }

     if (pre_samples < 0) {
       throw std::invalid_argument("pre-trigger samples can't be less than zero");
     }

     d_samples = static_cast<uint32_t>(samples);
     d_pre_samples = static_cast<uint32_t>(pre_samples);
     d_buffer_size = d_samples + d_pre_samples;
   }

   void
   digitizer_block_impl::set_samp_rate(double rate)
   {
     if (rate <= 0.0) {
       throw std::invalid_argument("sample rate should be greater than zero");
     }
     d_samp_rate = rate;
     d_actual_samp_rate = rate;
   }

   double
   digitizer_block_impl::get_samp_rate()
   {
     return d_actual_samp_rate;
   }

   void
   digitizer_block_impl::set_buffer_size(int buffer_size)
   {
     if (buffer_size < 0) {
       throw std::invalid_argument("buffer size can't be negative");
     }

     d_buffer_size = static_cast<uint32_t>(buffer_size);

     set_output_multiple(buffer_size);
   }

   void
   digitizer_block_impl::set_nr_buffers(int nr_buffers)
   {
     if (nr_buffers < 1) {
       throw std::invalid_argument("number of buffers can't be a negative number");
     }

     d_nr_buffers = static_cast<uint32_t>(nr_buffers);
   }

   void
   digitizer_block_impl::set_driver_buffer_size(int driver_buffer_size)
   {
     if (driver_buffer_size < 1) {
       throw std::invalid_argument("driver buffer size can't be a negative number");
     }

     d_driver_buffer_size = static_cast<uint32_t>(driver_buffer_size);
   }

   void
   digitizer_block_impl::set_auto_arm(bool auto_arm)
   {
     d_auto_arm = auto_arm;
   }

   void
   digitizer_block_impl::set_trigger_once(bool once)
   {
     d_trigger_once = once;
   }

   // Poll rate is in seconds
   void
   digitizer_block_impl::set_streaming(double poll_rate)
   {
     if (poll_rate < 0.0) {
       throw std::invalid_argument("poll rate can't be negative");
     }

     d_acquisition_mode = acquisition_mode_t::STREAMING;
     d_poll_rate = poll_rate;

     // just in case
     d_nr_captures = 1;
   }

   void
   digitizer_block_impl::set_rapid_block(int nr_captures)
   {
     if (nr_captures < 1) {
       throw std::invalid_argument("nr waveforms should be at least one");
     }

     d_acquisition_mode = acquisition_mode_t::RAPID_BLOCK;
     d_nr_captures = static_cast<uint32_t>(nr_captures);
   }

   void
   digitizer_block_impl::set_downsampling(downsampling_mode_t mode, int downsample_factor)
   {
     if (mode == downsampling_mode_t::DOWNSAMPLING_MODE_NONE) {
         downsample_factor = 1;
     }
     else if (downsample_factor < 2) {
       throw std::invalid_argument("downsampling factor should be at least 2");
     }

     d_downsampling_mode = mode;
     d_downsampling_factor = static_cast<uint32_t>(downsample_factor);
   }

   int
   digitizer_block_impl::convert_to_aichan_idx(const std::string &id) const
   {
     if (id.length() != 1) {
       throw std::invalid_argument("aichan id should be a single character: " + id);
     }

     int idx = std::toupper(id[0]) - 'A';
     if (idx < 0 || idx > MAX_SUPPORTED_AI_CHANNELS) {
       throw std::invalid_argument("invalid aichan id: " + id);
     }

     return idx;
   }

   void
   digitizer_block_impl::set_aichan(const std::string &id, bool enabled, double range, bool dc_coupling, double range_offset)
   {
     auto idx = convert_to_aichan_idx(id);
     d_channel_settings[idx].range = range;
     d_channel_settings[idx].offset = range_offset;
     d_channel_settings[idx].enabled = enabled;
     d_channel_settings[idx].dc_coupled = dc_coupling;
   }

   int
   digitizer_block_impl::get_enabled_aichan_count() const
   {
     auto count = 0;
     for (const auto &c : d_channel_settings) {
       count += c.enabled;
     }
     return count;
   }

   void
   digitizer_block_impl::set_aichan_range(const std::string &id, double range, double range_offset)
   {
     auto idx = convert_to_aichan_idx(id);
     d_channel_settings[idx].range = range;
     d_channel_settings[idx].offset = range_offset;
   }

   void
   digitizer_block_impl::set_aichan_trigger(const std::string &id, trigger_direction_t direction, double threshold)
   {
     convert_to_aichan_idx(id); // Just to verify id

     d_trigger_settings.source = id;
     d_trigger_settings.threshold = threshold;
     d_trigger_settings.direction = direction;
     d_trigger_settings.pin_number = 0; // not used
   }

   int
   digitizer_block_impl::convert_to_port_idx(const std::string &id) const
   {
     if (id.length() != 5) {
       throw std::invalid_argument("invalid port id: " + id + ", should be of the following format 'port<d>'");
     }

     int idx = boost::lexical_cast<int>(id[4]);
     if (idx < 0 || idx > MAX_SUPPORTED_PORTS) {
       throw std::invalid_argument("invalid port number: " + id);
     }

     return idx;
   }

   void
   digitizer_block_impl::set_diport(const std::string &id, bool enabled, double thresh_voltage)
   {
     auto port_number = convert_to_port_idx(id);

     d_port_settings[port_number].logic_level = thresh_voltage;
     d_port_settings[port_number].enabled = enabled;
   }

   int
   digitizer_block_impl::get_enabled_diport_count() const
   {
     auto count = 0;
     for (const auto &p : d_port_settings) {
       count += p.enabled;
     }
     return count;
   }

   void
   digitizer_block_impl::set_di_trigger(uint32_t pin, trigger_direction_t direction)
   {
     d_trigger_settings.source = TRIGGER_DIGITAL_SOURCE;
     d_trigger_settings.threshold = 0.0; // not used
     d_trigger_settings.direction = direction;
     d_trigger_settings.pin_number = pin;
   }

   void
   digitizer_block_impl::disable_triggers()
   {
     d_trigger_settings.source = TRIGGER_NONE_SOURCE;
   }

   void
   digitizer_block_impl::initialize()
   {
     if (d_initialized) {
       return;
     }

     auto ec = driver_initialize();
     if (ec) {
       add_error_code(ec);
       throw std::runtime_error("initialize failed: " + to_string(ec));
     }

     d_initialized = true;
   }

   void
   digitizer_block_impl::configure()
   {
     if (!d_initialized) {
       throw std::runtime_error("initialize first");;
     }

     if (d_armed) {
       throw std::runtime_error("disarm first");
     }

     auto ec = driver_configure();
     if (ec) {
       add_error_code(ec);
       throw std::runtime_error("configure failed: " + to_string(ec));
     }
     // initialize application buffer
     d_app_buffer.initialize(get_enabled_aichan_count(),
         get_enabled_diport_count(), d_buffer_size, d_nr_buffers);
   }

   void
   digitizer_block_impl::arm()
   {
     if (d_armed) {
       return;
     }

     // set estimated sample rate to expected
     float expected = static_cast<float>(get_samp_rate());
     for (auto i=0; i < AVERAGE_HISTORY_LENGTH; i++) {
       d_estimated_sample_rate.add(expected);
     }

     // arm the driver
     auto ec = driver_arm();
     if (ec) {
       add_error_code(ec);
       throw std::runtime_error("arm failed: " + to_string(ec));
     }

     d_armed = true;
     d_timebase_published = false;
     d_was_last_callback_timestamp_taken = false;

     // clear error condition in the application buffer
     d_app_buffer.notify_data_ready(std::error_code {});

     // notify poll thread to start with the poll request
     if(d_acquisition_mode == acquisition_mode_t::STREAMING) {
       transit_poll_thread_to_running();
     }

     //allocate buffer pointer vectors.
     int num_enabled_ai_channels = 0;
     int num_enabled_di_ports = 0;
     for (auto i = 0; i < d_ai_channels; i++) {
       if (d_channel_settings[i].enabled) {
         num_enabled_ai_channels++;
       }
     }
     for (auto i = 0; i < d_ports; i++) {
       if (d_port_settings[i].enabled) {
         num_enabled_di_ports++;
       }
     }
     ai_buffers.resize(num_enabled_ai_channels);
     ai_error_buffers.resize(num_enabled_ai_channels);
     port_buffers.resize(num_enabled_di_ports);
   }

   bool
   digitizer_block_impl::is_armed()
   {
     return d_armed;
   }

   void
   digitizer_block_impl::disarm()
   {
     if (!d_armed) {
       return;
     }

     if(d_acquisition_mode == acquisition_mode_t::STREAMING) {
       transit_poll_thread_to_idle();
     }

     auto ec = driver_disarm();
     if (ec) {
       add_error_code(ec);
       GR_LOG_WARN(d_logger, "disarm failed: " + to_string(ec));
     }

     d_armed = false;
   }

   void
   digitizer_block_impl::close()
   {
     auto ec = driver_close();
     if (ec) {
       add_error_code(ec);
       GR_LOG_WARN(d_logger, "close failed: " + to_string(ec));
     }

     d_initialized = false;
   }

   std::vector<error_info_t>
   digitizer_block_impl::get_errors()
   {
     return d_errors.get();
   }

   std::string
   digitizer_block_impl::getConfigureExceptionMessage()
   {
       return d_configure_exception_message;
   }

   bool
   digitizer_block_impl::start()
   {
     try {
       initialize();
       configure();

       // Needed in case start/run is called multiple times without destructing the flowgraph
       d_was_triggered_once = false;
       d_data_rdy_errc = std::error_code {};
       d_data_rdy = false;

       if (d_acquisition_mode == acquisition_mode_t::STREAMING) {
         start_poll_thread();
       }

       if(d_auto_arm && d_acquisition_mode == acquisition_mode_t::STREAMING) {
         arm();
       }
     } catch (const std::exception& ex) {
       d_configure_exception_message = ex.what();
       std::cout << "digitizer_block_impl::start(): " << ex.what() << std::endl;
       std::cout << "digitizer_block_impl::start(): " << d_configure_exception_message << std::endl;
       return false;
     } catch (...) {
       d_configure_exception_message = "Unknown Exception received in digitizer_block_impl::start";
       return false;
     }

       return true;
   }

   bool
   digitizer_block_impl::stop()
   {
     if (!d_initialized) {
       return true;
     }

     if (d_armed) {
       // Interrupt waiting function (workaround). From the scheduler point of view this is not
       // needed because it makes sure that the worker thread gets interrupted before the stop
       // method is called. But we have this in place in order to allow for manual intervention.
       notify_data_ready(digitizer_block_errc::Stopped);

       disarm();
     }

     if(d_acquisition_mode == acquisition_mode_t::STREAMING) {
       stop_poll_thread();
     }

     d_configure_exception_message = "";

     return true;
   }

   /**********************************************************************
    * Driver interface
    **********************************************************************/

   void
   digitizer_block_impl::notify_data_ready(std::error_code ec)
   {
     if(ec) {
       add_error_code(ec);
     }

     {
       boost::mutex::scoped_lock lock(d_mutex);
       d_data_rdy = true;
       d_data_rdy_errc = ec;
     }

     d_data_rdy_cv.notify_one();
   }

   std::error_code
   digitizer_block_impl::wait_data_ready()
   {
     boost::mutex::scoped_lock lock(d_mutex);

     d_data_rdy_cv.wait(lock, [this] { return d_data_rdy; });
     return d_data_rdy_errc;
   }

   void digitizer_block_impl::clear_data_ready()
   {
     boost::mutex::scoped_lock lock(d_mutex);

     d_data_rdy = false;
     d_data_rdy_errc = std::error_code {};
   }


   /**********************************************************************
    * GR worker functions
    **********************************************************************/

   int
   digitizer_block_impl::work_rapid_block(int noutput_items, gr_vector_void_star &output_items)
   {
     if (d_bstate.state == rapid_block_state_t::WAITING) {

       if (d_trigger_once &&  d_was_triggered_once) {
         return -1;
       }

       if (d_auto_arm) {
         disarm();
         while(true) {
           try {
             arm();
             break;
           }
           catch (...) {
             return -1;
           }
         }
       }

       // Wait conditional variable, when waken clear it
       auto ec = wait_data_ready();
       clear_data_ready();

       // Stop requested
       if (ec == digitizer_block_errc::Stopped) {
         GR_LOG_INFO(d_logger, "stop requested");
         return -1;
       }
       else if (ec) {
         GR_LOG_ERROR(d_logger, "error occurred while waiting for data: " + to_string(ec));
         return 0;
       }

       // we assume all the blocks are ready
       d_bstate.initialize(d_nr_captures);
     }

     if (d_bstate.state == rapid_block_state_t::READING_PART1) {

       // If d_trigger_once is true we will signal all done in the next iteration
       // with the block state set to WAITING
       d_was_triggered_once = true;

       auto samples_to_fetch = get_block_size();
       auto downsampled_samples = get_block_size_with_downsampling();

       // Instruct the driver to prefetch samples. Drivers might choose to ignore this call
       auto ec = driver_prefetch_block(samples_to_fetch, d_bstate.waveform_idx);
       if (ec) {
         add_error_code(ec);
         return -1;
       }

       // Initiate state machine for the current waveform. Note state machine track
       // and adjust the waveform index.
       d_bstate.set_waveform_params(0, downsampled_samples);

       // We are good to read first batch of samples
       noutput_items = std::min(noutput_items, d_bstate.samples_left);

       ec = driver_get_rapid_block_data(d_bstate.offset,
               noutput_items, d_bstate.waveform_idx, output_items, d_status);
       if (ec) {
         add_error_code(ec);
         return -1;
       }

       // Attach trigger info to value outputs and to all ports

       auto ttag = make_trigger_tag();
       ttag.offset = nitems_written(0) + get_pre_trigger_samples_with_downsampling();

       auto vec_idx = 0;
       for (auto i = 0; i < d_ai_channels
             && vec_idx < (int)output_items.size(); i++, vec_idx+=2) {
         if (!d_channel_settings[i].enabled) {
           continue;
         }

         auto trigger_tag = make_trigger_tag(
                 get_pre_trigger_samples_with_downsampling(),
                 get_post_trigger_samples_with_downsampling(),
                 d_status[i],
                 get_timebase_with_downsampling(),
                 get_timestamp_utc_ns());
         trigger_tag.offset = nitems_written(0);

         add_item_tag(vec_idx, trigger_tag);
         add_item_tag(vec_idx, ttag);
       }

       auto trigger_tag = make_trigger_tag(
             get_pre_trigger_samples_with_downsampling(),
             get_post_trigger_samples_with_downsampling(),
             0,
             get_timebase_with_downsampling(),
             get_timestamp_utc_ns());
       trigger_tag.offset = nitems_written(0);

       for (auto i = 0; i < d_ports
             && vec_idx < (int)output_items.size(); i++, vec_idx++) {
         if (!d_port_settings[i].enabled) {
           continue;
         }

         add_item_tag(vec_idx, trigger_tag);
         add_item_tag(vec_idx, ttag);
       }

       // update state
       d_bstate.update_state(noutput_items);

       return noutput_items;
     }
     else if (d_bstate.state == rapid_block_state_t::READING_THE_REST) {

       noutput_items = std::min(noutput_items, d_bstate.samples_left);

       auto ec = driver_get_rapid_block_data(d_bstate.offset, noutput_items,
               d_bstate.waveform_idx, output_items, d_status);
       if (ec) {
         add_error_code(ec);
         return -1;
       }

       // update state
       d_bstate.update_state(noutput_items);

       return noutput_items;
     }

     return -1;
   }

   void
   digitizer_block_impl::poll_work_function()
   {
     boost::unique_lock<boost::mutex> lock(d_poller_mutex, boost::defer_lock);
     auto poll_rate = boost::chrono::microseconds((long)(d_poll_rate * 1000000));

     gr::thread::set_thread_name(pthread_self(), "poller");

     //relax cpu with less lock calls.
     unsigned int check_every_n_times = 10;
     unsigned int poller_state_check_counter = check_every_n_times;
     poller_state_t state;

     while (true) {

       poller_state_check_counter++;
       if(poller_state_check_counter >= check_every_n_times) {
         lock.lock();
         state = d_poller_state;
         lock.unlock();
         poller_state_check_counter = 0;
       }


       if (state == poller_state_t::RUNNING) {
         // Start watchdog a new
         auto poll_start = boost::chrono::high_resolution_clock::now();
         auto ec = driver_poll();
         if (ec) {
           // Only print out an error message
           GR_LOG_ERROR(d_logger, "poll failed with: " + to_string(ec));
           // Notify work method about the error... Work method will re-arm the driver if required.
           d_app_buffer.notify_data_ready(ec);
         }

         // Watchdog is "turned on" only some time after the acquisition start for two reasons:
         // - to avoid false positives
         // - to avoid fast rearm attempts
         float estimated_samp_rate = 0.0;
         {
           // Note, mutex is not needed in case of PicoScope implementations but in order to make
           // the base class relatively generic we use mutex (streaming callback is called from this
           //  thread).
           boost::mutex::scoped_lock watchdog_guard(d_watchdog_mutex);
           estimated_samp_rate = d_estimated_sample_rate.get_avg_value();
         }

         if (estimated_samp_rate < (get_samp_rate() * WATCHDOG_SAMPLE_RATE_THRESHOLD)) {
           // This will wake up the worker thread (see do_work method), and that thread will
           // then rearm the device...
           GR_LOG_ERROR(d_logger, "Watchdog: estimated sample rate " + std::to_string(estimated_samp_rate)
                + "Hz, expected: " + std::to_string(get_samp_rate()) + "Hz");
           d_app_buffer.notify_data_ready(digitizer_block_errc::Watchdog);

         }
         boost::chrono::duration<float> poll_duration = boost::chrono::high_resolution_clock::now() - poll_start;

         boost::this_thread::sleep_for(poll_rate - poll_duration);
       }
       else {
         if (state == poller_state_t::PEND_IDLE) {
           lock.lock();
           d_poller_state = state = poller_state_t::IDLE;
           lock.unlock();

           d_poller_cv.notify_all();
         }
         else if (state == poller_state_t::PEND_EXIT) {
           lock.lock();
           d_poller_state = state = poller_state_t::EXIT;
           lock.unlock();

           d_poller_cv.notify_all();
           return;
         }

         // Relax CPU
         boost::this_thread::sleep_for(boost::chrono::microseconds(100));
       }
     }
   }

   void
   digitizer_block_impl::start_poll_thread()
   {
     if (!d_poller.joinable()) {
       boost::mutex::scoped_lock guard(d_poller_mutex);
       d_poller_state = poller_state_t::IDLE;
       d_poller = boost::thread(&digitizer_block_impl::poll_work_function, this);
     }
   }

   void
   digitizer_block_impl::stop_poll_thread()
   {
     if (!d_poller.joinable()) {
       return;
     }

     boost::unique_lock<boost::mutex> lock(d_poller_mutex);
     d_poller_state = poller_state_t::PEND_EXIT;
     d_poller_cv.wait_for(lock, boost::chrono::seconds(5),
             [this] { return d_poller_state == poller_state_t::EXIT;});
     lock.unlock();

     d_poller.join();
   }

   void
   digitizer_block_impl::transit_poll_thread_to_idle()
   {
     boost::unique_lock<boost::mutex> lock(d_poller_mutex);

     if (d_poller_state == poller_state_t::EXIT) {
       return; // nothing to do
     }

     d_poller_state = poller_state_t::PEND_IDLE;
     d_poller_cv.wait(lock, [this] { return d_poller_state == poller_state_t::IDLE;});
   }

   void
   digitizer_block_impl::transit_poll_thread_to_running()
   {
     boost::mutex::scoped_lock guard(d_poller_mutex);
     d_poller_state = poller_state_t::RUNNING;
   }

   int
   digitizer_block_impl::work_stream(int noutput_items, gr_vector_void_star &output_items)
   {
     assert(noutput_items >= static_cast<int>(d_buffer_size));

     // process only one buffer per iteration
     noutput_items = d_buffer_size;

     // wait data on application buffer
     auto ec = d_app_buffer.wait_data_ready();

     if (ec) { add_error_code(ec); }

     if (ec == digitizer_block_errc::Stopped) {
       GR_LOG_INFO(d_logger, "stop requested");
       return -1; // stop
     }
     else if (ec == digitizer_block_errc::Watchdog) {
       GR_LOG_ERROR(d_logger, "Watchdog triggered, rearming device...");
       // Rearm device
       disarm();
       arm();
       return 0; // work will be called again
     }
     if (ec) {
       GR_LOG_ERROR(d_logger, "Error reading stream data: " + to_string(ec));
       return -1;  // stop
     }

     int output_items_idx = 0;
     int buff_idx = 0;
     int port_idx = 0;

     for (auto i = 0; i < d_ai_channels; i++) {
       if (d_channel_settings[i].enabled) {
         ai_buffers[buff_idx] = static_cast<float *>(output_items[output_items_idx]);
         output_items_idx++;
         ai_error_buffers[buff_idx] = static_cast<float *>(output_items[output_items_idx]);
         output_items_idx++;
         buff_idx++;
       }
       else {
         output_items_idx += 2; // Skip disabled channels
       }
     }

     for (auto i = 0; i < d_ports; i++) {
       if (d_port_settings[i].enabled) {
         port_buffers[port_idx] = static_cast<uint8_t *>(output_items[output_items_idx]);
         output_items_idx++;
         port_idx++;
       }
       else {
         output_items_idx++;
       }
     }

     // This will write samples directly into GR output buffers
     std::vector<uint32_t> channel_status;
     int64_t local_timstamp;
     auto lost_count = d_app_buffer.get_data_chunk(ai_buffers, ai_error_buffers,
             port_buffers, channel_status, local_timstamp);

     if (lost_count) {
       GR_LOG_WARN(d_logger, std::to_string(lost_count) + " digitizer data buffers lost");
     }

     // Compile acquisition info tag
     acq_info_t tag_info{};

     tag_info.timestamp = get_timestamp_utc_ns();
     tag_info.timebase = get_timebase_with_downsampling();
     tag_info.user_delay = 0.0;
     tag_info.actual_delay = 0.0;
     tag_info.samples = d_buffer_size;
     tag_info.offset = nitems_written(0);
     tag_info.triggered_data = false;
     tag_info.trigger_timestamp = -1;

     // Attach tags to the channel values...
     int output_idx = 0;

     for (auto i = 0; i < d_ai_channels; i++) {
       if (d_channel_settings[i].enabled) {
         // add channel specific status
         tag_info.status = channel_status.at(i);

         auto tag = make_acq_info_tag(tag_info);
         add_item_tag(output_idx, tag);

         output_idx += 2;
       }
     }

     // ...and to all digital ports
     tag_info.status = 0;
     auto tag = make_acq_info_tag(tag_info);

     for (auto i = 0; i < d_ports; i++) {
       if (d_port_settings[i].enabled) {
           add_item_tag(output_idx, tag);
           output_idx ++;
       }
     }

     // Software-based trigger detection
     std::vector<int> trigger_offsets;

     if (d_trigger_settings.is_analog()) {

       // TODO: improve, check selected trigger on arm
       const auto aichan = convert_to_aichan_idx(d_trigger_settings.source);
       auto output_idx = 0;

       for (int i = 0; i < aichan; i++) {
         if (d_channel_settings[i].enabled) {
           output_idx += 2;
         }
       }

       auto buffer = static_cast<float const * const>(output_items[output_idx]);
       trigger_offsets = find_analog_triggers(buffer, d_buffer_size);
     }
     else if (d_trigger_settings.is_digital()) {
         auto port = d_trigger_settings.pin_number / 8;
         auto pin = d_trigger_settings.pin_number % 8;
         auto mask = 1 << pin;

         auto buffer = static_cast<uint8_t const * const>(output_items[output_items.size() - d_ports + port]);
         trigger_offsets = find_digital_triggers(buffer, d_buffer_size, mask);
     }

     // Attach trigger tags
     for (auto trigger_offset : trigger_offsets) {

       auto trigger_tag = make_trigger_tag();
       trigger_tag.offset = nitems_written(0) + trigger_offset;
       int output_idx = 0;

       for (auto i = 0; i < d_ai_channels; i++) {
         if (d_channel_settings[i].enabled) {
           add_item_tag(output_idx, trigger_tag);
           output_idx += 2;
         }
       }

       for (auto i = 0; i < d_ports; i++) {
         if (d_port_settings[i].enabled) {
           add_item_tag(output_idx, trigger_tag);
           output_idx++;
         }
       }
     }

     return noutput_items;
   }

   int
   digitizer_block_impl::work(int noutput_items,
       gr_vector_const_void_star &input_items,
       gr_vector_void_star &output_items)
   {
     int retval = -1;

     if(d_acquisition_mode == acquisition_mode_t::STREAMING) {
       retval = work_stream(noutput_items, output_items);
     }
     else if(d_acquisition_mode == acquisition_mode_t::RAPID_BLOCK) {
       retval = work_rapid_block(noutput_items, output_items);
     }

     if ((retval > 0) && !d_timebase_published) {
       auto timebase_tag = make_timebase_info_tag(get_timebase_with_downsampling());
       timebase_tag.offset = nitems_written(0);

       for (gr_vector_void_star::size_type i = 0; i < output_items.size(); i++) {
         add_item_tag(i, timebase_tag);
       }

       d_timebase_published = true;
     }

     return retval;
   }

  } /* namespace digitizers */
} /* namespace gr */

