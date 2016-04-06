/*!
 * \file gps_l2_m_dll_pll_tracking_cc.cc
 * \brief Implementation of a code DLL + carrier PLL tracking block
 * \author Carlos Aviles, 2010. carlos.avilesr(at)googlemail.com
 *         Javier Arribas, 2011. jarribas(at)cttc.es
 *
 * Code DLL + carrier PLL according to the algorithms described in:
 * [1] K.Borre, D.M.Akos, N.Bertelsen, P.Rinder, and S.H.Jensen,
 * A Software-Defined GPS and Galileo Receiver. A Single-Frequency
 * Approach, Birkhauser, 2007
 *
 * -------------------------------------------------------------------------
 *
 * Copyright (C) 2010-2015  (see AUTHORS file for a list of contributors)
 *
 * GNSS-SDR is a software defined Global Navigation
 *          Satellite Systems receiver
 *
 * This file is part of GNSS-SDR.
 *
 * GNSS-SDR is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GNSS-SDR is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNSS-SDR. If not, see <http://www.gnu.org/licenses/>.
 *
 * -------------------------------------------------------------------------
 */

#include "gps_l2_m_dll_pll_tracking_cc.h"
#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>
#include <boost/lexical_cast.hpp>
#include <gnuradio/io_signature.h>
#include <glog/logging.h>
#include <volk/volk.h>
#include "gps_l2c_signal.h"
#include "tracking_discriminators.h"
#include "lock_detectors.h"
#include "GPS_L2C.h"
#include "control_message_factory.h"


/*!
 * \todo Include in definition header file
 */
#define GPS_L2M_CN0_ESTIMATION_SAMPLES 10
#define GPS_L2M_MINIMUM_VALID_CN0 25
#define GPS_L2M_MAXIMUM_LOCK_FAIL_COUNTER 50
#define GPS_L2M_CARRIER_LOCK_THRESHOLD 0.75


using google::LogMessage;

gps_l2_m_dll_pll_tracking_cc_sptr
gps_l2_m_dll_pll_make_tracking_cc(
        long if_freq,
        long fs_in,
        unsigned int vector_length,
        boost::shared_ptr<gr::msg_queue> queue,
        bool dump,
        std::string dump_filename,
        float pll_bw_hz,
        float dll_bw_hz,
        float early_late_space_chips)
{
    return gps_l2_m_dll_pll_tracking_cc_sptr(new gps_l2_m_dll_pll_tracking_cc(if_freq,
            fs_in, vector_length, queue, dump, dump_filename, pll_bw_hz, dll_bw_hz, early_late_space_chips));
}



void gps_l2_m_dll_pll_tracking_cc::forecast (int noutput_items,
        gr_vector_int &ninput_items_required)
{
    if (noutput_items != 0)
        {
            ninput_items_required[0] = static_cast<int>(d_vector_length) * 2; //set the required available samples in each call
        }
}



