#include "control_parameters.h"
#include "output_header.h"
#include "utils.h"

#include <fstream>
#include <set>
#include <cstring>
#include <cctype>
#include <math.h>

#include <libgen.h>

#include <json/json.h>
#include <algorithm>


Control_parameters::Control_parameters()
    : initialised(false),pulsar_parameters(std::cout) {}

Control_parameters::Control_parameters(const char *ctrl_file,
                                       const char *vex_file,
                                       std::ostream& log_writer)
    : initialised(false), pulsar_parameters(log_writer){
  if(!initialise(ctrl_file, vex_file, log_writer))
    sfxc_abort();
}

bool
Control_parameters::
initialise(const char *ctrl_file, const char *vex_file,
           std::ostream& log_writer) {
  ctrl_filename = ctrl_file;
  vex_filename = vex_file;

  { // parse the control file
    Json::Reader reader;
    std::ifstream in(ctrl_file);
    if (!in.is_open()) {
      log_writer << "Could not open control file [" << ctrl_file << "]" << std::endl;
      return false;
    }
    bool ok = reader.parse(in, ctrl);
    if ( !ok ) {
      // report to the user the failure and their locations in the document.
      log_writer  << "Failed to parse control file\n"
      << reader.getFormatedErrorMessages()
      << std::endl;
      return false;
    }
  }

  { // VEX file
    std::ifstream in(vex_file);
    if (!in.is_open()) {
      log_writer << "Could not open vex file [" <<vex_file<<"]"<< std::endl;
      return false;
    }

    // parse the vex file
    if (!vex.open(vex_file)) {
      log_writer << "Could not parse vex file ["<<vex_file<<"]" << std::endl;
      return false;
    }
  }

  // set to the default
  if (ctrl["delay_directory"] == Json::Value()) {
    ctrl["delay_directory"] = "file:///tmp/";
  }

  // set the subbands
  if (ctrl["channels"] == Json::Value()) {
    std::set<std::string> result_set;

    // insert all channels
    for (Vex::Node::const_iterator frq_block = vex.get_root_node()["FREQ"]->begin();
         frq_block != vex.get_root_node()["FREQ"]->end(); ++frq_block) {
      for (Vex::Node::const_iterator freq_it = frq_block->begin("chan_def");
           freq_it != frq_block->end("chan_def"); ++freq_it) {
        result_set.insert(freq_it[4]->to_string());
      }
    }
    for (std::set<std::string>::const_iterator set_it = result_set.begin();
         set_it != result_set.end(); ++set_it) {
      ctrl["channels"].append(*set_it);
    }
  }

  // Checking reference station
  if (ctrl["reference_station"] == Json::Value()) {
    ctrl["reference_station"] = "";
  }

  if (ctrl["pulsar_binning"] == Json::Value()){
    ctrl["pulsar_binning"] = false;
  }else if(ctrl["pulsar_binning"].asBool()==true){
    // use pulsar binning
    DEBUG_MSG("Using pulsar binning");

    if (ctrl["pulsars"] == Json::Value()){
      log_writer << "Error : No pulsars block in control file.\n";
      return false;
    }
    Json::Value::iterator it = ctrl["pulsars"].begin();
    if(*it==Json::Value()){
      log_writer <<  "Error : Empty pulsars block in control file.\n";
      return false;
    }
    while(it!=ctrl["pulsars"].end()){
      if((*it)["interval"] == Json::Value()){
        (*it)["interval"].append(0.0); 
        (*it)["interval"].append(1.0);
      }
      if((*it)["nbins"]==Json::Value()){
        // If nbins is not set we default to the maxium possible (PULSAR_PERIOD/DURATION_SINGLE_FFT)
        // signalled by nbins = 0 
        (*it)["nbins"] = 0;
      }
      it++;
    }
  }
  if (ctrl["phased_array"] == Json::Value())
    ctrl["phased_array"] = false;

  // For backward compatibility
  if(ctrl["only_autocorrelations"] != Json::Value()) {
    if (ctrl["phased_array"].asBool() && ctrl["only_autocorrelations"].asBool()) {
      ctrl["phased_array"] = false;
      ctrl["filterbank"] = true;
    }
  }
  if (ctrl["filterbank"] == Json::Value())
    ctrl["filterbank"] = false;

  if (ctrl["multi_phase_center"] == Json::Value()){
    ctrl["multi_phase_center"] = false;
    if(!ctrl["pulsar_binning"].asBool() && !ctrl["filterbank"].asBool()){
      Vex::Node::const_iterator it = vex.get_root_node()["SCHED"]->begin();
      while(it != vex.get_root_node()["SCHED"]->end()){
        int n_sources = 0;
        Vex::Node::const_iterator sources = it->begin("source");
        while(sources != it->end("source")){
          n_sources++;
          sources++;
        }

        if(n_sources > 1){
          ctrl["multi_phase_center"] = true;
          break;
        }
        it++;
      }
    }
  } else if ((ctrl["multi_phase_center"].asBool() == true) && 
             (ctrl["pulsar_binning"].asBool() == true)){
    std::cout << "Pulsar binning cannot be used together with multiple phase centers\n";
    return false;
  }
  // No phased array and filterbank modes at the same time 
  if (ctrl["phased_array"].asBool() && ctrl["filterbank"].asBool()) {
    std::cout << "Cannot enable phased array and filterbank modes simultaneously\n";
    return false;
  }
  // No phased array in pulsar binning mode
  if ((ctrl["phased_array"].asBool() || ctrl["filterbank"].asBool()) &&
      ctrl["pulsar_binning"].asBool()) {
    std::cout << "Pulsar binning cannot be used with phased array or filterbank modes\n";
    return false;
  }
  // Set default windowing function, if necessary
  if (ctrl["window_function"] == Json::Value()){
    if (ctrl["multi_phase_center"].asBool())
      ctrl["window_function"] = "NONE";
    else
      ctrl["window_function"] = "HANN";
  }
  // Set the fft sizes
  if (ctrl["fft_size_correlation"] == Json::Value()){
    int min_size = ctrl["multi_phase_center"].asBool() ? 4096 : 256;

    if (ctrl["fft_size_delaycor"] != Json::Value())
      min_size = std::max(min_size, ctrl["fft_size_delaycor"].asInt());

    ctrl["fft_size_correlation"] = std::max(min_size, number_channels());
  }
  if (ctrl["fft_size_delaycor"] == Json::Value())
    ctrl["fft_size_delaycor"] = std::min(256, ctrl["fft_size_correlation"].asInt());
  
  // Set the sub integartion time
  if(ctrl["sub_integr_time"] == Json::Value()){
    double integr_time_usec = round(integration_time().get_time_usec());
    if (ctrl["multi_phase_center"].asBool()){
      // Default to +/- 20 ms sub integrations
      ctrl["sub_integr_time"] = std::min(integr_time_usec, 20480.);
    }else{
      // Default to 125 ms sub integrations
      ctrl["sub_integr_time"] = std::min(integr_time_usec, 125000.);
    }
  }

  // Set PhaseCal integration time
  if(ctrl["phasecal_integr_time"] == Json::Value()) {
    if (ctrl["phasecal_file"].asBool())
      ctrl["phasecal_integr_time"] = 10;
    else
      ctrl["phasecal_integr_time"] = 0;
  }

  // By default we abort the correlation if one of the input streams
  // contains no data
  if(ctrl["exit_on_empty_datastream"] == Json::Value())
    ctrl["exit_on_empty_datastream"] = true;

  if (ctrl["start"].asString().compare("now") == 0) {
    char *now;
    time_t t;
    struct tm tm;
    time(&t);
    gmtime_r(&t, &tm);
    int a = asprintf(&now, "%dy%dd%dh%dm%ds", tm.tm_year + 1900, tm.tm_yday + 1,
	              tm.tm_hour, tm.tm_min, tm.tm_sec);
    ctrl["start"] = now;
  }

  // Read pulsar parameters
  bool result = read_pulsar_parameters();
  if (ctrl["pulsar_binning"].asBool() && (pulsar_parameters.pulsars.size() == 0)){
    log_writer << "Error : No pulsar parameters specified in pulsar binning mode" << std::endl;
    return false;
  }else if (!result){
    log_writer << "Error parsing pulsar parameters" << std::endl;
    return false;
  }
  if (ctrl["stop"].asString().compare("end") == 0)
    ctrl["stop"] = vex.get_stop_time_of_experiment();

  // Get start date
  start_time = Time(vex.get_start_time_of_experiment());
  initialised = true;

  return true;
}

int
Control_parameters::reference_station_number() const {
  if (ctrl["reference_station"] == Json::Value())
    return -1;
  std::string reference_station = ctrl["reference_station"].asString();
  if (reference_station == "")
    return -1;

  for (size_t station_nr = 0;
       station_nr < ctrl["stations"].size(); ++station_nr) {
    if (ctrl["stations"][station_nr].asString() == reference_station) {
      return station_nr;
    }
  }
  std::cout << "Reference station not found" << std::endl;
  return -1;
}

