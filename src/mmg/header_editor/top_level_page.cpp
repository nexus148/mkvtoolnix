/*
   mkvmerge GUI -- utility for splicing together matroska files
   from component media subtypes

   Distributed under the GPL
   see the file COPYING for details
   or visit http://www.gnu.org/copyleft/gpl.html

   header editor: segment info page class

   Written by Moritz Bunkus <moritz@bunkus.org>.
*/

#include "common/os.h"

#include <wx/sizer.h>
#include <wx/statline.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "common/ebml.h"
#include "common/segmentinfo.h"
#include "common/segment_tracks.h"
#include "common/wx.h"
#include "mmg/header_editor/frame.h"
#include "mmg/header_editor/top_level_page.h"

he_top_level_page_c::he_top_level_page_c(header_editor_frame_c *parent,
                                         const wxString &title,
                                         EbmlElement *l1_element)
  : he_empty_page_c(parent, title, wxEmptyString)
{
  m_l1_element = l1_element;

  parent->append_page(this, title);
}

he_top_level_page_c::~he_top_level_page_c() {
}

void
he_top_level_page_c::do_modifications() {
  he_page_base_c::do_modifications();

  if (is_id(m_l1_element, KaxInfo))
    fix_mandatory_segmentinfo_elements(m_l1_element);

  else if (is_id(m_l1_element, KaxTracks))
    fix_mandatory_segment_tracks_elements(m_l1_element);

  m_l1_element->UpdateSize(true);
}
