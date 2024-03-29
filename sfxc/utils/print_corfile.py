#!/usr/bin/env python
import sys, struct, pdb
from numpy import *
from optparse import OptionParser

timeslice_header_size = 16
uvw_header_size = 32
stat_header_size = 24
baseline_header_size = 8
max_stations = 32
fringe_guard = 0.05  # Used to compute the SNR, this is the percentage that is ignored around the maximum

def parse_global_header(infile, doprint):
  stations = None
  sources = None
  infile.seek(0)
  gheader_buf = infile.read(global_header_size)
  if global_header_size == 64:
    global_header = struct.unpack('i32s2h5ib3c',gheader_buf[:64])
  elif global_header_size == 84:
    global_header = struct.unpack('i32s2h5ib15s2i',gheader_buf[:84])
  else:
    global_header = struct.unpack('i32s2h5ib15s2i4h',gheader_buf[:92])
    splitted = gheader_buf[92:].split('\0')
    nstations = global_header[13]
    nsources = global_header[15]
    stations = splitted[:nstations]
    sources = splitted[nstations:(nstations+nsources)]

  hour = global_header[4] / (60*60)
  minute = (global_header[4]%(60*60))/60
  second = global_header[4]%60
  inttime = global_header[6]
  nchan = global_header[5]
 
  if doprint: 
    if global_header_size == 64:
      print "SFXC version = %s"%(global_header[8])
    else:
      n = global_header[10].index('\0')
      print "SFXC version = %s, branch = %s, jobnr = %d, subjobnr = %d"%(global_header[8], global_header[10][:n], global_header[11], global_header[12])

    pol = ['LL', 'RR', 'LL+RR', 'LL+RR+LR+RL'][global_header[9]]
    n = global_header[1].index('\0')
    print "Experiment %s, date = %dy%03dd%02dh%02dm%02ds, int_time = %d, nchan = %d, polarization = %s"%(global_header[1][:n], global_header[2], global_header[3], hour, minute, second, inttime, nchan, pol)

    if global_header_size >= 92:
        print 'Stations =', ", ".join(stations)
        print 'Sources =', ", ".join(sources)

  start_date = (global_header[2], global_header[3], 60 * (hour*60 + minute) + second)
  return start_date, nchan, inttime, stations, sources

def make_time_string(start_date, inttime, slicenr):
    newsec = start_date[2] + slicenr * (inttime / 1000000.)
    days = int(newsec / 86400)
    newsec -= days * 86400
    newday = start_date[1] + days
    daysperyear = 366 if (start_date[0] % 4 == 0) else 365
    newyear = start_date[0] + int(newday / daysperyear)
    newday %= daysperyear
    hours = int(newsec / 3600)
    newsec -= hours * 3600
    minutes = int(newsec / 60)
    second = newsec - minutes * 60
    return "%dy%03dd%02dh%02dm%09.6fs"%(newyear, newday, hours, minutes, second)

def print_uvw(uvws, stations=None, sources=None):
  for uvw in uvws.iteritems():
    if stations == None:
      print "Station", uvw[0]
    else:
      print "Station", stations[uvw[0]]
    for src, nstr in uvw[1]:
      if sources != None:
        print "Source {}: {}".format(sources[src], nstr)
      else:
        print nstr

def print_stats(stats, stations=None):
  for stat in stats.iteritems():
    if stations == None:
      print "Station", stat[0]
    else:
      print "Station", stations[stat[0]]
    for nstr in stat[1]:
      print nstr

def print_baselines(data, stations=None):
  keys = data.keys()
  keys.sort()
  for key in keys:
    bl = data[key]
    if stations == None:
      print "Baseline: station1 = %s, station2 = %s"%(key[0], key[1])
    else:
      print "Baseline: station1 = %s, station2 = %s"%(stations[key[0]], stations[key[1]])
    for nstr in bl:
      print nstr

def get_baseline_stats(data):
  data[0] = data[0].real
  real_data = abs(fft.fftshift(fft.irfft(data)))
  n = real_data.size
  fringe_pos = real_data.argmax()
  fringe_val = real_data[fringe_pos]
  guard = max(int(round(real_data.size * fringe_guard)), 1)
  r1_min = max(0, fringe_pos + guard + 1 - n)
  r1_max = max(fringe_pos - guard,0)
  r2_min = min(fringe_pos + guard + 1, n)
  r2_max = min(n, n+ fringe_pos - guard)
  avg  = real_data[r1_min:r1_max].sum()
  avg += real_data[r2_min:r2_max].sum()
  navg = (r2_max-r2_min) + (r1_max-r1_min)
  avg /= max(navg, 1)
  noise  = pow(real_data[r1_min:r1_max] - avg, 2).sum()
  noise += pow(real_data[r2_min:r2_max] - avg, 2).sum()
  snr = sqrt(((fringe_val - avg)**2) * navg / max(noise, 1e-12))
  fringe_offset = fringe_pos - n / 2
  return (fringe_val, snr, fringe_offset)

