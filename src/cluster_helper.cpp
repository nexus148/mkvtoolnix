/*
  mkvmerge -- utility for splicing together matroska files
      from component media subtypes

  cluster_helper.cpp

  Written by Moritz Bunkus <moritz@bunkus.org>

  Distributed under the GPL
  see the file COPYING for details
  or visit http://www.gnu.org/copyleft/gpl.html
*/

/*!
    \file
    \version $Id$
    \brief cluster helper
    \author Moritz Bunkus <moritz@bunkus.org>
*/

#include <assert.h>

#include <vector>

#include <matroska/KaxBlockData.h>

#include "cluster_helper.h"
#include "common.h"
#include "mkvmerge.h"
#include "p_video.h"

#include <ebml/StdIOCallback.h>

vector<splitpoint_t *> cluster_helper_c::splitpoints;

//#define walk_clusters() check_clusters(__LINE__)
#define walk_clusters()

cluster_helper_c::cluster_helper_c() {
  num_clusters = 0;
  clusters = NULL;
  cluster_content_size = 0;
  max_timecode = 0;
  last_cluster_tc = 0;
  num_cue_elements = 0;
  next_splitpoint = 0;
  header_overhead = -1;
  packet_num = 0;
  out = NULL;
  timecode_offset = 0;
  last_packets = NULL;
  first_timecode = 0;
}

cluster_helper_c::~cluster_helper_c() {
  int i;

  for (i = 0; i < num_clusters; i++)
    free_contents(clusters[i]);

  if (clusters != NULL)
    safefree(clusters);
}

void cluster_helper_c::free_contents(ch_contents_t *clstr) {
  packet_t *p;
  int i;

  assert(clstr != NULL);
  assert(clstr->cluster != NULL);
  delete clstr->cluster;

  assert(!((clstr->num_packets != 0) && (clstr->packets == NULL)));
  for (i = 0; i < clstr->num_packets; i++) {
    p = clstr->packets[i];
    if (p->data != NULL)
      safefree(p->data);
    safefree(p);
  }
  if (clstr->packets != NULL)
    safefree(clstr->packets);
  safefree(clstr);
}

KaxCluster *cluster_helper_c::get_cluster() {
  if (clusters != NULL)
    return clusters[num_clusters - 1]->cluster;
  return NULL;
}

void cluster_helper_c::add_packet(packet_t *packet) {
  ch_contents_t *c;
  int64_t timecode;

  timecode = get_timecode();

  if (clusters == NULL)
    add_cluster(new KaxCluster());
  else if ((packet->timecode - timecode) > max_ms_per_cluster) {
    render();
    add_cluster(new KaxCluster());
  }

  packet->packet_num = packet_num;
  packet_num++;

  c = clusters[num_clusters - 1];
  c->packets = (packet_t **)saferealloc(c->packets, sizeof(packet_t *) *
                                        (c->num_packets + 1));

  c->packets[c->num_packets] = packet;
  c->num_packets++;
  cluster_content_size += packet->length;

  if ((packet->timecode + packet->duration) > max_timecode)
    max_timecode = (packet->timecode + packet->duration);

  walk_clusters();

  // Render the cluster if it is full (according to my many criteria).
  timecode = get_timecode();
  if (((packet->timecode - timecode) > max_ms_per_cluster) ||
      (get_packet_count() > max_blocks_per_cluster) ||
      (get_cluster_content_size() > 1500000)) {
    render();
    add_cluster(new KaxCluster());
  }
}

int64_t cluster_helper_c::get_timecode() {
  if (clusters == NULL)
    return 0;
  if (clusters[num_clusters - 1]->packets == NULL)
    return 0;
  return clusters[num_clusters - 1]->packets[0]->timecode;
}

packet_t *cluster_helper_c::get_packet(int num) {
  ch_contents_t *c;

  if (clusters == NULL)
    return NULL;
  c = clusters[num_clusters - 1];
  if (c->packets == NULL)
    return NULL;
  if ((num < 0) || (num > c->num_packets))
    return NULL;
  return c->packets[num];
}