bool
Control_parameters::check(std::ostream &writer) const {
  typedef Json::Value::const_iterator                    Value_it;
  bool ok = true;

  // check start and stop time
  if (ctrl["start"] == Json::Value()) {
    ok = false;
    writer << "Ctrl-file: start time not defined" << std::endl;
  } else {
    if (ctrl["stop"] == Json::Value()) {
      ok = false;
      writer << "Ctrl-file: stop time not defined" << std::endl;
    } else {
      Time start(ctrl["start"].asString());
      Time stop(ctrl["stop"].asString());
      if (stop <= start) {
        ok = false;
        writer << "Ctrl-file: stop time before start time" << std::endl;
      }
    }
  }

  { // Check integration time
    if (ctrl["integr_time"] == Json::Value()){
      ok = false;
      writer << "Ctrl-file: Integration time not set" << std::endl;
    } else {
      Time integr_time(ctrl["integr_time"].asDouble()*1000000);
      if (integr_time < Time(0)) {
        ok = false;
        writer << "Ctrl-file: Integration time is negative" << std::endl;
      }

      // Check sub integration time
      if (ctrl["sub_integr_time"] != Json::Value()){
        Time sub_integr_time(ctrl["sub_integr_time"].asDouble());
        if (sub_integr_time < Time(0)) {
          ok = false;
          writer << "Ctrl-file: Sub integration time is negative" << std::endl;
        } else if (integr_time < sub_integr_time){
          ok = false;
          writer << "Ctrl-file: Sub integration time is larger than the integration time" << std::endl;
        }
      }
    }
  }

  { // Check PhaseCal
    if (ctrl["phasecal_integr_time"].asInt() != 0 &&
	ctrl["phasecal_file"] == Json::Value()) {
      ok = false;
      writer << "Ctrl-file: PhaseCal output file not defined" << std::endl;
    } else {
      Time phasecal_integr_time(ctrl["phasecal_integr_time"].asInt() * 1000000);
      if (phasecal_integr_time < Time(0)) {
	ok = false;
	writer << "Ctrl-file: Phasecal integration time is negative" << std::endl;
      }
    }
  }

  { // Check FFT
    int fft = 0;
    if (ctrl["fft_size_delaycor"] != Json::Value()){
      if(!isPower2(ctrl["fft_size_delaycor"].asInt())){
        ok = false;
        writer << "Ctrl-file: fft_size_delaycor is not a power of two" << std::endl;
      }
      fft += 1;
    }
    if (ctrl["fft_size_correlation"] != Json::Value()){
      if(!isPower2(ctrl["fft_size_correlation"].asInt())){
        ok = false;
        writer << "Ctrl-file: fft_size_correlation is not a power of two" << std::endl;
      }
      if (ctrl["fft_size_correlation"].asInt() < ctrl["number_channels"].asInt()){
        ok = false;
        writer << "Ctrl-file: fft_size_correlation cannot be smaller than the number of channels\n";
      }
      fft += 1;
    }
    if(fft == 2){
      if(ctrl["fft_size_correlation"].asInt() < ctrl["fft_size_delaycor"].asInt()){
        ok = false;
        writer << "Ctrl-file: fft_size_correlation should not be smaller than fft_size_delaycor." << std::endl;
      }
    }
  }

  { // Check stations and reference station
    if (ctrl["stations"] != Json::Value()) {
      for (size_t station_nr = 0;
           station_nr < ctrl["stations"].size(); ++station_nr) {
        std::string station_name = ctrl["stations"][station_nr].asString();
        if (ctrl["data_sources"][station_name] == Json::Value()) {
          ok = false;
          writer << "Ctrl-file: No data source defined for "
          << station_name << std::endl;
        } else if (ctrl["data_sources"][station_name].size()==0) {
          ok = false;
          writer << "Ctrl-file: Empty list of data sources for "
          << ctrl["data_sources"][station_name]
          << std::endl;
        } else {
          const Json::Value data_source_it =
            ctrl["data_sources"][station_name];
          for (Json::Value::const_iterator source_it =
                 data_source_it.begin();
               source_it != data_source_it.end(); ++source_it) {
            std::string filename = create_path((*source_it).asString());

            if (filename.find("file://")  != 0 &&
                filename.find("mk5://") != 0) {
              ok = false;
              writer
              << "Ctrl-file: invalid data source '" << filename << "'"
              << std::endl;
            }
          }
        }
      }
    } else {
      ok = false;
      writer << "Ctrl-file: Stations not found" << std::endl;
    }

    if (ctrl["reference_station"] != Json::Value()) {
      if (ctrl["reference_station"].asString() != "") {
        if (reference_station_number() == -1) {
          ok = false;
          writer
          << "Ctrl-file: Reference station not one of the input stations"
          << std::endl;
        }
      }
    } else {
      ok = false;
      writer << "Ctrl-file: Reference station not found" << std::endl;
    }
  }

  { // Check output file
    if (ctrl["output_file"] != Json::Value()) {
      std::string output_file = create_path(ctrl["output_file"].asString());
      if (strncmp(output_file.c_str(), "file://", 7) != 0) {
        ok = false;
        writer
        << "Ctrl-file: Correlation output should start with 'file://'"
        << std::endl;
      }
    } else {
      ok = false;
      writer << "ctrl-file: output file not defined" << std::endl;
    }
  }

  // Check phasecal file
  if (ctrl["phasecal_file"] != Json::Value()) {
    std::string filename = create_path(ctrl["phasecal_file"].asString());
    if (strncmp(filename.c_str(), "file://", 7) != 0) {
      ok = false;
      writer << "Ctrl-file: Phasecal output should start with 'file://'"
	     << std::endl;
    }
  }

  // Check mask parameters
  if (ctrl["mask"] != Json::Value()) {
    if (ctrl["mask"]["mask"] != Json::Value()) {
      std::string filename = create_path(ctrl["mask"]["mask"].asString());
      if (strncmp(filename.c_str(), "file://", 7) != 0) {
        ok = false;
        writer << "Ctrl-file: Mask file should start with 'file://'"
	       << std::endl;
      }
    }
    if (ctrl["mask"]["window"] != Json::Value()) {
      std::string filename = create_path(ctrl["mask"]["window"].asString());
      if (strncmp(filename.c_str(), "file://", 7) != 0) {
        ok = false;
        writer << "Ctrl-file: Window file should start with 'file://'"
	       << std::endl;
      }
    }
  }

  // Check window function
  if (ctrl["window_function"] != Json::Value()){
    std::string window = ctrl["window_function"].asString();
    for(int i = 0; i < window.size(); i++)
      window[i] = toupper(window[i]);
    if ((window != "RECTANGULAR") and (window != "COSINE") and (window != "HAMMING") and 
      (window != "HANN") and (window != "NONE")){
      writer << "Invalid window function " << window 
             << ", valid choises are : RECTANGULAR, COSINE, HAMMING, HANN, and NONE" << std::endl;
      ok = false;
    }
  }
  // If phased array mode is activated, check for calibration tables
  if (ctrl["phased_array"].asBool()) { 
    if(ctrl["cl_table"] == Json::Value()){
      writer << "Phased array mode requires \"cl_table\" to be specified" << std::endl;
      ok = false;
    }
    if(ctrl["bp_table"] == Json::Value()){
      writer << "Phased array mode requires \"bp_table\" to be specified" << std::endl;
      ok = false;
    }
  }
  if (ctrl["multi_phase_center"].asBool() && ctrl["filterbank"].asBool()){
    writer << "Multiple phase centers cannot be set when \"filterbank\" mode is enabled.\n";
    ok = false;
  }
  // Check pulsar binning
  if (ctrl["pulsar_binning"].asBool()){
    // use pulsar binning
    if (ctrl["pulsars"] == Json::Value()){
      ok=false;
      writer << "ctrl-file : No pulsars block in control file.\n";
    }else{
      Json::Value::const_iterator it = ctrl["pulsars"].begin();
      if(*it==Json::Value()){
        ok = false;
        writer << "ctrl-file : Empty pulsars block in control file.\n";
      }else{
        while(it!=ctrl["pulsars"].end()){
          if((*it)["interval"].size() != 2){
            ok = false;
            writer << "ctrl-file : Invalid number of arguments in interval field.\n";
          }else{
            Json::Value interval = (*it)["interval"];
            unsigned int zero=0,one=1; // needed to prevent compilation error
            if ((interval[zero].asDouble()<0.)||(interval[zero].asDouble()>1)||
                (interval[one].asDouble()<0.)||(interval[one].asDouble()>=2)||
                (interval[one].asDouble() - interval[zero].asDouble() <= 0) ||
                (interval[one].asDouble() - interval[zero].asDouble() > 1)){
              ok = false;
              writer << "ctrl-file : Invalid range in interval field.\n";
            }
          }
          if((*it)["nbins"].asInt() < 0){
            ok= false;
            writer << "ctrl-file : Invalid number of bins : " << (*it)["nbins"].asInt()<<".\n";
          }
          if((*it)["polyco_file"] == Json::Value()){
            ok = false;
            writer << "ctrl-file : No polyco files specified.\n";
          }else if((*it)["polyco_file"].size() > 1 ){
            ok = false;
            writer << "ctrl-file : More than one polyco file specified for a pulsar.\n";
          } else {
            std::string filename = create_path((*it)["polyco_file"].asString());
            if (filename.find("file://") != 0){
              ok = false;
              writer << "Ctrl-file: polyco file definition doesn't start with file://  '" << filename << "'\n";
            }else{
              // Check whether the file exists
              std::ifstream in(create_path(filename).c_str()+7);
              if (!in.is_open()) {
               ok = false;
               writer << "Ctrl-file: Could not open polyco file : " << filename << std::endl;
              }else{
                writer << "Parsing polyco file : " << filename << "\n";
                Pulsar_parameters pc(writer);
                std::vector<Pulsar_parameters::Polyco_params> param;
                if (!pc.parse_polyco(param, filename.substr(7))){
                  ok = false;
                  writer << "Ctrl-file: Error parsing polyco file : " << filename << std::endl;
                }
              }
            }
          }
          it++;
        }
      }
    }
  }
  return ok;
}

Time
Control_parameters::get_start_time() const {
  return Time(ctrl["start"].asString());
}

Time
Control_parameters::get_stop_time() const {
  return Time(ctrl["stop"].asString());
}
void 
Control_parameters::set_reader_offset(const std::string &station, const Time t){
  reader_offsets[station] = t;
}

std::vector<std::string>
Control_parameters::data_sources(const std::string &station) const {
  std::vector<std::string> result;
  Json::Value data_sources = ctrl["data_sources"][station];
  SFXC_ASSERT(data_sources != Json::Value());
  for (size_t index = 0;
       index < ctrl["data_sources"][station].size(); ++index ) {
    result.push_back(create_path(ctrl["data_sources"][station][index].asString()));
  }
  return result;
}

std::string
Control_parameters::get_output_file() const {
  return create_path(ctrl["output_file"].asString());
}

std::string
Control_parameters::get_phasecal_file() const {
  return create_path(ctrl["phasecal_file"].asString());
}

std::string
Control_parameters::get_tsys_file() const {
  return create_path(ctrl["tsys_file"].asString());
}

std::string
Control_parameters::station(int i) const {
  return ctrl["stations"][i].asString();
}

size_t
Control_parameters::number_stations() const {
  return ctrl["stations"].size();
}

Time
Control_parameters::integration_time() const {
  return Time(round(ctrl["integr_time"].asDouble()*1000000));
}

Time
Control_parameters::sub_integration_time() const {
    return Time(ctrl["sub_integr_time"].asDouble());
}

Time
Control_parameters::phasecal_integration_time() const {
  return Time(ctrl["phasecal_integr_time"].asInt() * 1000000);
}

bool
Control_parameters::exit_on_empty_datastream() const{
  return ctrl["exit_on_empty_datastream"].asBool();
}

int
Control_parameters::number_channels() const {
  return ctrl["number_channels"].asInt();
}

int
Control_parameters::fft_size_delaycor() const {
  return ctrl["fft_size_delaycor"].asInt();
}

double
Control_parameters::dispersive_delay(double freq_low, double freq_high, double DM) const{
  // Compute the dispersive delay in microseconds for a given channel
  // The frequencies are in MHz.
  return 4149377593.3609958 * DM * (1. / (freq_low*freq_low) - 1. / (freq_high*freq_high));
}

int
Control_parameters::fft_size_dedispersion(const std::string &scan_name) const {
  get_dedispersion_parameters(scan_name);
  return dedispersion_parameters.fft_size_dedispersion;
}

int
Control_parameters::fft_size_correlation() const {
  return ctrl["fft_size_correlation"].asInt();
}

std::string
Control_parameters::cl_table() const {
  if(ctrl["cl_table"] == Json::Value())
    return std::string();
  std::string cl_table = ctrl["cl_table"].asString();
  if (cl_table.substr(0,7) == "file://") 
    return cl_table.substr(7);
  return cl_table;
}

std::string
Control_parameters::bp_table() const {
  if(ctrl["bp_table"] == Json::Value())
    return std::string();
  std::string bp_table = ctrl["bp_table"].asString();
  if (bp_table.substr(0,7) == "file://") 
    return bp_table.substr(7);
  return bp_table;
}

double
Control_parameters::LO_offset(const std::string &station) const {
  if (ctrl["LO_offset"] == Json::Value())
    return 0;
  if (ctrl["LO_offset"][station] == Json::Value())
    return 0;

  return ctrl["LO_offset"][station].asDouble();
}


int
Control_parameters::window_function() const{
  int windowval = SFXC_WINDOW_NONE;
  if (ctrl["window_function"] != Json::Value()){
    std::string window = ctrl["window_function"].asString();
    for(int i = 0; i < window.size(); i++)
      window[i] = toupper(window[i]);
    if(window == "RECTANGULAR")
      windowval = SFXC_WINDOW_RECT;
    else if(window == "COSINE")
      windowval = SFXC_WINDOW_COS;
    else if(window == "HAMMING")
      windowval = SFXC_WINDOW_HAMMING;
    else if(window == "HANN")
      windowval = SFXC_WINDOW_HANN;
    else if (window == "NONE")
      windowval = SFXC_WINDOW_NONE;
  }
  return windowval;
}

int
Control_parameters::job_nr() const {
  if (ctrl["job"] == Json::Value())
    return 0;
  else
    return ctrl["job"].asInt();
}

int
Control_parameters::subjob_nr() const {
  if (ctrl["subjob"] == Json::Value())
    return 0;
  else
    return ctrl["subjob"].asInt();
}

