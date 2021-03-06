/*
   mkvmerge -- utility for splicing together matroska files
   from component media subtypes

   Distributed under the GPL v2
   see the file COPYING for details
   or visit http://www.gnu.org/copyleft/gpl.html

   DTS output module

   Written by Peter Niemayer <niemayer@isg.de>.
   Modified by Moritz Bunkus <moritz@bunkus.org>.
*/

#include "common/common_pch.h"

#include "common/codec.h"
#include "common/dts.h"
#include "merge/connection_checks.h"
#include "merge/output_control.h"
#include "output/p_dts.h"

using namespace libmatroska;

dts_packetizer_c::dts_packetizer_c(generic_reader_c *p_reader,
                                   track_info_c &p_ti,
                                   mtx::dts::header_t const &dtsheader)
  : generic_packetizer_c(p_reader, p_ti)
  , m_packet_buffer(128 * 1024)
  , m_first_header(dtsheader)
  , m_previous_header(dtsheader)
  , m_skipping_is_normal(false)
  , m_reduce_to_core{get_option_for_track(m_ti.m_reduce_to_core, m_ti.m_id)}
  , m_timestamp_calculator{static_cast<int64_t>(m_first_header.core_sampling_frequency)}
{
  set_track_type(track_audio);
}

dts_packetizer_c::~dts_packetizer_c() {
}

memory_cptr
dts_packetizer_c::get_dts_packet(mtx::dts::header_t &dtsheader,
                                 bool flushing) {
  if (0 == m_packet_buffer.get_size())
    return nullptr;

  const unsigned char *buf = m_packet_buffer.get_buffer();
  int buf_size             = m_packet_buffer.get_size();
  int pos                  = mtx::dts::find_sync_word(buf, buf_size);

  if (0 > pos) {
    if (4 < buf_size)
      m_packet_buffer.remove(buf_size - 4);
    return nullptr;
  }

  if (0 < pos) {
    m_packet_buffer.remove(pos);
    buf      = m_packet_buffer.get_buffer();
    buf_size = m_packet_buffer.get_size();
  }

  pos = mtx::dts::find_header(buf, buf_size, dtsheader, flushing);

  if ((0 > pos) || (static_cast<int>(pos + dtsheader.frame_byte_size) > buf_size))
    return nullptr;

  if ((1 < verbose) && (dtsheader != m_previous_header)) {
    mxinfo(Y("DTS header information changed! - New format:\n"));
    dtsheader.print();
    m_previous_header = dtsheader;
  }

  if (verbose && (0 < pos) && !m_skipping_is_normal) {
    int i;
    bool all_zeroes = true;

    for (i = 0; i < pos; ++i)
      if (buf[i]) {
        all_zeroes = false;
        break;
      }

    if (!all_zeroes)
      mxwarn_tid(m_ti.m_fname, m_ti.m_id, boost::format(Y("Skipping %1% bytes (no valid DTS header found). This might cause audio/video desynchronisation.\n")) % pos);
  }

  auto bytes_to_remove = pos + dtsheader.frame_byte_size;

  if (   m_reduce_to_core
      && dtsheader.has_core
      && dtsheader.has_exss
      && (dtsheader.exss_part_size > 0)
      && (dtsheader.exss_part_size < static_cast<int>(dtsheader.frame_byte_size))) {
    dtsheader.frame_byte_size -= dtsheader.exss_part_size;
    dtsheader.has_exss         = false;
  }

  auto packet_buf = memory_c::clone(buf + pos, dtsheader.frame_byte_size);

  m_packet_buffer.remove(bytes_to_remove);

  return packet_buf;
}

void
dts_packetizer_c::set_headers() {
  set_codec_id(MKV_A_DTS);
  set_audio_sampling_freq(m_first_header.core_sampling_frequency);
  set_audio_channels(m_reduce_to_core ? m_first_header.get_core_num_audio_channels() : m_first_header.get_total_num_audio_channels());
  set_track_default_duration(m_first_header.get_packet_length_in_nanoseconds().to_ns());
  if (m_first_header.source_pcm_resolution > 0)
    set_audio_bit_depth(m_first_header.source_pcm_resolution);

  generic_packetizer_c::set_headers();
}

int
dts_packetizer_c::process(packet_cptr packet) {
  m_timestamp_calculator.add_timecode(packet);

  m_packet_buffer.add(packet->data->get_buffer(), packet->data->get_size());

  queue_available_packets(false);
  process_available_packets();

  return FILE_STATUS_MOREDATA;
}

void
dts_packetizer_c::queue_available_packets(bool flushing) {
  mtx::dts::header_t dtsheader;
  memory_cptr dts_packet;

  while ((dts_packet = get_dts_packet(dtsheader, flushing))) {
    m_queued_packets.emplace_back(std::make_pair(dtsheader, dts_packet));

    if (!m_first_header.core_sampling_frequency && dtsheader.core_sampling_frequency) {
      m_first_header.core_sampling_frequency = dtsheader.core_sampling_frequency;
      m_timestamp_calculator                  = timestamp_calculator_c{static_cast<int64_t>(m_first_header.core_sampling_frequency)};

      set_audio_sampling_freq(m_first_header.core_sampling_frequency);

      rerender_track_headers();
    }
  }
}

void
dts_packetizer_c::process_available_packets() {
  if (!m_first_header.core_sampling_frequency)
    return;

  for (auto const &header_and_packet : m_queued_packets) {
    auto samples_in_packet = header_and_packet.first.get_packet_length_in_core_samples();
    auto new_timecode      = m_timestamp_calculator.get_next_timecode(samples_in_packet);

    add_packet(std::make_shared<packet_t>(header_and_packet.second, new_timecode.to_ns(), header_and_packet.first.get_packet_length_in_nanoseconds().to_ns()));
  }

  m_queued_packets.clear();
}

void
dts_packetizer_c::flush_impl() {
  queue_available_packets(true);
  process_available_packets();
}

connection_result_e
dts_packetizer_c::can_connect_to(generic_packetizer_c *src,
                                 std::string &error_message) {
  dts_packetizer_c *dsrc = dynamic_cast<dts_packetizer_c *>(src);
  if (!dsrc)
    return CAN_CONNECT_NO_FORMAT;

  connect_check_a_samplerate(m_first_header.core_sampling_frequency, dsrc->m_first_header.core_sampling_frequency);
  connect_check_a_channels(m_first_header.audio_channels, dsrc->m_first_header.audio_channels);

  return CAN_CONNECT_YES;
}