gps_l2_m_dll_pll_tracking_cc::gps_l2_m_dll_pll_tracking_cc(
        long if_freq,
        long fs_in,
        unsigned int vector_length,
        boost::shared_ptr<gr::msg_queue> queue,
        bool dump,
        std::string dump_filename,
        float pll_bw_hz,
        float dll_bw_hz,
        float early_late_space_chips) :
        gr::block("gps_l2_m_dll_pll_tracking_cc", gr::io_signature::make(1, 1, sizeof(gr_complex)),
                gr::io_signature::make(1, 1, sizeof(Gnss_Synchro)))
{
    // Telemetry bit synchronization message port input
    this->message_port_register_in(pmt::mp("preamble_timestamp_s"));
    // initialize internal vars
    d_queue = queue;
    d_dump = dump;
    d_if_freq = if_freq;
    d_fs_in = fs_in;
    d_vector_length = vector_length;
    d_dump_filename = dump_filename;

    // DLL/PLL filter initialization
    d_carrier_loop_filter=Tracking_2nd_PLL_filter(GPS_L2_M_PERIOD);
    d_code_loop_filter=Tracking_2nd_DLL_filter(GPS_L2_M_PERIOD);

    // Initialize tracking  ==========================================
    d_code_loop_filter.set_DLL_BW(dll_bw_hz);
    d_carrier_loop_filter.set_PLL_BW(pll_bw_hz);

    //--- DLL variables --------------------------------------------------------
    d_early_late_spc_chips = early_late_space_chips; // Define early-late offset (in chips)

    // Initialization of local code replica
    // Get space for a vector with the C/A code replica sampled 1x/chip
    d_ca_code = static_cast<gr_complex*>(volk_malloc(static_cast<int>(GPS_L2_M_CODE_LENGTH_CHIPS) * sizeof(gr_complex), volk_get_alignment()));

    // correlator outputs (scalar)
    d_n_correlator_taps = 3; // Early, Prompt, and Late
    d_correlator_outs = static_cast<gr_complex*>(volk_malloc(d_n_correlator_taps*sizeof(gr_complex), volk_get_alignment()));
    for (int n = 0; n < d_n_correlator_taps; n++)
        {
            d_correlator_outs[n] = gr_complex(0,0);
        }
    d_local_code_shift_chips = static_cast<float*>(volk_malloc(d_n_correlator_taps*sizeof(float), volk_get_alignment()));
    // Set TAPs delay values [chips]
    d_local_code_shift_chips[0] = - d_early_late_spc_chips;
    d_local_code_shift_chips[1] = 0.0;
    d_local_code_shift_chips[2] = d_early_late_spc_chips;

    multicorrelator_cpu.init(2 * d_vector_length, d_n_correlator_taps);


    //--- Perform initializations ------------------------------
    // define initial code frequency basis of NCO
    d_code_freq_chips = GPS_L2_M_CODE_RATE_HZ;
    // define residual code phase (in chips)
    d_rem_code_phase_samples = 0.0;
    // define residual carrier phase
    d_rem_carr_phase_rad = 0.0;

    // sample synchronization
    d_sample_counter = 0;
    //d_sample_counter_seconds = 0;
    d_acq_sample_stamp = 0;

    d_enable_tracking = false;
    d_pull_in = false;
    d_last_seg = 0;

    d_current_prn_length_samples = static_cast<int>(d_vector_length);

    // CN0 estimation and lock detector buffers
    d_cn0_estimation_counter = 0;
    d_Prompt_buffer = new gr_complex[GPS_L2M_CN0_ESTIMATION_SAMPLES];
    d_carrier_lock_test = 1;
    d_CN0_SNV_dB_Hz = 0;
    d_carrier_lock_fail_counter = 0;
    d_carrier_lock_threshold = GPS_L2M_CARRIER_LOCK_THRESHOLD;

    systemName["G"] = std::string("GPS");

    set_relative_rate(1.0/((double)d_vector_length*2));
    //set_min_output_buffer((long int)300);

    d_channel_internal_queue = 0;
    d_acquisition_gnss_synchro = 0;
    d_channel = 0;
    d_acq_code_phase_samples = 0.0;
    d_acq_carrier_doppler_hz = 0.0;
    d_carrier_doppler_hz = 0.0;
    d_acc_carrier_phase_rad = 0.0;
    d_code_phase_samples = 0.0;
    d_acc_code_phase_secs = 0.0;

    LOG(INFO) << "d_vector_length" << d_vector_length;
}