std::string
Control_parameters::sideband(int i) const {
  return ctrl["subbands"][i]["sideband"].asString();
}

std::string
Control_parameters::reference_station() const {
  return ctrl["reference_station"].asString();
}

std::string
Control_parameters::setup_station() const {
  if (ctrl["setup_station"] == Json::Value())
    return station(0);
  else
    return ctrl["setup_station"].asString();
}

std::string
Control_parameters::channel(int i) const {
  return ctrl["channels"][i].asString();
}

int Control_parameters::message_level() const {
  return ctrl["message_level"].asInt();
}

bool Control_parameters::phased_array() const{
  return ctrl["phased_array"].asBool();
}

bool Control_parameters::pulsar_binning() const{
  return ctrl["pulsar_binning"].asBool();
}

bool Control_parameters::filterbank() const{
  return ctrl["filterbank"].asBool();
}

bool Control_parameters::multi_phase_center() const{
  return ctrl["multi_phase_center"].asBool();
}

bool
Control_parameters::read_pulsar_parameters(){
  if (ctrl["pulsars"] == Json::Value()){
    std::cout << "No pulsars found \n";
    return true;
  }
  for(Json::Value::iterator it = ctrl["pulsars"].begin();
      it!=ctrl["pulsars"].end(); it++){
    std::string name= it.key().asString();
    std::cout << "Found pulsar " << name << "\n";
    Pulsar_parameters::Pulsar newPulsar;
    if(name.size() > 10)
      name.resize(10);
    strcpy(&newPulsar.name[0], name.c_str());
    newPulsar.nbins = (*it)["nbins"].asInt();
    // If coherent dedispersion is not requested, default to incoherent dedispersion
    if ((*it)["coherent_dedispersion"] == Json::Value())
      newPulsar.coherent_dedispersion = false;
    else
      newPulsar.coherent_dedispersion = (*it)["coherent_dedispersion"].asBool();
    // Check if intra-channel de-dispersion is disabled
    if ((*it)["no_intra_channel_dedispersion"] == Json::Value())
      newPulsar.no_intra_channel_dedispersion = false;
    else
      newPulsar.no_intra_channel_dedispersion = (*it)["no_intra_channel_dedispersion"].asBool();
    unsigned int zero=0, one=1; //needed to prevent compiler error
    newPulsar.interval.start = (*it)["interval"][zero].asDouble();
    newPulsar.interval.stop  = (*it)["interval"][one].asDouble();
    if(!pulsar_parameters.parse_polyco(newPulsar.polyco_params,(*it)["polyco_file"].asString().substr(7))){
      std::cout << "BAD POLY!!!!\n";
      return false;
    }
    pulsar_parameters.pulsars.insert(std::pair<std::string,Pulsar_parameters::Pulsar>(name,newPulsar));
  }
  std::cout << "SUC6\n";
  return true;
}

bool
Control_parameters::get_mask_parameters(Mask_parameters &pars) const {
  if (ctrl["mask"] == Json::Value())
    return false;

  pars.normalize = ctrl["mask"]["normalize"].asBool();
  if (ctrl["mask"]["mask"] != Json::Value()) {
    std::string filename = create_path(ctrl["mask"]["mask"].asString());
    std::ifstream infile(filename.c_str() + 7);
    if (!infile) {
      std::cerr << "Could not open mask file " << filename << std::endl;
      sfxc_abort();
    }
    double d;
    while (infile >> d)
      pars.mask.push_back(d);
  }
  if (ctrl["mask"]["window"] != Json::Value()) {
    std::string filename = create_path(ctrl["mask"]["window"].asString());
    std::ifstream infile(filename.c_str() + 7);
    if (!infile) {
      std::cerr << "Could not open window file " << filename << std::endl;
      sfxc_abort();
    }
    double d;
    while (infile >> d)
      pars.window.push_back(d);
  }

  return true;
}

int
Control_parameters::bits_per_sample(const std::string &mode,
                                    const std::string &station) const
{
  if (data_format(station) == "VDIF") {
    const std::string threads_name = get_vex().get_section("THREADS", mode, station);
    Vex::Node::const_iterator thread = vex.get_root_node()["THREADS"][threads_name];
    for (Vex::Node::const_iterator thread_it = thread->begin("thread");
	 thread_it != thread->end("thread"); thread_it++) {
      return thread_it[5]->to_int();
    }
  }

  if (data_format(station) == "Mark5B") {
    const std::string &bitstreams_name = get_vex().get_bitstreams(mode, station);
    if (bitstreams_name != std::string()) {
      Vex::Node::const_iterator bitstream = vex.get_root_node()["BITSTREAMS"][bitstreams_name];
      for (Vex::Node::const_iterator fanout_def_it = bitstream->begin("stream_def");
	   fanout_def_it != bitstream->end("stream_def"); ++fanout_def_it) {
	if (fanout_def_it[1]->to_string() == "mag") {
	  return 2;
	}
      }

      return 1;
    }
  }

  // Fall back on the $TRACKS block for Mark5B recordings if there is
  // no $BITSTREAMS block.
  if (data_format(station) == "Mark4" || data_format(station) == "VLBA" ||
      data_format(station) == "Mark5B") {
    const std::string &track_name = get_vex().get_track(mode, station);
    Vex::Node::const_iterator track = vex.get_root_node()["TRACKS"][track_name];
    for (Vex::Node::const_iterator fanout_def_it = track->begin("fanout_def");
         fanout_def_it != track->end("fanout_def"); ++fanout_def_it) {
      if (fanout_def_it[2]->to_string() == "mag") {
        return 2;
      }
    }

    return 1;
  }

  sfxc_abort("Unable to determine bits/sample");
}

int
Control_parameters::sample_rate(const std::string &mode,
				const std::string &station) const
{
  const std::string &freq_name = get_vex().get_frequency(mode, station);
  Vex::Node::const_iterator freq = vex.get_root_node()["FREQ"][freq_name];

  return (int)(freq["sample_rate"]->to_double_amount("Ms/sec") * 1e6);
}

int
Control_parameters::bandwidth(const std::string &mode,
			      const std::string &station,
			      const std::string &channel) const
{
  const std::string &freq_name = get_vex().get_frequency(mode, station);
  Vex::Node::const_iterator freq = vex.get_root_node()["FREQ"][freq_name];

  for (Vex::Node::const_iterator chan = freq->begin("chan_def"); chan != freq->end("chan_def"); chan++) {
    if (chan[4]->to_string() == channel)
      return (int)chan[3]->to_double_amount("Hz");
  }

  SFXC_ASSERT(false);
}

int64_t
Control_parameters::channel_freq(const std::string &mode,
				 const std::string &station,
				 const std::string &channel) const
{
  const std::string &freq_name = get_vex().get_frequency(mode, station);
  Vex::Node::const_iterator freq = vex.get_root_node()["FREQ"][freq_name];

  for (Vex::Node::const_iterator chan = freq->begin("chan_def"); chan != freq->end("chan_def"); chan++) {
    if (chan[4]->to_string() == channel)
      return (int64_t)round(chan[1]->to_double_amount("Hz"));
  }

  SFXC_ASSERT(false);
}

std::string
Control_parameters::scan(int scan_nr) const {
  Vex::Node::const_iterator it = vex.get_root_node()["SCHED"]->begin();
  for (int curr=0; curr < scan_nr; ++curr) {
    ++it;
    SFXC_ASSERT(it != vex.get_root_node()["SCHED"]->end());
  }
  return it.key();
}

std::string
Control_parameters::scan_source(const std::string &scan) const {
  return vex.get_root_node()["SCHED"][scan]["source"]->to_string();
}

int Control_parameters::scan(const Time &time) const {
  Vex::Date date(time.date_string());

  int scannr = 0;
  Vex::Node::const_iterator it = vex.get_root_node()["SCHED"]->begin();
  while (it != vex.get_root_node()["SCHED"]->end()) {
    if ((vex.start_of_scan(it.key()) <= date) &&
        (date < vex.stop_of_scan(it.key()))) {
      return scannr;
    }
    scannr++;
    it++;
  }
  return -1;
}

size_t
Control_parameters::number_scans() const {
  return vex.get_root_node()["SCHED"]->size();
}

bool
Control_parameters::
station_in_scan(const std::string &scan, const std::string &station) const {
  for( Vex::Node::const_iterator it = vex.get_root_node()["SCHED"][scan]->begin("station");
       it != vex.get_root_node()["SCHED"][scan]->end("station"); it++){
    if(it[0]->to_string() == station)
      return true;
  }
  return false;
}

size_t
Control_parameters::number_stations_in_scan(const std::string& scan) const {
  size_t n_stations=0;
  for (Vex::Node::const_iterator it =
         vex.get_root_node()["SCHED"][scan]->begin("station");
       it != vex.get_root_node()["SCHED"][scan]->end("station");
       ++it) {
    n_stations++;
  }
  return n_stations;
}

int
Control_parameters::
number_correlation_cores_per_timeslice(const std::string &mode) const {
  if (cross_polarize()) {
    int n_cores=0;
    for (int i=0; i<(int)number_frequency_channels(); i++) {
      int cross = cross_channel(channel(i), mode);
      if ((cross == -1) || (cross > i)) {
        n_cores ++;
      }
    }
    return n_cores;
  } else {
    return number_frequency_channels();
  }
}

size_t
Control_parameters::number_frequency_channels() const {
  return ctrl["channels"].size();
}

// Lookup the name of the channel corresponding to CHANNEL_NR for
// station STATION_NAME in mode MODE_NAME.  Return an empty string if
// no matching channel was founnd.

std::string
Control_parameters::frequency_channel(size_t channel_nr, const std::string& mode_name, const std::string &station_name) const {
  SFXC_ASSERT(channel_nr < number_frequency_channels());

  char pol = polarisation(channel(channel_nr), setup_station(), mode_name);
  if (pol == ' ')
    return std::string(); // Channel not present
  int64_t freq_min, freq_max;
  if (sideband(channel(channel_nr), setup_station(), mode_name) == 'L') {
    freq_max = channel_freq(mode_name, setup_station(), channel(channel_nr));
    freq_min = freq_max - bandwidth(mode_name, setup_station(), channel(channel_nr));
  } else {
    freq_min = channel_freq(mode_name, setup_station(), channel(channel_nr));
    freq_max = freq_min + bandwidth(mode_name, setup_station(), channel(channel_nr));
  }

  const std::string &freq_name = get_vex().get_frequency(mode_name, station_name);
  Vex::Node::const_iterator freq = vex.get_root_node()["FREQ"][freq_name];

  int64_t ch_freq_min, ch_freq_max;
  for (Vex::Node::const_iterator chan = freq->begin("chan_def"); chan != freq->end("chan_def"); chan++) {
    if (chan[2]->to_char() == 'L') {
      ch_freq_max = (int64_t)round(chan[1]->to_double_amount("Hz"));
      ch_freq_min = ch_freq_max - (int)chan[3]->to_double_amount("Hz");
    } else {
      ch_freq_min = (int64_t)round(chan[1]->to_double_amount("Hz"));
      ch_freq_max = ch_freq_min + (int)chan[3]->to_double_amount("Hz");
    }

    // We have a match if the channel corresponding to CHANNEL_NR is
    // wholly conatined in this channel.  This covers the "normal"
    // case where all stations use the same setup as well as the case
    // of mixed 16/32 MHz and 16/64 MHz observations,
    if ((freq_min >= ch_freq_min && freq_max <= ch_freq_max) &&
	pol == polarisation(chan[4]->to_string(), station_name, mode_name))
      return chan[4]->to_string();

    // We also match if this channel is wholly contained in the
    // channel corresponding to CHANNEL_NR.  This covers the case of
    // mixed bandwidth observations where we correlate wide bands but
    // want to include narrower bands in the result.
    if ((ch_freq_min >= freq_min && ch_freq_max <= freq_max) &&
	pol == polarisation(chan[4]->to_string(), station_name, mode_name))
      return chan[4]->to_string();
  }

  return std::string();
}

