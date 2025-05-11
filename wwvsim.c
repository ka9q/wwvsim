// $Id: wwvsim.c,v 1.13 2018/11/07 19:24:47 karn Exp $
// WWV/WWVH simulator program. Generates their audio program as closely as possible
// Even supports UT1 offsets and leap second insertion
// Uses espeak synthesizer for speech announcements; needs a lot of work
// By default, uses system time, which should be NTP synchronized
// Time can be manually overridden for testing (announcements, leap seconds and other corner cases)

// July 2017, Phil Karn, KA9Q
// (Can you tell I have too much spare time?)

// Major rewrite 19 Oct 2018:
//   Decode generated timecode in verbose mode
//   When output is terminal, use portaudio to write to sound device with correct (?) timing
//   Factor out timecode, audio generation and textfile synthesis to more manageable functions
//   Changes to tone program tables to match actual WWV/WWVH schedule:
//     no GPS status
//     will probably require changes when oceanic weather goes away at end of Oct 2018

// Minor tweaks 16 Mar 2023
// Major rewrite 30 Aug 2023 to use a FIFO queue feeding a separate output thread
// Better able to handle slow speech synthesizers

#define USE_PORTAUDIO 1 // Enable direct on-time output to sound device with portaudio when stdout is a terminal
#define PIPER 1 // Piper TTS

#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <complex.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <limits.h>
#include <math.h>
#include <memory.h>
#include <sys/time.h>
#include <locale.h>
#include <sys/stat.h>
#include <pthread.h>
#include <getopt.h>

#ifdef USE_PORTAUDIO
#include <portaudio.h>
PaStream *Stream;
#define FRAMES_PER_BUFFER 1024
#endif

#if __APPLE__
#define pthread_setname(x) pthread_setname_np(x)
#else // !__APPLE__
// Not apple (Linux, etc)
#define pthread_setname(x) pthread_setname_np(pthread_self(),(x))
#endif // ifdef __APPLE__


char Libdir[] = "/usr/local/share/ka9q-radio";

int Samprate = 48000; // Samples per second - try to use this if possible
bool WWVH = false; // WWV by default
bool Verbose = false;
bool Negative_leap_second_pending = false; // If true, leap second will be removed at end of June or December, whichever is first
bool Positive_leap_second_pending = false; // If true, leap second will be inserted at end of June or December, whichever is first
bool NoTone = false;
bool NoVoice = false;
bool NoTimeCode = false;


// Applies only to non-leap years; you need special tests for February in leap year
int const Days_in_month[] = { // Index 1 = January, 12 = December
  0,31,28,31,30,31,30,31,31,30,31,30,31
};

// Tone schedules for each minute of the hour for each station
// Special exception: no 440 Hz tone in first hour of UTC day; must be handled ad-hoc
int const WWV_tone_schedule[60] = {
    0,600,440,  0,  0,600,500,600,  0,  0, // 3 is nist reserved at wwvh, 4 reserved at wwv; 8 research signal; 9-10 storms; 7 undoc wwv
    0,600,500,600,500,600,  0,600,  0,600, // 14-15 GPS (no longer used - tones), 16 nist reserved, 18 geoalerts; 11 undoc wwv
  500,600,500,600,500,600,500,600,500,  0, // 29 is silent to protect wwvh id
    0,600,500,600,500,600,500,600,500,600, // 30 is station ID
  500,600,500,  0,  0,  0,  0,  0,  0,  0, // 43-51 is silent period to protect wwvh
    0,  0,500,600,500,600,500,600,500,  0  // 59 is silent to protect wwvh id; 52 new special at wwvh, not protected by wwv
};

int const WWVH_tone_schedule[60] = {
    0,440,600,  0,  0,500,600,  0,  0,  0, // 0 silent to protect wwv id; 3 nist reserved; 4 reserved at wwv; 7 protects undoc wwv; 8-10 to protect wwv
    0,  0,600,500,  0,  0,  0,  0,  0,  0, // 14-19 is silent period to protect wwv; 11 silent to protect undoc wwv
  600,500,600,500,600,500,600,500,600,  0, // 29 is station ID
    0,500,600,500,600,500,600,500,600,500, // 30 silent to protect wwv id
  600,500,600,500,600,  0,600,  0,  0,  0, // 43-44 GPS (unused-tones); 45 geoalerts; 47 nist reserved; 48-51 storms
    0,  0,  0,500,600,500,600,500,600,  0  // 59 is station ID; 52 new special at wwvh?, NOT protected at WWV
};

