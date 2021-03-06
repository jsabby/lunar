/* eop_prec.cpp: precise precession matrix using EOP (Earth Orientation
Parameter) data

Copyright (C) 2016, Project Pluto

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
02110-1301, USA.    */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
#include "watdefs.h"
#include "afuncs.h"

double cubic_spline_interpolate_within_table(      /* spline.cpp */
         const double *table, const int n_entries, double x, int *err_code);

#define PI 3.1415926535897932384626433832795028841971693993751058209749445923
const double arcsec_to_radians = PI / (180. * 3600.);
const double marcsec_to_radians = PI / (180. * 3600.e+3);

#define TBLSIZE 4

/* This function loads Earth orientation parameters (EOP) from any of

http://maia.usno.navy.mil/ser7/finals.daily
http://maia.usno.navy.mil/ser7/finals.data
http://maia.usno.navy.mil/ser7/finals.all

   The 'daily' file gives EOPs for the last 90 days,  plus 90 days
of predictions.  'data' starts on 1992 January 1,  and 'all' on 1973
January 2,  each with a year of predictions.

   The EOP files give UT1-UTC.  This has one-second jumps and is not
suited to splining,  so we actually store Delta-T = TDT-UT1,  a continuous
function that _is_ suited to splining.  We could compute it by subtracting
the given UT1-UTC value from the result of the td_minus_utc( ) function.
To speed matters up a bit,  we do that _once_ for the first value.  After
that,  we just make sure each subsequent value is within 0.5 seconds of
the preceding value.

   The only errors that can be generated are that the file wasn't found,
or wasn't in the correct format,  or memory wasn't allocated for the data.
Otherwise,  the MJD for the last day of EOPs,  including the predictions,
is returned.  One can use that to tell the user,  "time to get new EOPs"
(or to automatically download them without even telling the user).

   In a multi-threaded environment,  call load_earth_orientation_params()
before forking/threading;  the following static values will then be set
and used thereafter in a read-only manner.  Call the function again with
a NULL filename to release the eop_data buffer.

   See 'prectest.cpp' for example usage.    */

static double *eop_data = NULL, eop_jd0;
static int eop_size, eop_usable, eop_usable_nutation;
const size_t eop_iline_len = 188;

static bool is_valid_eop_line( const char *iline)
{
   if( strlen( iline) != eop_iline_len || iline[12] != '.'
            || iline[27] != ' '
            || iline[20] != '.' || iline[13] != '0' || iline[14] != '0')
      return( false);
   else
      return( true);
}

int DLL_FUNC load_earth_orientation_params( const char *filename)
{
   int rval = 0;

   if( eop_data)
      {
      free( eop_data);
      eop_data = NULL;
      }
   if( filename)
      {
      FILE *ifile = fopen( filename, "rb");
      char buff[200];

      if( !ifile || !fgets( buff, sizeof( buff), ifile))
         rval = EOP_FILE_NOT_FOUND;
      else if( !is_valid_eop_line( buff) || buff[16] != 'I')
         rval = EOP_FILE_WRONG_FORMAT;
      else
         {
         int i;
         double initial_td_minus_utc;

         eop_jd0 = atof( buff + 7) + 2400000.5;
         initial_td_minus_utc = td_minus_utc( eop_jd0 + .1);
         fseek( ifile, 0L, SEEK_END);
         eop_size = (int)( ftell( ifile) / eop_iline_len);
         fseek( ifile, 0L, SEEK_SET);
         eop_data = (double *)calloc( eop_size * 5, sizeof( double));
         if( !eop_data)
            rval = EOP_ALLOC_FAILED;
         i = eop_usable = eop_usable_nutation = 0;
         while( !rval && i < eop_size && fgets( buff, sizeof( buff), ifile)
                      && buff[16] != ' ' && is_valid_eop_line( buff))
            {
            double *tptr = eop_data + i;

            *tptr = atof( buff + 18) * arcsec_to_radians;      /* Polar motion X, arcsec */
            tptr += eop_size;
            *tptr = atof( buff + 37) * arcsec_to_radians;      /* Polar motion Y, arcsec */
            tptr += eop_size;
            *tptr = -atof( buff + 58);      /* UTC - UT1,  in seconds: note sign flip */
            *tptr += initial_td_minus_utc;
            if( i)         /* correct if a leap second occurred */
               *tptr += floor( tptr[-1] - tptr[0] + .5);
            tptr += eop_size;
                  /* sigma at atof( buff + 69) */
            eop_usable++;
            if( buff[95] != ' ')
               {
               *tptr = atof( buff + 97) * marcsec_to_radians;   /* dPsi, milliarcsec */
               tptr += eop_size;
                        /* sigma at atof( buff + 109) */
               *tptr = atof( buff + 116) * marcsec_to_radians;  /* dEps, milliarcsec */
                        /* sigma at atof( buff + 128) */
               eop_usable_nutation++;
               }
            i++;
            }
         if( i < 16371)    /* as of 2016 Oct 29,  should be _at least_ */
            rval = EOP_FILE_WRONG_FORMAT;           /* this many lines */
         if( rval)   /* slightly tricky method to free up eop_data,  if */
            load_earth_orientation_params( NULL);  /* it's been alloced */
         else                            /* get MJD for preceding day */
            rval = atoi( buff + 7) - 1;
         }
      if( ifile)
         fclose( ifile);
      }
   return( rval);
}

