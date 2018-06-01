/* -*- c++ -*- */
/* Copyright (C) 2018 GSI Darmstadt, Germany - All Rights Reserved
 * co-developed with: Cosylab, Ljubljana, Slovenia and CERN, Geneva, Switzerland
 * You may use, distribute and modify this code under the terms of the GPL v.3  license.
 */

#ifndef INCLUDED_DIGITIZERS_CASCADE_SINK_IMPL_H
#define INCLUDED_DIGITIZERS_CASCADE_SINK_IMPL_H

#include <digitizers/cascade_sink.h>
#include <digitizers/block_aggregation.h>
#include <digitizers/time_domain_sink.h>
#include <digitizers/post_mortem_sink.h>

namespace gr {
  namespace digitizers {

    class cascade_sink_impl : public cascade_sink
    {
     private:
      block_aggregation::sptr d_agg1000;
      block_aggregation::sptr d_agg100;
      block_aggregation::sptr d_agg10;
      block_aggregation::sptr d_agg1;

      time_domain_sink::sptr d_snk1000;
      time_domain_sink::sptr d_snk100;
      time_domain_sink::sptr d_snk10;
      time_domain_sink::sptr d_snk1;

      post_mortem_sink::sptr d_pm_raw;
      post_mortem_sink::sptr d_pm_1000;

     public:
      cascade_sink_impl(int alg_id,
          int delay,
          const std::vector<float> &fir_taps,
          double low_freq,
          double up_freq,
          double tr_width,
          const std::vector<double> &fb_user_taps,
          const std::vector<double> &fw_user_taps,
          double samp_rate,
          float pm_buffer,
          std::string signal_name,
          std::string unit_name);

      ~cascade_sink_impl();

      std::vector<time_domain_sink::sptr> get_time_domain_sinks() override;

      std::vector<post_mortem_sink::sptr> get_post_mortem_sinks() override;

    };

  } // namespace digitizers
} // namespace gr

#endif /* INCLUDED_DIGITIZERS_CASCADE_SINK_IMPL_H */