int cluster_helper_c::get_packet_count() {
  if (clusters == NULL)
    return -1;
  return clusters[num_clusters - 1]->num_packets;
}

int cluster_helper_c::find_cluster(KaxCluster *cluster) {
  int i;

  if (clusters == NULL)
    return -1;
  for (i = 0; i < num_clusters; i++)
    if (clusters[i]->cluster == cluster)
      return i;
  return -1;
}

void cluster_helper_c::add_cluster(KaxCluster *cluster) {
  ch_contents_t *c;

  if (find_cluster(cluster) != -1)
    return;
  c = (ch_contents_t *)safemalloc(sizeof(ch_contents_t));
  clusters = (ch_contents_t **)saferealloc(clusters, sizeof(ch_contents_t *) *
                                           (num_clusters + 1));
  memset(c, 0, sizeof(ch_contents_t));
  clusters[num_clusters] = c;
  num_clusters++;
  c->cluster = cluster;
  cluster_content_size = 0;
  cluster->SetParent(*kax_segment);
  cluster->SetPreviousTimecode(last_cluster_tc, TIMECODE_SCALE);
}

int cluster_helper_c::get_cluster_content_size() {
  return cluster_content_size;
}

void cluster_helper_c::find_next_splitpoint() {
  int i;
  int64_t last, now;
  splitpoint_t *sp;

  if ((next_splitpoint >= splitpoints.size()) ||
      (file_num >= split_max_num_files)) {
    next_splitpoint = splitpoints.size();
    return;
  }

  sp = splitpoints[next_splitpoint];
  if (split_by_time)
    last = sp->timecode;
  else
    last = sp->filepos + sp->cues_size;

  i = next_splitpoint + 1;
  while (i < splitpoints.size()) {
    sp = splitpoints[i];

    if (split_by_time) {
      if ((sp->timecode - last) > split_after) {
        i--;
        break;
      }
    } else {
      now = sp->filepos + sp->cues_size;
      if ((now - last + header_overhead) > split_after) {
        i--;
        break;
      }
    }

    i++;
  }

  if (i == next_splitpoint)
    i++;

  next_splitpoint = i;
}

int cluster_helper_c::get_next_splitpoint() {
  return next_splitpoint;
}

void cluster_helper_c::set_output(mm_io_c *nout) {
  out = nout;
}

