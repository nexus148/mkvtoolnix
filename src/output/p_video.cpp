/*
   mkvmerge -- utility for splicing together matroska files
   from component media subtypes
  
   Distributed under the GPL
   see the file COPYING for details
   or visit http://www.gnu.org/copyleft/gpl.html
  
   $Id$
  
   video output module
  
   Written by Moritz Bunkus <moritz@bunkus.org>.
*/

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#include "common.h"
#include "hacks.h"
#include "matroska.h"
#include "mpeg4_common.h"
#include "output_control.h"
#include "p_video.h"

extern "C" {
#include <avilib.h>
}

using namespace libmatroska;

video_packetizer_c::video_packetizer_c(generic_reader_c *nreader,
                                       const char *ncodec_id,
                                       double nfps,
                                       int nwidth,
                                       int nheight,
                                       bool nbframes,
                                       track_info_c *nti)
  throw (error_c) : generic_packetizer_c(nreader, nti) {
  char *fourcc;

  fps = nfps;
  width = nwidth;
  height = nheight;
  frames_output = 0;
  bframes = nbframes;
  ref_timecode = -1;
  if (get_cue_creation() == CUE_STRATEGY_UNSPECIFIED)
    set_cue_creation(CUE_STRATEGY_IFRAMES);
  duration_shift = 0;
  bref_frame.type = '?';
  fref_frame.type = '?';
  aspect_ratio_extracted = false;

  set_track_type(track_video);

  mpeg_video = MPEG_VIDEO_NONE;
  if ((ti->private_data != NULL) &&
      (ti->private_size >= sizeof(alBITMAPINFOHEADER))) {
    fourcc = (char *)&((alBITMAPINFOHEADER *)ti->private_data)->bi_compression;
    if (!strncasecmp(fourcc, "DIVX", 4) ||
        !strncasecmp(fourcc, "XVID", 4) ||
        !strncasecmp(fourcc, "DX5", 3))
      mpeg_video = MPEG_VIDEO_V4_LAYER_2;
  }
  if ((mpeg_video == MPEG_VIDEO_NONE) && (ncodec_id != NULL) &&
      !strncmp(ncodec_id, MKV_V_MPEG4_SP, strlen(MKV_V_MPEG4_SP) - 2)) {
    if (!strcmp(ncodec_id, MKV_V_MPEG4_AVC))
      mpeg_video = MPEG_VIDEO_V4_LAYER_10;
    else
      mpeg_video = MPEG_VIDEO_V4_LAYER_2;
  }

  if ((mpeg_video == MPEG_VIDEO_NONE) && (ncodec_id != NULL) &&
      (!strcmp(ncodec_id, "V_MPEG1") || !strcmp(ncodec_id, "V_MPEG2")))
    mpeg_video = (ncodec_id[6] == '1') ? MPEG_VIDEO_V1 : MPEG_VIDEO_V2;

  if (ncodec_id != NULL)
    set_codec_id(ncodec_id);
  else if ((mpeg_video == MPEG_VIDEO_V4_LAYER_2) &&
           hack_engaged(ENGAGE_NATIVE_MPEG4)) {
    if (bframes)
      set_codec_id(MKV_V_MPEG4_ASP);
    else
      set_codec_id(MKV_V_MPEG4_SP);
  } else
    set_codec_id(MKV_V_MSCOMP);
  if (((mpeg_video != MPEG_VIDEO_V4_LAYER_2) ||
       !hack_engaged(ENGAGE_NATIVE_MPEG4)) &&
      (hcodec_id != "") && (hcodec_id == MKV_V_MSCOMP) &&
      (ti->private_data != NULL) &&
      (ti->private_size >= sizeof(alBITMAPINFOHEADER)) &&
      (ti->fourcc[0] != 0))
    memcpy(&((alBITMAPINFOHEADER *)ti->private_data)->bi_compression,
           ti->fourcc, 4);
  if ((mpeg_video != MPEG_VIDEO_V4_LAYER_2) ||
      !hack_engaged(ENGAGE_NATIVE_MPEG4))
    set_codec_private(ti->private_data, ti->private_size);
}

