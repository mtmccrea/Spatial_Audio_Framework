/*
 * Copyright 2020 Leo McCormack
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * @file saf_reverb_internal.h
 * @brief Internal part of the reverb processing module (saf_reverb)
 *
 * A collection of reverb and room simulation algorithms.
 *
 * @author Leo McCormack
 * @date 06.05.2020
 */

#ifndef __REVERB_INTERNAL_H_INCLUDED__
#define __REVERB_INTERNAL_H_INCLUDED__

#include <stdio.h>
#include <math.h> 
#include <string.h>
#include <assert.h>
#include "saf_reverb.h"
#include "../saf_utilities/saf_utilities.h"
#include "../saf_sh/saf_sh.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#ifndef NUM_EARS
# define NUM_EARS 2
#endif
#ifndef ISEVEN
# define ISEVEN(n)   ((n%2 == 0) ? 1 : 0)
#endif
#ifndef ISODD
# define ISODD(n)    ((n%2 != 0) ? 1 : 0)
#endif


/* ========================================================================== */
/*                         IMS Shoebox Room Simulator                         */
/* ========================================================================== */

#define IMS_NUM_WALLS_SHOEBOX ( 6 )
#define IMS_FIR_FILTERBANK_ORDER ( 1000 )

typedef void* voidPtr; 

typedef struct _position_xyz {
    union {
        struct { float x, y, z; };
        float v[3];
    };
} position_xyz;

typedef struct _echogram_data
{
    int numImageSources;  /**< Number of image sources in echogram */
    int nChannels;        /**< Number of channels */
    float** value;        /**< Echogram magnitudes per image source and channel;
                           *   numImageSources x nChannels */
    float* time;          /**< Propagation time (in seconds) for each image
                           *   source; numImageSources x 1 */
    int** order;          /**< Reflection order for each image and dimension;
                           *   numImageSources x 3 */
    position_xyz* coords; /**< Reflection coordinates (Cartesian);
                           *   numImageSources x 3 */
    int* sortedIdx;       /**< Indices that sort the echogram based on
                           *   propagation time, in accending order;
                           *   numImageSources x 1 */

} echogram_data;

/**
 * Helper structure, comprising variables used when computing echograms and
 * rendering RIRs. The idea is that there should be one instance of this per
 * source/reciever combination.
 */
typedef struct _ims_core_workspace
{
    /* Locals */
    int room[3];
    float d_max;
    position_xyz src, rec; 
    int nBands; 

    /* Internal */
    float Nx, Ny, Nz;
    int lengthVec, numImageSources;
    int* validIDs;
    float* II, *JJ, *KK;
    float* s_x, *s_y, *s_z, *s_d, *s_t, *s_att;

    /* Echograms */
    int refreshEchogramFLAG;
    void* hEchogram;
    void* hEchogram_rec;
    voidPtr* hEchogram_abs;

    /* Room impulse responses (only used/allocated when a render function is
     * called) */
    int refreshRIRFLAG;
    int rir_len_samples;
    float rir_len_seconds;
    float*** rir_bands; /* nBands x nChannels x rir_len_samples */

    /* Circular buffers (only used/allocated when a "applyEchogram" function is
     * called) */
 
}ims_core_workspace;

/**
 * Main structure for IMS. It comprises variables describing the room, and the
 * sources and receivers within it. It also includes "core workspace" handles
 * for each source/receiver combination.
 */
typedef struct _ims_scene_data
{
    /* Locals */
    int room_dimensions[3];
    float c_ms;
    float fs;
    int nBands;
    float** abs_wall;

    /* Source and receiver positions */
    position_xyz src_xyz[IMS_MAX_NUM_SOURCES];
    position_xyz rec_xyz[IMS_MAX_NUM_RECEIVERS];
    long src_IDs[IMS_MAX_NUM_SOURCES];
    long rec_IDs[IMS_MAX_NUM_RECEIVERS];
    long nSources;
    long nReceivers;

    /* Internal */
    voidPtr** hCoreWrkSpc;   /* one per source/receiver combination */
    float* band_centerfreqs;
    float** H_filt;  /* nBands x (IMS_FIR_FILTERBANK_ORDER+1) */
    ims_rir** rirs;  /* one per source/receiver combination */

} ims_scene_data;


