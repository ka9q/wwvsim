#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "timecode.h"
#include "paths.h"

static void encode(uint8_t *code,int x);
static int decode(uint8_t const *code);
static int dst_start_doy(int year);
static int day_of_year(int year,int month,int day);

// Applies only to non-leap years; you need special tests for February in leap year
int const Days_in_month[] = { // Index 1 = January, 12 = December
  0,31,28,31,30,31,30,31,31,30,31,30,31
};

// Construct time code as array of **61** unsigned chars with values 0 or 1
void maketimecode(uint8_t *code,int dut1,bool leap_pending,int year,int month,int day,int hour,int minute){
    memset(code,0,61 * sizeof *code); // All bits default to 0
    int doy = day_of_year(year,month,day);
    int dst_start = dst_start_doy(year);
    if(dst_start >= 1){
      // DST always lasts for 238 days
      if(doy > dst_start && doy <= dst_start + 238)
	code[2] = 1; // DST status at 00:00 UTC
      if(doy >= dst_start && doy < dst_start + 238)
	code[55] = 1; // DST status at 24:00 UTC
#if 0
      fprintf(stderr,"year %d month %d day %d doy %d dst_start_doy %d dst_start_doy + 238 %d\n",
	      year, month, day, doy, dst_start, dst_start + 238);
#endif
    }
    code[3] = leap_pending;
    // Year
    encode(code+4,year % 10); // Least significant digit
    encode(code+51,(year/10)%10); // Tens digit
    // Minute of hour, 0-59
    encode(code+10,minute%10); // Least significant digit
    encode(code+15,minute/10); // Most significant digit, extends into unused bit 18
    // Hour of day, 0-23
    encode(code+20,hour%10);   // Least significant digit
    encode(code+25,hour/10);   // Most significant digit, extends into unused bits 27-28
    // Day of year, 1-366
    encode(code+30,doy%10);    // Least significant digit
    encode(code+35,(doy/10)%10); // Middle digit
    encode(code+40,doy/100);   // High digit, extends into unused bits 42-43
    // UT1 offset, +/-0.0 through 0.7; adjusted after leap second
    code[50] = (dut1 >= 0); // sign
    encode(code+56,abs(dut1));  // magnitude, extends into marker 59 and is ignored
}
// Encode a BCD digit in little-endian format (lsb first)
// NB! Only WWV/WWVH; WWVB uses big-endian format
static void encode(uint8_t *code,int x){
  for(int i=0;i<4;i++){
    code[i] = x & 1;
    x >>= 1;
  }
}
static int decode(uint8_t const *code){
  int r = 0;
  for(int i=3; i>=0; i--){
    r <<= 1;
    assert(code[i] == 0 || code[i] == 1);
    r += code[i];
  }
  return r;
}
/* Determine day of year when daylight savings time starts
   Only US rules are needed, since WWV/WWVH are American stations
   US rules last changed in 2007 to 2nd sunday of March to first sunday in November
   Always lasts for 238 days (34 weeks)
   Pattern repeats every 28 years (7 days in week x 4 years in leap year cycle)
   Hopefully DST will be abolished before long!
                                          2007: 3/11 (70)    2008: 3/9  (69)
   2009: 3/8  (67)     2010: 3/14 (73)    2011: 3/13 (72)    2012: 3/11 (71)
   2013: 3/10 (69)     2014: 3/9  (68)    2015: 3/8  (67)    2016: 3/13 (73)
   2017: 3/12 (71)     2018: 3/11 (70)    2019: 3/10 (69)    2020: 3/8  (68)
   2021: 3/14 (73)     2022: 3/13 (72)    2023: 3/12 (71)    2024: 3/10 (70)
   2025: 3/9  (68)     2026: 3/8  (67)    2027: 3/14 (73)    2028: 3/12 (72)
   2029: 3/11 (70)     2030: 3/10 (69)    2031: 3/9  (68)    2032: 3/14 (74)

   2033: 3/13 (72)     2034: 3/12 (71)    2035: 3/11 (70)    2036: 3/9  (69)
   2037: 3/8  (67)     2038: 3/14 (73)    2039: 3/13 (72)    2040: 3/11 (71)
   2041: 3/10 (69)     2042: 3/9  (68)    2043: 3/8  (67)    2044: 3/13 (73)
   2045: 3/12 (71)     2046: 3/11 (70)    2047: 3/10 (69)    2048: 3/8  (68)
   2049: 3/14 (73)     2050: 3/13 (72)    2051: 3/12 (71)    2052: 3/10 (70)
   2053: 3/9  (68)     2054: 3/8  (67)    2055: 3/14 (73)    2056: 3/12 (72)
   2057: 3/11 (70)     2058: 3/10 (69)    2059: 3/9  (68)    2060: 3/14 (74)
*/
static int dst_start_doy(int year){
  int r = -1;
  if(year >= 2007){
    r = 72;  // DST would have started on day 72 in year 2005 if rule had been in effect then
    for(int ytmp = 2005; ytmp < year; ytmp++){
      r -= 1 + is_leap_year(ytmp);
      if(r < 67) // Never before day 67
	r += 7;
    }
    if(r == 67 && is_leap_year(year)) // day 67 is 1st sunday in march
	r += 7;
  }
  return r;
}
static int day_of_year(int year,int month,int day){
    // Compute day of year
    // don't use doy in tm struct in case date was manually overridden
    // (Bug found and reported by Jayson Smith jaybird@bluegrasspals.com)
    int doy = day;
    for(int i = 1; i < month; i++){
      if(i == 2 && is_leap_year(year))
	doy += 29;
      else
	doy += Days_in_month[i];
    }
    return doy;
}
// Decode frame of timecode to stderr for debugging
void decode_timecode(uint8_t const *code,int length){
  for(int s=0;s<length;s++){
    if((s % 10) == 0 && s < 60)
      fprintf(stderr,"%02d: ",s);
    if(s == 0)
      fputc(' ',stderr);
    else if((s % 10) == 9)
      fprintf(stderr,"M");
    else
      fputc(code[s] ? '1' : '0',stderr);
    if(s < 59 && (s % 10 == 9))
      fputc('\n',stderr);
  }
  fputc('\n',stderr);
  fprintf(stderr,"year %d%d",decode(code+51),decode(code+4));
  fprintf(stderr," doy %d%d%d",decode(code+40),decode(code+35),decode(code+30));
  fprintf(stderr," hour %d%d",decode(code+25),decode(code+20));
  fprintf(stderr," minute %d%d",decode(code+15),decode(code+10));
  int dut1 = decode(code+56);
  if(!code[50])
    dut1 = -dut1;
  fprintf(stderr,"; dut1 %+d",dut1);
  if(code[3])
    fprintf(stderr,"; leap second pending");
  if(code[2] && code[55])
    fprintf(stderr,"; DST in effect");
  else if(!code[2] && code[55])
    fprintf(stderr,"; DST starts today");
  else if(code[2]  && !code[55])
    fprintf(stderr,"; DST ends today");
  else
    fprintf(stderr,"; DST not in effect");
  fprintf(stderr,"\n\n");
}
// Is specified year a leap year?
bool const is_leap_year(int y){
  if((y % 4) != 0)
    return false; // Ordinary year; example: 2017
  if((y % 100) != 0)
    return true; // Examples: 1956, 2004 (i.e., most leap years)
  if((y % 400) != 0)
    return false; // Examples: 1900, 2100 (the big exception to the usual rule; non-leap US presidential election years)
  return true; // Example: 2000 (the exception to the exception)
}
