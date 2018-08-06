/*
 * Farstream - Farstream RTP Conference Implementation
 *
 * Copyright 2007 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * Copyright 2007 Nokia Corp.
 *
 * gstfsrtpconference.c - RTP implementation for Farstream Conference Gstreamer
 *                        Elements
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include "fs-rtp-conference.h"

static gboolean plugin_init (GstPlugin * plugin)
{
  return gst_element_register (plugin, "fsrtpconference",
                               GST_RANK_NONE, FS_TYPE_RTP_CONFERENCE);
}

#ifdef BUILD_GTK_DOC
void
fs_rtp_plugin_init_real (void)
{
  gst_plugin_register_static (
#else
GST_PLUGIN_DEFINE (
#endif
  GST_VERSION_MAJOR,
  GST_VERSION_MINOR,
  fsrtpconference,
  "Farstream RTP Conference plugin",
  plugin_init,
  VERSION,
  "LGPL",
  "Farstream",
  "http://www.freedesktop.org/wiki/Software/Farstream"
#ifdef BUILD_GTK_DOC
  );
}
#else
)
#endif