struct qentry {
  struct qentry *next;
  int16_t *buffer;
  int offset; // Starting offset
  int length; // Samples
};

struct qentry *Queue;
pthread_t Output_thread;
void *output_thread(void *p);
pthread_mutex_t Output_mutex; // Protect queue
pthread_cond_t Output_cond;
int Samprate_ms;      // Samples per millisecond - sampling rates not divisible by 1000 may break

void cleanup(void);
void maketimecode(uint8_t *code,int dut1,bool leap_pending,int year,int month,int day,int hour,int minute);
void decode_timecode(uint8_t *code,int length);
void makeminute(int16_t *output,int length,bool wwvh,uint8_t const *code,int dut1,int hour,int minute);
int qlen(void);
bool const is_leap_year(int y);

static char const Optstring[] = "HY:M:D:h:m:s:u:r:LNvn:";
static const struct option Options[] = {
  {"device", required_argument, NULL, 'n' },
  {"verbose", no_argument, NULL, 'v'},
  {"samprate", required_argument, NULL, 'r'},
  {"wwvh", no_argument, NULL, 'H'},
  {"ut1", required_argument, NULL, 'u'},
  {"year", required_argument, NULL, 'Y'},
  {"month", required_argument, NULL, 'M'},
  {"day", required_argument, NULL, 'D'},
  {"hour", required_argument, NULL, 'h'},
  {"minute", required_argument, NULL, 'm'},
  {"second", required_argument, NULL, 's'},
  {"positive", no_argument, NULL, 'P'},
  {"negative", no_argument, NULL, 'N'},
  {"help", no_argument, NULL, '?'},
  {"no-voice", no_argument, NULL, 'd'},
  {"no-tone", no_argument, NULL, 't'},
  {"no-code", no_argument, NULL, 'c'},
  { NULL, no_argument, NULL, 0},
};


