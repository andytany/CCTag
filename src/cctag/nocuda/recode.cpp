/*
 * Copyright 2016, Simula Research Laboratory
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */
#include "cctag/utils/Defines.hpp"

#include "cctag/nocuda/recode.hpp"
#include "cctag/Params.hpp"
#include "cctag/PlaneCV.hpp"
#include "cctag/utils/Talk.hpp" // do DO_TALK macro

#include "cctag/cuda/clamp.h" // clamp is in the cuda dir but useful here

#include <boost/timer.hpp>
#include <boost/math/special_functions/sign.hpp> // gives boost::sign

#include <cstdlib> // for ::abs
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <exception>

#define USE_INTEGER_REP

static const float gauss_filter[9] =
{
    0.000053390535453f,
    0.001768051711852f,
    0.021539279301849f,
    0.096532352630054f,
    0.159154943091895f,
    0.096532352630054f,
    0.021539279301849f,
    0.001768051711852f,
    0.000053390535453f,
};

static const float gauss_deriv[9] =
{
    -0.002683701023220f,
    -0.066653979229454f,
    -0.541341132946452f,
    -1.213061319425269f,
    0.0f,
    1.213061319425269f,
    0.541341132946452f,
    0.066653979229454f,
    0.002683701023220f,
};

static void filter_gauss_vert( int                   x,
                               int                   y,
                               const Plane<uint8_t>& src,
                               const Plane<int16_t>& dst,
                               float*                filter,
                               float                 scale )
{
    float out = 0.0f;
    for( int offset = 0; offset<9; offset++ )
    {
        float g  = filter[offset];

        int lookup = clamp( y + offset - 4, src.getRows() );
        float val = src.at( x, lookup );
        out += ( val * g );
    }

    out *= scale;
    dst.at(x,y) = (int16_t)out;
}

static void filter_gauss_horiz( int                   x,
                                int                   y,
                                const Plane<int16_t>& src,
                                const Plane<int16_t>& dst,
                                float*                filter,
                                float                 scale )
{
    float out = 0.0f;
    for( int offset = 0; offset<9; offset++ )
    {
        float g  = filter[offset];

        int lookup = clamp( x + offset - 4, src.getCols() );
        float val = src.at( lookup, y );
        out += ( val * g );
    }

    out *= scale;
    dst.at(x,y) = (int16_t)out;
}



static void applyGauss( cctag::Plane<uint8_t>& src,
                        cctag::Plane<int16_t>& dx,
                        cctag::Plane<int16_t>& dy )
{
    cctag::Plane<int16_t> interm( ... );

    const int grid_x  = src.getCols();
    const int grid_y  = src.getRows();

#ifdef NORMALIZE_GAUSS_VALUES
    const float normalize   = 1.0f / sum_of_gauss_values;
    const float normalize_d = 1.0f / normalize_derived;
#else // NORMALIZE_GAUSS_VALUES
    const float normalize   = 1.0f;
    const float normalize_d = 1.0f;
#endif // NORMALIZE_GAUSS_VALUES
    /*
     * Vertical sweep for DX computation: use Gaussian table
     */
    for( int y=0; y<grid_x; y++ )
        for( int x=0; x<grid_x; x++ )
            filter_gauss_vert( x, y, src, interm, gauss_filter, normalize );

    /*
     * Compute DX
     */
    for( int y=0; y<grid_x; y++ )
        for( int x=0; x<grid_x; x++ )
            filter_gauss_horiz( x, y, interm, dx, gauss_deriv, normalize_d );

    /*
     * Compute DY
     */
    for( int y=0; y<grid_x; y++ )
        for( int x=0; x<grid_x; x++ )
            filter_gauss_vert( x, y, src, interm, gauss_deriv, normalize_d );

    /*
     * Horizontal sweep for DY computation: use Gaussian table
     */
    for( int y=0; y<grid_x; y++ )
        for( int x=0; x<grid_x; x++ )
            filter_gauss_horiz( interm, dy, gauss_filter, normalize );
}

static void compute_mag_l1( int                          x,
                            int                          y,
                            const cctag::Plane<int16_t>& src_dx,
                            const cctag::Plane<int16_t>& src_dy,
                            cctag::Plane<int16_t>&       mag )
{
    int16_t dx = abs( src_dx.at(x,y) );
    int16_t dy = abs( src_dy.at(x,y) );
    mag.at(x,y) = dx + dy;
}

