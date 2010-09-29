#include "correlation_core.h"
#include "output_header.h"
#include <utils.h>

Correlation_core::Correlation_core()
    : current_fft(0), total_ffts(0), check_input_elements(true){
}

Correlation_core::~Correlation_core() {
#if PRINT_TIMER
  int N = 2 * fft_size();
  int numiterations = total_ffts;
  double time = fft_timer.measured_time()*1000000;
  PROGRESS_MSG("MFlops: " << 5.0*N*log2(N) * numiterations / (1.0*time));
#endif
}

void Correlation_core::do_task() {
  SFXC_ASSERT(has_work());
  if (current_fft % 1000 == 0) {
    PROGRESS_MSG("node " << node_nr_ << ", "
                 << current_fft << " of " << number_ffts_in_integration);
  }

  if (current_fft%number_ffts_in_integration == 0) {
    integration_initialise();
  }

  // Process the data of the current fft
  integration_step(accumulation_buffers);
  current_fft ++;

  if (current_fft == number_ffts_in_integration) {
    PROGRESS_MSG("node " << node_nr_ << ", "
                 << current_fft << " of " << number_ffts_in_integration);

    integration_normalize(accumulation_buffers);
    integration_write(accumulation_buffers);
    current_integration++;
  }
}

bool Correlation_core::almost_finished() {
  return current_fft == number_ffts_in_integration*9/10;
}

bool Correlation_core::finished() {
  return current_fft == number_ffts_in_integration;
}

void Correlation_core::connect_to(size_t stream, bit_statistics_ptr statistics_, Input_buffer_ptr buffer) {
  if (stream >= input_buffers.size()) {
    input_buffers.resize(stream+1);
    statistics.resize(stream+1);
  }
  input_buffers[stream] = buffer;
  statistics[stream] = statistics_;
}

void
Correlation_core::set_parameters(const Correlation_parameters &parameters,
                                 int node_nr) {
  node_nr_ = node_nr;
  current_integration = 0;
  current_fft = 0;

  correlation_parameters = parameters;
  oversamp = (int) round(parameters.sample_rate / (2 * parameters.bandwidth));

  create_baselines(parameters);

#ifdef SFXC_WRITE_STATS
  backward_buffer.resize(fft_size() + 1);
  backward_plan_ =
    FFTW_PLAN_DFT_1D(fft_size() + 1,
                   reinterpret_cast<FFTW_COMPLEX*>(plan_output_buffer.buffer()),
                   reinterpret_cast<FFTW_COMPLEX*>(&backward_buffer[0]),
                   FFTW_BACKWARD,
                   FFTW_ESTIMATE);
#endif // SFXC_WRITE_STATS
}

void
Correlation_core::create_baselines(const Correlation_parameters &parameters){
  number_ffts_in_integration =
    Control_parameters::nr_ffts_per_integration_slice(
      (int) parameters.integration_time.get_time_usec(),
      parameters.sample_rate,
      parameters.fft_size);

  baselines.clear();
  // Autos
  for (size_t sn = 0 ; sn < n_stations(); sn++) {
    baselines.push_back(std::pair<int,int>(sn,sn));
  }
  // Crosses
  int ref_station = parameters.reference_station;
  if (parameters.cross_polarize) {
    SFXC_ASSERT(n_stations() % 2 == 0);
    size_t n_st_2 = n_stations()/2;
    if (ref_station >= 0) {
      // cross polarize with a reference station
      for (int sn = 0 ; sn < ref_station; sn++) {
        baselines.push_back(std::make_pair(sn       , ref_station       ));
        baselines.push_back(std::make_pair(sn+n_st_2, ref_station       ));
        baselines.push_back(std::make_pair(sn       , ref_station+n_st_2));
        baselines.push_back(std::make_pair(sn+n_st_2, ref_station+n_st_2));
      }
      for (size_t sn = ref_station+1 ; sn < n_st_2; sn++) {
        baselines.push_back(std::make_pair(ref_station       , sn       ));
        baselines.push_back(std::make_pair(ref_station       , sn+n_st_2));
        baselines.push_back(std::make_pair(ref_station+n_st_2, sn       ));
        baselines.push_back(std::make_pair(ref_station+n_st_2, sn+n_st_2));
      }
    } else {
      // cross polarize without a reference station
      for (size_t sn = 0 ; sn < n_st_2 - 1; sn++) {
        for (size_t sno = sn + 1; sno < n_st_2; sno ++) {
          baselines.push_back(std::make_pair(sn       ,sno));
          baselines.push_back(std::make_pair(sn       ,sno+n_st_2));
          baselines.push_back(std::make_pair(sn+n_st_2,sno));
          baselines.push_back(std::make_pair(sn+n_st_2,sno+n_st_2));
        }
      }
    }
  } else { // No cross_polarisation
    if (parameters.reference_station >= 0) {
      // no cross polarisation with a reference station
      for (int sn = 0 ; sn < (int)n_stations(); sn++) {
        if (sn != ref_station) {
          baselines.push_back(std::pair<int,int>(sn,ref_station));
        }
      }
    } else { // No reference station
      // no cross polarisation without a reference station

      for (size_t sn = 0 ; sn < n_stations() - 1; sn++) {
        for (size_t sno = sn + 1; sno < n_stations() ; sno ++) {
          baselines.push_back(std::pair<int,int>(sn,sno));
        }
      }
    }
  }
}