void gps_l2_m_dll_pll_tracking_cc::start_tracking()
{
    /*
     *  correct the code phase according to the delay between acq and trk
     */
    d_acq_code_phase_samples = d_acquisition_gnss_synchro->Acq_delay_samples;
    d_acq_carrier_doppler_hz = d_acquisition_gnss_synchro->Acq_doppler_hz;
    d_acq_sample_stamp =  d_acquisition_gnss_synchro->Acq_samplestamp_samples;

    long int acq_trk_diff_samples;
    float acq_trk_diff_seconds;
    acq_trk_diff_samples = static_cast<long int>(d_sample_counter) - static_cast<long int>(d_acq_sample_stamp);//-d_vector_length;
    LOG(INFO) << "Number of samples between Acquisition and Tracking =" << acq_trk_diff_samples;
    acq_trk_diff_seconds = static_cast<float>(acq_trk_diff_samples) / static_cast<float>(d_fs_in);
    //doppler effect
    // Fd=(C/(C+Vr))*F
    double radial_velocity = (GPS_L2_FREQ_HZ + d_acq_carrier_doppler_hz) / GPS_L2_FREQ_HZ;
    // new chip and prn sequence periods based on acq Doppler
    double T_chip_mod_seconds;
    double T_prn_mod_seconds;
    double T_prn_mod_samples;
    d_code_freq_chips = radial_velocity * GPS_L2_M_CODE_RATE_HZ;
    T_chip_mod_seconds = 1/d_code_freq_chips;
    T_prn_mod_seconds = T_chip_mod_seconds * GPS_L2_M_CODE_LENGTH_CHIPS;
    T_prn_mod_samples = T_prn_mod_seconds * static_cast<float>(d_fs_in);

    d_current_prn_length_samples = round(T_prn_mod_samples);

    double T_prn_true_seconds = GPS_L2_M_CODE_LENGTH_CHIPS / GPS_L2_M_CODE_RATE_HZ;
    double T_prn_true_samples = T_prn_true_seconds * static_cast<float>(d_fs_in);
    double T_prn_diff_seconds=  T_prn_true_seconds - T_prn_mod_seconds;
    double N_prn_diff = acq_trk_diff_seconds / T_prn_true_seconds;
    double corrected_acq_phase_samples, delay_correction_samples;
    corrected_acq_phase_samples = fmod((d_acq_code_phase_samples + T_prn_diff_seconds * N_prn_diff * static_cast<float>(d_fs_in)), T_prn_true_samples);
    if (corrected_acq_phase_samples < 0)
        {
            corrected_acq_phase_samples = T_prn_mod_samples + corrected_acq_phase_samples;
        }
    delay_correction_samples = d_acq_code_phase_samples - corrected_acq_phase_samples;
    //TODO: debug the algorithm implementation and enable correction
    //d_acq_code_phase_samples = corrected_acq_phase_samples;

    d_carrier_doppler_hz = d_acq_carrier_doppler_hz;

    // DLL/PLL filter initialization
    d_carrier_loop_filter.initialize(); // initialize the carrier filter
    d_code_loop_filter.initialize();    // initialize the code filter

    // generate local reference ALWAYS starting at chip 1 (1 sample per chip)
    gps_l2c_m_code_gen_complex(d_ca_code, d_acquisition_gnss_synchro->PRN);

    multicorrelator_cpu.set_local_code_and_taps(static_cast<int>(GPS_L2_M_CODE_LENGTH_CHIPS), d_ca_code, d_local_code_shift_chips);
    for (int n = 0; n < d_n_correlator_taps; n++)
        {
            d_correlator_outs[n] = gr_complex(0,0);
        }

    d_carrier_lock_fail_counter = 0;
    d_rem_code_phase_samples = 0;
    d_rem_carr_phase_rad = 0;
    d_acc_carrier_phase_rad = 0;
    d_acc_code_phase_secs = 0;

    d_code_phase_samples = d_acq_code_phase_samples;

    std::string sys_ = &d_acquisition_gnss_synchro->System;
    sys = sys_.substr(0,1);

    // DEBUG OUTPUT
    std::cout << "Tracking start on channel " << d_channel << " for satellite " << Gnss_Satellite(systemName[sys], d_acquisition_gnss_synchro->PRN) <<" whith Doppler="<<d_acq_carrier_doppler_hz<<" [Hz]"<< std::endl;
    LOG(INFO) << "Starting tracking of satellite " << Gnss_Satellite(systemName[sys], d_acquisition_gnss_synchro->PRN) << " on channel " << d_channel;


    // enable tracking
    d_pull_in = true;
    d_enable_tracking = true;

    LOG(INFO) << "PULL-IN Doppler [Hz]=" << d_carrier_doppler_hz
            << " Code Phase correction [samples]=" << delay_correction_samples
            << " PULL-IN Code Phase [samples]=" << d_acq_code_phase_samples;
}

gps_l2_m_dll_pll_tracking_cc::~gps_l2_m_dll_pll_tracking_cc()
{
	d_dump_file.close();

	volk_free(d_local_code_shift_chips);
	volk_free(d_correlator_outs);
	volk_free(d_ca_code);

	delete[] d_Prompt_buffer;
	multicorrelator_cpu.free();
}