static void compute_mag_l2( int                          x,
                            int                          y,
                            const cctag::Plane<int16_t>& src_dx,
                            const cctag::Plane<int16_t>& src_dy,
                            cctag::Plane<int16_t>&       mag )
{
    int16_t dx = src_dx.at(x,y);
    int16_t dy = src_dy.at(x,y);
    dx *= dx;
    dy *= dy;
    mag.at(x,y) = (int16)t)sqrtf( (float)( dx + dy ) );
}

static void applyMag( const cctag::Plane<int16_t>& dx,
                      const cctag::Plane<int16_t>& dy,
                      cctag::Plane<int16_t>&       mag )
{
    const int grid_x  = src.getCols();
    const int grid_y  = src.getRows();

    for( int y=0; y<grid_y; y++ )
        for( int x=0; x<grid_x; x++ )
            compute_mag_l2( x, y,  dx, dy, mag );
}

static void compute_map( const int                    x,
                         const int                    y,
                         const cctag::Plane<int16_t>& src_dx,
                         const cctag::Plane<int16_t>& src_dy,
                         const cctag::Plane<int16_t>& src_mag,
                         cctag::Plane<uint8_t>&       map,
                         const float                  hiThr,
                         const float                  loThr )
{
    const int CANNY_SHIFT = 15;
    const int TG22 = (int32_t)(0.4142135623730950488016887242097*(1<<CANNY_SHIFT) + 0.5);

    int32_t  dxVal  = dx.at(x,y);
    int32_t  dyVal  = dy.at(x,y);
    uint32_t magVal = mag.pat(x,y);

    // -1 if only is negative, 1 else
    // const int32_t signVal = (dxVal ^ dyVal) < 0 ? -1 : 1;
    const int32_t signVal = boost::sign( dxVal ^ dyVal );

    dxVal = d_abs( dxVal );
    dyVal = d_abs( dyVal );

    // 0 - the pixel can not belong to an edge
    // 1 - the pixel might belong to an edge
    // 2 - the pixel does belong to an edge
    uint8_t edge_type = 0;

    if( magVal > loThr )
    {
        const int32_t tg22x = dxVal * TG22;
        const int32_t tg67x = tg22x + ((dxVal + dxVal) << CANNY_SHIFT);

        dyVal <<= CANNY_SHIFT;

        int x0;
        int x1;
        int y0;
        int y1;

        if( dyVal < tg22x ) {
            x0 = x-1;
            x1 = x+1;
            y0 = y;
            y1 = y;
        } else {
            y0 = y - 1;
            y1 = y + 1;
            if( dyVal > tg67x ) {
                x0 = x;
                x1 = x;
            } else {
                x0 = x - signVal;
                x1 = x + signVal;
            }
        }
        
        x0 = clamp( x0, dx.getCols() );
        x1 = clamp( x1, dx.getCols() );
        y0 = clamp( y0, dx.getRows() );
        y1 = clamp( y1, dx.getRows() );

        if( magVal > mag.at(x0,y0) && magVal >= mag.at(x1,y1) )
        {
            edge_type = 1 + (uint8_t)(magVal > hiThr );
        }
    }

    map.at(x,y) = edge_type;
}

static void applyMag( const cctag::Plane<int16_t>& dx,
                      const cctag::Plane<int16_t>& dy,
                      const cctag::Plane<int16_t>& mag,
                      cctag::Plane<uint8_t>&       map,
                      const cctag::Parameters* params )
{
    const float hiThr = params->_cannyThrHigh * 256.0f;
    const float loThr = params->_cannyThrLow  * 256.0f;

    const int grid_x  = src.getCols();
    const int grid_y  = src.getRows();

    for( int y=0; y<grid_y; y++ )
        for( int x=0; x<grid_x; x++ )
            compute_map( x, y, dx, dy, mag, map, hiThr, loThr );
}