int
Control_parameters::frequency_number(size_t channel_nr, const std::string& mode_name) const {
  std::set<int64_t> freq_set;
  std::set<int64_t>::const_iterator freq_set_it;
  int64_t frequency;

  const std::string& channel_name = channel(channel_nr);
  const std::string& station_name = setup_station();
  const std::string& freq_name = get_vex().get_frequency(mode_name, station_name);
  Vex::Node::const_iterator freq = vex.get_root_node()["FREQ"][freq_name];
  for (Vex::Node::const_iterator ch_it = freq->begin("chan_def");
       ch_it != freq->end("chan_def");
       ++ch_it) {
    if (ch_it[4]->to_string() == channel_name)
      frequency = (int64_t)round(ch_it[1]->to_double_amount("Hz"));
    freq_set.insert((int64_t)round(ch_it[1]->to_double_amount("Hz")));
  }

  int count = 0;
  for (freq_set_it = freq_set.begin(); freq_set_it != freq_set.end(); ++freq_set_it) {
    if (*freq_set_it == frequency)
      return count;
    count++;
  }

  return -1;
}

const Vex &
Control_parameters::get_vex() const {
  SFXC_ASSERT(initialised);
  return vex;
}

std::string
Control_parameters::get_exper_name() const {
  const Vex::Node &root = get_vex().get_root_node();
  if (root["GLOBAL"]["EXPER"] == root["GLOBAL"]->end()) {
    std::cerr << "Cannot find EXPER in $GLOBAL block" << std::endl;
    sfxc_abort();
  }
  const std::string exper = root["GLOBAL"]["EXPER"]->to_string();
  if (root["EXPER"][exper] == root["EXPER"]->end()) {
    std::cerr << "Cannot find " << exper << " in $EXPER block" << std::endl;
    sfxc_abort();
  }
  if (root["EXPER"][exper]["exper_name"] != root["EXPER"][exper]->end())
    return root["EXPER"][exper]["exper_name"]->to_string();
  return std::string();
}

std::vector<int>
Control_parameters::
get_track_bit_position(const std::string &mode, const std::string &station) const {
  std::vector<int> tracks; // bit positions of all tracks in the vex file
  tracks.resize(64); // tracks from headstack 2 are in position 32-63
  memset(&tracks[0], 0, tracks.size()*sizeof(int));
  const std::string &track_name = get_vex().get_track(mode, station);
  Vex::Node::const_iterator track = vex.get_root_node()["TRACKS"][track_name];
  for (Vex::Node::const_iterator fanout_def_it = track->begin("fanout_def");
         fanout_def_it != track->end("fanout_def"); ++fanout_def_it) {
    Vex::Node::const_iterator it = fanout_def_it->begin();
    ++it;
    ++it;
    ++it;
    int headstack = it->to_int();
    ++it;
    for (; it != fanout_def_it->end(); ++it) 
      tracks[32 * (headstack-1) + it->to_int() - 2] = 1;
  }
  int bit = -1; // the current bit
  for(int i = 0; i < tracks.size(); i++){
    bit +=tracks[i];
    tracks[i] *= bit;
  }
  return tracks;
}

int
Control_parameters::
n_mark5a_tracks(const std::string &mode, const std::string &station) const {
  const std::string &track_name = get_vex().get_track(mode, station);
  int n_tracks = 0;
  Vex::Node::const_iterator track = vex.get_root_node()["TRACKS"][track_name];
  for (Vex::Node::const_iterator fanout_def_it = track->begin("fanout_def");
         fanout_def_it != track->end("fanout_def"); ++fanout_def_it) {
    Vex::Node::const_iterator it = fanout_def_it->begin();
    ++it;
    ++it;
    ++it;
    ++it;
    for (; it != fanout_def_it->end(); ++it)
      n_tracks++;
  }
  return n_tracks;
}

void
Control_parameters::
get_mark5a_tracks(const std::string &mode,
                  const std::string &station,
                  Input_node_parameters &input_parameters) const {
  // Bit positions for all tracks in the vex file
  std::vector<int> track_pos = get_track_bit_position(mode, station);
  input_parameters.n_tracks = n_mark5a_tracks(mode, station);

  const std::string &track_name = get_vex().get_track(mode, station);
  Vex::Node::const_iterator track = vex.get_root_node()["TRACKS"][track_name];

  // Determine if data modulation is active
  Vex::Node::const_iterator mod_it = track->begin("data_modulation");
  if (mod_it != track->end() && mod_it->to_string() == "on")
    input_parameters.data_modulation=1;
  else
    input_parameters.data_modulation=0;

  std::vector<int> sign_tracks, mag_tracks;
  for (size_t ch_nr=0; ch_nr < number_frequency_channels(); ch_nr++) {
    const std::string &channel_name = frequency_channel(ch_nr, mode, station);

    if (channel_name != std::string()) {
      // tracks
      Input_node_parameters::Channel_parameters channel_param;
      channel_param.bits_per_sample = 1;
      channel_param.sideband = sideband(channel(ch_nr), setup_station(), mode);
      channel_param.polarisation = polarisation(channel(ch_nr), setup_station(), mode);
      channel_param.frequency_number = frequency_number(ch_nr, mode);
      sign_tracks.resize(0);
      mag_tracks.resize(0);

      for (Vex::Node::const_iterator fanout_def_it = track->begin("fanout_def");
           fanout_def_it != track->end("fanout_def"); ++fanout_def_it) {
        if (channel_name == fanout_def_it[1]->to_string()) {
          Vex::Node::const_iterator it = fanout_def_it->begin();
          ++it;
          ++it;
          ++it;
          int headstack = it->to_int();
          ++it;
          if(fanout_def_it[2]->to_string() == "sign"){
            for (; it != fanout_def_it->end(); ++it) {
              int track = (headstack - 1)*32 + it->to_int() - 2;
              sign_tracks.push_back(track_pos[track]);
            }
          } else{
            channel_param.bits_per_sample = 2;
            for (; it != fanout_def_it->end(); ++it) {
              int track = (headstack - 1)*32 + it->to_int() - 2;
              mag_tracks.push_back(track_pos[track]);
            }
          }
        }
      }
      if((channel_param.bits_per_sample == 2) && (mag_tracks.size() != sign_tracks.size()))
        sfxc_abort("Number of magnitude tracks do not match the number of sign tracks");
      for(int i=0; i<sign_tracks.size();i++){
        channel_param.tracks.push_back(sign_tracks[i]);
        if(channel_param.bits_per_sample == 2)
          channel_param.tracks.push_back(mag_tracks[i]);
      }
      input_parameters.channels.push_back(channel_param);
    }
  }
}

int
Control_parameters::
n_mark5b_bitstreams(const std::string &mode,
                    const std::string &station) const {
  // First determine if there is a bitstreams section for the current station in the vex file.
  const Vex::Node &root=get_vex().get_root_node();
  const std::string bitstreams_name = get_vex().get_bitstreams(mode, station);
  Vex::Node::const_iterator bitstream = vex.get_root_node()["BITSTREAMS"][bitstreams_name];
  int n_bitstream = 0;
  // Iterate over the bitstreams
  for (Vex::Node::const_iterator bitstream_it = bitstream->begin("stream_def");
      bitstream_it != bitstream->end("stream_def"); ++bitstream_it) {
    n_bitstream++;
  }
  return n_bitstream;
}

void
Control_parameters::
get_mark5b_tracks(const std::string &mode,
                  const std::string &station,
                  Input_node_parameters &input_parameters) const {
  // First determine if there is a bitstreams section for the current station in the vex file.
  const Vex::Node &root=get_vex().get_root_node();

  const std::string bitstreams_name = get_vex().get_bitstreams(mode, station);
  if (bitstreams_name != std::string()) {
    input_parameters.n_tracks = n_mark5b_bitstreams(mode, station);
    // Parse the bitstream section
    Vex::Node::const_iterator bitstream = vex.get_root_node()["BITSTREAMS"][bitstreams_name];
    for (size_t ch_nr=0; ch_nr < number_frequency_channels(); ch_nr++) {
      const std::string &channel_name = frequency_channel(ch_nr, mode, station);

      if (channel_name != std::string()) {
        // Iterate over the bitstreams
        int n_bitstream = 0;
        Input_node_parameters::Channel_parameters channel_param;
        channel_param.bits_per_sample = 1;
	channel_param.sideband = sideband(channel(ch_nr), setup_station(), mode);
	channel_param.polarisation = polarisation(channel(ch_nr), setup_station(), mode);
        channel_param.frequency_number = frequency_number(ch_nr, mode);
        int sign_track, mag_track;
        for (Vex::Node::const_iterator bitstream_it = bitstream->begin("stream_def");
            bitstream_it != bitstream->end("stream_def"); ++bitstream_it) {
          if (channel_name == bitstream_it[0]->to_string()) {
            Vex::Node::const_iterator it = bitstream_it->begin();
            ++it;
            ++it;
            ++it;
            if (bitstream_it[1]->to_string() == "sign"){
              sign_track = it->to_int();
            }else{
              channel_param.bits_per_sample = 2;
              mag_track = it->to_int();
            }
          }
          n_bitstream++;
        }
        // If there are 64 bitstreams then an input word is 8 bytes long, otherewise is is 4 bytes
        int word_size = (n_bitstream <= 32) ? 32 : 64;
        for(int i = 0; i < word_size / n_bitstream;  i++){
          int sign = sign_track + i * n_bitstream;
          channel_param.tracks.push_back(sign);
          if(channel_param.bits_per_sample == 2){
            int magn = mag_track + i * n_bitstream;
            channel_param.tracks.push_back(magn);
          }
        }
        input_parameters.channels.push_back(channel_param);
      }
    }
    return;
  }

  const std::string tracks_name = get_vex().get_track(mode, station);
  if (tracks_name != std::string()) {
    Vex::Node::const_iterator track = root["TRACKS"][tracks_name];
    if (track["track_frame_format"]->to_string() == "Mark5B" ||
	track["track_frame_format"]->to_string() == "MARK5B") {
      input_parameters.n_tracks = n_mark5a_tracks(mode, station);
      // Parse the $TRACKS section
      for (size_t ch_nr=0; ch_nr < number_frequency_channels(); ch_nr++) {
	const std::string &channel_name = frequency_channel(ch_nr, mode, station);

	if (channel_name != std::string()) {
	  // Iterate over the tracks, interpreting them as bitstreams
	  int n_bitstream = 0;
	  Input_node_parameters::Channel_parameters channel_param;
	  channel_param.bits_per_sample = 1;
	  channel_param.sideband = sideband(channel(ch_nr), setup_station(), mode);
	  channel_param.polarisation = polarisation(channel(ch_nr), setup_station(), mode);
	  channel_param.frequency_number = frequency_number(ch_nr, mode);
	  int sign_track, mag_track;
	  for (Vex::Node::const_iterator fanout_def_it = track->begin("fanout_def");
	       fanout_def_it != track->end("fanout_def"); ++fanout_def_it) {
	    if (channel_name == fanout_def_it[1]->to_string()) {
	      if (fanout_def_it[2]->to_string() == "sign") {
		sign_track = fanout_def_it[4]->to_int() - 2;
	      } else {
		channel_param.bits_per_sample = 2;
		mag_track = fanout_def_it[4]->to_int() - 2;
	      }
	    }
	    n_bitstream++;
	  }
	  // If there are 64 bitstreams then an input word is 8 bytes long, otherewise is is 4 bytes
	  int word_size = (n_bitstream <= 32) ? 32 : 64;
	  for (int i = 0; i < word_size / n_bitstream;  i++) {
	    int sign = sign_track + i * n_bitstream;
	    channel_param.tracks.push_back(sign);
	    if (channel_param.bits_per_sample == 2) {
	      int magn = mag_track + i * n_bitstream;
	      channel_param.tracks.push_back(magn);
	    }
	  }
	  input_parameters.channels.push_back(channel_param);
	}
      }
      return;
    }
  }

  get_mark5b_standard_mapping(mode, station, input_parameters);
}

