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
// 11 May 2025: Cleanups, --no-tone, --no-voice, --no-code options
// 14 Sep 2025: add short options to Optstring (N5TNL)
// 14 Sep 2025: add -1 option to exit after one minute of output (manual time mode only) (N5TNL)

#define USE_PORTAUDIO 1 // Enable direct on-time output to sound device with portaudio when stdout is a terminal
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
#include <errno.h>
#include <spawn.h>
#include <sys/wait.h>
#include "timecode.h"
#include "paths.h"

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


int Samprate = 48000; // Samples per second - try to use this if possible
bool WWVH = false; // WWV by default
bool Verbose = false;
bool Negative_leap_second_pending = false; // If true, leap second will be removed at end of June or December, whichever is first
bool Positive_leap_second_pending = false; // If true, leap second will be inserted at end of June or December, whichever is first
bool NoTone = false;
bool NoVoice = false;
bool NoTimeCode = false;
bool One_minute_mode;
bool Manual_time;

// Tone schedules for each minute of the hour for each station
// Special exception: no 440 Hz tone in first hour of UTC day; must be handled ad-hoc
// minutes 9 and 16 updated by observation 4 June 2026 0600-0700UTC
int const WWV_tone_schedule[60] = {
    0,600,440,  0,  0,600,500,600,  0,600, // 3 is nist reserved at wwvh, 4 reserved at wwv; 8 research signal; 7 undoc wwv
    0,600,500,600,500,600,500,600,  0,600, // 16 nist reserved, 18 geoalerts; 11 undoc wwv
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
pthread_mutex_t Output_mutex; // Protect queue
pthread_cond_t Output_cond;
int Samprate_ms;      // Samples per millisecond - sampling rates not divisible by 1000 may break

static char const Optstring[] = "HY:M:D:h:m:s:u:r:LNvn:Pdtc1";
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
  {"1-minute", no_argument, NULL, '1'},
  { NULL, no_argument, NULL, 0},
};
void gen_tone_or_announcement(int16_t *output, int length, bool wwvh, int hour, int minute);
void *output_thread(void *p);
void cleanup(void);
void maketimecode(uint8_t *code, int dut1, bool leap_pending, int year, int month, int day, int hour, int minute);
void decode_timecode(uint8_t const *code, int length);
void makeminute(int16_t *output, int length, bool wwvh, bool novoice, uint8_t const *code, int dut1, int hour, int minute);
int announce_audio_file(int16_t *output, int length, char const *file, int startms);
int add_tone(int16_t *output, int startms, int stopms, float freq, float amp);
int overlay_tone(int16_t *output, int startms, int stopms, float freq, float amp);
int overlay_silence(int16_t *output, int startms, int stopms);
static int qlen(void);

// Generate complex phasor with specified angle in radians for tone generation
static inline complex double const csincos(double x){
  return cos(x) + I*sin(x);
}