def read_uvw(infile, uvw, nuvw):
  uvw_buffer = infile.read(uvw_header_size * nuvw)
  if len(uvw_buffer) != uvw_header_size * nuvw:
    raise Exception("EOF")
  index = 0
  for i in range(nuvw):
    header = struct.unpack('2i3d', uvw_buffer[index:index + uvw_header_size])
    index += uvw_header_size
    station_nr = header[0]
    # NB this is only valid in SFXC versions newer than stable 3.5
    source_nr = header[1] 
    (u,v,w) = header[2:]
    nstr = 'u = %.15g, v = %.15g, w = %.15g'%(u,v,w)
    try:
      uvw[station_nr].append((source_nr, nstr))
    except KeyError:
      uvw[station_nr] = [(source_nr, nstr)]

def read_statistics(infile, stats, nstatistics):
  stat_buffer = infile.read(stat_header_size * nstatistics)
  if len(stat_buffer) != stat_header_size * nstatistics:
    raise Exception("EOF")
  index = 0
  for i in range(nstatistics):
    sheader = struct.unpack('4B5i', stat_buffer[index:index + stat_header_size])
    index += stat_header_size
    station_nr = sheader[0]
    frequency_nr = sheader[1]
    sideband = sheader[2]
    polarisation = sheader[3]
    levels = sheader[4:9]
    tot = sum(levels) + 0.0001
    levels = [a / tot for a in levels]
    nstr = 'freq = %d, sb = %d, pol = %d, levels: -- %.3f, -+ %.3f, +- %.3f, ++ %.3f, invalid %.3f' % (frequency_nr, sideband, polarisation, levels[0], levels[1], levels[2], levels[3], levels[4])
    try:
      stats[station_nr].append(nstr)
    except KeyError:
      stats[station_nr] = [nstr]

def read_baselines(infile, data, nbaseline, nchan, printauto):
  baseline_data_size = (nchan + 1) * 8 # data is complex floats
  baseline_buffer = infile.read(nbaseline * (baseline_header_size + baseline_data_size)) 
  if len(baseline_buffer) != nbaseline * (baseline_header_size + baseline_data_size):
    raise Exception("EOF")
  fmt = str(2*(nchan+1)) + 'f'

  index = 0
  for b in range(nbaseline):
    bheader = struct.unpack('i4B', baseline_buffer[index:index + baseline_header_size])
    index += baseline_header_size
    weight = bheader[0]
    station1 = bheader[1]
    station2 = bheader[2]
    baseline = (station1, station2)
    byte = bheader[3]
    pol = byte&3
    sideband = (byte>>2)&1
    freq_nr = byte>>3
    J = complex(0,1)
    if (station1 != station2) or (station1 == station2 and printauto):
      buf = struct.unpack(fmt, baseline_buffer[index:index + baseline_data_size])
      # Skip over the first/last channel
      vreal = array(buf[0:2*(nchan+1):2])
      vim = array(buf[1:2*(nchan+1):2])
      if isnan(vreal).any()==False and isnan(vim).any()==False:
        #pdb.set_trace()
        # format is [nbaseline, nif, num_sb, npol, nchan+1], dtype=complex128
        if (station1 == station2):
          amp_real = (sum(vreal[1:-1]) + vreal[0]/2 + vreal[-1]/2) / (nchan)
          amp_imag = sum(vim) / (nchan + 1)
          nstr = 'freq = %d, sb = %d, pol = %d, ampl_real = %.6e, ampl_imag = %.6e, weight = %.6f'%(freq_nr, sideband, pol, amp_real, amp_imag, weight)
        else:
          val, snr, offset = get_baseline_stats(vreal + J*vim)
          nstr = 'freq = %d, sb = %d, pol = %d, fringe ampl = %.6f, SNR = %.6f, offset = %d, weight = %.6f'%(freq_nr, sideband, pol, val, snr, offset, weight)
        try:
          data[baseline].append(nstr)
        except KeyError:
          data[baseline] = [nstr]
      else:
        print "b="+`baseline`+", freq_nr = "+`freq_nr`+",sb="+`sideband`+",pol="+`pol`
        pdb.set_trace()
    index += baseline_data_size