void
Control_parameters::
get_vdif_tracks(const std::string &mode,
		const std::string &station,
		Input_node_parameters &input_parameters) const {

  const Vex::Node &root = get_vex().get_root_node();
  const std::string threads_name = get_vex().get_section("THREADS", mode, station);
  if (threads_name == std::string()) {
    std::cerr << "Cannot find $THREADS reference for " << station
	      << " in mode" << mode << std::endl;
    sfxc_abort();
  }

  Vex::Node::const_iterator thread = vex.get_root_node()["THREADS"][threads_name];
  int num_threads = 0;
  input_parameters.frame_size = 0;
  for (Vex::Node::const_iterator thread_it = thread->begin("thread");
       thread_it != thread->end("thread"); thread_it++) {
    if (input_parameters.frame_size == 0)
      input_parameters.frame_size = thread_it[8]->to_int();
    num_threads++;
  }
  int num_channels = 0;
  for (Vex::Node::const_iterator channel_it = thread->begin("channel");
       channel_it != thread->end("channel"); channel_it++) {
    num_channels++;
  }

  // We can handle multi-thread, single-channel VDIF in a more
  // efficient way as we don't need to do any unpacking.
  if (num_threads == num_channels) {
      input_parameters.n_tracks = 0;
      for (size_t ch_nr = 0; ch_nr < number_frequency_channels(); ch_nr++) {
	const std::string &channel_name = frequency_channel(ch_nr, mode, station);

	Input_node_parameters::Channel_parameters channel_param;

	int thread_id = -1;
	for (Vex::Node::const_iterator channel_it = thread->begin("channel");
	     channel_it != thread->end("channel"); channel_it++) {
	  if (channel_name == channel_it[0]->to_string())
	    thread_id = channel_it[1]->to_int();
	}

	channel_param.bits_per_sample = bits_per_sample(mode, station);
	channel_param.sideband = sideband(channel(ch_nr), setup_station(), mode);
	channel_param.polarisation = polarisation(channel(ch_nr), setup_station(), mode);
	channel_param.frequency_number = frequency_number(ch_nr, mode);
	channel_param.tracks.push_back(thread_id);
	channel_param.tracks.push_back(-1); // XXX
	input_parameters.channels.push_back(channel_param);
      }

      return;
  }

  int num_tracks = 0;
  for (Vex::Node::const_iterator channel_it = thread->begin("channel");
       channel_it != thread->end("channel"); channel_it++) {
    num_tracks += bits_per_sample(mode, station);
  }

  input_parameters.n_tracks = num_tracks;
  for (size_t ch_nr = 0; ch_nr < number_frequency_channels(); ch_nr++) {
    const std::string &channel_name = frequency_channel(ch_nr, mode, station);

    if (channel_name != std::string()) {
      Input_node_parameters::Channel_parameters channel_param;
      channel_param.bits_per_sample = bits_per_sample(mode, station);
      channel_param.sideband = sideband(channel(ch_nr), setup_station(), mode);
      channel_param.polarisation = polarisation(channel(ch_nr), setup_station(), mode);
      channel_param.frequency_number = frequency_number(ch_nr, mode);

      // NB: Number of channels (and therefore num_tracks) is always a power of two
      const int word_size = (num_tracks <= 32) ? 32 : num_tracks; 
      for (int i = 0; i < word_size; i += num_tracks) {
        for (Vex::Node::const_iterator channel_it = thread->begin("channel");
             channel_it != thread->end("channel"); channel_it++) {
          if (channel_name == channel_it[0]->to_string()) {
            for (int track = bits_per_sample(mode, station) - 1; track >= 0; track--)
              channel_param.tracks.push_back(channel_it[2]->to_int() * bits_per_sample(mode, station) + track + i);
            //for (int track = 0 ; track < bits_per_sample(mode, station) ; track++)
            //  channel_param.tracks.push_back(channel_it[2]->to_int() * bits_per_sample(mode, station) + track + i);
          }
        }
      }
      input_parameters.channels.push_back(channel_param);
    }
  }
}

void
Control_parameters::
get_mark5b_standard_mapping(const std::string &mode,
                            const std::string &station,
                            Input_node_parameters &input_parameters) const {
 std::cout << RANK_OF_NODE << " : WARNING - No bitstream section for station " <<  station 
                           << ", using standard mapping.\n";
  const Vex::Node &root=get_vex().get_root_node();
  // Find the number of bits per sample
  int bits_per_sample_ = bits_per_sample(mode, station);

  // Get BBC and FREQUENCY nodes
  const std::string bbc = get_vex().get_BBC(mode, station);
  const std::string freq = get_vex().get_frequency(mode, station);
  if(bbc==std::string()){
    char buffer[52];
    snprintf(buffer, 52, "Error : couldn't find BBC section for station %s.", station.c_str());
    sfxc_abort(buffer);
  }
  if(freq==std::string()){
    char buffer[52];
    snprintf(buffer, 52, "Error : couldn't find FREQ section for station %s.", station.c_str());
    sfxc_abort(buffer);
  }
  // subband to bit-stream-nr conversion
  std::map<std::string, int> subband_to_track;
  {
    // Sort the bbc's
    std::map<int, std::string> bbc_map;
    Vex::Node::const_iterator bbc_it;
    for (bbc_it = root["BBC"][bbc]->begin("BBC_assign");
         bbc_it != root["BBC"][bbc]->end("BBC_assign");
         bbc_it ++) {
      bbc_map[bbc_it[1]->to_int()] = bbc_it[0]->to_string();
    }

    // Sorted list of bbc labels
    std::vector<std::string> bbc_labels;
    bbc_labels.resize(bbc_map.size());
    int i=0;
    for (std::map<int, std::string>::iterator it=bbc_map.begin();
         it != bbc_map.end(); it++) {
      bbc_labels[i] = (*it).second;
      i++;
    }

    { // Iterate over bbcs to find the right numbering of the bit streams
      int bit_stream = 0;
      Vex::Node::const_iterator freq_it;
      // Find the upper sidebands:
      for (size_t bbc_nr=0; bbc_nr < bbc_labels.size(); bbc_nr++) {
        for (freq_it = root["FREQ"][freq]->begin("chan_def");
             freq_it != root["FREQ"][freq]->end("chan_def");
             freq_it++) {
          if ((freq_it[2]->to_string() == std::string("U")) &&
              (freq_it[5]->to_string() == bbc_labels[bbc_nr])) {
            subband_to_track[freq_it[4]->to_string()] = bit_stream;
            bit_stream++;
          }
        }
      }
      // Find the lower sidebands:
      for (size_t bbc_nr=0; bbc_nr < bbc_labels.size(); bbc_nr++) {
        for (freq_it = root["FREQ"][freq]->begin("chan_def");
             freq_it != root["FREQ"][freq]->end("chan_def");
             freq_it++) {
          if ((freq_it[2]->to_string() == std::string("L")) &&
              (freq_it[5]->to_string() == bbc_labels[bbc_nr])) {
            subband_to_track[freq_it[4]->to_string()] = bit_stream;
            bit_stream++;
          }
        }
      }
    }
  }
  // Total number of bitstreams according to vex file
  input_parameters.n_tracks = subband_to_track.size() * bits_per_sample_;

  { // Fill the sign and magnitude bits:
    int nr_bit_streams = subband_to_track.size()*bits_per_sample_;
    for (size_t ch_nr=0; ch_nr < number_frequency_channels(); ch_nr++) {
      const std::string &channel_name = frequency_channel(ch_nr, mode, station);
      int bit_stream_nr = subband_to_track[channel_name]*bits_per_sample_;

      if (channel_name != std::string()) {
        Input_node_parameters::Channel_parameters channel_param;
        channel_param.bits_per_sample = bits_per_sample_;
	channel_param.sideband = sideband(channel(ch_nr), setup_station(), mode);
	channel_param.polarisation = polarisation(channel(ch_nr), setup_station(), mode);
        channel_param.frequency_number = frequency_number(ch_nr, mode);
        if (bits_per_sample_ == 2) {
          for (; bit_stream_nr < 32; bit_stream_nr += nr_bit_streams) {
            channel_param.tracks.push_back(bit_stream_nr);
            channel_param.tracks.push_back(bit_stream_nr+1);
          }
        } else {
          for (; bit_stream_nr < 32; bit_stream_nr += nr_bit_streams) {
            channel_param.tracks.push_back(bit_stream_nr);
          }
        }
        input_parameters.channels.push_back(channel_param);
      }
    }
  }
}

Input_node_parameters
Control_parameters::
get_input_node_parameters(const std::string &scan_name,
                          const std::string &station_name) const {
  Input_node_parameters result;
  result.track_bit_rate = -1;
  result.frame_size = -1;
  result.integr_time = integration_time();
  result.offset = reader_offset(station_name);
  result.phasecal_integr_time = phasecal_integration_time();
  result.exit_on_empty_datastream = exit_on_empty_datastream();

  const Vex::Node &root = vex.get_root_node();
  const std::string &mode_name = vex.get_mode(scan_name);
  Vex::Node::const_iterator mode = root["MODE"][mode_name];
  if (mode == root["MODE"]->end()) {
    std::cerr << "Cannot find mode " << mode_name << std::endl;
    sfxc_abort();
  }
  const std::string &freq_name = vex.get_frequency(mode_name, station_name);
  if (freq_name == std::string()) {
    std::cerr << "Cannot find $FREQ reference for " << station_name
	      << " in mode " << mode_name << std::endl;
    sfxc_abort();
  }
  if (root["FREQ"][freq_name] == root["FREQ"]->end()) {
    std::cerr << "Cannot find " << freq_name << " in $FREQ block" << std::endl;
    sfxc_abort();
  }

  const std::string &if_name = vex.get_IF(mode_name, station_name);
  if (if_name == std::string()) {
    std::cerr << "Cannot find $IF reference for " << station_name
	      << " in mode " << mode_name << std::endl;
    sfxc_abort();
  }
  if (root["IF"][if_name] == root["IF"]->end()) {
    std::cerr << "Cannot find " << if_name << " in $IF block" << std::endl;
    sfxc_abort();
  }

  const std::string &bbc_name = vex.get_BBC(mode_name, station_name);
  if (bbc_name == std::string()) {
    std::cerr << "Cannot find $BBC reference for " << station_name
	      << " in mode " << mode_name << std::endl;
    sfxc_abort();
  }
  if (root["BBC"][bbc_name] == root["BBC"]->end()) {
    std::cerr << "Cannot find " << bbc_name << " in $BBC block" << std::endl;
    sfxc_abort();
  }

  result.track_bit_rate = sample_rate(mode_name, station_name);
  if (data_format(station_name) == "VDIF") {
    get_vdif_tracks(mode_name, station_name, result);
  } else if (data_format(station_name) == "Mark4" ||
	     data_format(station_name) == "VLBA")  {
    get_mark5a_tracks(mode_name, station_name, result);
  } else {
    SFXC_ASSERT(data_format(station_name) == "Mark5B");
    get_mark5b_tracks(mode_name, station_name, result);
  }

  // Set Channel offsets and dispersive delays 
  get_dedispersion_parameters(scan_name);
  const int sample_rate_ = sample_rate(mode_name, setup_station());
  const int nchannel = result.channels.size();
  for(size_t i = 0; i < nchannel; i++){
    int ch = result.channels[i].frequency_number;
    char sb = result.channels[i].sideband;
    result.channels[i].channel_offset = dedispersion_parameters.channel_offset[std::make_pair(ch, sb)];
  }
  result.buffer_time =  dedispersion_parameters.fft_size_dedispersion * 
                        (sample_rate(mode_name, station_name) / sample_rate_) / 
                        (2*sample_rate_ / 1000000.);

  // Set slice size
  result.slice_size = nr_samples_per_slice(integration_time(), 
                                           sample_rate_, 
                                           fft_size_dedispersion(scan_name),
                                           fft_size_correlation());
  // Scale the slice size based on the sample rate.  This is important for
  // "mixed bandwidth" correlation where we need to make sure that we
  // send enough data to the correlator nodes.
  result.slice_size *=  sample_rate(mode_name, station_name) / sample_rate_; 
  
  SFXC_ASSERT(!result.channels[0].tracks.empty());
  result.track_bit_rate /= result.channels[0].tracks.size() / result.channels[0].bits_per_sample;
  return result;
}