int cluster_helper_c::render() {
  KaxCluster *cluster;
  KaxBlockGroup *new_block_group, *last_block_group;
  KaxSlices *slices;
  KaxTimeSlice *slice;
  DataBuffer *data_buffer;
  int i, k, elements_in_cluster, num_cue_elements_here;
  ch_contents_t *clstr;
  packet_t *pack, *bref_packet, *fref_packet;
  int64_t block_duration, old_max_timecode, def_duration;
  splitpoint_t *sp;
  generic_packetizer_c *source, *last_source;
  bool first_lace;

  if ((clusters == NULL) || (num_clusters == 0))
    return 0;

  walk_clusters();
  clstr = clusters[num_clusters - 1];
  cluster = clstr->cluster;

  // Splitpoint stuff
  if ((header_overhead == -1) && (pass != 0))
    header_overhead = out->getFilePointer();

  elements_in_cluster = 0;
  num_cue_elements_here = 0;
  block_duration = 0;
  last_block_group = NULL;
  last_source = NULL;
  durations.clear();

  for (i = 0; i < clstr->num_packets; i++) {
    pack = clstr->packets[i];
    source = ((generic_packetizer_c *)pack->source);

    if (first_timecode == -1)
      first_timecode = pack->timecode;
    if (timecode_offset == -1)
      timecode_offset = pack->timecode;

    data_buffer = new DataBuffer((binary *)pack->data, pack->length);
    KaxTrackEntry &track_entry =
      static_cast<KaxTrackEntry &>(*source->get_track_entry());

    // Now put the packet into the cluster.
    if (pack->bref != -1) { // P and B frames: add backward reference.
      bref_packet = find_packet(pack->bref);
      if (bref_packet == NULL) {
        string err = "bref_packet == NULL. Wanted bref: " +
          to_string(pack->bref) + ". Contents of the queue:\n";
        for (i = 0; i < clstr->num_packets; i++) {
          pack = clstr->packets[i];
          err += "Packet " + to_string(i) + ", timecode " +
            to_string(pack->timecode) + ", bref " + to_string(pack->bref) +
            ", fref " + to_string(pack->fref) + "\n";
        }
        die(err.c_str());
      }
      assert(bref_packet->group != NULL);
      if (pack->fref != -1) { // It's even a B frame: add forward ref
        fref_packet = find_packet(pack->fref);
        if (fref_packet == NULL) {
          string err = "fref_packet == NULL. Wanted fref: " +
            to_string(pack->fref) + ". Contents of the queue:\n";
          for (i = 0; i < clstr->num_packets; i++) {
            pack = clstr->packets[i];
            err += "Packet " + to_string(i) + ", timecode " +
              to_string(pack->timecode) + ", bref " + to_string(pack->bref) +
              ", fref " + to_string(pack->fref) + "\n";
          }
          die(err.c_str());
        }
        assert(fref_packet->group != NULL);
        cluster->AddFrame(track_entry,
                          (pack->timecode - timecode_offset) * 1000000,
                          *data_buffer, new_block_group, *bref_packet->group,
                          *fref_packet->group);
      } else {
        cluster->AddFrame(track_entry,
                          (pack->timecode - timecode_offset) * 1000000,
                          *data_buffer, new_block_group, *bref_packet->group);
      }

    } else {                    // This is a key frame. No references.

      cluster->AddFrame(track_entry,
                        (pack->timecode - timecode_offset) * 1000000,
                        *data_buffer, new_block_group);
      // All packets with an ID smaller than this packet's ID are not
      // needed anymore. Be happy!
      free_ref(pack->timecode, pack->source);
    }

    // Set the reference priority if it was wanted.
    if ((new_block_group != NULL) && (pack->ref_priority > 0))
      *static_cast<EbmlUInteger *>
        (&GetChild<KaxReferencePriority>(*new_block_group)) =
        pack->ref_priority;

    elements_in_cluster++;

    if (new_block_group == NULL)
      new_block_group = last_block_group;
    else if (write_cues) {
      // Update the cues (index table) either if cue entries for
      // I frames were requested and this is an I frame...
      if (((source->get_cue_creation() == CUES_IFRAMES) && (pack->bref == -1))
          ||
      // ... or if the user requested entries for all frames.
          (source->get_cue_creation() == CUES_ALL)) {
        kax_cues->AddBlockGroup(*new_block_group);
        num_cue_elements++;
        num_cue_elements_here++;
        cue_writing_requested = 1;
      }
    }

    if ((new_block_group != last_block_group) && (last_block_group != NULL)) {
      // Use time slices to indicate each block's duration if there's more
      // than one block in a block group.

      def_duration = last_source->get_track_default_duration_ns() / 1000000;

      if (durations.size() > 1) {
        slices = &GetChild<KaxSlices>(*last_block_group);

        first_lace = true;

        for (k = 0; k < durations.size(); k++) {
          if (first_lace) {
            slice = &GetChild<KaxTimeSlice>(*slices);
            first_lace = false;
          } else
            slice = &GetNextChild<KaxTimeSlice>(*slices, *slice);
          *static_cast<EbmlUInteger *>
            (&GetChild<KaxSliceFrameNumber>(*slice)) = k;
          if (durations[k] != def_duration)
            *static_cast<EbmlUInteger *>(&GetChild<KaxSliceDuration>(*slice)) =
              (int64_t)(durations[k] * TIMECODE_SCALE / 1000000);
        }
      }

      for (k = 0, block_duration = 0; k < durations.size(); k++)
        block_duration += durations[k];
      if (def_duration != block_duration)
        last_block_group->SetBlockDuration(block_duration * 1000000);

      block_duration = 0;
      durations.clear();
    }

    durations.push_back(pack->duration);
    if (pass == 2) {
      if (next_splitpoint < splitpoints.size())
        sp = splitpoints[next_splitpoint];
      else
        sp = NULL;
    } else
      sp = NULL;

    pack->group = new_block_group;
    last_block_group = new_block_group;

    // The next stuff is for splitting files.
    if (pass == 1) {            // first pass: find splitpoints
      if ((pack->bref == -1) && // this is a keyframe
          ((video_fps == -1) || // either no video track present...
           // ...or this is the video track
           (source->get_track_type() == track_video))) { 
        sp = (splitpoint_t *)safemalloc(sizeof(splitpoint_t));
        sp->timecode = pack->timecode;
        if ((num_cue_elements - num_cue_elements_here) > 0) {
          kax_cues->UpdateSize();
          sp->cues_size = kax_cues->ElementSize();
        } else
          sp->cues_size = 0;
        sp->filepos = out->getFilePointer() - header_overhead;
        if (elements_in_cluster > 0) {
          cluster->UpdateSize();
          sp->filepos += cluster->ElementSize();
        }
        sp->packet_num = pack->packet_num;
        sp->last_packets = last_packets;
        last_packets = NULL;
        splitpoints.push_back(sp);
      } else {
        if (last_packets == NULL) {
          last_packets = (int64_t *)safemalloc(track_number * sizeof(int64_t));
          memset(last_packets, 0, track_number * sizeof(int64_t));
        }
        last_packets[source->get_track_num()] = pack->packet_num;
      }

    } else if ((pass == 2) &&   // second pass: process and split
               (next_splitpoint < splitpoints.size()) && // splitpoint's avail
               ((splitpoints[next_splitpoint]->packet_num - 1) ==
                pack->packet_num)) {
      if (elements_in_cluster > 0) {
        cluster->Render(*out, *kax_cues);
        if (kax_seekhead != NULL)
          kax_seekhead->IndexThis(*cluster, *kax_segment);
      }
      find_next_splitpoint();

      old_max_timecode = max_timecode;
      max_timecode = pack->timecode;

      mxprint(stdout, "\n");
      finish_file();
      create_next_output_file(next_splitpoint >= splitpoints.size());

      max_timecode = old_max_timecode;

      delete cluster;
      cluster = new KaxCluster();
      clstr->cluster = cluster;

      cluster->SetParent(*kax_segment);
      cluster->SetPreviousTimecode(0, TIMECODE_SCALE);

      elements_in_cluster = 0;
      if (no_linking) {
        timecode_offset = -1;
        first_timecode = 0;
      } else
        first_timecode = -1;
    }

    last_source = source;
  }

  if (elements_in_cluster > 0) {
    def_duration = last_source->get_track_default_duration_ns() / 1000000;
    if ((block_duration != 0) && (def_duration != block_duration)) {
      last_block_group->SetBlockDuration(block_duration * 1000000);
    }

    cluster->Render(*out, *kax_cues);

    if (kax_seekhead != NULL)
      kax_seekhead->IndexThis(*cluster, *kax_segment);

    last_cluster_tc = cluster->GlobalTimecode();
  } else
    last_cluster_tc = 0;


  for (i = 0; i < clstr->num_packets; i++) {
    pack = clstr->packets[i];
    safefree(pack->data);
    pack->data = NULL;
  }

  clstr->rendered = 1;

  free_clusters();

  return 1;
}