int gps_l2_m_dll_pll_tracking_cc::general_work (int noutput_items __attribute__((unused)), gr_vector_int &ninput_items __attribute__((unused)),
        gr_vector_const_void_star &input_items, gr_vector_void_star &output_items)
{
    // process vars
    double carr_error_hz = 0;
    double carr_error_filt_hz = 0;
    double code_error_chips = 0;
    double code_error_filt_chips = 0;

    // GNSS_SYNCHRO OBJECT to interchange data between tracking->telemetry_decoder
    Gnss_Synchro current_synchro_data = Gnss_Synchro();

    // Block input data and block output stream pointers
    const gr_complex* in = (gr_complex*) input_items[0]; //PRN start block alignment
    Gnss_Synchro **out = (Gnss_Synchro **) &output_items[0];

    if (d_enable_tracking == true)
        {
            // Receiver signal alignment
            if (d_pull_in == true)
                {
                    int samples_offset;
                    double acq_trk_shif_correction_samples;
                    int acq_to_trk_delay_samples;
                    acq_to_trk_delay_samples = (d_sample_counter - (d_acq_sample_stamp-d_current_prn_length_samples));
                    acq_trk_shif_correction_samples = -fmod(static_cast<float>(acq_to_trk_delay_samples), static_cast<float>(d_current_prn_length_samples));
                    samples_offset = round(d_acq_code_phase_samples + acq_trk_shif_correction_samples);//+(1.5*(d_fs_in/GPS_L2_M_CODE_RATE_HZ)));
                    // /todo: Check if the sample counter sent to the next block as a time reference should be incremented AFTER sended or BEFORE
                    //d_sample_counter_seconds = d_sample_counter_seconds + (((double)samples_offset) / static_cast<double>(d_fs_in));
                    d_sample_counter = d_sample_counter + samples_offset; //count for the processed samples
                    d_pull_in = false;
                    std::cout<<" acq_to_trk_delay_samples="<<acq_to_trk_delay_samples<<std::endl;
                    std::cout<<" acq_trk_shif_correction_samples="<<acq_trk_shif_correction_samples<<std::endl;
                    std::cout<<" d_acq_code_phase_samples="<<d_acq_code_phase_samples<<std::endl;
                    std::cout<<" d_current_prn_length_samples="<<d_current_prn_length_samples<<std::endl;
                    std::cout<<" samples_offset="<<samples_offset<<std::endl;
                    // Fill the acquisition data
                    current_synchro_data = *d_acquisition_gnss_synchro;
                    current_synchro_data.Flag_valid_tracking = false;
                    *out[0] = current_synchro_data;
                    consume_each(samples_offset); //shift input to perform alignment with local replica
                    return 1;
                }

            // Fill the acquisition data
            current_synchro_data = *d_acquisition_gnss_synchro;

            // ################# CARRIER WIPEOFF AND CORRELATORS ##############################
            // perform carrier wipe-off and compute Early, Prompt and Late correlation
            multicorrelator_cpu.set_input_output_vectors(d_correlator_outs,in);
            multicorrelator_cpu.Carrier_wipeoff_multicorrelator_resampler(d_rem_carr_phase_rad,
            		d_carrier_phase_step_rad,
            		d_rem_code_phase_chips,
            		d_code_phase_step_chips,
            		d_current_prn_length_samples);

            // ################## PLL ##########################################################
            // PLL discriminator
            carr_error_hz = pll_cloop_two_quadrant_atan(d_correlator_outs[1]) / GPS_L2_TWO_PI;
            // Carrier discriminator filter
            carr_error_filt_hz = d_carrier_loop_filter.get_carrier_nco(carr_error_hz);
            // New carrier Doppler frequency estimation
            d_carrier_doppler_hz = d_acq_carrier_doppler_hz + carr_error_filt_hz;
            // New code Doppler frequency estimation
            d_code_freq_chips = GPS_L2_M_CODE_RATE_HZ + ((d_carrier_doppler_hz * GPS_L2_M_CODE_RATE_HZ) / GPS_L2_FREQ_HZ);
            //carrier phase accumulator for (K) doppler estimation
            d_acc_carrier_phase_rad -= GPS_L2_TWO_PI * d_carrier_doppler_hz * GPS_L2_M_PERIOD;
            //remanent carrier phase to prevent overflow in the code NCO
            d_rem_carr_phase_rad = d_rem_carr_phase_rad + GPS_L2_TWO_PI * d_carrier_doppler_hz * GPS_L2_M_PERIOD;
            d_rem_carr_phase_rad = fmod(d_rem_carr_phase_rad, GPS_L2_TWO_PI);

            // ################## DLL ##########################################################
            // DLL discriminator
            code_error_chips = dll_nc_e_minus_l_normalized(d_correlator_outs[0], d_correlator_outs[2]); //[chips/Ti]
            // Code discriminator filter
            code_error_filt_chips = d_code_loop_filter.get_code_nco(code_error_chips); //[chips/second]
            //Code phase accumulator
            double code_error_filt_secs;
            code_error_filt_secs = (GPS_L2_M_PERIOD * code_error_filt_chips) / GPS_L2_M_CODE_RATE_HZ; //[seconds]
            d_acc_code_phase_secs = d_acc_code_phase_secs + code_error_filt_secs;

            // ################## CARRIER AND CODE NCO BUFFER ALIGNEMENT #######################
            // keep alignment parameters for the next input buffer
            double T_chip_seconds;
            double T_prn_seconds;
            double T_prn_samples;
            double K_blk_samples;
            // Compute the next buffer length based in the new period of the PRN sequence and the code phase error estimation
            T_chip_seconds = 1.0 / d_code_freq_chips;
            T_prn_seconds = T_chip_seconds * GPS_L2_M_CODE_LENGTH_CHIPS;
            T_prn_samples = T_prn_seconds * static_cast<double>(d_fs_in);
            K_blk_samples = T_prn_samples + d_rem_code_phase_samples + code_error_filt_secs * static_cast<double>(d_fs_in);
            d_current_prn_length_samples = round(K_blk_samples); //round to a discrete samples

            //################### PLL COMMANDS #################################################
            //carrier phase step (NCO phase increment per sample) [rads/sample]
            d_carrier_phase_step_rad = GPS_L2_TWO_PI * d_carrier_doppler_hz / static_cast<double>(d_fs_in);

            //################### DLL COMMANDS #################################################
            //code phase step (Code resampler phase increment per sample) [chips/sample]
            d_code_phase_step_chips = d_code_freq_chips / static_cast<double>(d_fs_in);

            //remnant code phase [chips]
            d_rem_code_phase_chips = d_rem_code_phase_samples * (d_code_freq_chips / static_cast<double>(d_fs_in));

            // ####### CN0 ESTIMATION AND LOCK DETECTORS ######
            if (d_cn0_estimation_counter < GPS_L2M_CN0_ESTIMATION_SAMPLES)
                {
                    // fill buffer with prompt correlator output values
                    d_Prompt_buffer[d_cn0_estimation_counter] = d_correlator_outs[1];
                    d_cn0_estimation_counter++;
                }
            else
                {
                    d_cn0_estimation_counter = 0;
                    // Code lock indicator
                    d_CN0_SNV_dB_Hz = cn0_svn_estimator(d_Prompt_buffer, GPS_L2M_CN0_ESTIMATION_SAMPLES, d_fs_in, GPS_L2_M_CODE_LENGTH_CHIPS);
                    // Carrier lock indicator
                    d_carrier_lock_test = carrier_lock_detector(d_Prompt_buffer, GPS_L2M_CN0_ESTIMATION_SAMPLES);
                    // Loss of lock detection
                    if (d_carrier_lock_test < d_carrier_lock_threshold or d_CN0_SNV_dB_Hz < GPS_L2M_MINIMUM_VALID_CN0)
                        {
                            d_carrier_lock_fail_counter++;
                        }
                    else
                        {
                            if (d_carrier_lock_fail_counter > 0) d_carrier_lock_fail_counter--;
                        }
                    if (d_carrier_lock_fail_counter > GPS_L2M_MAXIMUM_LOCK_FAIL_COUNTER)
                        {
                            std::cout << "Loss of lock in channel " << d_channel << "!" << std::endl;
                            LOG(INFO) << "Loss of lock in channel " << d_channel << "!";
                            std::unique_ptr<ControlMessageFactory> cmf(new ControlMessageFactory());
                            if (d_queue != gr::msg_queue::sptr())
                                {
                                    d_queue->handle(cmf->GetQueueMessage(d_channel, 2));
                                }
                            d_carrier_lock_fail_counter = 0;
                            d_enable_tracking = false; // TODO: check if disabling tracking is consistent with the channel state machine
                        }
                }
            // ########### Output the tracking data to navigation and PVT ##########
            current_synchro_data.Prompt_I = static_cast<double>(d_correlator_outs[1].real());
            current_synchro_data.Prompt_Q = static_cast<double>(d_correlator_outs[1].imag());

            // Tracking_timestamp_secs is aligned with the CURRENT PRN start sample (Hybridization OK!, but some glitches??)
            current_synchro_data.Tracking_timestamp_secs = (static_cast<double>(d_sample_counter) + d_rem_code_phase_samples) / static_cast<double>(d_fs_in);
            //compute remnant code phase samples AFTER the Tracking timestamp
            d_rem_code_phase_samples = K_blk_samples - d_current_prn_length_samples; //rounding error < 1 sample

            //current_synchro_data.Tracking_timestamp_secs = ((double)d_sample_counter)/static_cast<double>(d_fs_in);
            // This tracking block aligns the Tracking_timestamp_secs with the start sample of the PRN, thus, Code_phase_secs=0
            current_synchro_data.Code_phase_secs = 0;
            current_synchro_data.Carrier_phase_rads = d_acc_carrier_phase_rad;
            current_synchro_data.Carrier_Doppler_hz = d_carrier_doppler_hz;
            current_synchro_data.CN0_dB_hz = d_CN0_SNV_dB_Hz;
            current_synchro_data.Flag_valid_tracking = true;
            current_synchro_data.Flag_valid_symbol_output = true;
            *out[0] = current_synchro_data;

            // ########## DEBUG OUTPUT
            /*!
             *  \todo The stop timer has to be moved to the signal source!
             */
            // debug: Second counter in channel 0
            if (d_channel == 0)
                {
                    if (floor(d_sample_counter / d_fs_in) != d_last_seg)
                        {
                            d_last_seg = floor(d_sample_counter / d_fs_in);
                            std::cout << "Current input signal time = " << d_last_seg << " [s]" << std::endl;
                            std::cout  << "GPS L2C M Tracking CH " << d_channel <<  ": Satellite "
                            		<< Gnss_Satellite(systemName[sys], d_acquisition_gnss_synchro->PRN) << ", CN0 = " << d_CN0_SNV_dB_Hz << " [dB-Hz] "<<"Doppler="<<d_carrier_doppler_hz<<" [Hz]"<< std::endl;
                            //if (d_last_seg==5) d_carrier_lock_fail_counter=500; //DEBUG: force unlock!
                        }
                }
            else
                {
                    if (floor(d_sample_counter / d_fs_in) != d_last_seg)
                        {
                            d_last_seg = floor(d_sample_counter / d_fs_in);
                            std::cout  << "GPS L2C M Tracking CH " << d_channel <<  ": Satellite "
                            << Gnss_Satellite(systemName[sys], d_acquisition_gnss_synchro->PRN)
                            << ", CN0 = " << d_CN0_SNV_dB_Hz << " [dB-Hz] "<<"Doppler="<<d_carrier_doppler_hz<<" [Hz]"<< std::endl;
                            //std::cout<<"TRK CH "<<d_channel<<" Carrier_lock_test="<<d_carrier_lock_test<< std::endl;
                        }
                }
        }
    else
        {
            // ########## DEBUG OUTPUT (TIME ONLY for channel 0 when tracking is disabled)
            /*!
             *  \todo The stop timer has to be moved to the signal source!
             */
            // stream to collect cout calls to improve thread safety
            std::stringstream tmp_str_stream;
            if (floor(d_sample_counter / d_fs_in) != d_last_seg)
                {
                    d_last_seg = floor(d_sample_counter / d_fs_in);

                    if (d_channel == 0)
                        {
                            // debug: Second counter in channel 0
                            tmp_str_stream << "Current input signal time = " << d_last_seg << " [s]" << std::endl << std::flush;
                            std::cout << tmp_str_stream.rdbuf() << std::flush;
                        }
                }
            for (int n = 0; n < d_n_correlator_taps; n++)
                {
                    d_correlator_outs[n] = gr_complex(0,0);
                }

            current_synchro_data.Flag_valid_pseudorange = false;
            current_synchro_data.Flag_valid_symbol_output = false;
            *out[0] = current_synchro_data;
        }

    if(d_dump)
        {
			// MULTIPLEXED FILE RECORDING - Record results to file
			float prompt_I;
			float prompt_Q;
			float tmp_E, tmp_P, tmp_L;
			double tmp_double;
			prompt_I = d_correlator_outs[1].real();
			prompt_Q = d_correlator_outs[1].imag();
			tmp_E = std::abs<float>(d_correlator_outs[0]);
			tmp_P = std::abs<float>(d_correlator_outs[1]);
			tmp_L = std::abs<float>(d_correlator_outs[2]);
            try
            {
                    // EPR
                    d_dump_file.write(reinterpret_cast<char*>(&tmp_E), sizeof(float));
                    d_dump_file.write(reinterpret_cast<char*>(&tmp_P), sizeof(float));
                    d_dump_file.write(reinterpret_cast<char*>(&tmp_L), sizeof(float));
                    // PROMPT I and Q (to analyze navigation symbols)
                    d_dump_file.write(reinterpret_cast<char*>(&prompt_I), sizeof(float));
                    d_dump_file.write(reinterpret_cast<char*>(&prompt_Q), sizeof(float));
                    // PRN start sample stamp
                    //tmp_float=(float)d_sample_counter;
                    d_dump_file.write(reinterpret_cast<char*>(&d_sample_counter), sizeof(unsigned long int));
                    // accumulated carrier phase
                    d_dump_file.write(reinterpret_cast<char*>(&d_acc_carrier_phase_rad), sizeof(double));

                    // carrier and code frequency
                    d_dump_file.write(reinterpret_cast<char*>(&d_carrier_doppler_hz), sizeof(double));
                    d_dump_file.write(reinterpret_cast<char*>(&d_code_freq_chips), sizeof(double));

                    //PLL commands
                    d_dump_file.write(reinterpret_cast<char*>(&carr_error_hz), sizeof(double));
                    d_dump_file.write(reinterpret_cast<char*>(&d_carrier_doppler_hz), sizeof(double));

                    //DLL commands
                    d_dump_file.write(reinterpret_cast<char*>(&code_error_chips), sizeof(double));
                    d_dump_file.write(reinterpret_cast<char*>(&code_error_filt_chips), sizeof(double));

                    // CN0 and carrier lock test
                    d_dump_file.write(reinterpret_cast<char*>(&d_CN0_SNV_dB_Hz), sizeof(double));
                    d_dump_file.write(reinterpret_cast<char*>(&d_carrier_lock_test), sizeof(double));

                    // AUX vars (for debug purposes)
                    tmp_double = d_rem_code_phase_samples;
                    d_dump_file.write(reinterpret_cast<char*>(&tmp_double), sizeof(double));
                    tmp_double = static_cast<double>(d_sample_counter + d_current_prn_length_samples);
                    d_dump_file.write(reinterpret_cast<char*>(&tmp_double), sizeof(double));
            }
            catch (std::ifstream::failure& e)
            {
                    LOG(WARNING) << "Exception writing trk dump file " << e.what();
            }
        }
    consume_each(d_current_prn_length_samples); // this is necessary in gr::block derivates
    d_sample_counter += d_current_prn_length_samples; //count for the processed samples
    return 1; //output tracking result ALWAYS even in the case of d_enable_tracking==false
}