void
Control_parameters::get_dedispersion_parameters(const std::string &scan) const{
  // Only update dedispersion parameters when we move to a new scan
  if(scan == dedispersion_parameters.scan)
    return;

  int fft_size = ctrl["fft_size_correlation"].asInt();

  const Vex::Node &root = vex.get_root_node();
  const std::string &mode_name = vex.get_mode(scan);
  const int nchannel = number_frequency_channels();
  
  // Initialize all parameters
  dedispersion_parameters.scan = scan;
  {
  std::string ref = setup_station();
  for(size_t ch = 0; ch < nchannel; ch++){
    int freq = frequency_number(ch, mode_name);
    char sb  = sideband(channel(ch), ref, mode_name);
    dedispersion_parameters.channel_offset[std::make_pair(freq, sb)] = 0.;
  }
  }
  dedispersion_parameters.ref_frequency = 0.;

  // check for coherent dedispersion
  if(pulsar_binning() || phased_array() || filterbank()){
    std::string source = scan_source(scan);
    // See if current source is a pulsar and if coherent dedispersion is requested
    std::map<std::string, Pulsar_parameters::Pulsar>::const_iterator it = 
                                       pulsar_parameters.pulsars.find(source);
    if((it != pulsar_parameters.pulsars.end()) && 
      (it->second.coherent_dedispersion)){
      // Coherent dedisperion is requested, start by computing the mid point
      // of the lowest channel. All other channels will be aligned to this frequency
      std::string ref = setup_station();
      struct Channel{ double freq, bw; int sb, freq_nr;};
      Channel channels[nchannel];
      int max_ch = 0;
      for(size_t ch = 0; ch < nchannel; ch++){
        channels[ch].freq_nr = frequency_number(ch, mode_name);
        channels[ch].freq = channel_freq(mode_name, ref, channel(ch)) / 1000000.;
        channels[ch].bw = bandwidth(mode_name, ref, channel(ch)) / 1000000.;
        channels[ch].sb  = sideband(channel(ch), ref, mode_name) == 'L' ? -1 : 1;
        double freq = channels[ch].freq + channels[ch].sb*channels[ch].bw/2;
        double max_freq = channels[max_ch].freq + channels[max_ch].sb*channels[max_ch].bw/2;
        if(freq > max_freq)
          max_ch = ch;
      }
      // Compute the dispersion for each channel
      const double sample_rate_ = sample_rate(mode_name, ref) / 1000000.;
      const double DM = it->second.polyco_params[0].DM;
      const double max_freq = channels[max_ch].freq + channels[max_ch].sb*channels[max_ch].bw/2; 
      double max_dt = 0;
      for(size_t ch = 0; ch < nchannel; ch++){
        double base_freq = channels[ch].freq;
        int freq_nr = channels[ch].freq_nr;
        int sb  = channels[ch].sb;
        char sb_label  = (channels[ch].sb == -1) ? 'L': 'U';
        double bw = channels[ch].bw;
        double band_edge = base_freq + sb * bw;
        double dt = sb * dispersive_delay(base_freq, band_edge, DM);
        double offset;
        if (it->second.no_intra_channel_dedispersion)
          offset = 0;
        else
          offset = dispersive_delay(base_freq + sb*bw/2, max_freq, DM);
        max_dt = std::max(dt, max_dt);
        // FIXME Due to bug/limitation in Input_node_data_writer the offset has to be an integer microsecond
        dedispersion_parameters.channel_offset[std::make_pair(freq_nr, sb_label)] = round(offset);
        std::cout.precision(16);
        std::cout << "CH "<<ch << ", dt = " << dt << ", offset = "
                  << offset << ", dm = " << DM  << ", base " << base_freq 
                  << ", edge = " << band_edge << ", mid="<< base_freq + sb*bw/2 << ", bw = " << sb*bw 
                  << ", max = " << max_freq << "\n";
      }
      int cur_fft_size=1;
      while (cur_fft_size < max_dt * sample_rate_)
         cur_fft_size <<= 1;
      // NB: fft_size is misnamed, it contains the number of spectral channels
      fft_size = std::max(cur_fft_size, fft_size);
      dedispersion_parameters.ref_frequency = max_freq;
    }
  }
  dedispersion_parameters.fft_size_dedispersion = fft_size;
}
 
std::string
Control_parameters::transport_type(const std::string &station) const {
  const Vex::Node &root = vex.get_root_node();
  Vex::Node::const_iterator station_block = root["STATION"][station];
  for (Vex::Node::const_iterator das_it = station_block->begin("DAS");
       das_it != station_block->end("DAS"); ++das_it) {
    const std::string das = das_it->to_string();
    if (root["DAS"][das] == root["DAS"]->end()) {
      std::cerr << "Cannot find " << das << " in $DAS block" << std::endl;
      sfxc_abort();
    }
    if (root["DAS"][das]["record_transport_type"] != root["DAS"][das]->end())
      return root["DAS"][das]["record_transport_type"]->to_string();
  }
  return std::string();
}

std::string
Control_parameters::data_format(const std::string &station) const {
  if (transport_type(station) == "Mark5A") {
    if (rack_type(station) == "VLBA4")
      return "Mark4";
    return rack_type(station);
  }

  // Temporary until the various VEX parsers learn about Mark5C
  // (and Dr. Bob stops needlessly editing VEX files)
  if (transport_type(station) == "Mark5B") {
    if (rack_type(station) == "DVP" || rack_type(station) == "RDBE2" ||
	rack_type(station) == "WIDAR")
      return "VDIF";
  }
  if (transport_type(station) == "Mark5C") {
    if (rack_type(station) == "DBBC" || rack_type(station) == "DVP" ||
	rack_type(station) == "RDBE2" || rack_type(station) == "WIDAR")
      return "VDIF";
  }
  if (transport_type(station) == "None") {
    if (rack_type(station) == "DBBC")
      return "VDIF";
  }

  return transport_type(station);
}

std::string
Control_parameters::rack_type(const std::string &station) const {
  const Vex::Node &root = vex.get_root_node();
  Vex::Node::const_iterator station_block = root["STATION"][station];
  for (Vex::Node::const_iterator das_it = station_block->begin("DAS");
       das_it != station_block->end("DAS"); ++das_it) {
    const std::string das = das_it->to_string();
    if (root["DAS"][das] == root["DAS"]->end()) {
      std::cerr << "Cannot find " << das << " in $DAS block" << std::endl;
      sfxc_abort();
    }
    if (root["DAS"][das]["electronics_rack_type"] != root["DAS"][das]->end())
      return root["DAS"][das]["electronics_rack_type"]->to_string();
  }
  return std::string();
}

bool
Control_parameters::cross_polarize() const {
  if (!ctrl["cross_polarize"].asBool())
    return false;
  for (Vex::Node::const_iterator mode_it =
         vex.get_root_node()["MODE"]->begin();
       mode_it != vex.get_root_node()["MODE"]->end();
       ++mode_it) {
    for (size_t ch_nr=0; ch_nr<number_frequency_channels(); ch_nr++) {
      if (cross_channel(ch_nr, mode_it.key()) != -1)
        return true;
    }
  }
  return false;
}

std::string
Control_parameters::
get_mode(const Time &start_time) const {
  std::string scan_name = vex.get_scan_name(Vex::Date(start_time.date_string()));
  if(scan_name == std::string()){
    std::string error_msg = std::string("get_mode called with invalid time : ");
    error_msg += start_time.date_string();
    sfxc_abort(error_msg.c_str());
    return std::string();
  }
  return vex.get_mode(scan_name);
}

int
Control_parameters::
cross_channel(int channel_nr, const std::string &mode) const {
  if (channel_nr >= (int)number_frequency_channels())
    return -1;
  return cross_channel(channel(channel_nr), mode);
}

int
Control_parameters::
cross_channel(const std::string &channel_name,
              const std::string &mode) const {
  std::string freq = frequency(channel_name, setup_station(), mode);
  if (freq != std::string()){
    char side = sideband(channel_name, setup_station(), mode);
    char pol  = polarisation(channel_name, setup_station(), mode);
    if(pol != ' '){
      for (size_t i = 0; i < number_frequency_channels(); i++) {
        if (channel(i) != channel_name) {
	  if ((freq == frequency(channel(i), setup_station(), mode)) &&
	      (side == sideband(channel(i), setup_station(), mode)) &&
	      (pol != polarisation(channel(i), setup_station(), mode))) {
  	    return i;
  	  } 
        }
      }
    }
  }
  return -1;
}

char
Control_parameters::
polarisation(const std::string &channel_name,
             const std::string &station_name,
             const std::string &mode_name) const {
  const Vex::Node &root = vex.get_root_node();
  Vex::Node::const_iterator mode = root["MODE"][mode_name];
  if (mode == root["MODE"]->end()) {
    std::cerr << "Cannot find mode " << mode_name << std::endl;
    sfxc_abort();
  }
  const std::string &freq_name = vex.get_frequency(mode_name, station_name);
  if (freq_name == std::string()) {
    std::cerr << "Cannot find $FREQ reference for " << station_name
	      << " in mode " << mode_name << std::endl;
    sfxc_abort();
  }
  if (root["FREQ"][freq_name] == root["FREQ"]->end()) {
    std::cerr << "Cannot find " << freq_name << " in $FREQ block" << std::endl;
    sfxc_abort();
  }

  const std::string &if_name = vex.get_IF(mode_name, station_name);
  if (if_name == std::string()) {
    std::cerr << "Cannot find $IF reference for " << station_name
	      << " in mode " << mode_name << std::endl;
    sfxc_abort();
  }
  if (root["IF"][if_name] == root["IF"]->end()) {
    std::cerr << "Cannot find " << if_name << " in $IF block" << std::endl;
    sfxc_abort();
  }

  const std::string &bbc_name = vex.get_BBC(mode_name, station_name);
  if (bbc_name == std::string()) {
    std::cerr << "Cannot find $BBC reference for " << station_name
	      << " in mode " << mode_name << std::endl;
    sfxc_abort();
  }
  if (root["BBC"][bbc_name] == root["BBC"]->end()) {
    std::cerr << "Cannot find " << bbc_name << " in $BBC block" << std::endl;
    sfxc_abort();
  }

  std::string bbc_ref;
  Vex::Node::const_iterator freq = root["FREQ"][freq_name];
  for (Vex::Node::const_iterator chan = freq->begin("chan_def");
       chan != freq->end("chan_def"); chan++) {
    if (chan[4]->to_string() == channel_name)
      bbc_ref = chan[5]->to_string();
  }

  std::string if_ref;
  Vex::Node::const_iterator bbc = root["BBC"][bbc_name];
  for (Vex::Node::const_iterator bbc_it = bbc->begin("BBC_assign");
       bbc_it != bbc->end("BBC_assign"); bbc_it++) {
    if (bbc_it[0]->to_string() == bbc_ref)
      if_ref = bbc_it[2]->to_string();
  }

  return vex.polarisation(if_name, if_ref);
}