ch_contents_t *cluster_helper_c::find_packet_cluster(int64_t ref_timecode) {
  int i, k;

  if (clusters == NULL)
    return NULL;

  for (i = 0; i < num_clusters; i++)
    for (k = 0; k < clusters[i]->num_packets; k++)
      if (clusters[i]->packets[k]->timecode == ref_timecode)
        return clusters[i];

  return NULL;
}

packet_t *cluster_helper_c::find_packet(int64_t ref_timecode) {
  int i, k;

  if (clusters == NULL)
    return NULL;

  for (i = 0; i < num_clusters; i++)
    for (k = 0; k < clusters[i]->num_packets; k++)
      if (clusters[i]->packets[k]->timecode == ref_timecode)
        return clusters[i]->packets[k];

  return NULL;
}

void cluster_helper_c::check_clusters(int num) {
  int i, k;
  packet_t *p;
  ch_contents_t *clstr;

  for (i = 0; i < num_clusters; i++) {
    for (k = 0; k < clusters[i]->num_packets; k++) {
      p = clusters[i]->packets[k];
      if (clusters[i]->rendered && p->superseeded)
        continue;
      if (p->bref == -1)
        continue;
      clstr = find_packet_cluster(p->bref);
      if (clstr == NULL)
        die("cluster_helper.cpp/cluster_helper_c::check_clusters(): Error: "
            "backward refenrece could not be resolved (%lld -> %lld). Called "
            "from line %d.\n", p->timecode, p->bref, num);
    }
  }
}