void
video_packetizer_c::set_headers() {
  // Set MinCache to 1 for I- and P-frames. If you only
  // have I-frames then it can be set to 0 (e.g. MJPEG). 2 is needed
  // if there are B-frames as well.
  if (bframes)
    set_track_min_cache(2);
  else
    set_track_min_cache(1);
  if (fps > 0.0)
    set_track_default_duration((int64_t)(1000000000.0 / fps));

  set_video_pixel_width(width);
  set_video_pixel_height(height);

  generic_packetizer_c::set_headers();

  track_entry->EnableLacing(false);
}

// Semantics:
// bref == -1: I frame, no references
// bref == -2: P or B frame, use timecode of last I / P frame as the bref
// bref > 0: P or B frame, use this bref as the backward reference
//           (absolute reference, not relative!)
// fref == 0: P frame, no forward reference
// fref > 0: B frame with given forward reference (absolute reference,
//           not relative!)
int
video_packetizer_c::process(memory_c &mem,
                            int64_t old_timecode,
                            int64_t duration,
                            int64_t bref,
                            int64_t fref) {
  int64_t timecode;
  vector<video_frame_t> frames;
  uint32_t i;

  debug_enter("video_packetizer_c::process");

  if (!aspect_ratio_extracted) {
    if (mpeg_video == MPEG_VIDEO_V4_LAYER_2)
      extract_mpeg4_aspect_ratio(mem.data, mem.size);
    else if (mpeg_video == MPEG_VIDEO_V4_LAYER_10)
      extract_mpeg4_aspect_ratio(ti->private_data, ti->private_size);
    aspect_ratio_extracted = true;
  }

  if ((fps < 0.0) &&
      (mpeg_video == MPEG_VIDEO_V1) || (mpeg_video == MPEG_VIDEO_V2))
    extract_mpeg1_2_fps(mem.data, mem.size);

  if ((mpeg_video == MPEG_VIDEO_V4_LAYER_2) &&
      hack_engaged(ENGAGE_NATIVE_MPEG4))
    mpeg4_find_frame_types(mem.data, mem.size, frames);

  if ((mpeg_video == MPEG_VIDEO_V4_LAYER_2) &&
      hack_engaged(ENGAGE_NATIVE_MPEG4) && (fps != 0.0)) {
    for (i = 0; i < frames.size(); i++) {
      if ((frames[i].type == 'I') ||
          ((frames[i].type != 'B') && (fref_frame.type != '?')))
        flush_frames(frames[i].type);

      if (old_timecode == -1)
        timecode = (int64_t)(1000000000.0 * frames_output / fps) +
          duration_shift;
      else
        timecode = old_timecode;

      if ((duration == -1) || (duration == (int64_t)(1000.0 / fps)))
        duration = (int64_t)(1000000000.0 / fps);
      else
        duration_shift += duration - (int64_t)(1000000000.0 / fps);

      frames_output++;
      frames[i].timecode = timecode;
      frames[i].duration = duration;
      frames[i].data = (unsigned char *)safememdup(&mem.data[frames[i].pos],
                                                   frames[i].size);

      if (frames[i].type == 'I') {
        frames[i].bref = -1;
        frames[i].fref = -1;
        if (bref_frame.type == '?') {
          bref_frame = frames[i];
          memory_c mem(frames[i].data, frames[i].size, false);
          add_packet(mem, frames[i].timecode, frames[i].duration);
        } else
          fref_frame = frames[i];

      } else if (frames[i].type != 'B') {
        frames_output--;
        if (bref_frame.type == '?')
          mxerror("video_packetizer: Found a P frame but no I frame. This "
                  "should not have happened. Either this is a bug in mkvmerge "
                  "or the video stream is damaged.\n");
        frames[i].bref = bref_frame.timecode;
        frames[i].fref = -1;
        fref_frame = frames[i];

      } else {
        if (!bframes) {
          set_codec_id(MKV_V_MPEG4_ASP);
          set_track_min_cache(2);
          rerender_track_headers();
        }
        bframes = true;
        queued_frames.push_back(frames[i]);

      }
    }

    debug_leave("video_packetizer_c::process");

    return FILE_STATUS_MOREDATA;
  }

  if (old_timecode == -1)
    timecode = (int64_t)(1000000000.0 * frames_output / fps) + duration_shift;
  else
    timecode = old_timecode;

  if (duration == -1)
    duration = (int64_t)(1000000000.0 / fps);
  else
    duration_shift += duration - (int64_t)(1000000000.0 / fps);
  frames_output++;

  if (bref == VFT_IFRAME) {
    // Add a key frame and save its timecode so that we can reference it later.
    add_packet(mem, timecode, duration);
    ref_timecode = timecode;
  } else {
    // P or B frame. Use our last timecode if the bref is -2, or the provided
    // one otherwise. The forward ref is always taken from the reader.
    add_packet(mem, timecode, duration, false,
               bref == VFT_PFRAMEAUTOMATIC ? ref_timecode : bref, fref);
    if (fref == VFT_NOBFRAME)
      ref_timecode = timecode;
  }

  debug_leave("video_packetizer_c::process");

  return FILE_STATUS_MOREDATA;
}