/* If the above function was called and successfully loaded EOPs,  and if all
five parameters are successfully interpolated,  the return value is zero.  If
we don't have EOPs,  or they don't extend to the given JD,  a bit field is set
for each uncomputed parameter (and that parameter is set to zero).  The predictions
for polar motion and UT1-UTC run further than those for dEps and dPhi.  If you're
completely out of coverage (before the start time of the file or after the
predictions),  the return value is 31 = 11111 base 2.  If you're just past
the predictions for dPhi/dEps,  the return value is 24 = 11000 base 2.  */

int DLL_FUNC get_earth_orientation_params( const double jd,
                              earth_orientation_params *params)
{
   int i, rval = 0;
   double results[5];

   for( i = 0; i < 5; i++)
      results[i] = 0.;
   if( eop_data && params)
      {
      const double dt = jd - eop_jd0;

      for( i = 0; i < 5; i++)
         {
         int t_rval;
         double result;

         result = cubic_spline_interpolate_within_table(
                  eop_data + eop_size * i,
                  (i < 3 ? eop_usable : eop_usable_nutation),
                  dt, &t_rval);
         if( t_rval)
            rval |= (1 << i);
         else
            results[i] = result;
         }
      }
   else
      rval = -1;
   if( !results[2])        /* fill in using default Delta-T formula if we */
      results[2] = td_minus_ut( jd);   /* don't get a value from EOP data */
   params->dX = results[0];
   params->dY = results[1];
   params->tdt_minus_ut1 = results[2];
   params->dPsi = results[3];
   params->dEps = results[4];
   return( rval);
}

static const double J2000 = 2451545.;

/* Note that the matrix returned by this function gives the instantaneous
orientation of the earth,  with

matrix[0, 1, 2] = vector pointing at equator, 0 lon,  in J2000/ICRF
matrix[3, 4, 5] = vector pointing at equator, 90 E lon,  also J2000/ICRF
matrix[6, 7, 8] = vector pointing at north pole (+90 lat),  also J2000/ICRF
*/

int DLL_FUNC setup_precession_with_nutation_eops( double DLLPTR *matrix,
                    const double year)
{
   const double jdt = J2000 + (year - 2000.) * 365.25;
   earth_orientation_params eo_params;
   double ut1, rotation;
   const int rval = get_earth_orientation_params( jdt, &eo_params);

   setup_precession_with_nutation_delta( matrix, year,
                                    eo_params.dPsi, eo_params.dEps);
   ut1 = jdt - eo_params.tdt_minus_ut1 / seconds_per_day;
   rotation = green_sidereal_time( ut1);

   spin_matrix( matrix, matrix + 3, -rotation);
   spin_matrix( matrix, matrix + 6, -eo_params.dX);          /* polar motion in x */
   spin_matrix( matrix + 3, matrix + 6, eo_params.dY);      /* polar motion in y */
   return( rval);
}