int main(int argc, char *argv[]){
  int dut1 = 0;
  int devnum = -1;
  // Use current computer clock time as default
  struct timeval start_time;
  gettimeofday(&start_time, NULL);
  struct tm const * const tm = gmtime(&start_time.tv_sec);
  int sec = tm->tm_sec;
  int minute = tm->tm_min;
  int hour = tm->tm_hour;
  int day = tm->tm_mday;
  int month = tm->tm_mon + 1;
  int year = tm->tm_year + 1900;
  setlocale(LC_ALL, getenv("LANG"));
#if 0
  for(int y=2007;y < 2100;y++){
    fprintf(stderr, "year %d dst start %d\n", y, dst_start_doy(y));
  }
#endif
  umask(002); // make cached files group writeable

  int c;
  // Read and process command line arguments
  while((c = getopt_long(argc, argv, Optstring, Options, NULL)) != EOF){
    switch(c){
    case 'c':
      NoTimeCode = true;
      break;
    case 'd':
      NoVoice = true;
      break;
    case 't':
      NoTone = true; // Nicht diese Tone!
      break;
    case 'n':
      devnum = strtol(optarg, NULL, 0);
      break;
    case 'v':
      Verbose = true;
      break;
    case 'r':
      Samprate = strtol(optarg, NULL, 0); // Try not to change this, may not work
      break;
    case 'H': // Simulate WWVH, otherwise WWV
      WWVH = true;
      break;
    case 'u': // UT1 offset in tenths of a second, +/- 7
      dut1 = strtol(optarg, NULL, 0);
      break;
    case 'Y': // Manual year setting
      year = strtol(optarg, NULL, 0);
      Manual_time = true;
      break;
    case 'M': // Manual month setting
      month = strtol(optarg, NULL, 0);
      Manual_time = true;
      break;
    case 'D': // Manual day setting
      day = strtol(optarg, NULL, 0);
      Manual_time = true;
      break;
    case 'h': // Manual hour setting
      hour = strtol(optarg, NULL, 0);
      Manual_time = true;
      break;
    case 'm': // Manual minute setting
      minute = strtol(optarg, NULL, 0);
      Manual_time = true;
      break;
    case 's': // Manual second setting
      sec = strtol(optarg, NULL, 0);
      Manual_time = true;
      break;
    case 'L':
      Positive_leap_second_pending = true; // Positive leap second at end of current month
      break;
    case 'N':
      Negative_leap_second_pending = true;  // Leap second at end of current month
      break;
    case '1':
      One_minute_mode = true;
      break;
    case '?':
      fprintf(stderr, "Usage: %s [options]\n", argv[0]);
      fprintf(stderr, "[-n | --device <number>] select output device\n");
      fprintf(stderr, "[-v | --verbose]\n");
      fprintf(stderr, "[-r | --samprate <Hz>]\n");
      fprintf(stderr, "[-H | --wwvh] set WWVH mode; default WWV\n");
      fprintf(stderr, "[-u | --ut1 <offset>] ut1-utc offset in tenths of a second\n");
      fprintf(stderr, "[-Y | --year <year>] default system clock, indivdual fields can be overridden\n");
      fprintf(stderr, "[-M | --month <1-12>]\n");
      fprintf(stderr, "[-D | --day <1-31>]\n");
      fprintf(stderr, "[-h | --hour <0-23>]\n");
      fprintf(stderr, "[-m | --minute <0-59>]\n");
      fprintf(stderr, "[-s | --second <0-60>]\n");
      fprintf(stderr, "[-P | --positive] flag upcoming positive leap second \n");
      fprintf(stderr, "[-N | --negative] flag upcoming negative leap second \n");
      fprintf(stderr, "[-t | --no-tone] suppress 440, 500 and 600 Hz tones\n");
      fprintf(stderr, "[-d | --no-voice] suppress all voice announcements\n");
      fprintf(stderr, "[-c | --no-code] suppress 100 Hz timecode\n");
      fprintf(stderr, "[-1 | --1-minute] output 1 minute of samples (manual time mode only)\n");
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
    PaStreamParameters param = {
      .device = dev,
      .channelCount = 1,
      .sampleFormat = paInt16,
      .suggestedLatency = .04, // Don't make too small
      .hostApiSpecificStreamInfo = NULL
    };
    int err = Pa_OpenStream(&Stream, NULL, &param, (double)Samprate, 0, 0, NULL, NULL);
    if(err != paNoError){
      fprintf(stderr, "Pa_OpenStream failed\n");
      exit(1);
    }
    atexit(cleanup);
#else
    fprintf(stderr, "Won't send PCM to a terminal (direct mode not compiled in)\n");
    exit(1);
#endif
  }
  if(year < 2007)
    fprintf(stderr, "Warning: DST rules prior to %d not implemented; DST bits = 0\n", year);    // Punt
  if(Positive_leap_second_pending && Negative_leap_second_pending){
    fprintf(stderr, "Positive and negative leap seconds can't both be pending! Both cancelled\n");
    Positive_leap_second_pending = Negative_leap_second_pending = false;
  }
  if(dut1 > 7 || dut1 < -7){
    fprintf(stderr, "ut1 offset %d out of range, limited to -7 to +7 tenths\n", dut1);
    dut1 = 0;
  }
  if(Positive_leap_second_pending && dut1 > -3){
    fprintf(stderr, "Postive leap second cancelled since dut1 > -0.3 sec\n");
    Positive_leap_second_pending = false;
  } else if(Negative_leap_second_pending && dut1 < 3){
    fprintf(stderr, "Negative leap second cancelled since dut1 < +0.3 sec\n");
    Negative_leap_second_pending = false;
  }
  Samprate_ms = Samprate/1000; // Samples per ms
  bool startup = true;
  // Set up output thread to write asynchronously
  pthread_create(&Output_thread, NULL, output_thread, NULL);
  while(true){
    int length = 60;    // Default length 60 seconds
    if((month == 6 || month == 12) && hour == 23 && minute == 59){
      if(Positive_leap_second_pending){
	length = 61; // This minute ends with a leap second!
      } else if(Negative_leap_second_pending){
	length = 59; // Negative leap second
      }
    }
    struct qentry *qe = calloc(1, sizeof *qe);
    assert(qe != NULL);
    qe->length = length * Samprate; // Worst case
    qe->buffer = calloc(sizeof *qe->buffer, qe->length);
    assert(qe->buffer != NULL);
    // Generate timecode
    uint8_t code[61] = {0}; // one extra for a possible leap second
    if(!NoTimeCode){
      bool leap_pending = (Positive_leap_second_pending || Negative_leap_second_pending);
      maketimecode(code, dut1, leap_pending, year, month, day, hour, minute);
      // Optionally dump timecode
      if(Verbose){
	fprintf(stderr, "%d/%d/%d %02d:%02d\n", month, day, year, hour, minute);
	decode_timecode(code, length);
      }
    }
    // Build a minute of audio
    makeminute(qe->buffer, length, WWVH, NoVoice, NoTimeCode ? NULL : code, dut1, hour, minute);
    if(!Manual_time && startup){
      // Buffers are constructed starting on the minute, so compute
      // how much of it to skip in the first one.
      // Don't do with this when time is manually set
      struct timeval now;
      gettimeofday(&now, NULL);
      struct tm const * const tm = gmtime(&now.tv_sec);
      if(minute != tm->tm_min){
	// We're already into the next minute? Speech synthesis can be slow.
	// Discard this first one and continue with the next minute
	// (What if we start during a leap second? geez...it never ends...)
	fprintf(stderr, "Discarding first minute\n");
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
    for(struct qentry *q = Queue;q != NULL;last = q, q = q->next)
      ;
    if(last)
      last->next = qe;
    else
      Queue = qe; // First on empty queue
    pthread_cond_signal(&Output_cond);
    pthread_mutex_unlock(&Output_mutex);
    if (One_minute_mode && Manual_time){
      // manual time mode and only output one minute, so wait for the output thread to exit
      if (pthread_join(Output_thread, NULL) != 0) {
        perror("pthread_join() failed");
        return 1;
      }
      return 0;
    }
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
  exit(0);
}
// Build a minute of composite audio
void makeminute(int16_t *output, int length, bool wwvh, bool novoice, uint8_t const *code, int dut1, int hour, int minute){
  // Amplitudes
  // NIST 250-67, p 50
  const double marker_high_amp = pow(10., -6.0/20.);
  //  NIST 250-67, p 47 says 1/3.3 (about -10 dB) but is apparently incorrect; observed is ~ -20 dB
  // WWV staff says it's meant to be off, but the hardware won't go there so they set it to minimum
  //  const double marker_low_amp = marker_high_amp / 3.3;
  //  const double marker_low_amp = marker_high_amp / 10;
  const double marker_low_amp = 0;
  const double tick_amp = 1.0; // 100%, 0dBFS
  const double tickfreq = wwvh ? 1200.0 : 1000.0;
  const double hourbeep = 1500.0; // Both WWV and WWVH

  memset(output, 0, length*Samprate*sizeof *output); // Clear previous audio
  gen_tone_or_announcement(output, length, wwvh, hour, minute);
  // Insert minute announcement
  // What are the next hour and minute?
  int nextminute = minute + 1;
  int nexthour = hour;
  if(nextminute == 60){
    nextminute = 0;
    if(++nexthour == 24)
      nexthour = 0;
  }
  if(!novoice){
    // Insert hour/minute announcement into 45-60: 45 for WWVH, 52.5 for WWV
    char vfile[PATH_MAX];
    if(wwvh){
      // Times need adjustment
      int start = 45000;
      int len = 0;
      snprintf(vfile, sizeof vfile, "wwvh/minute/h_at_the_tone");
      announce_audio_file(output, length, vfile, start+500);
      snprintf(vfile, sizeof vfile, "wwvh/minute/h_%d", nexthour);
      len = announce_audio_file(output, length, vfile, start+1500);
      snprintf(vfile, sizeof vfile, "wwvh/minute/%s", nexthour == 1 ? "h_hour" : "h_hours");
      announce_audio_file(output, length, vfile, start + 1500 + 1000 * len/Samprate);
      snprintf(vfile, sizeof vfile, "wwvh/minute/h_%d", nextminute);
      len = announce_audio_file(output, length, vfile, start + 3000);
      snprintf(vfile, sizeof vfile, "wwvh/minute/%s", nextminute == 1 ? "h_minute" : "h_minutes");
      announce_audio_file(output, length, vfile, start + 3000 + 1000 * len/Samprate);
      snprintf(vfile, sizeof vfile, "wwvh/minute/h_utc");
      announce_audio_file(output, length, vfile, start + 4750);
    } else {      // WWV
      int start = 52500;
      int len = 0;
      snprintf(vfile, sizeof vfile, "wwv/minute/v_at_the_tone");
      announce_audio_file(output, length, vfile, start);
      snprintf(vfile, sizeof vfile, "wwv/minute/v_%d",nexthour);
      len = announce_audio_file(output, length, vfile, start + 650);
      snprintf(vfile, sizeof vfile, "wwv/minute/%s", nexthour == 1 ? "v_hour" : "v_hours");
      announce_audio_file(output, length, vfile, start + 650 + 1000 * len/Samprate);
      snprintf(vfile, sizeof vfile, "wwv/minute/v_%d", nextminute);
      len = announce_audio_file(output, length, vfile, start + 2300);
      snprintf(vfile, sizeof vfile, "wwv/minute/%s", nextminute == 1 ? "v_minute" : "v_minutes");
      announce_audio_file(output, length, vfile, start + 2300 + 1000 * len/Samprate);
      snprintf(vfile, sizeof vfile, "wwv/minute/v_utc");
      announce_audio_file(output, length, vfile, start + 4000);
    }
  }
  if(code != NULL){
    // Modulate time code onto 100 Hz subcarrier
    for(int s=1; s<length; s++){ // No subcarrier during second 0 (minute/hour beep)
      if((s % 10) == 9){
	add_tone(output, s*1000, s*1000+800, 100, marker_high_amp);	 // 800 ms position markers on seconds 9, 19, 29, ...
	add_tone(output, s*1000+800, s*1000+1000, 100, marker_low_amp);
      } else if(code[s]){
	add_tone(output, s*1000, s*1000+500, 100, marker_high_amp);	 // 500 ms = 1 bit
      add_tone(output, s*1000+500, s*1000+1000, 100, marker_low_amp);
      } else {
	add_tone(output, s*1000, s*1000+200, 100, marker_high_amp);	 // 200 ms = 0 bit
	add_tone(output, s*1000+200, s*1000+1000, 100, marker_low_amp);
      }
    }
  }
  // Pre-empt with minute/hour beep and guard interval
  overlay_tone(output, 0, 800, minute == 0 ? hourbeep : tickfreq, tick_amp);
  overlay_silence(output, 800, 1000);
  // Pre-empt with second ticks and guard interval
  for(int s=1; s<length; s++){
    if(s != 29 && s < 59){
      // No ticks or blanking on 29, 59 or 60
      // Blank with silence from t-10 ms to t+30, total 40 ms
      overlay_silence(output, 1000*s-10, 1000*s+30);
      overlay_tone(output, 1000*s, 1000*s+5, tickfreq, tick_amp); // 5 ms tick at 100% modulation on second
    }
    // Double ticks without guard time for UT1 offset
    if((dut1 > 0 && s >= 1 && s <= dut1)
       || (-dut1 > 0 && s >= 9 && s <= 8-dut1)){
      overlay_tone(output, 1000*s+100, 1000*s+105, tickfreq, tick_amp); // 5 ms second tick at 100 ms
    }
  }
}
// Insert tone or announcement into seconds 1-44
void gen_tone_or_announcement(int16_t *output, int length, bool wwvh, int hour, int minute){
  const double tone_amp = pow(10., -6.0/20.); // -6 dB
  // A raw audio file pre-empts everything else
  char *rawfilename = NULL;
  char *textfilename = NULL;
  if(!NoVoice && asprintf(&rawfilename, "%s/announce/%d", wwvh ? "wwvh" : "wwv", minute)
     && access(rawfilename, R_OK) == 0){
    announce_audio_file(output, length, rawfilename, 1000);
    goto done;
  } else if (!NoTone){
    // Otherwise generate a tone, unless silent
    double tone = wwvh ? WWVH_tone_schedule[minute] : WWV_tone_schedule[minute];
    // Special case: no 440 Hz tone during hour 0
    if(tone == 440 && hour == 0)
      tone = 0;
    if(tone != 0)
      add_tone(output, 1000, 45000, tone, tone_amp); // Continuous tone from 1 sec until 45 sec
  }
 done:;
  free(rawfilename);
  free(textfilename);
}
// Insert PCM audio file into audio output at specified offset
// Note: length is in seconds, startms is in milliseconds
// Takes relative file name of form "wwv/minute/foo", appends .raw, looks first in /var/cache/wwvsim
// If not found, generates cached copy from .mp3 source in /usr/share/wwvsim
int announce_audio_file(int16_t *output, int length, char const *file, int startms){
  if(startms < 0 || startms >= 61000)
    return -1;
  char rawname[PATH_MAX];
  snprintf(rawname,sizeof rawname, "%s/%s.raw",CACHE_DIR,file);
  FILE *fp = fopen(rawname, "r");
  if(fp == NULL){
    // see if it's in the share directory
    snprintf(rawname,sizeof rawname, "%s/%s.raw",SHARE_DIR,file);
    fp = fopen(rawname, "r");
    if(fp == NULL){
      // Try to regenerate
      char sourcename[PATH_MAX];
      snprintf(sourcename,sizeof sourcename, "%s/%s.mp3",SHARE_DIR, file);
      char *argv[] = {
	"/usr/bin/sox",
	sourcename,
	"-t", "raw",
	"-e", "signed-integer",
	"-b", "16",
	"-r", "48000",
	"-c", "1",
	rawname,
	NULL
      };
      int pid = 0;
    int status = 0;
    posix_spawn(&pid, argv[0], NULL, NULL, argv, NULL);
    waitpid(pid, &status, 0);
    snprintf(rawname,sizeof rawname, "%s/%s.raw",CACHE_DIR,file);
    fp = fopen(rawname, "r");
    }
  }
  if(fp != NULL){
    size_t ret = fread(output+startms*Samprate_ms,
		    sizeof *output,
		    Samprate_ms*(1000*length-startms),
		    fp);
    if(ret == 0)
      fprintf(stderr, "Can't read %s: %s\n", file, strerror(errno));
    fclose(fp);
    return ret;
  }
  return -1;
}
// Same as overlay_tone() except that the tone is added to whatever is already in the audio buffer
// Take care to avoid overmodulation; the result will be clipped but could still sound bad
// Used mainly for 100 Hz subcarrier
int add_tone(int16_t *output, int startms, int stopms, float freq, float amp){
  if(startms < 0 || stopms <= startms || stopms > 61000)
    return -1;
  assert((startms * (int)freq % 1000) == 0); // All tones start with a positive zero crossing?
  complex double phase = 1 + I*0;
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
// Overlay a sine tone with frequency 'freq' in audio buffer, overwriting whatever was there
// starting at 'startms' within the minute and stopping one sample before 'stopms'.
// Amplitude 1.0 is 100% modulation, 0.5 is 50% modulation, etc
// Used first for 500/600 Hz continuous audio tones
// Then used for 1000/1200 Hz minute/hour beeps and second ticks, which pre-empt everything else.
int overlay_tone(int16_t *output, int startms, int stopms, float freq, float amp){
  if(startms < 0 || stopms <= startms || stopms > 61000)
    return -1;
  assert((startms * (int)freq % 1000) == 0); // All tones start with a positive zero crossing?
  complex double phase = 1 + I*0;
  complex double const phase_step = csincos(2*M_PI*freq/Samprate);
  output += startms*Samprate_ms;
  int samples = (stopms - startms)*Samprate_ms;
  while(samples-- > 0){
    *output++ = cimag(phase) * amp * SHRT_MAX; // imaginary component is sine, real is cosine
    phase *= phase_step;  // Rotate the tone phasor
  }
 return 0;
}
// Blank out whatever is in the audio buffer starting at startms and ending just before stopms
// Used mainly to blank out 40 ms guard interval around seconds ticks
int overlay_silence(int16_t *output, int startms, int stopms){
  if(startms < 0 || stopms <= startms || stopms > 61000)
    return -1;
  output += startms*Samprate_ms;
  int const samples = (stopms - startms)*Samprate_ms;
  memset(output, 0, samples * sizeof *output);
  return 0;
}
// Read from buffer, send to standard output
// In separate thread to run parallel with next buffer generation (similar to port audio for direct output)
void *output_thread(void *p){
  pthread_setname("output");

  bool started = false;

  while(true){
    struct qentry *qe;
    pthread_mutex_lock(&Output_mutex);
    while(Queue == NULL)
      pthread_cond_wait(&Output_cond, &Output_mutex);
    qe = Queue;
    Queue = qe->next;
    qe->next = NULL;
    pthread_mutex_unlock(&Output_mutex);

#if USE_PORTAUDIO
    if(!started && Stream){
      int err = Pa_StartStream(Stream);
      if(err != paNoError){
	fprintf(stderr, "Portaudio error: %s\n", Pa_GetErrorText(err));
	exit(1);
      }
      started = true;
    }
    if(Stream){
      int err = Pa_WriteStream(Stream, qe->buffer + qe->offset, qe->length - qe->offset);
      if(err != paNoError){
	fprintf(stderr, "Portaudio error: %s\n", Pa_GetErrorText(err));
      }
    } else {
      fwrite(qe->buffer + qe->offset, sizeof(int16_t), qe->length - qe->offset, stdout);
      if (One_minute_mode && Manual_time)
        pthread_exit(NULL);
    }
#else
    fwrite(qe->buffer + qe->offset, sizeof(int16_t), qe->length - qe->offset, stdout);
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
static int qlen(void){
  int len = 0;
  pthread_mutex_lock(&Output_mutex);
  for(struct qentry *q = Queue;q != NULL;q = q->next)
    len++;
  pthread_mutex_unlock(&Output_mutex);
  return len;
}