video_packetizer_c::~video_packetizer_c() {
}

void
video_packetizer_c::dump_debug_info() {
  mxdebug("video_packetizer_c: queue: %d; frames_output: %d; "
          "ref_timecode: %lld\n", packet_queue.size(), frames_output,
          ref_timecode);
}

void
video_packetizer_c::flush() {
  flush_frames(true);
}

void
video_packetizer_c::extract_mpeg4_aspect_ratio(const unsigned char *buffer,
                                               int size) {
  uint32_t num, den;

  aspect_ratio_extracted = true;
  if (ti->aspect_ratio_given || ti->display_dimensions_given)
    return;

  if (((mpeg_video == MPEG_VIDEO_V4_LAYER_2) &&
       mpeg4_extract_par(buffer, size, num, den))
      ||
      ((mpeg_video == MPEG_VIDEO_V4_LAYER_10) &&
       mpeg4_l10_extract_par(buffer, size, num, den))) {
    ti->aspect_ratio_given = true;
    ti->aspect_ratio = (float)hvideo_pixel_width /
      (float)hvideo_pixel_height * (float)num / (float)den;
    generic_packetizer_c::set_headers();
    rerender_track_headers();
    mxinfo("Track %lld of '%s': Extracted the aspect ratio information "
           "from the MPEG4 video data and set the display dimensions to "
           "%u/%u.\n", (int64_t)ti->id, ti->fname.c_str(),
           (uint32_t)ti->display_width, (uint32_t)ti->display_height);
  }
}

void
video_packetizer_c::extract_mpeg1_2_fps(const unsigned char *buffer,
                                        int size) {
  int idx;

  idx = mpeg1_2_extract_fps_idx(buffer, size);
  if (idx >= 0) {
    fps = mpeg1_2_get_fps(idx);
    if (fps > 0) {
      set_track_default_duration((int64_t)(1000000000.0 / fps));
      rerender_track_headers();
    } else
      fps = 0.0;
  }
}

void
video_packetizer_c::flush_frames(char next_frame,
                                 bool flush_all) {
  uint32_t i;

  if (bref_frame.type == '?') {
    if (fref_frame.type != '?')
      die("video_packetizer: bref_frame.type == '?' but fref_frame.type != "
          "'?'. This should not have happened.\n");
    if (queued_frames.size() > 0) {
      mxwarn("video_packetizer: No I frame found but B frames queued. This "
             "indicates a broken video stream.\n");
      for (i = 0; i < queued_frames.size(); i++)
        safefree(queued_frames[i].data);
      queued_frames.clear();
    }
    return;
  }

  if (fref_frame.type == '?') {
    if (queued_frames.size() != 0) {
      mxwarn("video_packetizer: B frames queued but only one reference frame "
             "found. This indicates a broken video stream, or the frames are "
             "placed in display order which is not supported.\n");
      for (i = 0; i < queued_frames.size(); i++)
        safefree(queued_frames[i].data);
      queued_frames.clear();
    }
    if (flush_all) {
      memory_c mem(bref_frame.data, bref_frame.size, false);
      add_packet(mem, bref_frame.duration, false, bref_frame.bref,
                 bref_frame.fref);
      bref_frame.type = '?';
    }
    return;
  }

  if (fref_frame.type != '?') {
    if ((fref_frame.type == 'P') || (fref_frame.type == 'S'))
      frames_output++;
    fref_frame.timecode = (int64_t)(fref_frame.timecode +
                                    queued_frames.size() * 1000000000 / fps);
    memory_c fref_mem(fref_frame.data, fref_frame.size, false);
    add_packet(fref_mem, fref_frame.timecode,
               fref_frame.duration, false, fref_frame.bref, fref_frame.fref);

    for (i = 0; i < queued_frames.size(); i++) {
      memory_c mem(queued_frames[i].data, queued_frames[i].size, false);
      add_packet(mem, queued_frames[i].timecode, queued_frames[i].duration,
                 false, bref_frame.timecode, fref_frame.timecode);
    }
    queued_frames.clear();

    bref_frame = fref_frame;
    fref_frame.type = '?';
  }

  if (flush_all || ((next_frame == 'I') && (bref_frame.type == 'P')))
    bref_frame.type = '?';
}

