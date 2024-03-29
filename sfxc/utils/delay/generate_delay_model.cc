/*
  purpose    : delmo generates delay tables in sfxc type format according to
  the parameters set in a delmo_control_file, which was generated
  with the utility vex2ccf

  last change: 29-05-2007
  authors    : RHJ Oerlemans, M Kettenis

  dependencies: files ocean.dat tilt.dat and DE405_le.jpl should be in $HOME/bin.


  TODO RHJO
  1) eop_ref_epoch (Julian Day) in double or long?

*/

#include <iostream>
#include <cstring>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <fstream>
#include <vector>
#include <time.h>
#include <math.h>
#include <unistd.h>

#include "generate_delay_model.h"
#include "vex/Vex++.h"
#include "correlator_time.h"

#define PI (3.14159265358979323846) //copied from prep_job/calc_model.h
#define SPEED_OF_LIGHT (299792458.0)
#define SECS_PER_DAY (86400)
#define IPS_FEET (12)

extern "C" void generate_delay_tables(FILE *output, char *stationname,
		    double start, double stop);

// Time between sample points
const double delta_time = 1; // in seconds
// The number of additional seconds before and after each scan should be computed
const int n_padding_seconds = 1;

// Data needed to generate the delay table
struct Station_data station_data;
int                 n_scans;
struct Scan_data    *scan_data;
int n_sources;
struct Source_data *source_data;

// Reads the data from the vex-file
int initialise_data(const char *vex_file,
                    const std::string &station_name);


double vex2time(std::string str);

void
usage(void)
{
  extern char *__progname;

  fprintf(stderr, "usage: %s: [-a] vexfile station outfile [start stop]\n", __progname);
  exit(EXIT_FAILURE);
}

int
main(int argc, char *argv[])
{
  int ch, append = 0;
  double start, stop;

  while ((ch = getopt(argc, argv, "a")) != -1) {
    switch(ch) {
    case 'a':
      append = 1;
      break;
    default:
      usage();
      break;
    }
  }

  argc -= optind;
  argv += optind;

  if (argc != 3 && argc != 5)
    usage();

  //Read the vex-file
  int err;
  err = initialise_data(argv[0], argv[1]);
  if (err != 0) {
    std::cout << "Could not initialise the delay model" << std::endl;
    exit(1);
  }

  if (argc == 5) {
    start = vex2time(argv[3]) - n_padding_seconds;
    stop = vex2time(argv[4]) + n_padding_seconds;
  } else {
    start = scan_data[0].scan_start;
    stop = scan_data[n_scans - 1].scan_stop;
  }

  // Open the output file
  FILE *output_file = fopen(argv[2], append ? "a" : "w");
  if (output_file == NULL) {
    std::cout << "Error: Could not open delay file \"" << argv[2] << "\" for writing\n";
    exit(1);
  }

  // Change to the CALC-directory
  // Goto the location of calc-10 files ocean.dat, tilt.dat and DE405_le.jpl
  char *dir = getenv("CALC_DIR");
  if (dir != NULL) {
    int err = chdir(dir);
    if (err != 0) {
      printf("Error : Invalid CALC_DIR = %s\n", dir);
      exit(1);
    }
  } else {
    printf("Warning: CALC_DIR enviroment variable not set, will try to get ocean loading file from current working directory\n");
  }

  // call the c-function that calls the FORTRAN calc code
  generate_delay_tables(output_file, argv[1], start, stop);

  return EXIT_SUCCESS;
}

/*******************************************/
/**  Helping functions                    **/
/*******************************************/


long str_to_long (std::string inString, int pos, int length) {
  std::string str=inString.substr(pos,length);
  char tmp[length+1];
  strcpy(tmp,str.c_str());

  char *endp;
  long sval = strtol(tmp, &endp, 10);
  if (endp == tmp) {
    fprintf(stderr,"**** Unable to convert string %s into long\n",tmp);
    return -1;
  } else {
    return sval;
  }
}

