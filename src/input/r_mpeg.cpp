/*
   mkvmerge -- utility for splicing together matroska files
   from component media subtypes
  
   Distributed under the GPL
   see the file COPYING for details
   or visit http://www.gnu.org/copyleft/gpl.html
  
   $Id$
  
   MPEG ES demultiplexer module
  
   Written by Moritz Bunkus <moritz@bunkus.org>.
*/

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "common.h"
#include "error.h"
#include "M2VParser.h"
#include "r_mpeg.h"
#include "p_video.h"

#define PROBESIZE 4
#define READ_SIZE 1024 * 1024

int
mpeg_es_reader_c::probe_file(mm_io_c *mm_io,
                             int64_t size) {
  unsigned char *buf;
  int num_read, i;
  uint32_t value;
  M2VParser parser;

  if (size < PROBESIZE)
    return 0;
  try {
    buf = (unsigned char *)safemalloc(READ_SIZE);
    mm_io->setFilePointer(0, seek_beginning);
    num_read = mm_io->read(buf, READ_SIZE);
    if (num_read < 4) {
      safefree(buf);
      return 0;
    }
    mm_io->setFilePointer(0, seek_beginning);

    // MPEG TS starts with 0x47.
    if (buf[0] == 0x47) {
      safefree(buf);
      return 0;
    }

    // MPEG PS starts with 0x000001ba.
    value = get_uint32_be(buf);
    if (value == MPEGVIDEO_PACKET_START_CODE) {
      safefree(buf);
      return 0;
    }

    // Let's look for a MPEG ES start code inside the first 1 MB.
    for (i = 4; i <= num_read; i++) {
      if (value == MPEGVIDEO_SEQUENCE_START_CODE)
        break;
      if (i < num_read) {
        value <<= 8;
        value |= buf[i];
      }
    }
    safefree(buf);
    if (value != MPEGVIDEO_SEQUENCE_START_CODE)
      return 0;

    // Let's try to read one frame.
    if (!read_frame(parser, *mm_io, READ_SIZE))
      return 0;

  } catch (exception &ex) {
    return 0;
  }

  return 1;
}

mpeg_es_reader_c::mpeg_es_reader_c(track_info_c *nti)
  throw (error_c):
  generic_reader_c(nti) {

  try {
    MPEG2SequenceHeader seq_hdr;
    M2VParser parser;
    MPEGChunk *raw_seq_hdr;

    mm_io = new mm_file_io_c(ti->fname);
    size = mm_io->get_size();

    // Let's find the first frame. We need its information like
    // resolution, MPEG version etc.
    if (!read_frame(parser, *mm_io, 1024 * 1024)) {
      delete mm_io;
      throw "";
    }

    mm_io->setFilePointer(0);
    version = parser.GetMPEGVersion();
    seq_hdr = parser.GetSequenceHeader();
    width = seq_hdr.width;
    height = seq_hdr.height;
    frame_rate = seq_hdr.frameRate;
    aspect_ratio = seq_hdr.aspectRatio;
    raw_seq_hdr = parser.GetRealSequenceHeader();
    if (raw_seq_hdr != NULL) {
      ti->private_data = (unsigned char *)
        safememdup(raw_seq_hdr->GetPointer(), raw_seq_hdr->GetSize());
      ti->private_size = raw_seq_hdr->GetSize();
    }

    mxverb(2, "mpeg_es_reader: v %d width %d height %d FPS %e AR %e\n",
           version, width, height, frame_rate, aspect_ratio);

  } catch (exception &ex) {
    throw error_c("mpeg_es_reader: Could not open the file.");
  }
  if (verbose)
    mxinfo(FMT_FN "Using the MPEG ES demultiplexer.\n", ti->fname.c_str());
}

mpeg_es_reader_c::~mpeg_es_reader_c() {
  delete mm_io;
}

void
mpeg_es_reader_c::create_packetizer(int64_t) {
  if (NPTZR() != 0)
    return;

  add_packetizer(new mpeg_12_video_packetizer_c(this, version, frame_rate,
                                                width, height,
                                                (int)(height * aspect_ratio),
                                                height, ti));

  mxinfo(FMT_TID "Using the MPEG 1/2 video output module.\n",
         ti->fname.c_str(), (int64_t)0);
}

file_status_e
mpeg_es_reader_c::read(generic_packetizer_c *,
                       bool) {
  unsigned char *chunk;
  int num_read;

  chunk = (unsigned char *)safemalloc(20000);
  num_read = mm_io->read(chunk, 20000);
  if (num_read <= 0) {
    safefree(chunk);
    return FILE_STATUS_DONE;
  }

  memory_c mem(chunk, num_read, true);
  PTZR0->process(mem);

  bytes_processed = mm_io->getFilePointer();

  return FILE_STATUS_MOREDATA;
}

bool
mpeg_es_reader_c::read_frame(M2VParser &parser,
                             mm_io_c &in,
                             int64_t max_size) {
  int bytes_probed;

  bytes_probed = 0;
  while (true) {
    int state;

    state = parser.GetState();

    if (state == MPV_PARSER_STATE_NEED_DATA) {
      unsigned char *buffer;
      int bytes_read, bytes_to_read;

      if ((max_size != -1) && (bytes_probed > max_size))
        return false;

      bytes_to_read = (parser.GetFreeBufferSpace() < READ_SIZE) ?
        parser.GetFreeBufferSpace() : READ_SIZE;
      buffer = new unsigned char[bytes_to_read];
      bytes_read = in.read(buffer, bytes_to_read);
      if (bytes_read == 0) {
        delete [] buffer;
        break;
      }
      bytes_probed += bytes_read;

      parser.WriteData(buffer, bytes_read);
      delete [] buffer;

    } else if (state == MPV_PARSER_STATE_FRAME)
      return true;

    else if ((state == MPV_PARSER_STATE_EOS) ||
             (state == MPV_PARSER_STATE_ERROR))
      return false;
  }

  return false;
}