int
Control_parameters::
polarisation_type_for_global_output_header(const std::string &mode) const {
  if (cross_polarize())
    return Output_header_global::LEFT_RIGHT_POLARISATION_WITH_CROSSES;

  bool left = false, right = false;
  // Assume station 0 is in all scans
  std::string station_name = setup_station();
  for (size_t ch_nr=0; ch_nr<number_frequency_channels(); ch_nr++) {
    std::string channel_name = frequency_channel(ch_nr, mode, station_name);
    if (channel_name != std::string()){
      if (channel_name != std::string()){
        char pol = polarisation(channel_name, station_name, mode);
        if (std::toupper(pol) == 'L')
          left = true;
        else if (std::toupper(pol) == 'R')
          right = true;
      }
    }
  }
  if (left && right)
    return Output_header_global::LEFT_RIGHT_POLARISATION;
  if (left)
    return Output_header_global::LEFT_POLARISATION;

  SFXC_ASSERT(right);
  return Output_header_global::RIGHT_POLARISATION;
}

std::string
Control_parameters::
frequency(const std::string &channel_name,
          const std::string &station_name,
          const std::string &mode_name) const {
  std::string freq_name;

  Vex::Node::const_iterator mode = vex.get_root_node()["MODE"][mode_name];
  for (Vex::Node::const_iterator freq_it = mode->begin("FREQ");
       freq_it != mode->end("FREQ"); freq_it++) {
    for (Vex::Node::const_iterator elem_it = freq_it->begin();
         elem_it != freq_it->end(); elem_it++) {
      if (elem_it->to_string() == station_name) {
        freq_name = freq_it[0]->to_string();
      }
    }
  }

  if (freq_name != std::string()){
    Vex::Node::const_iterator freq = vex.get_root_node()["FREQ"][freq_name];
    for (Vex::Node::const_iterator ch_it = freq->begin("chan_def");
         ch_it != freq->end("chan_def"); ch_it++) {
      if (ch_it[4]->to_string() == channel_name) {
        return ch_it[1]->to_string();
      }
    }
  }

  return std::string();
}

char
Control_parameters::
sideband(const std::string &channel_name,
         const std::string &station_name,
         const std::string &mode) const {

  std::string if_mode_freq;
  std::string if_node_Node;
  std::string if_ref_BBC;
  std::string if_ref_BBCnr;
  std::string if_ref_Ref;
  char sband = 'x';

  Vex::Node::const_iterator mode_block = vex.get_root_node()["MODE"][mode];
  for (Vex::Node::const_iterator if_it = mode_block->begin("FREQ");
       if_it != mode_block->end("FREQ"); ++if_it) {
    for (Vex::Node::const_iterator elem_it = if_it->begin();
         elem_it != if_it->end(); ++elem_it) {
      if (elem_it->to_string() == station_name) {
        if_mode_freq = if_it[0]->to_string();
      }
    }
  }
  for (Vex::Node::const_iterator if_it = mode_block->begin("IF");
       if_it != mode_block->end("IF"); ++if_it) {
    for (Vex::Node::const_iterator elem_it = if_it->begin();
         elem_it != if_it->end(); ++elem_it) {
      if (elem_it->to_string() == station_name) {
        if_node_Node = if_it[0]->to_string();
      }
    }
  }
  for (Vex::Node::const_iterator bbc_it = mode_block->begin("BBC");
       bbc_it != mode_block->end("BBC"); ++bbc_it) {
    for (size_t i=1; i<bbc_it->size(); i++) {
      if (bbc_it[i]->to_string() == station_name) {
        if_ref_BBC = bbc_it[0]->to_string();
      }
    }
  }


  for (Vex::Node::const_iterator frq_block = vex.get_root_node()["FREQ"][if_mode_freq]->begin("chan_def");
       frq_block != vex.get_root_node()["FREQ"][if_mode_freq]->end("chan_def"); ++frq_block) {
    for (Vex::Node::const_iterator elem_it = frq_block->begin();
         elem_it != frq_block->end(); ++elem_it) {
      if (elem_it->to_string() == channel_name) {
        sband = frq_block[2]->to_char();
      }
    }
  }

  return sband;
}

int
Control_parameters::station_number(const std::string &station_name) const
{
  if (station_map.empty()) {
    for (Vex::Node::const_iterator station_it =
	   vex.get_root_node()["STATION"]->begin();
	 station_it != vex.get_root_node()["STATION"]->end(); ++station_it) {
      station_map[station_it.key()] = -1;
    }

    int station_number = 0;
    for (std::map<std::string, int>::iterator it = station_map.begin();
	 it != station_map.end(); it++) {
      it->second = station_number;
      std::cout << it->first << " : nr = " << station_number << "\n";
      station_number++;
    }
  }
  return station_map[station_name];
}

Correlation_parameters
Control_parameters::
get_correlation_parameters(const std::string &scan_name,
                           const Time start_time,
			   size_t channel_nr,
                           const std::map<std::string, int> &correlator_node_station_to_input) const {
  std::string bbc_nr;
  std::string bbc_mode;
  std::string if_nr;
  std::string if_mode;

  Vex::Node::const_iterator scan =
    vex.get_root_node()["SCHED"][scan_name];
  std::string mode_name = scan["mode"]->to_string();
  Vex::Node::const_iterator mode =
    vex.get_root_node()["MODE"][mode_name];

  const std::string &station_name = setup_station();
  const std::string &channel_name =
    frequency_channel(channel_nr, mode_name, station_name);

  Correlation_parameters corr_param;
  corr_param.experiment_start = vex.get_start_time_of_experiment();
  corr_param.integration_time = integration_time();
  corr_param.sub_integration_time = sub_integration_time();
  corr_param.number_channels = number_channels();
  corr_param.fft_size_delaycor = fft_size_delaycor();
  corr_param.fft_size_correlation = fft_size_correlation();
  corr_param.window = window_function();  
  corr_param.slice_offset =
    number_correlation_cores_per_timeslice(mode_name);
  corr_param.sample_rate = sample_rate(mode_name, station_name);

  corr_param.sideband = ' ';
  const std::string &freq_name = get_vex().get_frequency(mode_name, station_name);
  Vex::Node::const_iterator freq = vex.get_root_node()["FREQ"][freq_name];
  for (Vex::Node::const_iterator ch_it = freq->begin("chan_def");
       ch_it != freq->end("chan_def");
       ++ch_it) {
    if (ch_it[4]->to_string() == channel_name) {
      corr_param.channel_freq = (int64_t)round(ch_it[1]->to_double_amount("Hz"));
      corr_param.bandwidth = (int)ch_it[3]->to_double_amount("Hz");
      corr_param.sideband = ch_it[2]->to_char();
      bbc_nr = ch_it[5]->to_string();
    }
  }
  corr_param.frequency_nr = frequency_number(channel_nr, mode_name);

  //in the following two blocks (if_mode and bbc_mode) we assume only one of the
  //station name HO
  for (Vex::Node::const_iterator if_it = mode->begin("IF");
       if_it != mode->end("IF"); ++if_it) {
    for (Vex::Node::const_iterator elem_it = if_it->begin();
         elem_it != if_it->end(); ++elem_it) {
      if (elem_it->to_string() == station_name) {
        if_mode = if_it[0]->to_string();
      }
    }
  }
  for (Vex::Node::const_iterator bbc_it = mode->begin("BBC");
       bbc_it != mode->end("BBC"); ++bbc_it) {
    for (size_t i=1; i<bbc_it->size(); i++) {
      if (bbc_it[i]->to_string() == station_name) {
        bbc_mode = bbc_it[0]->to_string();
      }
    }
  }

  for (Vex::Node::const_iterator bbc_block = vex.get_root_node()["BBC"][bbc_mode]->begin();
       bbc_block != vex.get_root_node()["BBC"][bbc_mode]->end(); ++bbc_block) {
    for (Vex::Node::const_iterator bbcnr_it = bbc_block->begin();
         bbcnr_it != bbc_block->end(); ++bbcnr_it) {
      if (bbcnr_it->to_string() == bbc_nr) {
        if_nr = bbc_block[2]->to_string();
      }
    }
  }

  corr_param.polarisation = vex.polarisation(if_mode, if_nr);

  SFXC_ASSERT(corr_param.sideband == 'L' || corr_param.sideband == 'U');

  corr_param.cross_polarize = cross_polarize();
  if (cross_channel(channel_name, mode_name) == -1) {
    corr_param.cross_polarize = false;
  }

  corr_param.reference_station = -1;
  if (reference_station() != "") {
    for (size_t station_nr=0; station_nr < number_stations(); station_nr++) {
      if (reference_station() == station(station_nr)) {
        corr_param.reference_station = station_nr;
      }
    }
    SFXC_ASSERT(corr_param.reference_station != -1);
  }

  for (Vex::Node::const_iterator station = scan->begin("station");
       station != scan->end("station"); ++station) {
    const std::string &channel_name =
      frequency_channel(channel_nr, mode_name, station[0]->to_string());
    std::map<std::string, int>::const_iterator station_nr_it =
      correlator_node_station_to_input.find(station[0]->to_string());
    if (station_nr_it != correlator_node_station_to_input.end()) {
      if (station_nr_it->second >= 0) {
        if (channel_name != std::string()) {
          Correlation_parameters::Station_parameters station_param;
          station_param.station_number = station_number(station[0]->to_string());
          station_param.station_stream = station_nr_it->second;
          station_param.start_time = station[1]->to_int_amount("sec");
          station_param.stop_time = station[2]->to_int_amount("sec");
          station_param.bits_per_sample = bits_per_sample(mode_name, station[0]->to_string());
          station_param.sample_rate = sample_rate(mode_name, station[0]->to_string());
          station_param.channel_freq = channel_freq(mode_name, station[0]->to_string(), channel_name);
          station_param.bandwidth = bandwidth(mode_name, station[0]->to_string(), channel_name);
          station_param.sideband = sideband(channel_name, station[0]->to_string(), mode_name);
          station_param.LO_offset = LO_offset(station[0]->to_string());
          corr_param.station_streams.push_back(station_param);
        }
      }
    }
  }
  // Get source information
  strncpy(corr_param.source, scan_source(scan_name).c_str(), 11);

  // Compute stream start / stop
  get_dedispersion_parameters(scan_name);
  corr_param.dedispersion_ref_frequency = dedispersion_parameters.ref_frequency; 
  char sb = corr_param.sideband;
  int freq_nr = corr_param.frequency_nr;
  corr_param.channel_offset = dedispersion_parameters.channel_offset[std::make_pair(freq_nr, sb)];
  Time integer_offset = round(corr_param.channel_offset * corr_param.sample_rate/1000000) /
                        (corr_param.sample_rate/1000000);
/*  std::cout.precision(16);
  std::cout << channel_nr << " : channel_offset = " << dedispersion_parameters.channel_offset[channel_nr]
            << ", integer_offset = " << integer_offset.get_time_usec() 
            << ", fft_early = " << dedispersion_parameters.fft_size_dedispersion / (corr_param.sample_rate/1000000.)
            << "\n";*/
  corr_param.integration_start = start_time + integer_offset;
  // The stream starts half a frame early
  corr_param.stream_start = corr_param.integration_start - 
                            Time(dedispersion_parameters.fft_size_dedispersion /
                                 (2*corr_param.sample_rate/1000000.));
                                             
  corr_param.fft_size_dedispersion = fft_size_dedispersion(scan_name); 
  corr_param.slice_size = nr_samples_per_slice(integration_time(), 
                                            corr_param.sample_rate,
                                            corr_param.fft_size_dedispersion,
                                            fft_size_correlation());

/*  std::cout << corr_param.fft_size_delaycor
            <<","<<corr_param.fft_size_dedispersion
            <<","<<corr_param.fft_size_correlation
            << ", " << corr_param.slice_size << "\n";*/
  return corr_param;
}