def read_time_slice(infile, stats, uvw, data, nchan, printauto):
  #get timeslice header
  tsheader_buf = infile.read(timeslice_header_size)
  if len(tsheader_buf) != timeslice_header_size:
    raise Exception("EOF")
  timeslice_header = struct.unpack('4i', tsheader_buf)
  current_slice = timeslice_header[0]  
  
  while current_slice == timeslice_header[0]:
    #Read UVW coordinates
    nuvw = timeslice_header[2]
    read_uvw(infile, uvw, nuvw)
    # Read the bit statistics
    nstatistics = timeslice_header[3]
    read_statistics(infile, stats, nstatistics)
    # Read the baseline data    
    nbaseline = timeslice_header[1]
    read_baselines(infile, data, nbaseline, nchan, printauto)
    # Get next time slice header
    tsheader_buf = infile.read(timeslice_header_size)
    if len(tsheader_buf) != timeslice_header_size:
      return current_slice 
    timeslice_header = struct.unpack('4i', tsheader_buf)
  # We read one time slice to many
  infile.seek(-timeslice_header_size, 1) 
  return current_slice

def get_stations(vex_file):
  f = open(vex_file, 'r')
  stations = []
  rawline = f.readline()
  line = rawline.lstrip().upper()
  while rawline != "" and line[:9] != "$STATION;":
    rawline = f.readline()
    line = rawline.lstrip().upper()
  # Now get the station names
  rawline = f.readline()
  line = rawline.lstrip()
  while rawline != "" and line[:1] != "$":
    if line[:4] == "def ":
      z = line.find(';')
      stations.append(line[4:z].lstrip())
    rawline = f.readline()
    line = rawline.lstrip()

  if len(stations) == 0:
    print "Error, no stations found in vex_file : ", vex_file
    sys.exit(1)
  stations.sort()
  return stations

def get_options():
  parser = OptionParser('%prog [options] <correlator output file>')
  parser.add_option('-S', '--no-sampler-stats', action='store_false', dest="printstats", 
                    default=True, help='Do not print sampler statistics')
  parser.add_option('-V', '--no-visibilities', action='store_false', dest="printvis", 
                    default=True, help='Do not print visibilities')
  parser.add_option('-U', '--no-uvw', action='store_false', dest="printuvw", 
                    default=True, help='Do not print UVW coordinates')
  parser.add_option('-a', '--auto-correlations', action='store_true', dest="printauto",
                    default=False, help='Print auto-correlations')
  parser.add_option('-n', '--no-header', action='store_true',
                    default=False, help='Do not print the global header')
  parser.add_option('-v', '--vex', dest="vex_file", type="string",
                    help='Get station names from vex file (has to be the same as used during correlation)')
  (opts, args) = parser.parse_args()
  if len(args) == 0:
    parser.print_help()
    parser.exit()
  if len(args) != 1:
    parser.error('No correlator file specified')
  return (args[0], opts.no_header, opts.printstats, opts.printvis, opts.printuvw, opts.printauto, opts.vex_file)

############################## Main program ##########################

filename, noheader, printstats, printvis, printuvw, printauto, vex_file = get_options()

try:
  infile = open(filename, 'rb')
except:
  print "Could not open file : " + filename
  sys.exit()

# Read global header
global_header_size = struct.unpack('i', infile.read(4))[0]
start_date, nchan, inttime, stations, sources = parse_global_header(infile, not noheader)

# Get list of station names from vex file (older SFXC versions didn't store
# this information in the global headers)
if vex_file != None:
    stations = get_stations(vex_file)

infile.seek(0)
gheader_buf = infile.read(global_header_size)
global_header = struct.unpack('i32s2h5i4c',gheader_buf[:64])
while True:
  stats = {}
  uvw = {}
  data = {}
  try:
    slicenr = read_time_slice(infile, stats, uvw, data, nchan, printauto)
  except Exception, e:
    if e.args[0] != 'EOF':
      raise
    print "Reached end of file"
    sys.exit(0)
   
  t = make_time_string(start_date, inttime, slicenr)
  print "---------- time slice %d, t = %s ---------"%(slicenr, t)
  if printstats:
    print_stats(stats, stations)

  if printuvw:
    print_uvw(uvw, stations, sources)

  if printvis:
    print_baselines(data, stations)