//#define PRINT_CLUSTERS

int cluster_helper_c::free_clusters() {
  int i, k, idx;
  packet_t *p;
  ch_contents_t *clstr, **new_clusters;
#ifdef PRINT_CLUSTERS
  int num_freed = 0;
#endif

  if (clusters == NULL)
    return 0;

  for (i = 0; i < num_clusters; i++)
    clusters[i]->is_referenced = 0;

  // Part 1 - Mark all packets superseeded for which their source has
  // an appropriate free_refs entry.
  for (i = 0; i < num_clusters; i++) {
    for (k = 0; k < clusters[i]->num_packets; k++) {
      p = clusters[i]->packets[k];
      if (((generic_packetizer_c *)p->source)->get_free_refs() > p->timecode)
        p->superseeded = 1;
    }
  }

  // Part 2 - Mark all clusters that are still referenced.
  for (i = 0; i < num_clusters; i++) {
    for (k = 0; k < clusters[i]->num_packets; k++) {
      p = clusters[i]->packets[k];
      if (!p->superseeded) {
        clusters[i]->is_referenced = 1;
        if (p->bref == -1)
          continue;
        clstr = find_packet_cluster(p->bref);
        if (clstr == NULL)
          die("cluster_helper.cpp/cluster_helper_c::free_clusters(): Error: "
              "backward refenrece could not be resolved (%lld).\n", p->bref);
        clstr->is_referenced = 1;
      }
    }
  }

  // Part 3 - remove all clusters and the data belonging to them that
  // are not referenced anymore and that have already been rendered.
  // Also count the number of clusters that are still referenced.
  k = 0;
  for (i = 0; i < num_clusters; i++) {
    if (!clusters[i]->rendered) {
      k++;
      continue;
    }

    if (!clusters[i]->is_referenced) {
      free_contents(clusters[i]);
      clusters[i] = NULL;
#ifdef PRINT_CLUSTERS
      num_freed++;
#endif
    } else
      k++;
  }

  // Part 4 - prune the cluster list and remove all the entries freed in
  // part 3.
  if (k == 0) {
    safefree(clusters);
    num_clusters = 0;
    add_cluster(new KaxCluster());
  } else if (k != num_clusters) {
    new_clusters = (ch_contents_t **)safemalloc(sizeof(ch_contents_t *) * k);

    idx = 0;
    for (i = 0; i < num_clusters; i++)
      if (clusters[i] != NULL) {
        new_clusters[idx] = clusters[i];
        idx++;
      }

    safefree(clusters);
    clusters = new_clusters;
    num_clusters = k;
  }

#ifdef PRINT_CLUSTERS
  mxprint(stdout, "numcl: %8d freed: %3d ", num_clusters, num_freed);
  for (i = 0; i < num_clusters; i++)
    mxprint(stdout, "#");
  mxprint(stdout, "\n");
#endif

  return 1;
}

int cluster_helper_c::free_ref(int64_t ref_timecode, void *source) {
  ((generic_packetizer_c *)source)->set_free_refs(ref_timecode);

  return 1;
}

int64_t cluster_helper_c::get_max_timecode() {
  return max_timecode - timecode_offset;
}

int64_t cluster_helper_c::get_first_timecode() {
  return first_timecode;
}