connection_result_e
video_packetizer_c::can_connect_to(generic_packetizer_c *src) {
  video_packetizer_c *vsrc;

  vsrc = dynamic_cast<video_packetizer_c *>(src);
  if (vsrc == NULL)
    return CAN_CONNECT_NO_FORMAT;
  if ((width != vsrc->width) || (height != vsrc->height) ||
      (fps != vsrc->fps) || (hcodec_id != vsrc->hcodec_id))
    return CAN_CONNECT_NO_PARAMETERS;
  if (((ti->private_data == NULL) && (vsrc->ti->private_data != NULL)) ||
      ((ti->private_data != NULL) && (vsrc->ti->private_data == NULL)) ||
      (ti->private_size != vsrc->ti->private_size))
    return CAN_CONNECT_NO_PARAMETERS;
  if ((ti->private_data != NULL) &&
      memcmp(ti->private_data, vsrc->ti->private_data, ti->private_size))
    return CAN_CONNECT_NO_PARAMETERS;
  return CAN_CONNECT_YES;
}

// ----------------------------------------------------------------

mpeg_12_video_packetizer_c::
mpeg_12_video_packetizer_c(generic_reader_c *_reader,
                           int _version,
                           double _fps,
                           int _width,
                           int _height,
                           int _dwidth,
                           int _dheight,
                           track_info_c *_ti):
  video_packetizer_c(_reader, "V_MPEG1", _fps, _width, _height, true, _ti) {

  mpeg_video = (_version == 1) ? MPEG_VIDEO_V1 : MPEG_VIDEO_V2;
  set_codec_id(mxsprintf("V_MPEG%d", _version));
  if (!ti->aspect_ratio_given && !ti->display_dimensions_given) {
    ti->display_dimensions_given = true;
    ti->display_width = _dwidth;
    ti->display_height = _dheight;
  }
}

int
mpeg_12_video_packetizer_c::process(memory_c &mem,
                                    int64_t,
                                    int64_t,
                                    int64_t,
                                    int64_t) {
  unsigned char *data_ptr;
  int new_bytes, state;

  state = parser.GetState();
  if ((state == MPV_PARSER_STATE_EOS) ||
      (state == MPV_PARSER_STATE_ERROR))
    return FILE_STATUS_DONE;

  data_ptr = mem.data;
  new_bytes = mem.size;

  do {
    int bytes_to_add;

    bytes_to_add = (parser.GetFreeBufferSpace() < new_bytes) ?
      parser.GetFreeBufferSpace() : new_bytes;
    if (bytes_to_add > 0) {
      parser.WriteData(data_ptr, bytes_to_add);
      data_ptr += bytes_to_add;
      new_bytes -= bytes_to_add;
    }

    state = parser.GetState();
    while (state == MPV_PARSER_STATE_FRAME) {
      MPEGFrame *frame;

      frame = parser.ReadFrame();
      if (frame == NULL)
        break;

      memory_c new_mem(frame->data, frame->size, true);
      video_packetizer_c::process(new_mem, frame->timecode, frame->duration,
                                  frame->firstRef, frame->secondRef);
      frame->data = NULL;
      delete frame;

      state = parser.GetState();
    }
  } while (new_bytes > 0);

  return FILE_STATUS_MOREDATA;
}
