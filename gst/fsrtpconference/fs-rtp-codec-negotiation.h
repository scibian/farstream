/*
 * Farstream - Farstream RTP Codec Negotiation
 *
 * Copyright 2007 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * Copyright 2007 Nokia Corp.
 *
 * fs-discover-codecs.h - A Farstream RTP Codec Negotiation
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

#ifndef __FS_RTP_CODEC_NEGOTIATION_H__
#define __FS_RTP_CODEC_NEGOTIATION_H__

#include "fs-rtp-discover-codecs.h"

G_BEGIN_DECLS

/**
 * CodecAssociation:
 * @blueprint: Blueprint used to construct this CodecAssociation (or NULL if
 *  this is strictly from profiles)
 * @codec: The codec this represents.. With the receive-side paremeters
 * @send_codec: The same codec, but with the send-side parameters
 * @reserved: Marks a payload-type reserved at the users request
 * @disable: means that its not a real association, just a spot thats disabled,
 *  its marks the PT of a remote codec that has been refused
 * @need_config: means that the config has to be retreived from the codec data
 * @recv_only: means thats its not a real negotiated codec, just a codec that
 * we have offered from which we have to be ready to receive stuff, just in case
 *
 * The codec association structure represents the link between a #FsCodec and
 * a CodecBlueprint that implements it.
 *
 * It should be treated as opaque by any function outside of
 * fs-rtp-codec-negotiation.c
 *
 */

typedef struct _CodecAssociation {
  CodecBlueprint *blueprint;
  FsCodec *codec;
  FsCodec *send_codec;

  gchar *send_profile;
  gchar *recv_profile;

  /*< private >*/

  gboolean reserved;
  gboolean disable;
  gboolean need_config;
  gboolean recv_only;

} CodecAssociation;

typedef struct _CodecPreference {
  FsCodec *codec;

  GstCaps *input_caps;
  GstCaps *output_caps;
} CodecPreference;

GList *validate_codecs_configuration (
    FsMediaType media_type,
    GList *blueprints,
    GList *codecs);

GList *
create_local_codec_associations (
    GList *blueprints,
    GList *codec_prefs,
    GList *current_codec_associations,
    GstCaps *input_caps,
    GstCaps *output_caps);

GList *
negotiate_stream_codecs (
    const GList *remote_codecs,
    GList *current_codec_associations,
    gboolean multi_stream);

GList *
finish_codec_negotiation (
    GList *old_codec_associations,
    GList *new_codec_associations);

CodecAssociation *
lookup_codec_association_by_pt (GList *codec_associations, gint pt);

CodecAssociation *
lookup_codec_association_by_codec (GList *codec_associations, FsCodec *codec);

CodecAssociation *
lookup_codec_association_by_codec_for_sending (GList *codec_associations,
    FsCodec *codec);

gboolean
codec_association_is_valid_for_sending (CodecAssociation *ca,
    gboolean needs_codecbin);

GList *
codec_associations_to_codecs (GList *codec_associations,
    gboolean include_config);

GList *
codec_associations_to_send_codecs (GList *codec_associations);

gboolean
codec_associations_list_are_equal (GList *list1, GList *list2);

void
codec_association_list_destroy (GList *list);

typedef gboolean (*CAFindFunc) (CodecAssociation *ca, gpointer user_data);

CodecAssociation *
lookup_codec_association_custom (GList *codec_associations,
    CAFindFunc func, gpointer user_data);

GstElement *
parse_bin_from_description_all_linked (const gchar *bin_description,
    FsStreamDirection direction, guint *src_pad_count, guint *sink_pad_count,
    GError **error);



GList *
create_local_header_extensions (GList *hdrext_old, GList *hdrext_prefs,
    guint8 *used_ids);
GList *
negotiate_stream_header_extensions (GList *hdrext, GList *hdrext_remote,
    gboolean favor_remote, guint8 *used_ids);
GList *
finish_header_extensions_nego (GList *hdrexts, guint8 *used_ids);

void
codec_preference_destroy (CodecPreference *cp);

G_END_DECLS

#endif /* __FS_RTP_CODEC_NEGOTIATION_H__ */