inline static void compute_hyst_recurse( int d, const int x, const int y, cctag::Plane<uint8_t>& hyst )
{
    hyst.at(x,y) = 2;
    if( d > 100 ) return; // d limits stack depth. We loop outside anyway.

    if( hyst.at(x-1,y-1) == 1 ) compute_hyst_recurse(d+1,x-1,y-1,hyst);
    if( hyst.at(x  ,y-1) == 1 ) compute_hyst_recurse(d+1,x  ,y-1,hyst);
    if( hyst.at(x+1,y-1) == 1 ) compute_hyst_recurse(d+1,x+1,y-1,hyst);

    if( hyst.at(x-1,y  ) == 1 ) compute_hyst_recurse(d+1,x-1,y  ,hyst);
    if( hyst.at(x+1,y  ) == 1 ) compute_hyst_recurse(d+1,x+1,y  ,hyst);

    if( hyst.at(x-1,y+1) == 1 ) compute_hyst_recurse(d+1,x-1,y+1,hyst);
    if( hyst.at(x  ,y+1) == 1 ) compute_hyst_recurse(d+1,x  ,y+1,hyst);
    if( hyst.at(x+1,y+1) == 1 ) compute_hyst_recurse(d+1,x+1,y+1,hyst);
}

static bool compute_hyst( const int x, const int y, cctag::Plane<uint8_t>& hyst )
{
    if( hyst.at(x,y) != 1 ) return false;

    if( hyst.at(x-1,y-1) == 2 || 
        hyst.at(x  ,y-1) == 2 || 
        hyst.at(x+1,y-1) == 2 || 
        hyst.at(x-1,y  ) == 2 || 
        hyst.at(x+1,y  ) == 2 || 
        hyst.at(x-1,y+1) == 2 || 
        hyst.at(x  ,y+1) == 2 || 
        hyst.at(x+1,y+1) == 2 )
    {
        // If we have upgrade a pixel from 1 to 2, check its neighbours
        // recursively - other algos use complex data structures, we use
        // recursion with a depth limit plus the outer loop doing everything
        // again if anything changed.
        compute_hyst_recurse(1,x,y,hyst);
        return true;
    }

    if( hyst.at(x-1,y-1) == 0 &&
        hyst.at(x  ,y-1) == 0 &&
        hyst.at(x+1,y-1) == 0 &&
        hyst.at(x-1,y  ) == 0 &&
        hyst.at(x+1,y  ) == 0 &&
        hyst.at(x-1,y+1) == 0 &&
        hyst.at(x  ,y+1) == 0 &&
        hyst.at(x+1,y+1) == 0 )
    {
        hyst.at(x,y) = 0;
        return true;
    }

    return false;
}

static void applyHyst( const cctag::Plane<uint8_t>& map,
                       cctag::Plane<uint8_t>&       hyst )
{
    const int grid_x  = map.getCols();
    const int grid_y  = map.getRows();

    map = hyst ... ; // make a clone to avoid special case

    bool changes = true;
    while( changes )
    {
        changes = false;
        for( int y=1; y<grid_y-1; y++ )
            for( int x=1; x<grid_x-1; x++ )
                if( compute_hyst( x, y, hyst ) ) changes = true;
    }
}

static void applyFinal( const cctag::Plane<uint8_t>& hyst,
                        cctag::Plane<uint8_t>&       canny )
{
    const int grid_x  = hyst.getCols();
    const int grid_y  = hyst.getRows();

    for( int y=1; y<grid_y-1; y++ )
        for( int x=1; x<grid_x-1; x++ )
            canny.at(x,y) = hyst.at(x,y)==2 ? 0xff : 0;
}

void recodedCanny( cctag::Plane<uint8_t>& imgGraySrc,
                   cctag::Plane<uint8_t>& imgCanny,
                   cctag::Plane<int16_t>& imgDX,
                   cctag::Plane<int16_t>& imgDY,
                   float low_thresh,
                   float high_thresh,
                  econst cctag::Parameters* params )
{
    Plane<int16_t> imgMag( ... );
    Plane<uint8_t> imgMap( ... );
    Plane<uint8_t> imgHyst( ... );
    
    applyGauss( imgGraySrc, imgDX, imgDY );
    applyMag( imgDX, imgDY, imgMag );
    applyMap( imgDX, imgDY, imgMag, imgMap, params );
    applyHyst( imgMap, imgHyst);
    applyFinal( imgHyst, imgCanny );
}