int main(int argc,char *argv[]){

  int dut1 = 0;
  bool manual_time = false;
  int devnum = -1;

  // Use current computer clock time as default
  struct timeval start_time;
  gettimeofday(&start_time,NULL);
  struct tm const * const tm = gmtime(&start_time.tv_sec);
  int sec = tm->tm_sec;
  int minute = tm->tm_min;
  int hour = tm->tm_hour;
  int day = tm->tm_mday;
  int month = tm->tm_mon + 1;
  int year = tm->tm_year + 1900;
  setlocale(LC_ALL,getenv("LANG"));

#if 0
  for(int y=2007;y < 2100;y++){
    fprintf(stderr,"year %d dst start %d\n",y,dst_start_doy(y));
  }
#endif

  int c;
  // Read and process command line arguments
  while((c = getopt_long(argc,argv,Optstring,Options,NULL)) != EOF){
    switch(c){
    case 'c':
      NoTimeCode = true;
      break;
    case 'd':
      NoVoice = true;
      break;
    case 't':
      NoTone = true; // Nicht diese tone!
      break;
    case 'n':
      devnum = strtol(optarg,NULL,0);
      break;
    case 'v':
      Verbose = true;
      break;
    case 'r':
      Samprate = strtol(optarg,NULL,0); // Try not to change this, may not work
      break;
    case 'H': // Simulate WWVH, otherwise WWV
      WWVH = true;
      break;
    case 'u': // UT1 offset in tenths of a second, +/- 7
      dut1 = strtol(optarg,NULL,0);
      break;
    case 'Y': // Manual year setting
      year = strtol(optarg,NULL,0);
      manual_time = true;
      break;
    case 'M': // Manual month setting
      month = strtol(optarg,NULL,0);
      manual_time = true;
      break;
    case 'D': // Manual day setting
      day = strtol(optarg,NULL,0);
      manual_time = true;
      break;
    case 'h': // Manual hour setting
      hour = strtol(optarg,NULL,0);
      manual_time = true;
      break;
    case 'm': // Manual minute setting
      minute = strtol(optarg,NULL,0);
      manual_time = true;
      break;
    case 's': // Manual second setting
      sec = strtol(optarg,NULL,0);
      manual_time = true;
      break;
    case 'L':
      Positive_leap_second_pending = true; // Positive leap second at end of current month
      break;
    case 'N':
      Negative_leap_second_pending = true;  // Leap second at end of current month
      break;
    case '?':
      fprintf(stderr,"Usage: %s [options]\n",argv[0]);
      fprintf(stderr,"[-n | --device <number>] select output device\n");
      fprintf(stderr,"[-v | --verbose]\n");
      fprintf(stderr,"[-r | --samprate <Hz>]\n");
      fprintf(stderr,"[-H | --wwvh] set WWVH mode; default WWV\n");
      fprintf(stderr,"[-u | --ut1 <offset>] ut1-utc offset in tenths of a second\n");
      fprintf(stderr,"[-Y | --year <year>] default system clock, indivdual fields can be overridden\n");
      fprintf(stderr,"[-M | --month <1-12>]\n");
      fprintf(stderr,"[-D | --day <1-31>]\n");
      fprintf(stderr,"[-h | --hour <0-23>]\n");
      fprintf(stderr,"[-m | --minute <0-59>]\n");
      fprintf(stderr,"[-s | --second <0-60>]\n");
      fprintf(stderr,"[-P | --positive] flag upcoming positive leap second \n");
      fprintf(stderr,"[-N | --negative] flag upcoming negative leap second \n");
      fprintf(stderr,"[-t | --no-tone] suppress 440, 500 and 600 Hz tones\n");
      fprintf(stderr,"[-d | --no-voice] suppress all voice announcements\n");
      fprintf(stderr,"[-c | --no-code] suppress 100 Hz timecode\n");
      exit(1);

    }
  }
  if(isatty(fileno(stdout))){
#ifdef USE_PORTAUDIO
    // No output redirection, so use portaudio to write directly to audio hardware with "precise" (?) timing
    Pa_Initialize();
    PaDeviceIndex dev = Pa_GetDefaultOutputDevice();
    if(devnum != -1)
      dev = devnum;

    PaStreamParameters param;
    param.device = dev;
    param.channelCount = 1;
    param.sampleFormat = paInt16;
    param.suggestedLatency = .02; // Don't make too small
    param.hostApiSpecificStreamInfo = NULL;

    int err = Pa_OpenStream(&Stream,NULL,&param,(double)Samprate,0,0,NULL,NULL);
    if(err != paNoError){
      fprintf(stderr,"Pa_OpenStream failed\n");
      exit(1);
    }
    atexit(cleanup);
#else
    fprintf(stderr,"Won't send PCM to a terminal (direct mode not compiled in)\n");
    exit(1);
#endif

  }
  if(year < 2007)
    fprintf(stderr,"Warning: DST rules prior to %d not implemented; DST bits = 0\n",year);    // Punt

  if(Positive_leap_second_pending && Negative_leap_second_pending){
    fprintf(stderr,"Positive and negative leap seconds can't both be pending! Both cancelled\n");
    Positive_leap_second_pending = Negative_leap_second_pending = false;
  }

  if(dut1 > 7 || dut1 < -7){
    fprintf(stderr,"ut1 offset %d out of range, limited to -7 to +7 tenths\n",dut1);
    dut1 = 0;
  }
  if(Positive_leap_second_pending && dut1 > -3){
    fprintf(stderr,"Postive leap second cancelled since dut1 > -0.3 sec\n");
    Positive_leap_second_pending = false;
  } else if(Negative_leap_second_pending && dut1 < 3){
    fprintf(stderr,"Negative leap second cancelled since dut1 < +0.3 sec\n");
    Negative_leap_second_pending = false;
  }

  Samprate_ms = Samprate/1000; // Samples per ms
  bool startup = true;
  // Set up output thread to write asynchronously
  pthread_create(&Output_thread,NULL,output_thread,NULL);

  while(1){
    int length = 60;    // Default length 60 seconds
    if((month == 6 || month == 12) && hour == 23 && minute == 59){
      if(Positive_leap_second_pending){
	length = 61; // This minute ends with a leap second!
      } else if(Negative_leap_second_pending){
	length = 59; // Negative leap second
      }
    }

    struct qentry *qe = calloc(1,sizeof(*qe));
    assert(qe != NULL);
    qe->length = length * Samprate;
    qe->buffer = calloc(sizeof(*qe->buffer),qe->length);
    assert(qe->buffer != NULL);

    // Generate timecode
    uint8_t code[61] = {0}; // one extra for a possible leap second
    if(!NoTimeCode){
      bool leap_pending = (Positive_leap_second_pending || Negative_leap_second_pending);
      maketimecode(code,dut1,leap_pending,year,month,day,hour,minute);

      // Optionally dump timecode
      if(Verbose){
	fprintf(stderr,"%d/%d/%d %02d:%02d\n",month,day,year,hour,minute);
	decode_timecode(code,length);
      }
    }
    // Build a minute of audio
    makeminute(qe->buffer,length,WWVH,NoTimeCode ? NULL : code,dut1,hour,minute);

    if(!manual_time && startup){
      // Buffers are constructed starting on the minute, so compute
      // how much of it to skip in the first one.
      // Speech synthesis can be slow, so look at the clock again
      // Don't do with this when time is manually set
      struct timeval now;
      gettimeofday(&now,NULL);
      struct tm const * const tm = gmtime(&now.tv_sec);
      if(minute != tm->tm_min){
	// We're already into the next minute? Speech synthesis can be slow.
	// Discard this first one and continue with the next minute
	// (What if we start during a leap second? geez...it never ends...)
	fprintf(stderr,"Discarding first minute\n");
	free(qe->buffer);
	free(qe);
	goto next_minute;
      } else {
	// Calculate starting offset into first buffer
	qe->offset = Samprate * ((long long)1000000 * tm->tm_sec + now.tv_usec) /  1000000;
	assert(qe->offset < Samprate * 60);
	startup = false;
      }
    }

    // Append to queue, wake output
    pthread_mutex_lock(&Output_mutex);
    struct qentry *last = NULL;
    for(struct qentry *q = Queue;q != NULL;last = q,q = q->next)
      ;

    if(last)
      last->next = qe;
    else
      Queue = qe; // First on empty queue

    pthread_cond_signal(&Output_cond);
    pthread_mutex_unlock(&Output_mutex);

    // Wait for queue to drain a little
    while(qlen() >= 2){
      useconds_t interval = 30000000; // Pause 30 sec
      usleep(interval);
    }
  next_minute:;
    if(length == 61){
      // Leap second just occurred in this last minute
      Positive_leap_second_pending = false;
      dut1 += 10;
    } else if(length == 59){
      Negative_leap_second_pending = false;
      dut1 -= 10;
    }
    // Advance to next minute
    sec = 0;
    if(++minute > 59){
      // New hour
      minute = 0;
      if(++hour > 23){
	// New day
	hour = 0;
	if(++day > ((month == 2 && is_leap_year(year))? 29 : Days_in_month[month])){
	  // New month
	  day = 1;
	  if(++month > 12){
	    // New year
	    month = 1;
	    ++year;
	  }
	}
      }
    }
  }
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


char *chomp(char *str){
  char *cp = strchr(str,'\n');
  if(cp != NULL)
    *cp = '\0';
  cp = strchr(str,'\r');
  if(cp != NULL)
    *cp = '\0';
  return str;
}

// Generate complex phasor with specified angle in radians
// Used for tone generation
complex double const csincos(double x){
  return cos(x) + I*sin(x);
}

// Insert PCM audio file into audio output at specified offset
int announce_audio_file(int16_t *output, char const *file, int startms){
  if(startms < 0 || startms >= 61000)
    return -1;

  int r = -1;

  FILE *fp;
  if((fp = fopen(file,"r")) != NULL){
    r = fread(output+startms*Samprate_ms,sizeof(*output),Samprate_ms*(61000-startms),fp);
    fclose(fp);
  }
  return r;
}

// Synthesize speech from a text file and insert into audio output at specified offset
// Use female = true for WWVH, false for WWV
int announce_text_file(int16_t *output,char const *file, int startms, bool female){
  int r = -1;

  char tempfile_raw[L_tmpnam+1];
  memset(tempfile_raw,0,sizeof(tempfile_raw));
  strncpy(tempfile_raw,"/tmp/srawXXXXXX.raw",sizeof(tempfile_raw));
  mkstemps(tempfile_raw,4);

#if defined(__APPLE__) || defined(PIPER)
  char tempfile_wav[L_tmpnam+1];
  memset(tempfile_wav,0,sizeof(tempfile_wav));
  strncpy(tempfile_wav,"/tmp/swavXXXXXX.wav",sizeof(tempfile_wav));
  mkstemps(tempfile_wav,4);
#endif

  int asr = -1;
  char *fullname = NULL;
  if(file[0] == '/')
    asr = asprintf(&fullname,"%s",file); // Leading slash indicates absolute path name
  else
    asr = asprintf(&fullname,"%s/%s",Libdir,file); // Otherwise relative to library directory

  if(asr == -1 || !fullname)
    goto done; // asprintf failed for some reason

  chomp(fullname);
  if(access(fullname,R_OK) != 0)
    goto done; // file isn't readable (what if it's a directory?

  char const *voice = NULL;
  asr = -1;

  char *command = NULL;

#ifdef __APPLE__
  voice = female ? "Samantha" : "Alex";
  asr = asprintf(&command,"say -v %s --output-file=%s --data-format=LEI16@48000 -f %s; sox %s -t raw -r 48000 -c 1 -b 16 -e signed-integer %s",
	   voice,tempfile_wav,fullname,tempfile_wav,tempfile_raw);

#elif defined(PIPER)
  voice = female ? "en_US-kathleen-low.onnx" : "en_US-ryan-medium.onnx";
  asr = asprintf(&command,"/usr/local/bin/piper --model /usr/local/lib/piper/%s --output_file - < %s | sox -t wav - -t raw -r 48000 -c 1 -b 16 -e signed-integer %s",
		 voice,fullname,tempfile_raw);

#else // crappy espeak
  voice = female ? "en-us+f3" : "en-us";
  asr = asprintf(&command,"espeak -v %s -a 70 -f %s --stdout | sox -t wav - -t raw -r 48000 -c 1 -b 16 -e signed-integer %s",
	   voice,fullname,tempfile_raw);
#endif
  if(asr == -1 || !command)
    goto done; // asprintf failed somehow

  if(Verbose){
    fprintf(stderr,"Executing \"%s\" to speak:\n",command);
    FILE *in = fopen(fullname,"r");
    int c;
    while((c = fgetc(in)) != EOF)
      fputc(c,stderr);
    fputc('\n',stderr);
    fflush(stderr);
    fclose(in);
  }

  system(command);

  r = announce_audio_file(output,tempfile_raw, startms);

 done:; // Go here directly on errors
  // Clean up
  unlink(tempfile_raw);
#if defined(__APPLE__) || defined(PIPER)
  unlink(tempfile_wav);
#endif

  if(command)
    free(command);
  if(fullname)
    free(fullname);
  return r;
}

// Synthesize a text announcement and insert into output buffer
int announce_text(int16_t *output,char const *message,int startms,int female){

  char tempfile_txt[L_tmpnam+1];
  memset(tempfile_txt,0,sizeof(tempfile_txt));
  strncpy(tempfile_txt,"/tmp/stextXXXXXX.txt",sizeof(tempfile_txt));
  mkstemps(tempfile_txt,4);

  FILE *fp;
  if ((fp = fopen(tempfile_txt,"w")) == NULL)
    return -1;
  fputs(message,fp);
  fclose(fp);
  int r = announce_text_file(output,tempfile_txt,startms,female);
  unlink(tempfile_txt);
  return r;
}


// Overlay a tone with frequency 'freq' in audio buffer, overwriting whatever was there
// starting at 'startms' within the minute and stopping one sample before 'stopms'.
// Amplitude 1.0 is 100% modulation, 0.5 is 50% modulation, etc
// Used first for 500/600 Hz continuous audio tones
// Then used for 1000/1200 Hz minute/hour beeps and second ticks, which pre-empt everything else.
int overlay_tone(int16_t *output,int startms,int stopms,float freq,float amp){
  if(startms < 0 || stopms <= startms || stopms > 61000)
    return -1;

  assert((startms * (int)freq % 1000) == 0); // All tones start with a positive zero crossing?

  complex double phase = 1;
  complex double const phase_step = csincos(2*M_PI*freq/Samprate);
  output += startms*Samprate_ms;
  int samples = (stopms - startms)*Samprate_ms;
  while(samples-- > 0){
    *output++ = cimag(phase) * amp * SHRT_MAX; // imaginary component is sine, real is cosine
    phase *= phase_step;  // Rotate the tone phasor
  }
 return 0;
}

// Same as overlay_tone() except that the tone is added to whatever is already in the audio buffer
// Take care to avoid overmodulation; the result will be clipped but could still sound bad
// Used mainly for 100 Hz subcarrier
int add_tone(int16_t *output,int startms,int stopms,float freq,float amp){
  if(startms < 0 || stopms <= startms || stopms > 61000)
    return -1;

  assert((startms * (int)freq % 1000) == 0); // All tones start with a positive zero crossing?

  complex double phase = 1;
  complex double const phase_step = csincos(2*M_PI*freq/Samprate);
  output += startms*Samprate_ms;
  int samples = (stopms - startms)*Samprate_ms;
  while(samples-- > 0){
    // Add and clip
    float const samp = *output + cimag(phase) * amp * SHRT_MAX;
    *output++ = samp > 32767 ? 32767 : samp < -32767 ? -32767 : samp;
    phase *= phase_step; // Rotate the tone phasor
  }
  return 0;
}

// Blank out whatever is in the audio buffer starting at startms and ending just before stopms
// Used mainly to blank out 40 ms guard interval around seconds ticks
int overlay_silence(int16_t *output,int startms,int stopms){
  if(startms < 0 || stopms <= startms || stopms > 61000)
    return -1;
  output += startms*Samprate_ms;
  int const samples = (stopms - startms)*Samprate_ms;

  memset(output,0,samples * sizeof(*output));
  return 0;
}

// Encode a BCD digit in little-endian format (lsb first)
// NB! Only WWV/WWVH; WWVB uses big-endian format
void encode(uint8_t *code,int x){
  for(int i=0;i<4;i++){
    code[i] = x & 1;
    x >>= 1;
  }
}
int decode(uint8_t const *code){
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
int dst_start_doy(int year){
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

int day_of_year(int year,int month,int day){
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


// Construct time code as array of **61** unsigned chars with values 0 or 1
void maketimecode(uint8_t *code,int dut1,bool leap_pending,int year,int month,int day,int hour,int minute){
    memset(code,0,61*sizeof(*code)); // All bits default to 0

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

// Decode frame of timecode to stderr for debugging
void decode_timecode(uint8_t *code,int length){
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

// Insert tone or announcement into seconds 1-44
void gen_tone_or_announcement(int16_t *output,bool wwvh,int hour,int minute){
  const double tone_amp = pow(10.,-6.0/20.); // -6 dB

  // A raw audio file pre-empts everything else
  char *rawfilename = NULL;
  char *textfilename = NULL;

  if(!NoVoice && asprintf(&rawfilename,"%s/%s/%d.raw",Libdir,wwvh ? "wwvh" : "wwv",minute)
     && access(rawfilename,R_OK) == 0){
    announce_audio_file(output,rawfilename,1000);
    goto done;
  } else if(!NoVoice && asprintf(&textfilename,"%s/%s/%d.txt",Libdir,wwvh ? "wwvh" : "wwv",minute)
	    && access(textfilename,R_OK) == 0){
    announce_text_file(output,textfilename,1000,wwvh);
    goto done;
  } else if (!NoTone){
    // Otherwise generate a tone, unless silent
    double tone = wwvh ? WWVH_tone_schedule[minute] : WWV_tone_schedule[minute];

    // Special case: no 440 Hz tone during hour 0
    if(tone == 440 && hour == 0)
      tone = 0;

    if(tone)
      add_tone(output,1000,45000,tone,tone_amp); // Continuous tone from 1 sec until 45 sec
  }

 done:;
  if(rawfilename)
    free(rawfilename);
  if(textfilename)
    free(textfilename);
}



void makeminute(int16_t *output,int length,bool wwvh,uint8_t const *code,int dut1,int hour,int minute){
  // Amplitudes
  // NIST 250-67, p 50
  const double marker_high_amp = pow(10.,-6.0/20.);
  //  NIST 250-67, p 47 says 1/3.3 (about -10 dB) but is apparently incorrect; observed is ~ -20 dB
  // WWV staff says it's meant to be off, but the hardware won't go there so they set it to minimum
  //  const double marker_low_amp = marker_high_amp / 3.3;
  //  const double marker_low_amp = marker_high_amp / 10;
  const double marker_low_amp = 0;
  const double tick_amp = 1.0; // 100%, 0dBFS

  const double tickfreq = wwvh ? 1200.0 : 1000.0;
  const double hourbeep = 1500.0; // Both WWV and WWVH

  // Build a minute of audio
  memset(output,0,length*Samprate*sizeof(*output)); // Clear previous audio
  gen_tone_or_announcement(output,wwvh,hour,minute);

  // Insert minute announcement
  // What are the next hour and minute?
  int nextminute = minute;
  int nexthour = hour;
  if(++nextminute == 60){
    nextminute = 0;
    if(++nexthour == 24)
      nexthour = 0;
  }
  if(!NoVoice){
    char *message = NULL;
    int asr = asprintf(&message,"At the tone, %d %s %d %s Coordinated Universal Time",
		       nexthour,nexthour == 1 ? "hour" : "hours",
		       nextminute,nextminute == 1 ? "minute" : "minutes");
    if(asr != -1 && message){
      if(!wwvh)
	announce_text(output,message,52500,0); // WWV: male voice at 52.5 seconds
      else
	announce_text(output,message,45000,1); // WWVH: female voice at 45 seconds
      free(message); message = NULL;
    }
  }
  if(code != NULL){
    // Modulate time code onto 100 Hz subcarrier
    for(int s=1; s<length; s++){ // No subcarrier during second 0 (minute/hour beep)
      if((s % 10) == 9){
	add_tone(output,s*1000,s*1000+800,100,marker_high_amp);	 // 800 ms position markers on seconds 9, 19, 29, ...
	add_tone(output,s*1000+800,s*1000+1000,100,marker_low_amp);
      } else if(code[s]){
	add_tone(output,s*1000,s*1000+500,100,marker_high_amp);	 // 500 ms = 1 bit
      add_tone(output,s*1000+500,s*1000+1000,100,marker_low_amp);
      } else {
	add_tone(output,s*1000,s*1000+200,100,marker_high_amp);	 // 200 ms = 0 bit
	add_tone(output,s*1000+200,s*1000+1000,100,marker_low_amp);
      }
    }
  }
  // Pre-empt with minute/hour beep and guard interval
  overlay_tone(output,0,800,minute == 0 ? hourbeep : tickfreq,tick_amp);
  overlay_silence(output,800,1000);

  // Pre-empt with second ticks and guard interval
  for(int s=1; s<length; s++){
    if(s != 29 && s < 59){
      // No ticks or blanking on 29, 59 or 60
      // Blank with silence from t-10 ms to t+30, total 40 ms
      overlay_silence(output,1000*s-10,1000*s+30);
      overlay_tone(output,1000*s,1000*s+5,tickfreq,tick_amp); // 5 ms tick at 100% modulation on second
    }
    // Double ticks without guard time for UT1 offset
    if((dut1 > 0 && s >= 1 && s <= dut1)
       || (-dut1 > 0 && s >= 9 && s <= 8-dut1)){
      overlay_tone(output,1000*s+100,1000*s+105,tickfreq,tick_amp); // 5 ms second tick at 100 ms
    }
  }
}


// Read from buffer, send to standard output
// In separate thread to run parallel with next buffer generation (similar to port audio for direct output)
void *output_thread(void *p){
  pthread_setname("output");

  bool started = false;

  while(1){
    struct qentry *qe;
    pthread_mutex_lock(&Output_mutex);
    while(Queue == NULL)
      pthread_cond_wait(&Output_cond,&Output_mutex);
    qe = Queue;
    Queue = qe->next;
    qe->next = NULL;
    pthread_mutex_unlock(&Output_mutex);

#if USE_PORTAUDIO
    if(!started && Stream){
      int err = Pa_StartStream(Stream);
      if(err != paNoError){
	fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(err));
	exit(1);
      }
      started = true;
    }
    if(Stream){
      int err = Pa_WriteStream(Stream,qe->buffer + qe->offset,qe->length - qe->offset);
      if(err != paNoError){
	fprintf(stderr,"Portaudio error: %s\n",Pa_GetErrorText(err));
      }
    } else {
      fwrite(qe->buffer + qe->offset,sizeof(int16_t),qe->length - qe->offset,stdout);
      fflush(stdout);
    }
#else
    fwrite(qe->buffer + qe->offset,sizeof(int16_t),qe->length - qe->offset,stdout);
    fflush(stdout);
#endif
    free(qe->buffer);
    free(qe);
  }
  return NULL;
}
void cleanup(void){
#if USE_PORTAUDIO
  Pa_Terminate();
#endif
}


// Return length of output queue
int qlen(void){
  int len = 0;
  pthread_mutex_lock(&Output_mutex);
  for(struct qentry *q = Queue;q != NULL;q = q->next)
    len++;
  pthread_mutex_unlock(&Output_mutex);
  return len;
}