std::string
Control_parameters::
get_delay_table_name(const std::string &station_name) const {
  if (strncmp(ctrl["delay_directory"].asString().c_str(),  "file://", 7) != 0)
    sfxc_abort("Ctrl-file: Delay directory doesn't start with 'file://'");
  std::string delay_table_name;
  if (ctrl["delay_directory"].asString().size()==7)
    // delay files are in the current directory
    delay_table_name = get_exper_name() + "_" +station_name + ".del";
  else
    delay_table_name = std::string(ctrl["delay_directory"].asString().c_str()+7) +
      "/" + get_exper_name() + "_" + station_name + ".del";

  if (access(delay_table_name.c_str(), R_OK) == 0)
    return delay_table_name;
  generate_delay_table(station_name, delay_table_name);
  if (access(delay_table_name.c_str(), R_OK) == 0)
    return delay_table_name;
  DEBUG_MSG("Tried to create the delay table at " << delay_table_name);
  sfxc_abort("Couldn't create the delay table.");
  return std::string("");
}

void
Control_parameters::
generate_delay_table(const std::string &station_name,
                     const std::string &filename) const {
  std::string cmd =
    "generate_delay_model "+vex_filename+" "+station_name+" "+filename;
  DEBUG_MSG("Creating the delay model: " << cmd);
  int result = system(cmd.c_str());
  if (result != 0) {
    sfxc_abort("Generation of the delay table failed (generate_delay_model)");
  }
}

std::string
Control_parameters::create_path(const std::string &path) const {
  if (strncmp(path.c_str(), "file://", 7) == 0) {
    if (path[7] != '/') {
      std::string result = "file://";
      char c_ctrl_filename[ctrl_filename.size()+1];
      strcpy(c_ctrl_filename, ctrl_filename.c_str());
      result += dirname(c_ctrl_filename);
      result += "/";
      result += path.c_str()+7;
      return result;
    } else {
      return path;
    }
  } else {
    return path;
  }
}

bool
Input_node_parameters::operator==(const Input_node_parameters &other) const {
  if (channels != other.channels)
    return false;
  if (track_bit_rate != other.track_bit_rate)
    return false;
  if (integr_time != other.integr_time)
    return false;

  return true;
}

std::ostream &
operator<<(std::ostream &out,
           const Input_node_parameters &param) {
  out << "{ \"n_tracks\": " << param.n_tracks << ", "
      <<"\"track_bit_rate\": " << param.track_bit_rate << ", "
      << "\"integr_time\": " << param.integr_time << ", "
      << std::endl;

  out << " channels: [";
  for (size_t i=0; i<param.channels.size(); i++) {
    if (i > 0)
      out << ",";
    out << std::endl;
    int bps = param.channels[i].bits_per_sample;
    for (size_t track = 0; track < param.channels[i].tracks.size(); track+=bps) {
      if (track > 0)
        out << ", ";
      out << param.channels[i].tracks[track];
    }
    out << "] ], ";
    if(bps == 2){
      for (size_t track = 1; track < param.channels[i].tracks.size(); track+=bps) {
        if (track > 0)
          out << ", ";
        out << param.channels[i].tracks[track];
      }
    }
    out << "] ] }";
  }
  out << "] }" << std::endl;

  return out;
}

int
Input_node_parameters::bits_per_sample() const {
  SFXC_ASSERT(!channels.empty());
  for (Channel_const_iterator it=channels.begin();
       it!=channels.end(); it++) {
    SFXC_ASSERT(channels.begin()->bits_per_sample ==
                it->bits_per_sample);
  }
  return channels.begin()->bits_per_sample;
}

int
Input_node_parameters::subsamples_per_sample() const {
  SFXC_ASSERT(!channels.empty());
  for (Channel_const_iterator it=channels.begin();
       it!=channels.end(); it++) {
    SFXC_ASSERT(channels.begin()->tracks.size() ==
                it->tracks.size());
  }
  return channels.begin()->tracks.size() / channels.begin()->bits_per_sample;
}

int
Input_node_parameters::sample_rate() const {
  return track_bit_rate * subsamples_per_sample();
}

bool
Input_node_parameters::Channel_parameters::
operator==(const Input_node_parameters::Channel_parameters &other) const {
  if (tracks != other.tracks)
    return false;
  if (bits_per_sample != other.bits_per_sample)
    return false;

  return true;
}

bool
Correlation_parameters::operator==(const Correlation_parameters& other) const {
  if (stream_start != other.stream_start)
    return false;
  if (slice_size != other.slice_size)
    return false;
  if (integration_time != other.integration_time)
    return false;
  if (number_channels != other.number_channels)
    return false;
  if (fft_size_delaycor != other.fft_size_delaycor)
    return false;
  if (fft_size_correlation != other.fft_size_correlation)
    return false;
  if (window != other.window)
    return false;
  if (integration_nr != other.integration_nr)
    return false;
  if (slice_nr != other.slice_nr)
    return false;
  if (slice_offset != other.slice_offset)
    return false;

  if (sample_rate != other.sample_rate)
    return false;

  if (channel_freq != other.channel_freq)
    return false;
  if (bandwidth != other.bandwidth)
    return false;
  if (sideband != other.sideband)
    return false;

  if (station_streams != other.station_streams)
    return false;
  return true;
}

std::ostream &operator<<(std::ostream &out,
                         const Correlation_parameters &param) {
  out << "{ ";
  out << "\"stream_start\": " << param.stream_start << ", " << std::endl;
  out << "  \"slice_size\": " << param.slice_size << ", " << std::endl;
  out << "  \"integr_time\": " << param.integration_time << ", " << std::endl;
  out << "  \"number_channels\": " << param.number_channels << ", " << std::endl;
  out << "  \"fft_size_delaycor\": " << param.fft_size_delaycor << ", " << std::endl;
  out << "  \"fft_size_correlation\": " << param.fft_size_correlation << ", " << std::endl;
  out << "  \"window\": " << param.window << ", " << std::endl;
  out << "  \"slice_nr\": " << param.slice_nr << ", " << std::endl;
  out << "  \"slice_offset\": " << param.slice_offset << ", " << std::endl;
  out << "  \"sample_rate\": " << param.sample_rate << ", " << std::endl;
  out << "  \"channel_freq\": " << param.channel_freq << ", " << std::endl;
  out << "  \"bandwidth\": " << param.bandwidth<< ", " << std::endl;
  out << "  \"sideband\": " << param.sideband << ", " << std::endl;
  out << "  \"cross_polarize\": " << (param.cross_polarize ? "true" : "false")<< ", " << std::endl;
  out << "  \"reference_station\": " << param.reference_station << ", " << std::endl;
  out << "  \"station_streams\": [";
  for (size_t i=0; i<param.station_streams.size(); i++) {
    if (i!=0)
      out << ", ";
    out << std::endl;
    out << "{ \"stream\": " <<param.station_streams[i].station_stream
    << ", \"start\": " <<param.station_streams[i].start_time
    << ", \"stop\": " <<param.station_streams[i].stop_time
    << ", \"bits_per_sample\": " << param.station_streams[i].bits_per_sample
    << ", \"sample_rate\": " << param.station_streams[i].sample_rate
    << ", \"bandwidth\": " << param.station_streams[i].bandwidth
    << "  \"channel_freq\": " << param.station_streams[i].channel_freq
    << "  \"sideband\": " << param.station_streams[i].sideband
    << " }";
  }
  out << "] }" << std::endl;
  return out;
}

bool
Correlation_parameters::Station_parameters::
operator==(const Correlation_parameters::Station_parameters& other) const {
  if (station_number != other.station_number)
    return false;
  if (station_stream != other.station_stream)
    return false;
  if (start_time != other.start_time)
    return false;
  if (stop_time != other.stop_time)
    return false;

  return true;
}

Pulsar_parameters::Pulsar_parameters(std::ostream& log_writer_):log_writer(log_writer_){
}

bool 
Pulsar_parameters::parse_polyco(std::vector<Polyco_params> &param, std::string filename){
  bool polyco_completed = false, read_error = false;
  std::ifstream inp(filename.c_str());
  std::string line, temp;

  if(!inp){
    log_writer << "Could not open polyco file [" <<filename<<"]\n";
    return false;
  }
  int line_nr=0;
  int coef_idx=0;
  int n_coef=0;
  int block_index=0;
  int end_of_prev_block=0;
  param.resize(0);
  std::getline(inp, line);
  while(!inp.eof()){
    std::stringstream inpline(line);
    if(line_nr-end_of_prev_block==0){
      inpline >> temp;
      param.resize(block_index+1);
      strncpy(param[block_index].name,temp.c_str(),11);
      param[block_index].name[10]=0; // make sure sting is null terminated
      inpline >> temp;
      strncpy(param[block_index].date,temp.c_str(),10);
      param[block_index].date[9]=0; // make sure sting is null terminated
      inpline >> param[block_index].utc;
      inpline >> param[block_index].tmid;
      inpline >> param[block_index].DM;
      inpline >> param[block_index].doppler;
      inpline >> param[block_index].residual;

      polyco_completed = false;
      read_error = inpline.fail();
    }else if(line_nr-end_of_prev_block == 1){
      inpline >> param[block_index].ref_phase;
      //std::cout << param[block_index].ref_phase << " ";
      inpline >> param[block_index].ref_freq;
      //std::cout << param[block_index].ref_freq << " ";
      inpline >> temp;
      strncpy(param[block_index].site, temp.c_str(), 6);
      param[block_index].site[5]=0; // make sure sting is null terminated
      //std::cout << param[block_index].site << " ";
      inpline >> param[block_index].data_span;
      //std::cout << param[block_index].data_span << " ";
      inpline >> param[block_index].n_coef;
      //std::cout << param[block_index].n_coef << " ";
      n_coef = param[block_index].n_coef;
      param[block_index].coef.resize(n_coef);
      inpline >> param[block_index].obs_freq;
      //std::cout << param[block_index].obs_freq << " ";
      read_error = inpline.fail();
      // The binary phase parameters are optional
      inpline >> param[block_index].bin_phase[0];
      //std::cout << param[block_index].bin_phase[0] << " ";
      if(!inpline.fail()){
        inpline >> param[block_index].bin_phase[1];
        //std::cout << param[block_index].bin_phase[1] << "\n";
        read_error = inpline.fail();
      }else{
        param[block_index].bin_phase[0]=0;
        param[block_index].bin_phase[1]=0;
      }
    }else{
      while((!inpline.eof())&&(!inpline.fail())&&(coef_idx<n_coef)){
        inpline >> param[block_index].coef[coef_idx];
        //std::cout << param[block_index].coef[coef_idx] << " "; 
        coef_idx++;
      }
      //std::cout << "\n";
      if((!inpline.fail())&&(coef_idx == n_coef)){
        polyco_completed = true;
        block_index++;
        coef_idx=0;
        end_of_prev_block=line_nr+1;
      }
      read_error = inpline.fail();
    }
    if(read_error){
      log_writer << " Error parsing line " << line_nr + 1 << " of polyco file [" << filename << "]\n";
      return false;
    }
    line_nr++;
    std::getline(inp, line);
  }
  if(!polyco_completed)
    log_writer << " Eof reached prematurely while parsing polyco file [" << filename << "]\n";

  return polyco_completed;
}