void
Correlation_core::
set_data_writer(boost::shared_ptr<Data_writer> writer_) {
  writer = writer_;
}

bool Correlation_core::has_work() {
  if(check_input_elements){
    for (size_t i=0, nstreams=number_input_streams_in_use(); i < nstreams; i++) {
      if (input_buffers[i]->empty())
        return false;
    }
  }
  return true;
}

void Correlation_core::integration_initialise() {
  if (accumulation_buffers.size() != baselines.size()) {
    accumulation_buffers.resize(baselines.size());
    for (size_t i=0; i<accumulation_buffers.size(); i++) {
      accumulation_buffers[i].resize(fft_size() + 1);
    }
  }

  SFXC_ASSERT(accumulation_buffers.size() == baselines.size());
  for (size_t i=0; i<accumulation_buffers.size(); i++) {
    SFXC_ASSERT(accumulation_buffers[i].size() == fft_size() + 1);
    for (size_t j=0; j<accumulation_buffers[i].size(); j++) {
      accumulation_buffers[i][j] = 0;
    }
  }
}

void Correlation_core::integration_step(std::vector<Complex_buffer> &integration_buffer) {
  if (input_elements.size() != number_input_streams_in_use()) {
    input_elements.resize(number_input_streams_in_use());
  }
  if(check_input_elements){
    for (size_t i=0, nstreams=number_input_streams_in_use(); i<nstreams; i++) {
      if(!input_elements[i].valid()) 
        input_elements[i].set(&input_buffers[i]->front().data()[0],
                              input_buffers[i]->front().data().size());
    }
    check_input_elements=false;
  }

#ifndef DUMMY_CORRELATION
  // do the correlation
  for (size_t i=0; i < number_input_streams_in_use(); i++) {
    // Auto correlations
    std::pair<size_t,size_t> &stations = baselines[i];
    SFXC_ASSERT(stations.first == stations.second);
    auto_correlate_baseline(/* in1 */ &input_elements[stations.first][0],
                            /* out */ &integration_buffer[i][0]);
  }

  for (size_t i=number_input_streams_in_use(); i < baselines.size(); i++) {
    // Cross correlations
    std::pair<size_t,size_t> &stations = baselines[i];
    SFXC_ASSERT(stations.first != stations.second);
    correlate_baseline(/* in1 */  &input_elements[stations.first][0],
                       /* in2 */  &input_elements[stations.second][0],
                       /* out */ &integration_buffer[i][0]);
  }

#ifdef SFXC_WRITE_STATS
  {
#ifndef SFXC_DETERMINISTIC
    sfxc_abort("SFXC_WRITE_STATS only works with SFXC_DETERMINISTIC\n");
#endif

    if (! stats_out.is_open()) {
      char filename[80];
      snprintf(filename, 80, "stats_%d.txt", RANK_OF_NODE);
      stats_out.open(filename);
    }
    SFXC_ASSERT(stats_out.is_open());

    // Reset buffer:
    for (size_t i = 0; i < fft_size() + 1; i++)
      backward_buffer[i] = 0;

    int baseline = number_input_streams_in_use();
    std::pair<size_t,size_t> &stations = baselines[baseline];

    correlate_baseline
      (/* in1 */ input_elements[stations.first].buffer(),
       /* in2 */ input_elements[stations.second].buffer(),
       /* out */ &backward_buffer[0]);

    // Hardcode the position of the fringe here
    const int fringe_pos = 12;

    FFTW_EXECUTE_DFT(backward_plan_,
                     (FFTW_COMPLEX *)&backward_buffer[0],
                     (FFTW_COMPLEX *)&backward_buffer[0]);
    FLOAT fft_abs   = std::abs(backward_buffer[fringe_pos]);
    FLOAT fft_phase = std::arg(backward_buffer[fringe_pos]);

    FFTW_EXECUTE_DFT(backward_plan_,
                     (FFTW_COMPLEX *)&integration_buffer[baseline][0],
                     (FFTW_COMPLEX *)&backward_buffer[0]);
    FLOAT integr_abs   = std::abs(backward_buffer[fringe_pos]);
    FLOAT integr_phase = std::arg(backward_buffer[fringe_pos]);
    int max_pos = 0;
    for (size_t i = 1; i < fft_size() + 1; i++) {
      if (std::abs(backward_buffer[i]) > std::abs(backward_buffer[max_pos]))
        max_pos = i;
    }

    stats_out << fft_abs << " \t" << fft_phase << " \t"
              << integr_abs << " \t" << integr_phase << " \t"
              << max_pos << std::endl;
  }
#endif // SFXC_WRITE_STATS
#endif // DUMMY_CORRELATION


  for (size_t i=0, nstreams=number_input_streams_in_use(); i<nstreams; i++){
    input_elements[i]++; // advance the iterator
    if(!input_elements[i].valid()){
      input_buffers[i]->pop();
      check_input_elements=true;
    }
  }
}