double
vex2time(std::string str)
{
  int doy = str_to_long(str, 5, 3);
  int hour = str_to_long(str, 9, 2);
  int min = str_to_long(str, 12, 2);
  int sec = str_to_long(str, 15, 2);

  return sec + 60 * (min + 60 * (hour + 24 * (double)doy));
}

bool leap_year(int year) {
  return ( (year%4==0) && ((year%100!=0) || (year%400==0)) ? 1 : 0);
}

//input  year, day of year
//output month, day of month
void yd2md(int year, int doy, int &month, int &dom) {

  const int monthdays[] = {
                            31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
                          };
  int rest_of_days=doy;
  int current_month=1;
  int length_current_month = monthdays[current_month-1];
  while (rest_of_days > length_current_month) {
    rest_of_days -= length_current_month;
    current_month++;
    length_current_month =
      (monthdays[current_month-1] +
       (current_month==2 && leap_year(year) ? 1 : 0));
  }
  dom = rest_of_days;
  month = current_month;
}

//input year month day
//output Julian Day
long long JD(int y, int m, int d) {
  return ( 1461 * ( y + 4800 + ( m - 14 ) / 12 ) ) / 4 +
         ( 367 * ( m - 2 - 12 * ( ( m - 14 ) / 12 ) ) ) / 12 -
         ( 3 * ( ( y + 4900 + ( m - 14 ) / 12 ) / 100 ) ) / 4 +
         d - 32075;
}

//returns clock epoch in seconds for timeString
//assumption: |year-ref_year|<=1
long ceps(std::string timeString, int ref_year) {
  long clock_epoch=0;
  int year = str_to_long(timeString,0,4);  //pos=0, length=4
  int doy = str_to_long(timeString,5,3);
  int hr = str_to_long(timeString,9,2);
  int min = str_to_long(timeString,12,2);
  int sec = str_to_long(timeString,15,2);
  assert(abs(year-ref_year)<=1);
  if (ref_year == year) {
    clock_epoch = sec + 60*min + 3600*hr + 86400*doy;
  } else {
    int days_per_year = (leap_year(ref_year) ? 366 : 365);
    clock_epoch = sec + 60*min + 3600*hr + 86400*doy +
                  (year-ref_year)*days_per_year * 86400;
  }
  return clock_epoch;
}

void
check_site(Vex::Node& root, const std::string& site) {
  // Check if all required information is there.
  const char *params[] = { "site_name", "site_position", NULL };
  for (int i = 0; params[i]; i++) {
    if (root["SITE"][site][params[i]] == root["SITE"][site]->end()) {
      std::cerr << "Parameter " << params[i] << " missing for site " << site
		<< std::endl;
      exit(EXIT_FAILURE);
    }
  }
}

void
check_antenna(Vex::Node& root, const std::string& antenna) {
  // Check if all required information is there.
  const char *params[] = { "axis_type", "axis_offset", NULL };
  for (int i = 0; params[i]; i++) {
    if (root["ANTENNA"][antenna][params[i]] == root["ANTENNA"][antenna]->end()) {
      std::cerr << "Parameter " << params[i] << " missing for antenna " << antenna
		<< std::endl;
      exit(EXIT_FAILURE);
    }
  }
}

void
check_source(Vex::Node& root, const std::string& source) {
  // Check if all required information is there.
  const char *params[] = { "ra", "dec", "ref_coord_frame", NULL };
  for (int i = 0; params[i]; i++) {
    if (root["SOURCE"][source][params[i]] == root["SOURCE"][source]->end()) {
      std::cerr << "Parameter " << params[i] << " missing for source " << source
		<< std::endl;
      exit(EXIT_FAILURE);
    }
  }

  // We only support J2000 for now.
  if (root["SOURCE"][source]["ref_coord_frame"]->to_string() != "J2000") {
    std::cerr << "Unsupported reference frame "
	      << root["SOURCE"][source]["ref_coord_frame"]->to_string()
	      << " for source " << source << std::endl;
    exit(EXIT_FAILURE);
  }
}