int
mpeg_es_reader_c::get_progress() {
  return 100 * bytes_processed / size;
}

void
mpeg_es_reader_c::identify() {
  mxinfo("File '%s': container: MPEG elementary stream (ES)\n" 
         "Track ID 0: video (MPEG %d)\n", ti->fname.c_str(), version);
}

// ------------------------------------------------------------------------

#define PS_PROBE_SIZE 1024 * 1024

int
mpeg_ps_reader_c::probe_file(mm_io_c *mm_io,
                             int64_t size) {
  try {
    autofree_ptr<unsigned char> af_buf(safemalloc(PS_PROBE_SIZE));
    unsigned char *buf = af_buf;
    int num_read;

    mm_io->setFilePointer(0, seek_beginning);
    num_read = mm_io->read(buf, PS_PROBE_SIZE);
    if (num_read < 4)
      return 0;
    mm_io->setFilePointer(0, seek_beginning);

    if (get_uint32_be(buf) != MPEGVIDEO_PACKET_START_CODE)
      return 0;

    return 1;

  } catch (exception &ex) {
    return 0;
  }
}

mpeg_ps_reader_c::mpeg_ps_reader_c(track_info_c *nti)
  throw (error_c):
  generic_reader_c(nti) {
  try {
    uint32_t header;
    uint8_t byte;
    bool streams_found[256], done;
    int i;

    mm_io = new mm_file_io_c(ti->fname);
    size = mm_io->get_size();

    bytes_processed = 0;

    memset(streams_found, 0, sizeof(bool) * 256);
    header = mm_io->read_uint32_be();
    done = mm_io->eof();
    version = -1;

    while (!done) {
      uint8_t stream_id;
      uint16_t pes_packet_length;

      switch (header) {
        case MPEGVIDEO_PACKET_START_CODE:
          mxverb(3, "mpeg_ps: packet start at %lld\n",
                 mm_io->getFilePointer() - 4);

          if (version == -1) {
            byte = mm_io->read_uint8();
            if ((byte & 0xc0) != 0)
              version = 2;      // MPEG-2 PS
            else
              version = 1;
            mm_io->skip(-1);
          }

          mm_io->skip(2 * 4);   // pack header
          if (version == 2) {
            mm_io->skip(1);
            byte = mm_io->read_uint8() & 0x07;
            mm_io->skip(byte);  // stuffing bytes
          }
          header = mm_io->read_uint32_be();
          break;

        case MPEGVIDEO_SYSTEM_HEADER_START_CODE:
          mxverb(3, "mpeg_ps: system header start code at %lld\n",
                 mm_io->getFilePointer() - 4);

          mm_io->skip(2 * 4);   // system header
          byte = mm_io->read_uint8();
          while ((byte & 0x80) == 0x80) {
            mm_io->skip(2);     // P-STD info
            byte = mm_io->read_uint8();
          }
          mm_io->skip(-1);
          header = mm_io->read_uint32_be();
          break;

        case MPEGVIDEO_MPEG_PROGRAM_END_CODE:
          done = true;
          break;

        default:
          if (!mpeg_is_start_code(header)) {
            mxverb(3, "mpeg_ps: unknown header 0x%08x at %lld\n",
                   header, mm_io->getFilePointer() - 4);
            done = true;
            break;
          }

          stream_id = header & 0xff;
          streams_found[stream_id] = true;
          pes_packet_length = mm_io->read_uint16_be();
          mxverb(3, "mpeg_ps: id 0x%02x len %u at %lld\n", stream_id,
                 pes_packet_length, mm_io->getFilePointer() - 4 - 2);

          switch (stream_id) {
            
          }

          mm_io->skip(pes_packet_length);

          header = mm_io->read_uint32_be();
          while (!mpeg_is_start_code(header) && !mm_io->eof() &&
                 (mm_io->getFilePointer() < PS_PROBE_SIZE)) {
            header <<= 8;
            header |= mm_io->read_uint8();
          }
          if (!mpeg_is_start_code(header))
            done = true;

          break;
      }

      done |= mm_io->eof() || (mm_io->getFilePointer() >= PS_PROBE_SIZE);
    } // while (!done)

    mxverb(3, "mpeg_ps: Streams found: ");
    for (i = 0; i < 256; i++)
      if (streams_found[i])
        mxverb(3, "%02x ", i);
    mxverb(3, "\n");

  } catch (exception &ex) {
    throw error_c("mpeg_ps_reader: Could not open the file.");
  }
  if (verbose)
    mxinfo(FMT_FN "Using the MPEG PS demultiplexer.\n", ti->fname.c_str());
}

mpeg_ps_reader_c::~mpeg_ps_reader_c() {
  delete mm_io;
}

void
mpeg_ps_reader_c::create_packetizer(int64_t) {
}

file_status_e
mpeg_ps_reader_c::read(generic_packetizer_c *,
                       bool) {
  return FILE_STATUS_DONE;
}

int
mpeg_ps_reader_c::get_progress() {
  return 100 * bytes_processed / size;
}

void
mpeg_ps_reader_c::identify() {
  mxinfo("File '%s': container: MPEG %d program stream (PS)\n",
         ti->fname.c_str(), version);
}