void Correlation_core::integration_normalize(std::vector<Complex_buffer> &integration_buffer) {
  std::vector<FLOAT> norms;
  norms.resize(n_stations());
  memset(&norms[0], 0, norms.size()*sizeof(FLOAT));

  // Average the auto correlations
  for (size_t station=0; station < n_stations(); station++) {
    for (size_t i = 0; i < fft_size() + 1; i++) {
      norms[station] += integration_buffer[station][i].real();
    }
    norms[station] /= (fft_size() / oversamp);
    if(norms[station] < 1)
      norms[station] = 1;

    for (size_t i = 0; i < fft_size() + 1; i++) {
      // imaginary part should be zero!
      integration_buffer[station][i] =
        integration_buffer[station][i].real() / norms[station];
    }
  }

  // Average the cross correlations
  for (size_t station=n_stations(); station < baselines.size(); station++) {
    std::pair<size_t,size_t> &stations = baselines[station];
    FLOAT norm = sqrt(norms[stations.first]*norms[stations.second]);
    for (size_t i = 0 ; i < fft_size() + 1; i++) {
      integration_buffer[station][i] /= norm;
    }
  }
}

void Correlation_core::integration_write(std::vector<Complex_buffer> &integration_buffer) {

  // Make sure that the input buffers are released
  // This is done by reference counting

  SFXC_ASSERT(writer != boost::shared_ptr<Data_writer>());
  SFXC_ASSERT(integration_buffer.size() == baselines.size());

  int polarisation = 1;
  if (correlation_parameters.polarisation == 'R') {
    polarisation =0;
  } else {
    SFXC_ASSERT(correlation_parameters.polarisation == 'L');
  }

  size_t n_stations = correlation_parameters.station_streams.size();
  std::vector<int> stream2station;

  {
    // initialise with -1
    stream2station.resize(input_buffers.size(), -1);

    for (size_t i=0;
         i < correlation_parameters.station_streams.size();
         i++) {
      size_t station_stream =
        correlation_parameters.station_streams[i].station_stream;
      SFXC_ASSERT(station_stream < stream2station.size());
      stream2station[station_stream] =
        correlation_parameters.station_streams[i].station_number;
    }
  }

  { // Writing the timeslice header
    Output_header_timeslice htimeslice;

    htimeslice.number_baselines = baselines.size();
    htimeslice.integration_slice =
      correlation_parameters.integration_nr + current_integration;
    htimeslice.number_uvw_coordinates = uvw_tables.size();
    htimeslice.number_statistics = input_buffers.size();

   // write the uvw coordinates
    Output_uvw_coordinates uvw[htimeslice.number_uvw_coordinates];
    // We evaluate in the middle of time slice
    Time time = correlation_parameters.start_time + correlation_parameters.integration_time / 2;
    for (size_t station=0; station < uvw_tables.size(); station++){
     double u,v,w;
     uvw_tables[station].get_uvw(time, &u, &v, &w);
     uvw[station].station_nr=stream2station[station];
     uvw[station].u=u;
     uvw[station].v=v;
     uvw[station].w=w;
    }

    // Write the bit statistics
    Output_header_bitstatistics stats[input_buffers.size()];
    for (size_t stream=0; stream < input_buffers.size(); stream++){
      int station = stream2station[stream]-1;
      int32_t *levels=statistics[stream]->get_statistics();
      if(correlation_parameters.cross_polarize){
        int nstreams=correlation_parameters.station_streams.size();
        stats[stream].polarisation=(stream>=nstreams/2)?1-polarisation:polarisation;
      }else{
        stats[stream].polarisation=polarisation;
      }
      stats[stream].station_nr=station+1;
      stats[stream].sideband = (correlation_parameters.sideband=='L') ? 0 : 1;

      stats[stream].frequency_nr=(unsigned char)correlation_parameters.channel_nr;
      if(statistics[stream]->bits_per_sample==2){
        stats[stream].levels[0]=levels[0];
        stats[stream].levels[1]=levels[1];
        stats[stream].levels[2]=levels[2];
        stats[stream].levels[3]=levels[3];
        stats[stream].n_invalid=levels[4];
      }else{
        stats[stream].levels[0]=0;
        stats[stream].levels[1]=levels[0];
        stats[stream].levels[2]=levels[1];
        stats[stream].levels[3]=0;
        stats[stream].n_invalid=levels[2];
      }
    }

    size_t nWrite = sizeof(htimeslice);
    writer->put_bytes(nWrite, (char *)&htimeslice);
    nWrite=sizeof(uvw);
    writer->put_bytes(nWrite, (char *)&uvw[0]);
    nWrite=sizeof(stats);
    writer->put_bytes(nWrite, (char *)&stats[0]);
  }

  integration_buffer_float.resize(number_channels() + 1);
  size_t n = fft_size() / number_channels();

  Output_header_baseline hbaseline;
  for (size_t i = 0; i < baselines.size(); i++) {
    std::pair<size_t,size_t> &stations = baselines[i];

    for (size_t j = 0; j < number_channels() + 1; j++) {
      integration_buffer_float[j] = integration_buffer[i][j * n];
      for (size_t k = 1; k < n && j < number_channels(); k++)
        integration_buffer_float[j] += integration_buffer[i][j * n + k];
      integration_buffer_float[j] /= n;
    }

    hbaseline.weight = 0;       // The number of good samples

    // Station number in the vex-file
    SFXC_ASSERT(stations.first < n_stations);
    SFXC_ASSERT(stations.second < n_stations);
    hbaseline.station_nr1 = stream2station[stations.first];
    // Station number in the vex-file
    hbaseline.station_nr2 = stream2station[stations.second];

    // Polarisation for the first station
    SFXC_ASSERT((polarisation == 0) || (polarisation == 1)); // (RCP: 0, LCP: 1)
    hbaseline.polarisation1 = polarisation;
    hbaseline.polarisation2 = polarisation;
    if (correlation_parameters.cross_polarize) {
      if (stations.first >= n_stations/2)
        hbaseline.polarisation1 = 1-polarisation;
      if (stations.second >= n_stations/2)
        hbaseline.polarisation2 = 1-polarisation;
    }
    // Upper or lower sideband (LSB: 0, USB: 1)
    if (correlation_parameters.sideband=='U') {
      hbaseline.sideband = 1;
    } else {
      SFXC_ASSERT(correlation_parameters.sideband == 'L');
      hbaseline.sideband = 0;
    }
    // The number of the channel in the vex-file,
    hbaseline.frequency_nr = (unsigned char)correlation_parameters.channel_nr;
    // sorted increasingly
    // 1 byte left:
    hbaseline.empty = ' ';

    int nWrite = sizeof(hbaseline);
    writer->put_bytes(nWrite, (char *)&hbaseline);
    writer->put_bytes((number_channels() + 1) * sizeof(std::complex<float>),
                      ((char*)&integration_buffer_float[0]));
  }
}

void
Correlation_core::
auto_correlate_baseline(std::complex<FLOAT> in[],
                        std::complex<FLOAT> out[]) {
  for (int i = 0; i < fft_size() + 1; i++)
    out[i] += (in[i].real()*in[i].real() +
               in[i].imag()*in[i].imag());
}

void
Correlation_core::
correlate_baseline(std::complex<FLOAT> in1[],
                   std::complex<FLOAT> in2[],
                   std::complex<FLOAT> out[]) {
  for (int i = 0; i < fft_size() + 1; i++)
    out[i] += in1[i]*std::conj(in2[i]);
}

void Correlation_core::add_uvw_table(int sn, Uvw_model &table) {
  if (sn>=uvw_tables.size())
    uvw_tables.resize(sn+1);

  uvw_tables[sn]=table;
}
