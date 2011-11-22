/*
 * Copyright (c) 2011 Joseph Gaeddert
 * Copyright (c) 2011 Virginia Polytechnic Institute & State University
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

//
// wlanframesync.c
//

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>

#include "liquid-wlan.internal.h"

#define DEBUG_WLANFRAMESYNC             1
#define DEBUG_WLANFRAMESYNC_PRINT       0
#define DEBUG_WLANFRAMESYNC_FILENAME    "wlanframesync_internal_debug.m"
#define DEBUG_WLANFRAMESYNC_BUFFER_LEN  (2048)

#define WLANFRAMESYNC_ENABLE_SQUELCH    0

struct wlanframesync_s {
    // callback
    wlanframesync_callback callback;
    void * userdata;

    // options
    unsigned int rate;      // primitive data rate
    unsigned int length;    // original data length (bytes)
    unsigned int seed;      // data scrambler seed

    // transform object
    FFT_PLAN fft;           // ifft object
    float complex * X;      // frequency-domain buffer
    float complex * x;      // time-domain buffer
    windowcf input_buffer;  // input sequence buffer

    // synchronizer objects
    nco_crcf nco_rx;        // numerically-controlled oscillator
    msequence ms_pilot;     // pilot sequence generator
    modem demod;            // DATA field demodulator

    // gain/equalization
    float complex G0a[64], G0b[64]; // complex channel gain (short sequences)
    float complex G[64];            // complex channel gain

    // lengths
    unsigned int ndbps;             // number of data bits per OFDM symbol
    unsigned int ncbps;             // number of coded bits per OFDM symbol
    unsigned int nbpsc;             // number of bits per subcarrier (modulation depth)
    unsigned int dec_msg_len;       // length of decoded message (bytes)
    unsigned int enc_msg_len;       // length of encoded message (bytes)
    unsigned int nsym;              // number of OFDM symbols in the DATA field
    unsigned int ndata;             // number of bits in the DATA field
    unsigned int npad;              // number of pad bits

    // data arrays
    unsigned char   signal_int[6];  // interleaved message (SIGNAL field)
    unsigned char   signal_enc[6];  // encoded message (SIGNAL field)
    unsigned char   signal_dec[3];  // decoded message (SIGNAL field)
    unsigned char * msg_enc;        // encoded message (DATA field)
    unsigned char   modem_syms[48]; // modem symbols
    
    // counters/states
    enum {
        WLANFRAMESYNC_STATE_SEEKPLCP=0, // seek initial PLCP
        WLANFRAMESYNC_STATE_RXSHORT0,   // receive first 'short' sequence
        WLANFRAMESYNC_STATE_RXSHORT1,   // receive second 'short' sequence
        WLANFRAMESYNC_STATE_RXLONG0,    // receive first 'long' sequence
        WLANFRAMESYNC_STATE_RXLONG1,    // receive second 'long' sequence
        WLANFRAMESYNC_STATE_RXSIGNAL,   // receive SIGNAL field
        WLANFRAMESYNC_STATE_RXDATA,     // receive DATA field
    } state;
    signed int timer;                   // sample timer

#if DEBUG_WLANFRAMESYNC
    agc_crcf agc_rx;        // automatic gain control (rssi)
    windowcf debug_x;
    windowf  debug_rssi;
#endif
};

// create WLAN framing synchronizer object
//  _callback   :   user-defined callback function
//  _userdata   :   user-defined data structure
wlanframesync wlanframesync_create(wlanframesync_callback _callback,
                                   void *                 _userdata)
{
    // allocate main object memory
    wlanframesync q = (wlanframesync) malloc(sizeof(struct wlanframesync_s));
    
    // set callback data
    q->callback = _callback;
    q->userdata = _userdata;

    // create transform object
    q->X = (float complex*) malloc(64*sizeof(float complex));
    q->x = (float complex*) malloc(64*sizeof(float complex));
    q->fft = FFT_CREATE_PLAN(64, q->x, q->X, FFT_DIR_FORWARD, FFT_METHOD);
 
    // create input buffer the length of the transform
    q->input_buffer = windowcf_create(80);

    // synchronizer objects
    q->nco_rx = nco_crcf_create(LIQUID_VCO);
    q->ms_pilot = msequence_create(7, 0x91, 0x7f);
    q->demod = modem_create(LIQUID_MODEM_BPSK, 1);

    // set initial properties
    q->rate   = WLANFRAME_RATE_6;
    q->length = 100;
    q->seed   = 0x5d;

    // allocate memory for encoded message
    q->enc_msg_len = wlan_packet_compute_enc_msg_len(q->rate, q->length);
    q->msg_enc = (unsigned char*) malloc(q->enc_msg_len*sizeof(unsigned char));

    // reset object
    wlanframesync_reset(q);

#if DEBUG_WLANFRAMESYNC
    // agc, rssi
    q->agc_rx = agc_crcf_create();
    agc_crcf_set_bandwidth(q->agc_rx,  1e-2f);
    agc_crcf_set_gain_limits(q->agc_rx, 1e-5f, 1e5f);

    q->debug_x =        windowcf_create(DEBUG_WLANFRAMESYNC_BUFFER_LEN);
    q->debug_rssi =     windowf_create(DEBUG_WLANFRAMESYNC_BUFFER_LEN);
#endif

    // return object
    return q;
}

// destroy WLAN framing synchronizer object
void wlanframesync_destroy(wlanframesync _q)
{
#if DEBUG_WLANFRAMESYNC
    wlanframesync_debug_print(_q, DEBUG_WLANFRAMESYNC_FILENAME);

    agc_crcf_destroy(_q->agc_rx);

    windowcf_destroy(_q->debug_x);
    windowf_destroy(_q->debug_rssi);
#endif

    // free transform object
    windowcf_destroy(_q->input_buffer);
    free(_q->X);
    free(_q->x);
    FFT_DESTROY_PLAN(_q->fft);
    
    // destroy synchronizer objects
    nco_crcf_destroy(_q->nco_rx);       // numerically-controlled oscillator
    msequence_destroy(_q->ms_pilot);    // pilot sequence generator
    modem_destroy(_q->demod);           // DATA field (payload) demodulator

    // free memory for encoded message
    free(_q->msg_enc);

    // free main object memory
    free(_q);
}

// print WLAN framing synchronizer object internals
void wlanframesync_print(wlanframesync _q)
{
    printf("wlanframesync:\n");
}

// reset WLAN framing synchronizer object internal state
void wlanframesync_reset(wlanframesync _q)
{
    // clear buffer
    windowcf_clear(_q->input_buffer);

    // reset timers/state
    _q->state = WLANFRAMESYNC_STATE_SEEKPLCP;
    _q->timer = 0;
}

// execute framing synchronizer on input buffer
//  _q      :   framing synchronizer object
//  _buffer :   input buffer [size: _n x 1]
//  _n      :   input buffer size
void wlanframesync_execute(wlanframesync          _q,
                           liquid_float_complex * _buffer,
                           unsigned int           _n)
{
    unsigned int i;
    float complex x;
    for (i=0; i<_n; i++) {
        x = _buffer[i];

        // correct for carrier frequency offset (only if not in
        // initial 'seek PLCP' state)
        if (_q->state != WLANFRAMESYNC_STATE_SEEKPLCP) {
            nco_crcf_mix_down(_q->nco_rx, x, &x);
            nco_crcf_step(_q->nco_rx);
        }

        // save input sample to buffer
        windowcf_push(_q->input_buffer,x);

#if DEBUG_WLANFRAMESYNC
        // apply agc (estimate initial signal gain)
        float complex y;
        agc_crcf_execute(_q->agc_rx, x, &y);

        windowcf_push(_q->debug_x, x);
        windowf_push(_q->debug_rssi, agc_crcf_get_rssi(_q->agc_rx));
#endif

        switch (_q->state) {
        case WLANFRAMESYNC_STATE_SEEKPLCP:
            wlanframesync_execute_seekplcp(_q);
            break;
        case WLANFRAMESYNC_STATE_RXSHORT0:
            wlanframesync_execute_rxshort0(_q);
            break;
        case WLANFRAMESYNC_STATE_RXSHORT1:
            wlanframesync_execute_rxshort1(_q);
            break;
        case WLANFRAMESYNC_STATE_RXLONG0:
            wlanframesync_execute_rxlong0(_q);
            break;
        case WLANFRAMESYNC_STATE_RXLONG1:
            wlanframesync_execute_rxlong1(_q);
            break;
        case WLANFRAMESYNC_STATE_RXSIGNAL:
            wlanframesync_execute_rxsignal(_q);
            break;
        case WLANFRAMESYNC_STATE_RXDATA:
            wlanframesync_execute_rxdata(_q);
            break;
        default:;
            // should never get to this point
            fprintf(stderr,"error: wlanframesync_execute(), invalid state\n");
            exit(1);
        }
    } // for (i=0; i<_n; i++)
}

// get receiver RSSI
float wlanframesync_get_rssi(wlanframesync _q)
{
    return 0.0f;
}

// get receiver carrier frequency offset estimate
float wlanframesync_get_cfo(wlanframesync _q)
{
    return 0.0f;
}


//
// internal methods
//

// frame detection
void wlanframesync_execute_seekplcp(wlanframesync _q)
{
    _q->timer++;

    if (_q->timer < 64)
        return;

    // reset timer
    _q->timer = 0;

    // read contents of input buffer
    float complex * rc;
    windowcf_read(_q->input_buffer, &rc);
    
    // estimate gain
    unsigned int i;
    float g = 0.0f;
    for (i=16; i<80; i++) {
        // compute |rc[i]|^2 efficiently
        g += crealf(rc[i])*crealf(rc[i]) + cimagf(rc[i])*cimagf(rc[i]);
    }
    g = 64.0f / (g + 1e-6f);

    //printf("rssi : %12.8f dB\n", -10*log10f(g));
    
    // estimate S0 gain
    wlanframesync_estimate_gain_S0(_q, &rc[16], _q->G0a);
    
    // compute S0 metrics
    float complex s_hat;
    wlanframesync_S0_metrics(_q, _q->G0a, &s_hat);
    //float g = agc_crcf_get_gain(_q->agc_rx);
    s_hat *= g;

    float tau_hat  = cargf(s_hat) * (float)(32.0f) / (2*M_PI);
#if 1 //DEBUG_WLANFRAMESYNC_PRINT
    printf(" - gain=%12.3f, rssi=%8.4f dB, s_hat=%12.4f <%12.8f>, tau_hat=%8.3f\n",
            sqrt(g),
            -10*log10(g),
            cabsf(s_hat), cargf(s_hat),
            tau_hat);
#endif

}

// frame detection
void wlanframesync_execute_rxshort0(wlanframesync _q)
{
}

// frame detection
void wlanframesync_execute_rxshort1(wlanframesync _q)
{
}

void wlanframesync_execute_rxlong0(wlanframesync _q)
{
}

void wlanframesync_execute_rxlong1(wlanframesync _q)
{
}

void wlanframesync_execute_rxsignal(wlanframesync _q)
{
}

void wlanframesync_execute_rxdata(wlanframesync _q)
{
}

// estimate short sequence gain
//  _q      :   wlanframesync object
//  _x      :   input array (time), [size: M x 1]
//  _G      :   output gain (freq)
void wlanframesync_estimate_gain_S0(wlanframesync _q,
                                    float complex * _x,
                                    float complex * _G)
{
    // move input array into fft input buffer
    memmove(_q->x, _x, 64*sizeof(float complex));

    // compute fft, storing result into _q->X
    FFT_EXECUTE(_q->fft);
    
    // compute gain, ignoring NULL subcarriers
    unsigned int i;
    float gain = 0.054127f; // sqrt(12)/64 ; sqrtf(_q->M_S0) / (float)(_q->M);

    // clear input
    for (i=0; i<64; i++) _G[i] = 0.0f;

    // NOTE : if cabsf(_q->S0[i]) == 0 then we can multiply by conjugate
    //        rather than compute division
    //_G[i] = _q->X[i] / _q->S0[i];
    _G[40] = _q->X[40] * conjf(wlanframe_S0[40]) * gain;
    _G[44] = _q->X[44] * conjf(wlanframe_S0[44]) * gain;
    _G[48] = _q->X[48] * conjf(wlanframe_S0[48]) * gain;
    _G[52] = _q->X[52] * conjf(wlanframe_S0[52]) * gain;
    _G[56] = _q->X[56] * conjf(wlanframe_S0[56]) * gain;
    _G[60] = _q->X[60] * conjf(wlanframe_S0[60]) * gain;
    //
    _G[ 4] = _q->X[ 4] * conjf(wlanframe_S0[ 4]) * gain;
    _G[ 8] = _q->X[ 8] * conjf(wlanframe_S0[ 8]) * gain;
    _G[12] = _q->X[12] * conjf(wlanframe_S0[12]) * gain;
    _G[16] = _q->X[16] * conjf(wlanframe_S0[16]) * gain;
    _G[20] = _q->X[20] * conjf(wlanframe_S0[20]) * gain;
    _G[24] = _q->X[24] * conjf(wlanframe_S0[24]) * gain;
}

// compute S0 metrics
void wlanframesync_S0_metrics(wlanframesync _q,
                              float complex * _G,
                              float complex * _s_hat)
{
    // timing, carrier offset correction
    float complex s_hat = 0.0f;

    // compute timing estimate, accumulate phase difference across
    // gains on subsequent pilot subcarriers (note that all the odd
    // subcarriers are NULL)
#if 0
    unsigned int i;
    for (i=0; i<64; i+=4)
        s_hat += _G[(i+4)%64]*conjf(_G[i]);
#else
    s_hat += _G[44] * conjf(_G[40]);
    s_hat += _G[48] * conjf(_G[44]);
    s_hat += _G[52] * conjf(_G[48]);
    s_hat += _G[56] * conjf(_G[52]);
    s_hat += _G[60] * conjf(_G[56]);
    //           0             60
    //           4              0
    s_hat += _G[ 8] * conjf(_G[ 4]);
    s_hat += _G[12] * conjf(_G[ 8]);
    s_hat += _G[16] * conjf(_G[12]);
    s_hat += _G[20] * conjf(_G[16]);
    s_hat += _G[24] * conjf(_G[20]);
#endif

    // set output values, normalizing by number of elements
    *_s_hat = s_hat * 0.1f;
}


// estimate long sequence gain
//  _q      :   wlanframesync object
//  _x      :   input array (time), [size: M x 1]
//  _G      :   output gain (freq)
void wlanframesync_estimate_gain_S1(wlanframesync _q,
                                    float complex * _x,
                                    float complex * _G)
{
}

// estimate complex equalizer gain from G0 and G1
//  _q      :   wlanframesync object
//  _ntaps  :   number of time-domain taps for smoothing
void wlanframesync_estimate_eqgain(wlanframesync _q,
                                   unsigned int _ntaps)
{
}

// estimate complex equalizer gain from G0 and G1 using polynomial fit
//  _q      :   wlanframesync object
//  _order  :   polynomial order
void wlanframesync_estimate_eqgain_poly(wlanframesync _q,
                                        unsigned int _order)
{
}

// recover symbol, correcting for gain, pilot phase, etc.
void wlanframesync_rxsymbol(wlanframesync _q)
{
}


void wlanframesync_debug_print(wlanframesync _q,
                               const char * _filename)
{
    FILE * fid = fopen(_filename,"w");
    if (!fid) {
        fprintf(stderr,"error: wlanframe_debug_print(), could not open '%s' for writing\n", _filename);
        return;
    }
    fprintf(fid,"%% %s : auto-generated file\n", _filename);
#if DEBUG_WLANFRAMESYNC
    fprintf(fid,"close all;\n");
    fprintf(fid,"clear all;\n");
    fprintf(fid,"n = %u;\n", DEBUG_WLANFRAMESYNC_BUFFER_LEN);
    unsigned int i;
    float complex * rc;
    float * r;

    fprintf(fid,"x = zeros(1,n);\n");
    windowcf_read(_q->debug_x, &rc);
    for (i=0; i<DEBUG_WLANFRAMESYNC_BUFFER_LEN; i++)
        fprintf(fid,"x(%4u) = %12.4e + j*%12.4e;\n", i+1, crealf(rc[i]), cimagf(rc[i]));
    fprintf(fid,"figure;\n");
    fprintf(fid,"plot(0:(n-1),real(x),0:(n-1),imag(x));\n");
    fprintf(fid,"xlabel('sample index');\n");
    fprintf(fid,"ylabel('received signal, x');\n");

    // write agc_rssi
    fprintf(fid,"\n\n");
    fprintf(fid,"agc_rssi = zeros(1,%u);\n", DEBUG_WLANFRAMESYNC_BUFFER_LEN);
    windowf_read(_q->debug_rssi, &r);
    for (i=0; i<DEBUG_WLANFRAMESYNC_BUFFER_LEN; i++)
        fprintf(fid,"agc_rssi(%4u) = %12.4e;\n", i+1, r[i]);
    fprintf(fid,"figure;\n");
    fprintf(fid,"plot(agc_rssi)\n");
    fprintf(fid,"ylabel('RSSI [dB]');\n");
#else
    fprintf(fid,"disp('no debugging info available');\n");
#endif

    fclose(fid);
    printf("wlanframesync/debug: results written to '%s'\n", _filename);
}


