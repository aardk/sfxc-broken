#include "data_reader_blocking.h"
#include "vdif_reader.h"

VDIF_reader::VDIF_reader(shared_ptr<Data_reader> data_reader,
			 Data_frame &data, Time ref_time)
  : Input_data_format_reader(data_reader),
    debug_level_(CHECK_PERIODIC_HEADERS),
    sample_rate(0), first_header_seen(false),
    frame_size(0)
{
  ref_jday = (int)ref_time.get_mjd();
}

VDIF_reader::~VDIF_reader() {}

bool 
VDIF_reader::open_input_stream(Data_frame &data) {
  if (!read_new_block(data)) {
    return false;
  }

  is_open_ = true;
  current_time_ = get_current_time();
  data.start_time = current_time_;
  int32_t epoch_jday = current_header.jday_epoch();
  uint32_t start_sec = current_header.sec_from_epoch;
  uint32_t epoch = current_header.ref_epoch;
  LOG_MSG("Start of VDIF data at jday=" << epoch_jday + start_sec / (24 * 60 * 60)
           << ", seconds in epoch = " << start_sec << ", epoch=" << epoch 
           << ", t=" <<  current_time_);
  int data_frame_size = first_header.dataframe_length * 8 - 32 + 
                        16 * first_header.legacy_mode;
  if (frame_size != data_frame_size) 
    LOG_MSG("WARNING: Frame size in vexfile is " << frame_size 
            << ", while frame size according to data is " << data_frame_size
            << "\n");
  return true;
}

void 
VDIF_reader::print_header() {
  std::cerr << ID_OF_NODE << "------------ full header ------------\n";
  std::cerr << current_header.sec_from_epoch<<" ; " << (int)current_header.legacy_mode << " ; "
            << (int)current_header.invalid<<"\n";
  std::cerr << current_header.dataframe_in_second << " ; " << (int)current_header.ref_epoch
            << " ; " << (int) current_header.unassiged<<"\n";
  std::cerr << current_header.dataframe_length << " ; " << (int) current_header.log2_nchan << " ; "
            << (int)current_header.version<<"\n";
  std::cerr << (int) current_header.station_id << " ; " << (int)current_header.thread_id << " ; "
            << (int) current_header.bits_per_sample << " ;" << (int) current_header.data_type<<"\n";
  std::cerr << current_header.user_data1<<" ; "<<(int)current_header.edv<<"\n";
  std::cerr << current_header.user_data2<<" ; "<<current_header.user_data3<<" ; "<< current_header.user_data4<<"\n";
  std::cerr << ID_OF_NODE << "-------------------------------------\n";
}

Time
VDIF_reader::goto_time(Data_frame &data, Time time) {
  if (!data_reader_->is_seekable()) {
    while (time > get_current_time()) {
      if (!read_new_block(data))
        break;
    }
  } else if (time > get_current_time()) {
    // FIXME nthreads will be smaller than the actual number of threads when
    // correlating a subset of all channels, causing goto_time to take smaller steps. 
    // Currently, the input node doesn't know how many threads were recorded to disk.
    const int nthreads = thread_map.size();
    const size_t header_size = (first_header.legacy_mode == 0) ? 32 : 16;
    const size_t vdif_block_size = vdif_frames_per_block * (header_size + frame_size);

    // first search until we are within 1 sec from requested time
    const Time one_sec(1000000.);
    Time delta_time = time - get_current_time();
    while (delta_time >= one_sec) {
      size_t n_blocks = nthreads * (one_sec / time_between_headers_);
      // Don't read the last header, to be able to check whether we are at the right time
      size_t bytes_to_read = (n_blocks-1) * vdif_block_size;
      size_t byte_read = Data_reader_blocking::get_bytes_s( data_reader_.get(), bytes_to_read, NULL );
      if (bytes_to_read != byte_read)
        return get_current_time();

      // Read last block:
      read_new_block(data);
      delta_time = time - get_current_time();
    }
    // Now read the last bit of data up to the requested time
    while (time > get_current_time()) {
      if (!read_new_block(data))
        break;
    }
  }
  return get_current_time();
}

Time
VDIF_reader::get_current_time() {
  Time time;

  if (is_open_) {
    double seconds_since_reference = (double)current_header.sec_from_epoch - (ref_jday - current_header.jday_epoch()) * 24 * 60 * 60;
    double subsec = 0;
    if (sample_rate > 0) {
      int samples_per_frame = 8 * frame_size / ((first_header.bits_per_sample + 1) * (1 << first_header.log2_nchan));
      subsec = (double)current_header.dataframe_in_second * samples_per_frame / sample_rate;
    }
    time.set_time(ref_jday, seconds_since_reference + subsec);
  }

  return time - offset;
}