void
check_eop(Vex::Node& root, const std::string& eop) {
  // Check if all required information is there.
  const char *params[] = { "eop_ref_epoch", "TAI-UTC", "ut1-utc", "x_wobble", "y_wobble", NULL };
  for (int i = 0; params[i]; i++) {
    if (root["EOP"][eop][params[i]] == root["EOP"][eop]->end()) {
      std::cerr << "Parameter " << params[i] << " missing for eop " << eop
		<< std::endl;
      exit(EXIT_FAILURE);
    }
  }
}

int initialise_data(const char *vex_filename,
                    const std::string &station_name) {
  Vex vex(vex_filename);

  Vex::Node root = vex.get_root_node();

  if (root["STATION"][station_name] == root["STATION"]->end()) {
    std::cerr << "station " << station_name << " not found" << std::endl;
    exit(EXIT_FAILURE);
  }

  std::string site = root["STATION"][station_name]["SITE"]->to_string();
  check_site(root, site);
  std::string site_name = root["SITE"][site]["site_name"]->to_string();

  strcpy(station_data.site_name, site_name.c_str());
  for (int i=site_name.size(); i<8; i++) {
    station_data.site_name[i] = ' ';
  }

  station_data.site_name[8]='\0';

  int i = 0;
  Vex::Node::const_iterator position =
    vex.get_root_node()["SITE"][site]["site_position"];
  for (Vex::Node::const_iterator site = position->begin();
       site != position->end(); ++site) {
    double pos;
    int err = sscanf(site->to_string().c_str(), "%lf m", &pos);
    assert(err == 1);
    station_data.site_position[i] = pos;
    i++;
  }

  Vex::Node::const_iterator velocity =
    vex.get_root_node()["SITE"][site]["site_velocity"];
  Vex::Node::const_iterator epoch =
    vex.get_root_node()["SITE"][site]["site_position_epoch"];
  if (velocity != vex.get_root_node()["SITE"][site]->end() &&
      epoch == vex.get_root_node()["SITE"][site]->end()) {
    // Don't insist on having a site_position_epoch if the velocity vector is 0.
    if (velocity[0]->to_double() != 0.0 || velocity[1]->to_double() != 0.0 ||
	velocity[2]->to_double() != 0.0) {
      std::cerr << "missing site_position_epoch" << std::endl;
      exit(EXIT_FAILURE);
    }
  }

  if (velocity != vex.get_root_node()["SITE"][site]->end() &&
      epoch != vex.get_root_node()["SITE"][site]->end()) {
    double epoch_mjd = epoch->to_double();
    if (epoch_mjd < 50000) {
      struct tm tm;
      ::strptime(epoch->to_date().to_string().c_str(), "%Yy%jd%Hh%Mm%Ss", &tm);
      epoch_mjd = 40587 + ::timegm(&tm) / 86400;
    }
    Vex::Node::const_iterator start = vex.get_root_node()["SCHED"]->begin()["start"];
    struct tm tm;
    ::strptime(start->to_string().c_str(), "%Yy%jd%Hh%Mm%Ss", &tm);
    double mjd = 40587 + ::timegm(&tm) / 86400;
    double years = ((mjd - epoch_mjd) / 364.25);

    station_data.site_position[0] += velocity[0]->to_double_amount("m/yr") * years;
    station_data.site_position[1] += velocity[1]->to_double_amount("m/yr") * years;
    station_data.site_position[2] += velocity[2]->to_double_amount("m/yr") * years;
  }

  std::string antenna = root["STATION"][station_name]["ANTENNA"]->to_string();
  check_antenna(root, antenna);

  if (vex.get_root_node()["ANTENNA"][antenna]["axis_type"][0]->to_string()=="ha")
    station_data.axis_type=1;
  if (vex.get_root_node()["ANTENNA"][antenna]["axis_type"][0]->to_string()=="az")
    station_data.axis_type=3;
  if (vex.get_root_node()["ANTENNA"][antenna]["axis_type"][0]->to_string()=="x" &&
      vex.get_root_node()["ANTENNA"][antenna]["axis_type"][1]->to_string()=="yew")
    station_data.axis_type=4;

  station_data.axis_offset =
    vex.get_root_node()["ANTENNA"][antenna]["axis_offset"]->to_double_amount("m");

  { // EOP information
    int i = station_data.num_eop_points = 0;
    for (Vex::Node::const_iterator eop = vex.get_root_node()["EOP"]->begin();
         eop != vex.get_root_node()["EOP"]->end(); ++eop) {
      std::string str = eop["eop_ref_epoch"]->to_string();
      int year = 0, doy = 0, hour = 0, n;
      n = sscanf(str.c_str(), "%dy%dd%dh", &year, &doy, &hour);
      assert(n >= 2);
      int month, day;
      yd2md(year, doy, month, day);
      double eop_ref_epoch = JD(year,month,day) + (hour - 12.) / 24; // Julian day

      if (station_data.num_eop_points == 0) {
	station_data.tai_utc = eop["TAI-UTC"]->to_double_amount("sec");
	station_data.eop_ref_epoch = eop_ref_epoch;
      } else {
	if (station_data.tai_utc != eop["TAI-UTC"]->to_double_amount("sec")) {
	  std::cerr << "observing over leap seconds is not supported" << std::endl;
	  exit(EXIT_FAILURE);
	}
	if (station_data.eop_ref_epoch + station_data.num_eop_points != eop_ref_epoch) {
	  std::cerr << "incorrect interval for EOP points" << std::endl;
	  exit(EXIT_FAILURE);
	}
      }
      int num_eop_points = eop["num_eop_points"]->to_int();
      station_data.num_eop_points += num_eop_points;
      assert(station_data.num_eop_points <= 10);
      if (num_eop_points > 1) {
	for (int j = 0; j < num_eop_points; j++) {
	  station_data.ut1_utc[i] = eop["ut1-utc"][j]->to_double_amount("sec");
	  station_data.x_wobble[i] = eop["x_wobble"][j]->to_double_amount("asec");
	  station_data.y_wobble[i] = eop["y_wobble"][j]->to_double_amount("asec");
	  i++;
	}
      } else {
	station_data.ut1_utc[i] = eop["ut1-utc"]->to_double_amount("sec");
	station_data.x_wobble[i] = eop["x_wobble"]->to_double_amount("asec");
	station_data.y_wobble[i] = eop["y_wobble"]->to_double_amount("asec");
	i++;
      }
    }

    if (station_data.num_eop_points < 3) {
      std::cerr << "a minimum of 3 EOP points are required (only "
		<< station_data.num_eop_points
		<< " specified)" << std::endl;
      exit(EXIT_FAILURE);
    }
  }

  // Source related data
  n_sources = 0;
  for (Vex::Node::const_iterator source = vex.get_root_node()["SOURCE"]->begin();
       source != vex.get_root_node()["SOURCE"]->end(); ++source) {
    n_sources++;
  }
  source_data = new struct Source_data[n_sources];
  int source_idx = 0;
  for (Vex::Node::const_iterator source_block = vex.get_root_node()["SOURCE"]->begin();
        source_block != vex.get_root_node()["SOURCE"]->end(); ++source_block) {
    struct Source_data &source = source_data[source_idx];
    check_source(root, source_block.key());

    strncpy(source.source_name, source_block.key().c_str(), 80);
    for (int i = strlen(source_block.key().c_str()); i < 80; i++) {
      source.source_name[i] = ' ';
    }
    source.source_name[80]='\0';

    int   hours, minutes, degs;
    double seconds;
    sscanf(source_block["ra"]->to_string().c_str(), "%dh%dm%lfs", &hours, &minutes, &seconds );
    source.ra = ((hours * 3600 + 60 * minutes + seconds ) * 2 * PI) / SECS_PER_DAY;
    sscanf(source_block["dec"]->to_string().c_str(), "%dd%d\'%lf\"", &degs, &minutes, &seconds );
    if (strchr(source_block["dec"]->to_string().c_str(), '-'))
      source.dec = -1*(PI/180)*(abs(degs)+minutes/60.0+seconds/3600);
    else
      source.dec = (PI/180)*(abs(degs)+minutes/60.0+seconds/3600);

    source_idx++;
  }

  // Scan related data
  n_scans = 0;
  for (Vex::Node::const_iterator scan = vex.get_root_node()["SCHED"]->begin();
       scan != vex.get_root_node()["SCHED"]->end(); ++scan) {
    for (Vex::Node::const_iterator scan_it = scan->begin("station");
         scan_it != scan->end("station"); ++scan_it) {
      if (scan_it[0]->to_string() == station_name) {
        n_scans++;
      }
    }
  }

  Time startTime;
  scan_data = new struct Scan_data[n_scans];
  int scan_nr=0;
  for (Vex::Node::const_iterator scan_block = vex.get_root_node()["SCHED"]->begin();
       scan_block != vex.get_root_node()["SCHED"]->end(); ++scan_block) {
    // First get the duration of the scan
    double duration = 0;
    for (Vex::Node::const_iterator scan_it = scan_block->begin("station");
             scan_it != scan_block->end("station"); ++scan_it) {
      double station_duration = scan_it[2]->to_double_amount("sec");      
      if (station_duration > duration)
        duration = station_duration;
    }
    // account for extra data points before and after scan
    duration += 2 * n_padding_seconds; 
    for (Vex::Node::const_iterator scan_it = scan_block->begin("station");
         scan_it != scan_block->end("station"); ++scan_it) {
      if (scan_it[0]->to_string() == station_name) {
        assert(scan_nr < n_scans);
        struct Scan_data &scan = scan_data[scan_nr];

	memset(scan.scan_name, 0, sizeof(scan.scan_name));
	strncpy(scan.scan_name, scan_block.key().c_str(), 80);
        startTime = scan_block["start"]->to_string();
        startTime -= 1000000. * n_padding_seconds;
        int doy;
        double sec;
        startTime.get_date(scan.year, doy);
        // convert day of year to (month,day)
        yd2md(scan.year,doy,scan.month,scan.day);
        startTime.get_time(scan.hour, scan.min, sec);
        scan.sec = (int)round(sec);
        // delay table for sfxc needs this one
        scan.sec_of_day = scan.hour*3600. + scan.min*60. + scan.sec;
        scan.scan_start =
          scan.sec + 60*(scan.min + 60*(scan.hour + 24*(double)doy));
        scan.scan_stop = scan.scan_start + duration;
        scan.nr_of_intervals = (int)(duration/delta_time);
        int n_sources_in_scan = vex.n_sources(scan_block.key());
        typedef struct Source_data *Source_data_ptr;
        scan.sources = new Source_data_ptr[n_sources_in_scan];
        scan.n_sources = n_sources_in_scan;

        int source_idx = 0;
        for (Vex::Node::const_iterator source_it = scan_block->begin("source");
             source_it != scan_block->end("source"); ++source_it) {
          int i;
          const char *source = source_it->to_c_string();
          if (root["SOURCE"][source] == root["SOURCE"]->end()) {
	    std::cerr << "source " << source << " not found" << std::endl;
            exit(EXIT_FAILURE);
          }
          const int source_len = strlen(source);
          for(i = 0; i < n_sources ; i++){
            int pos;
            for(pos = 79; (pos > 0) && (source_data[i].source_name[pos] == ' '); pos--)
              ;
            if((pos + 1 == source_len) && 
               (strncmp(source_data[i].source_name, source, pos + 1) == 0)){
              assert(source_idx < n_sources_in_scan);
              scan.sources[source_idx] = &source_data[i];
              source_idx++;
              break;
            }
          }
          assert(i != n_sources);
        }
        scan_nr++;
      }
    }
  }
  return 0;
}

int mjd(int day, int month, int year)
// Calculate the modified julian day, formula taken from the all knowing wikipedia
{
  int a = (14-month)/12;
  int y = year + 4800 - a;
  int m = month + 12*a - 3;
  int jdn = day + ((153*m+2)/5) + 365*y + (y/4) - (y/100) + (y/400) - 32045;
  return jdn - 2400000.5;
}