/* =========================== Internal Functions =========================== */

/**
 * Creates an instance of the core workspace
 *
 * The idea is that there is one core workspace instance per source/receiver
 * combination.
 *
 * @param[in] phWork (&) address of the workspace handle
 * @param[in] nBands Number of bands
 */
void ims_shoebox_coreWorkspaceCreate(void** phWork,
                                     int nBands);

/**
 * Destroys an instance of the core workspace
 *
 * @param[in] phWork  (&) address of the workspace handle
 */
void ims_shoebox_coreWorkspaceDestroy(void** phWork);

/**
 * Creates an instance of an echogram container
 *
 * @param[in] phEcho (&) address of the echogram container
 */
void ims_shoebox_echogramCreate(void** phEcho);

/**
 * Resizes an echogram container
 *
 * @note The container is only resized if the number of image sources or
 *       channels have changed.
 *
 * @param[in] hEcho           echogram container
 * @param[in] numImageSources New number of image sources
 * @param[in] nChannels       New number of channels
 */
void ims_shoebox_echogramResize(void* hEcho,
                                int numImageSources,
                                int nChannels);

/**
 * Destroys an instance of an echogram container
 *
 * @param[in] phEcho (&) address of the echogram container
 */
void ims_shoebox_echogramDestroy(void** phEcho);

/**
 * Calculates an echogram of a rectangular space using the image source method,
 * for a specific source/reciever combination
 *
 * Note the coordinates of source/receiver are specified from the left ground
 * corner of the room:
 *                ^x
 *             __|__    _
 *             |  |  |   |
 *             |  |  |   |
 *          y<----.  |   | l
 *             |     |   |
 *             |     |   |
 *             o_____|   -
 *
 *             |-----|
 *                w
 *
 * @param[in] hWork   workspace handle
 * @param[in] room    Room dimensions, in meters
 * @param[in] src     Source position, in meters
 * @param[in] rec     Receiver position, in meters
 * @param[in] maxTime Maximum propagation time to compute the echogram, seconds
 * @param[in] c_ms    Speed of source, in meters per second
 */
void ims_shoebox_coreInit(void* hWork,
                          int room[3],
                          position_xyz src,
                          position_xyz rec,
                          float maxTime,
                          float c_ms);

/**
 * Imposes spherical harmonic directivies onto the echogram computed with
 * ims_shoebox_coreInit, for a specific source/reciever combination
 *
 * @note Call ims_shoebox_coreInit before applying the directivities
 *
 * @param[in] hWork    workspace handle
 * @param[in] sh_order Spherical harmonic order
 */
void ims_shoebox_coreRecModuleSH(void* hWork,
                                 int sh_order);

/**
 * Applies boundary absoption per frequency band, onto the echogram computed
 * with ims_shoebox_coreRecModuleSH, for a specific source/reciever combination
 *
 * Absorption coefficients are given for each of the walls on the respective
 * planes [x+ y+ z+; x- y- z-].
 *
 * @note Call ims_shoebox_coreRecModuleSH before applying the absoption
 *
 * @param[in] hWork    workspace handle
 * @param[in] abs_wall Absorption coefficients; nBands x 6
 */
void ims_shoebox_coreAbsorptionModule(void* hWork,
                                      float** abs_wall);

/**
 * Renders a room impulse response for a specific source/reciever combination
 *
 * @note Call ims_shoebox_coreAbsorptionModule before rendering rir
 *
 * @param[in]  hWork               workspace handle
 * @param[in]  fractionalDelayFLAG 0: disabled, 1: use Lagrange interpolation
 * @param[in]  fs                  SampleRate, Hz
 * @param[in]  H_filt              filterbank; nBands x (filterOrder+1)
 * @param[out] rir                 Room impulse response
 */
void ims_shoebox_renderRIR(void* hWork,
                           int fractionalDelayFLAG,
                           float fs,
                           float** H_filt,
                           ims_rir* rir);


#ifdef __cplusplus
} /* extern "C" */
#endif /* __cplusplus */

#endif /* __REVERB_INTERNAL_H_INCLUDED__ */