void gps_l2_m_dll_pll_tracking_cc::set_channel(unsigned int channel)
{
    d_channel = channel;
    LOG(INFO) << "Tracking Channel set to " << d_channel;
    // ############# ENABLE DATA FILE LOG #################
    if (d_dump == true)
        {
            if (d_dump_file.is_open() == false)
                {
                    try
                    {
                            d_dump_filename.append(boost::lexical_cast<std::string>(d_channel));
                            d_dump_filename.append(".dat");
                            d_dump_file.exceptions (std::ifstream::failbit | std::ifstream::badbit);
                            d_dump_file.open(d_dump_filename.c_str(), std::ios::out | std::ios::binary);
                            LOG(INFO) << "Tracking dump enabled on channel " << d_channel << " Log file: " << d_dump_filename.c_str() << std::endl;
                    }
                    catch (std::ifstream::failure& e)
                    {
                            LOG(WARNING) << "channel " << d_channel << " Exception opening trk dump file " << e.what() << std::endl;
                    }
                }
        }
}



void gps_l2_m_dll_pll_tracking_cc::set_channel_queue(concurrent_queue<int> *channel_internal_queue)
{
    d_channel_internal_queue = channel_internal_queue;
}


void gps_l2_m_dll_pll_tracking_cc::set_gnss_synchro(Gnss_Synchro* p_gnss_synchro)
{
    d_acquisition_gnss_synchro = p_gnss_synchro;
}