bool
VDIF_reader::read_new_block(Data_frame &data) {
  std::vector<value_type> &buffer = data.buffer->data;
  const int max_restarts = 256;
  int restarts = 0;

 restart:
  if (!first_header_seen) {
    Data_reader_blocking::get_bytes_s(data_reader_.get(), 16, (char *)&current_header);
    if (data_reader_->eof())
      return false;
    if (check_header(current_header) == INVALID) {
      // NB we default to non-legacy VDIF, at this point there is no way to tell
      Data_reader_blocking::get_bytes_s(data_reader_.get(), frame_size + 16, NULL);
      if (++restarts > max_restarts)
        return false;
      goto restart;
    }
    memcpy(&first_header, &current_header, 16);
    first_header_seen = true;
    if (first_header.legacy_mode == 0) {
      // FIXME : If first header has fill pattern this will fail
      // We should use the information that vex2 provides
      char *header = (char *)&current_header;
      Data_reader_blocking::get_bytes_s(data_reader_.get(), 16, (char *)&header[16]);
      if (data_reader_->eof())
	return false;
    }
  } else {
    if (first_header.legacy_mode == 0) {
      Data_reader_blocking::get_bytes_s(data_reader_.get(), 32, (char *)&current_header);
    } else {
      Data_reader_blocking::get_bytes_s(data_reader_.get(), 16, (char *)&current_header);
    }
    if (data_reader_->eof())
      return false;
  }

  if (buffer.size() == 0)
    buffer.resize(size_data_block());
  
  // NB: This check rejects frames with an invalid header (== bad headers whoose contents cannot be trusted, 
  //     not frames with the invalid bit set), and frames for which dataframe_in_second % vdif_frames_per_block != 0 
  if (check_header(current_header) != VALID) {
    Data_reader_blocking::get_bytes_s(data_reader_.get(), frame_size, NULL);
    if (++restarts > max_restarts)
      return false;
    goto restart;
  }

  Data_reader_blocking::get_bytes_s( data_reader_.get(), frame_size, (char *)&buffer[0]);
  if (data_reader_->eof())
    return false;

  if (current_header.invalid > 0) {
    struct Input_node_types::Invalid_block invalid;
    invalid.invalid_begin = 0;
    invalid.nr_invalid = frame_size;
    data.invalid.push_back(invalid);
    if (thread_map.count(current_header.thread_id) > 0)
      data.channel = thread_map[current_header.thread_id];
    else
      data.channel = 0;
  } else {
    if (thread_map.count(current_header.thread_id) == 0) {
      // If this is the only thread we obtain its thread_id from the data
      if(thread_map.size() == 0) {
        thread_map[current_header.thread_id] = 0;
      } else {
	if (++restarts > max_restarts)
	  return false;
        goto restart;
      }
    }
    data.channel = thread_map[current_header.thread_id];
  }

  for (int i = 1; i < vdif_frames_per_block; i++) {
    Header header;
    if (first_header.legacy_mode == 0) {
      Data_reader_blocking::get_bytes_s( data_reader_.get(), 32, (char *)&header);
    } else {
      Data_reader_blocking::get_bytes_s( data_reader_.get(), 16, (char *)&header);
    }
    if (data_reader_->eof())
      return false;

    Data_reader_blocking::get_bytes_s( data_reader_.get(), frame_size, (char *)&buffer[i * frame_size]);
    if (data_reader_->eof())
      return false;

    if (header.invalid > 0) {
      struct Input_node_types::Invalid_block invalid;
      invalid.invalid_begin = i * frame_size;
      invalid.nr_invalid = frame_size;
      data.invalid.push_back(invalid);
    }
  }

  data.start_time = get_current_time();
  return true;
}

bool VDIF_reader::eof() {
  return data_reader_->eof();
}

int32_t VDIF_reader::Header::jday_epoch() const {
  int year = 2000 + ref_epoch / 2;
  int month = 1 + 6 * (ref_epoch % 2);
  return mjd(1, month, year);
}

void VDIF_reader::set_parameters(const Input_node_parameters &param) {
  sample_rate = param.sample_rate();
  SFXC_ASSERT(((uint64_t)sample_rate % 1000000) == 0);
  offset = param.offset;

  // Create a mapping from thread ID to channel number.
  // If n_tracks > 0 then the data contains a single VDIF thread,
  // the thread_id of this thread comes from the actual data
  thread_map.clear();
  frame_size = param.frame_size;
  if (param.n_tracks == 0) {
    for (size_t i = 0; i < param.channels.size(); i++) {
      if (param.channels[i].tracks[0] >= 0)
	thread_map[param.channels[i].tracks[0]] = i;
    }
    time_between_headers_ = Time(frame_size * 8.e6 / (sample_rate * param.bits_per_sample()));
    bits_per_complete_sample = param.bits_per_sample();
    vdif_frames_per_block = 1;
  } else {
    vdif_frames_per_block = std::max(1, VDIF_FRAME_BUFFER_SIZE / frame_size);
    uint64_t bits_per_second = param.sample_rate() * param.n_tracks;
    while ((bits_per_second % (vdif_frames_per_block * frame_size * 8)) != 0 && vdif_frames_per_block > 1)
      vdif_frames_per_block--;
    time_between_headers_ = Time(vdif_frames_per_block * frame_size * 8.e6 / (sample_rate * param.n_tracks));
    bits_per_complete_sample = param.n_tracks;
  }
  SFXC_ASSERT(time_between_headers_.get_time_usec() > 0);
}
