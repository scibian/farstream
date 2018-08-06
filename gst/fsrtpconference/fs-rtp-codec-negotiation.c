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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fs-rtp-codec-negotiation.h"

#include <farstream/fs-rtp.h>

#include <string.h>

#include "fs-rtp-bin-error-downgrade.h"
#include "fs-rtp-codec-specific.h"
#include "fs-rtp-conference.h"


#define GST_CAT_DEFAULT fsrtpconference_nego

#define SEND_PROFILE_ARG "farstream-send-profile"
#define RECV_PROFILE_ARG "farstream-recv-profile"


static CodecAssociation *
lookup_codec_association_by_pt_list (GList *codec_associations, gint pt,
    gboolean want_empty);

static CodecAssociation *
codec_association_copy (CodecAssociation *ca);

static CodecAssociation *
lookup_codec_association_custom_internal (GList *codec_associations,
    gboolean want_disabled, CAFindFunc func, gpointer user_data);

static gboolean
link_unlinked_pads (GstElement *bin,
    GstPadDirection dir,
    const gchar *pad_name,
    guint *pad_count,
    GError **error)
{
  GstPad *pad = NULL;
  guint i = 0;

  while ((pad = gst_bin_find_unlinked_pad (GST_BIN (bin), dir)))
  {
    GstPad *ghostpad;
    gchar *tmp;

    if (i)
      tmp = g_strdup_printf ("%s%d", pad_name, i);
    else
      tmp = g_strdup (pad_name);
    i++;

    ghostpad = gst_ghost_pad_new (tmp, pad);
    gst_object_unref (pad);
    g_free (tmp);

    if (!ghostpad)
    {
      g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
          "Could not create ghostpad for pad %s:%s",
          GST_DEBUG_PAD_NAME (pad));
      return FALSE;
    }

    if (!gst_element_add_pad (bin, ghostpad))
    {
      g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
          "Could not add pad %s to bin", GST_OBJECT_NAME (ghostpad));
      return FALSE;
    }
  }

  if (pad_count)
    *pad_count = i;

  return TRUE;
}

GstElement *
parse_bin_from_description_all_linked (const gchar *bin_description,
    FsStreamDirection direction, guint *src_pad_count, guint *sink_pad_count,
    GError **error)
{
  GstElement *bin;
  gchar *desc;

  if (direction == FS_DIRECTION_SEND)
  {
    desc = g_strdup_printf ("bin.( %s )", bin_description);
  }
  else if (direction == FS_DIRECTION_RECV)
  {
    fs_rtp_bin_error_downgrade_register ();
    desc = g_strdup_printf ("fsrtpbinerrordowngrade.( %s )", bin_description);
  }
  else
  {
    g_assert_not_reached ();
  }
  bin = gst_parse_launch_full (desc, NULL, GST_PARSE_FLAG_NONE,
      error);
  g_free (desc);

  if (!bin)
    return NULL;

  if (!link_unlinked_pads (bin, GST_PAD_SRC, "src", src_pad_count,
          error))
    goto error;

  if (!link_unlinked_pads (bin, GST_PAD_SINK, "sink", sink_pad_count,
          error))
    goto error;

  return bin;
 error:
  gst_object_unref (bin);
  return NULL;
}

static gint
find_matching_pad (gconstpointer a, gconstpointer b)
{
  const GValue *val = a;
  GstPad *pad = GST_PAD (g_value_get_object (val));
  GstCaps *caps = GST_CAPS (b);
  GstCaps *padcaps = NULL;
  gint ret = 1;

  padcaps = gst_pad_query_caps (pad, NULL);

  if (gst_caps_can_intersect (caps, padcaps))
    ret = 0;

  gst_caps_unref (padcaps);

  return ret;
}

static gboolean
validate_codec_profile (CodecPreference *cp, const gchar *bin_description,
    FsStreamDirection direction)
{
  GError *error = NULL;
  GstElement *codecbin = NULL;
  guint src_pad_count = 0, sink_pad_count = 0;
  GstCaps *caps;
  GstIterator *iter;
  gboolean has_matching_pad = FALSE;
  GValue val = {0,};
  gboolean ret = FALSE;

  codecbin = parse_bin_from_description_all_linked (bin_description, direction,
      &src_pad_count, &sink_pad_count, &error);

  /* if could not build bin, fail */
  if (!codecbin)
  {
    GST_WARNING ("Could not build profile (%s): %s", bin_description,
        error->message);
    g_clear_error (&error);
    return FALSE;
  }
  g_clear_error (&error);

  caps = fs_codec_to_gst_caps (cp->codec);

  if (direction == FS_DIRECTION_SEND)
    iter = gst_element_iterate_src_pads (codecbin);
  else if (direction == FS_DIRECTION_RECV)
    iter = gst_element_iterate_sink_pads (codecbin);
  else
    g_assert_not_reached ();

  has_matching_pad = gst_iterator_find_custom (iter, find_matching_pad,
      &val, caps);
  g_value_unset (&val);
  gst_iterator_free (iter);

  if (!has_matching_pad)
  {
    GST_WARNING ("Invalid profile (%s), has no %s pad that matches the codec"
        " details", (direction == FS_DIRECTION_SEND) ? "src" : "sink",
        bin_description);
    goto done;
  }
  if (direction == FS_DIRECTION_SEND)
  {
    if (src_pad_count == 0)
    {
      GST_WARNING ("Invalid profile (%s), has 0 src pad", bin_description);
      goto done;
    }
  }
  else if (direction == FS_DIRECTION_RECV)
  {
    if (src_pad_count != 1)
    {
      GST_WARNING ("Invalid profile (%s), has %u src pads, should have one",
          bin_description, src_pad_count);
      goto done;
    }
  }

  if (sink_pad_count != 1)
  {
    GST_WARNING ("Invalid profile (%s), has %u sink pads, should have one",
        bin_description, sink_pad_count);
    goto done;
  }

  if (direction == FS_DIRECTION_SEND)
  {
    cp->input_caps = codec_get_in_out_caps (cp->codec, caps, FS_DIRECTION_SEND,
        codecbin);
    if (!cp->input_caps)
      goto done;
  }
  else if (direction == FS_DIRECTION_RECV)
  {
    cp->output_caps = codec_get_in_out_caps (cp->codec, caps, FS_DIRECTION_RECV,
        codecbin);
    if (!cp->output_caps)
      goto done;
  }

  ret = TRUE;
done:

  gst_caps_unref (caps);
  gst_object_unref (codecbin);

  return ret;
}

static gboolean
codec_sdp_compare (FsCodec *local_codec, FsCodec *remote_codec)
{
  FsCodec *nego_codec = sdp_negotiate_codec (
      local_codec, FS_PARAM_TYPE_ALL & ~FS_PARAM_TYPE_CONFIG,
      remote_codec, FS_PARAM_TYPE_ALL & ~FS_PARAM_TYPE_CONFIG);

  if (!nego_codec)
    return FALSE;

  fs_codec_destroy (nego_codec);
  return TRUE;
}

/**
 * validate_codecs_configuration:
 * @media_type: The #FsMediaType these codecs should be for
 * @blueprints: A #GList of #CodecBlueprints to validate the codecs agsint
 * @codecs: a #GList of #FsCodec that represent the preferences
 *
 * This function validates a GList of passed FsCodec structures
 * against the valid discovered payloaders
 * It returns a list of the valid ones.
 *
 * Returns: a #GList of #CodecPreference
 */
GList *
validate_codecs_configuration (FsMediaType media_type, GList *blueprints,
  GList *codecs)
{
  GQueue result = G_QUEUE_INIT;
  GList *codec_e = codecs;

  for (codec_e = codecs; codec_e; codec_e = codec_e->next)
  {
    FsCodec *codec = codec_e->data;
    GList *blueprint_e = NULL;
    FsCodecParameter *param;
    CodecPreference *cp = NULL;

    /* Check if codec is for the wrong media_type.. this would be wrong
     */
    if (media_type != codec->media_type)
      goto ignore_this_codec;

    cp = g_slice_new0 (CodecPreference);
    cp->codec = fs_codec_copy (codec);

    if (codec->id >= 0 && codec->id < 128 && codec->encoding_name &&
        !g_ascii_strcasecmp (codec->encoding_name, "reserve-pt"))
      goto accept_codec;

    for (blueprint_e = g_list_first (blueprints);
         blueprint_e;
         blueprint_e = g_list_next (blueprint_e))
    {
      CodecBlueprint *blueprint = blueprint_e->data;

      /* First, lets check the encoding name */
      if (g_ascii_strcasecmp (blueprint->codec->encoding_name,
              codec->encoding_name))
        continue;
      /* If both have a clock_rate, it must be the same */
      if (blueprint->codec->clock_rate && codec->clock_rate &&
          blueprint->codec->clock_rate != codec->clock_rate)
        continue;
        /* At least one needs to have a clockrate */
      else if (!blueprint->codec->clock_rate && !codec->clock_rate)
        continue;

      if (codec_sdp_compare (blueprint->codec, codec))
        break;
    }

    /* If there are send and/or recv profiles, lets test them */
    param = fs_codec_get_optional_parameter (codec, RECV_PROFILE_ARG, NULL);
    if (param && !validate_codec_profile (cp, param->value,
            FS_DIRECTION_RECV))
      goto ignore_this_codec;

    param = fs_codec_get_optional_parameter (codec, SEND_PROFILE_ARG, NULL);
    if (param && !validate_codec_profile (cp, param->value,
            FS_DIRECTION_SEND))
      goto ignore_this_codec;

    /* If no blueprint was found */
    if (blueprint_e == NULL)
    {
      gchar *tmp;

      /* Accept codecs with no blueprints if they have a valid profile */
      if (fs_codec_get_optional_parameter (codec, RECV_PROFILE_ARG, NULL) &&
          codec->encoding_name && codec->clock_rate)
        goto accept_codec;

      tmp = fs_codec_to_string (codec);
      GST_DEBUG ("Preferred codec %s could not be matched with a blueprint",
          tmp);
      g_free (tmp);
      goto ignore_this_codec;
    }

  accept_codec:

    g_queue_push_tail (&result, cp);
    continue;
  ignore_this_codec:
    if (cp)
      codec_preference_destroy (cp);
    continue;
  }

  return result.head;
}


static void
_codec_association_destroy (CodecAssociation *ca)
{
  if (!ca)
    return;

  fs_codec_destroy (ca->codec);
  fs_codec_destroy (ca->send_codec);
  g_free (ca->send_profile);
  g_free (ca->recv_profile);
  g_slice_free (CodecAssociation, ca);
}


static CodecBlueprint *
_find_matching_blueprint (FsCodec *codec, GList *blueprints)
{
  GList *item = NULL;
  GstCaps *caps = NULL;

  caps = fs_codec_to_gst_caps (codec);

  if (!caps)
  {
    gchar *tmp = fs_codec_to_string (codec);
    GST_WARNING ("Could not transform codec into caps: %s", tmp);
    g_free (tmp);
    return NULL;
  }

  for (item = g_list_first (blueprints); item; item = g_list_next (item))
  {
    CodecBlueprint *bp = item->data;

    if (gst_caps_can_intersect (caps, bp->rtp_caps))
      break;
  }

  gst_caps_unref (caps);

  if (item)
    return item->data;
  else
    return NULL;
}

static gint
_find_first_empty_dynamic_entry (
    GList *new_codec_associations,
    GList *old_codec_associations)
{
  int id;

  for (id = 96; id < 128; id++)
  {
    if (lookup_codec_association_by_pt_list (new_codec_associations, id, TRUE))
      continue;
    if (lookup_codec_association_by_pt_list (old_codec_associations, id, TRUE))
      continue;
    return id;
  }

  return -1;
}

static gboolean
_is_disabled (GList *codec_prefs, CodecBlueprint *bp)
{
  GList *item = NULL;

  for (item = g_list_first (codec_prefs); item; item = g_list_next (item))
  {
    CodecPreference *cp = item->data;
    GstCaps *caps = NULL;
    gboolean ok = FALSE;

    /* Only check for DISABLE entries */
    if (cp->codec->id != FS_CODEC_ID_DISABLE)
      continue;

    caps = fs_codec_to_gst_caps (cp->codec);
    if (!caps)
      continue;

    if (gst_caps_can_intersect (caps, bp->rtp_caps))
      ok = TRUE;

    gst_caps_unref (caps);

    if (ok)
      return TRUE;
  }

  return FALSE;
}

/*
 * This function should return TRUE if the codec pref is a "base" of the
 * negotiated codec, but %FALSE otherwise.
 */

static gboolean
match_original_codec_and_codec_pref (CodecAssociation *ca, gpointer user_data)
{
  FsCodec *codec_pref = user_data;

  return codec_sdp_compare (codec_pref, ca->codec);
}

static void
codec_remove_parameter (FsCodec *codec, const gchar *param_name)
{
  FsCodecParameter *param;

  param = fs_codec_get_optional_parameter (codec, param_name, NULL);

  if (param)
    fs_codec_remove_optional_parameter (codec, param);
}

static gchar *
dup_param_value (FsCodec *codec, const gchar *param_name)
{
  FsCodecParameter *param;

  param = fs_codec_get_optional_parameter (codec, param_name, NULL);

  if (param)
    return g_strdup (param->value);
  else
    return NULL;
}

/*
 * Put the recv-only codecs after all of the codecs that are
 * valid for sending.
 * This should have the effect of putting stranges "codecs"
 * like telephone-event and CN at the end
 */

static GList *
list_insert_local_ca (GList *list, CodecAssociation *ca)
{
  if (codec_association_is_valid_for_sending (ca, TRUE))
  {
    GList *item;

    for (item = list; item; item = item->next)
      if (!codec_association_is_valid_for_sending (item->data, TRUE))
        break;
    if (item)
      return g_list_insert_before (list, item, ca);
  }

  return g_list_append (list, ca);
}

static gboolean
verify_caps (CodecPreference *cp, CodecBlueprint *bp, GstCaps *input_caps,
    GstCaps *output_caps)
{
  if (cp && cp->input_caps)
  {
    if (!gst_caps_can_intersect (input_caps, cp->input_caps))
    {
      GST_LOG ("Rejected codec " FS_CODEC_FORMAT " by input caps, filter: %"
          GST_PTR_FORMAT " pref caps: %" GST_PTR_FORMAT,
          FS_CODEC_ARGS (cp->codec), input_caps, cp->input_caps);
      return FALSE;
   }
  }
  else if (bp && bp->input_caps)
  {
    if (!gst_caps_can_intersect (input_caps, bp->input_caps))
    {
      GST_LOG ("Rejected codec " FS_CODEC_FORMAT " by input caps, filter: %"
          GST_PTR_FORMAT " blueprint caps: %" GST_PTR_FORMAT,
          FS_CODEC_ARGS (bp->codec), input_caps, bp->input_caps);
      return FALSE;
    }
  }

  if (cp && cp->output_caps)
  {
    if (!gst_caps_can_intersect (output_caps, cp->output_caps))
    {
      GST_LOG ("Rejected codec " FS_CODEC_FORMAT " by output caps, filter: %"
          GST_PTR_FORMAT " pref caps: %" GST_PTR_FORMAT,
          FS_CODEC_ARGS (cp->codec), output_caps, cp->output_caps);
      return FALSE;
   }
  }
  else if (bp && bp->output_caps)
  {
    if (!gst_caps_can_intersect (output_caps, bp->output_caps))
    {
      GST_LOG ("Rejected codec " FS_CODEC_FORMAT " by output caps, filter: %"
          GST_PTR_FORMAT " blueprint caps: %" GST_PTR_FORMAT,
          FS_CODEC_ARGS (bp->codec), output_caps, bp->output_caps);
      return FALSE;
    }
  }

  return TRUE;
}

/**
 * create_local_codec_associations:
 * @blueprints: The #GList of #CodecBlueprint
 * @codec_prefs: The #GList of #CodecPreference representing codec preferences
 * @current_codec_associations: The #GList of current #CodecAssociation
 *
 * This function creates a list of codec associations from installed codecs
 * and the preferences. It also takes into account the currently negotiated
 * codecs to keep the same payload types and optional parameters.
 *
 * Returns: a #GList of #CodecAssociation
 */

GList *
create_local_codec_associations (
    GList *blueprints,
    GList *codec_prefs,
    GList *current_codec_associations,
    GstCaps *input_caps,
    GstCaps *output_caps)
{
  GList *codec_associations = NULL;
  GList *bp_e = NULL;
  GList *codec_pref_e = NULL;
  GList *lca_e = NULL;
  gboolean has_valid_codec = FALSE;
  CodecAssociation *oldca = NULL;

  if (blueprints == NULL)
    return NULL;

  GST_DEBUG ("Creating local codec associations");

  /* First, lets create the original table by looking at our preferred codecs */
  for (codec_pref_e = codec_prefs;
       codec_pref_e;
       codec_pref_e = g_list_next (codec_pref_e))
  {
    CodecPreference *cp = codec_pref_e->data;
    CodecBlueprint *bp = _find_matching_blueprint (cp->codec,
        blueprints);
    CodecAssociation *ca = NULL;
    GList *bp_param_e = NULL;

    /* If its a negative pref, ignore it in this stage */
    if (cp->codec->id == FS_CODEC_ID_DISABLE)
      continue;

    /* If we want to disable a codec ID, we just insert a reserved codec assoc
     * in the list
     */
    if (cp->codec->id >= 0 && cp->codec->id < 128 &&
        cp->codec->encoding_name &&
        !g_ascii_strcasecmp (cp->codec->encoding_name, "reserve-pt"))
    {
      CodecAssociation *ca = g_slice_new0 (CodecAssociation);
      ca->codec = fs_codec_copy (cp->codec);
      ca->reserved = TRUE;
      codec_associations = g_list_append (codec_associations, ca);
      GST_DEBUG ("Add reserved payload type %d", cp->codec->id);
      continue;
    }

    /* No matching blueprint, can't use this codec */
    if (!bp &&
        !fs_codec_get_optional_parameter (cp->codec, RECV_PROFILE_ARG,
                NULL))
    {
      GST_LOG ("Could not find matching blueprint for preferred codec %s/%s",
          fs_media_type_to_string (cp->codec->media_type),
          cp->codec->encoding_name);
      continue;
    }

    if (!verify_caps (cp, bp, input_caps, output_caps))
      continue;

    /* Now lets see if there is an existing codec that matches this preference
     */

    if (cp->codec->id == FS_CODEC_ID_ANY)
    {
      oldca = lookup_codec_association_custom_internal (
          current_codec_associations, TRUE,
          match_original_codec_and_codec_pref, cp->codec);
    }
    else
    {
      oldca = lookup_codec_association_by_pt_list (current_codec_associations,
          cp->codec->id, FALSE);
      if (oldca && oldca->reserved)
        oldca = NULL;
    }

    /* In this case, we have a matching codec association, lets keep the
     * payload type from it
     */
    if (oldca)
    {
      FsCodec *codec = sdp_negotiate_codec (
          oldca->codec, FS_PARAM_TYPE_BOTH | FS_PARAM_TYPE_CONFIG,
          cp->codec, FS_PARAM_TYPE_ALL);
      FsCodec *send_codec;

      if (codec)
      {
        fs_codec_destroy (codec);

        send_codec = sdp_negotiate_codec (
            oldca->send_codec, FS_PARAM_TYPE_SEND,
            cp->codec,
            FS_PARAM_TYPE_SEND | FS_PARAM_TYPE_SEND_AVOID_NEGO);
        if (send_codec)
          fs_codec_destroy (send_codec);
        else
          oldca = NULL;
      }
      else
      {
        oldca = NULL;
      }
    }

    ca = g_slice_new0 (CodecAssociation);
    ca->blueprint = bp;
    ca->codec = fs_codec_copy (cp->codec);
    codec_remove_parameter (ca->codec, SEND_PROFILE_ARG);
    codec_remove_parameter (ca->codec, RECV_PROFILE_ARG);
    ca->send_codec = codec_copy_filtered (cp->codec,
        FS_PARAM_TYPE_CONFIG);
    codec_remove_parameter (ca->send_codec, SEND_PROFILE_ARG);
    codec_remove_parameter (ca->send_codec, RECV_PROFILE_ARG);
    if (oldca)
      ca->send_codec->id = ca->codec->id = oldca->codec->id;
    ca->send_profile = dup_param_value (cp->codec, SEND_PROFILE_ARG);
    ca->recv_profile = dup_param_value (cp->codec, RECV_PROFILE_ARG);

    if (bp)
    {
      /* Codec pref does not come with a number, but
       * The blueprint has its own id, lets use it */
      if (ca->codec->id == FS_CODEC_ID_ANY &&
          (bp->codec->id >= 0 && bp->codec->id < 128))
        ca->send_codec->id = ca->codec->id = bp->codec->id;

      if (ca->codec->clock_rate == 0)
        ca->codec->clock_rate = ca->send_codec->clock_rate =
            bp->codec->clock_rate;

      if (ca->codec->channels == 0)
        ca->codec->channels = ca->send_codec->channels = bp->codec->channels;

      for (bp_param_e = bp->codec->optional_params;
           bp_param_e;
           bp_param_e = g_list_next (bp_param_e))
      {
        FsCodecParameter *bp_param = bp_param_e->data;

        if (!fs_codec_get_optional_parameter (ca->codec, bp_param->name, NULL))
          fs_codec_add_optional_parameter (ca->codec, bp_param->name,
              bp_param->value);
        if (!fs_codec_get_optional_parameter (ca->send_codec, bp_param->name,
                NULL))
          fs_codec_add_optional_parameter (ca->send_codec, bp_param->name,
              bp_param->value);
      }
    }

    {
      gchar *tmp = fs_codec_to_string (ca->codec);
      GST_LOG ("Added preferred codec %s", tmp);
      g_free (tmp);
    }

    codec_associations = list_insert_local_ca (codec_associations, ca);
  }

  /* Now, only codecs with specified ids are here,
   * the rest are dynamic
   * Lets attribute them here */
  for (lca_e = codec_associations;
       lca_e;
       lca_e = g_list_next (lca_e))
  {
    CodecAssociation *lca = lca_e->data;

    if (lca->reserved)
      continue;

    if (lca->codec->id < 0)
    {
      lca->send_codec->id = lca->codec->id = _find_first_empty_dynamic_entry (
          current_codec_associations, codec_associations);
      if (lca->codec->id < 0)
      {
        GST_ERROR ("We've run out of dynamic payload types");
        goto error;
      }
    }
  }

  /* Now, lets add all other codecs from the blueprints */
  for (bp_e = g_list_first (blueprints); bp_e; bp_e = g_list_next (bp_e)) {
    CodecBlueprint *bp = bp_e->data;
    CodecAssociation *ca = NULL;
    GList *tmpca_e = NULL;
    gboolean next = FALSE;
    FsCodec *codec;

    /* Lets skip codecs that dont have all of the required informations */
    if (bp->codec->clock_rate == 0)
      continue;

    /* Check if its already used */
    for (tmpca_e = codec_associations;
         tmpca_e;
         tmpca_e = g_list_next (tmpca_e))
    {
      CodecAssociation *tmpca = tmpca_e->data;
      if (tmpca->blueprint == bp)
        break;
    }
    if (tmpca_e)
      continue;

    /* Check if it is disabled in the list of preferred codecs */
    if (_is_disabled (codec_prefs, bp))
    {
      gchar *tmp = fs_codec_to_string (bp->codec);
      GST_DEBUG ("Codec %s disabled by config", tmp);
      g_free (tmp);
      continue;
    }

    /* Re-use already existing codec associations with this blueprint
     * if any, we only keep the PT from the old assoc
     * (the rest will be regenerated by the renegotiation)
     */
    for (tmpca_e = current_codec_associations;
         tmpca_e;
         tmpca_e = g_list_next (tmpca_e))
    {
      CodecAssociation *tmpca = tmpca_e->data;

      if (tmpca->blueprint == bp)
      {
        /* Ignore reserved (we've just regenerated them )*/
        if (tmpca->reserved)
          continue;

        /* Ignore it if there is already something for this PT */
        if (lookup_codec_association_by_pt_list (codec_associations,
                tmpca->codec->id, TRUE))
          continue;

        /* Can't keep this codec, for some reason its wrong */
        codec = sdp_negotiate_codec (tmpca->codec, FS_PARAM_TYPE_CONFIG,
            bp->codec, FS_PARAM_TYPE_ALL);
        if (!codec)
          continue;
        fs_codec_destroy (codec);

        if (!verify_caps (NULL, bp, input_caps, output_caps))
          continue;

        ca = g_slice_new0 (CodecAssociation);
        ca->blueprint = bp;
        ca->codec = fs_codec_copy (bp->codec);
        ca->send_codec = codec_copy_filtered (bp->codec, FS_PARAM_TYPE_CONFIG);
        ca->codec->id = ca->send_codec->id = tmpca->codec->id;

        codec_associations = list_insert_local_ca (codec_associations, ca);
        next = TRUE;
      }
    }
    if (next)
      continue;

    codec = sdp_negotiate_codec (bp->codec, FS_PARAM_TYPE_ALL,
        bp->codec, FS_PARAM_TYPE_ALL);

    /* If it does not negotiate against itself, there must be something wrong */
    if (!codec)
      continue;
    fs_codec_destroy (codec);

    if (!verify_caps (NULL, bp, input_caps, output_caps))
      continue;

    ca = g_slice_new0 (CodecAssociation);
    ca->blueprint = bp;
    ca->codec = fs_codec_copy (bp->codec);

    if (ca->codec->id < 0)
    {
      ca->codec->id = _find_first_empty_dynamic_entry (
          current_codec_associations, codec_associations);
      if (ca->codec->id < 0)
      {
        GST_WARNING ("We've run out of dynamic payload types");
        goto error;
      }
    }

    ca->send_codec = codec_copy_filtered (ca->codec, FS_PARAM_TYPE_CONFIG);

    {
      gchar *tmp = fs_codec_to_string (ca->codec);
      GST_LOG ("Added discovered codec %s from blueprint", tmp);
      g_free (tmp);
    }

    codec_associations = list_insert_local_ca (codec_associations, ca);
  }

  for (lca_e = codec_associations;
       lca_e;
       lca_e = g_list_next (lca_e))
  {
    CodecAssociation *ca = lca_e->data;

    if (codec_association_is_valid_for_sending (ca, TRUE))
    {
      has_valid_codec = TRUE;
      break;
    }
  }

  if (!has_valid_codec)
  {
    GST_WARNING ("All codecs disabled by preferences");
    goto error;
  }

  return codec_associations;

 error:
  codec_association_list_destroy (codec_associations);

  return NULL;
}

static void
intersect_feedback_params (FsCodec *new_codec, FsCodec *orig_codec)
{
  GList *item = new_codec->feedback_params;

  while (item)
  {
    GList *nextitem = item->next;
    FsFeedbackParameter *param = item->data;

    if (!fs_codec_get_feedback_parameter (orig_codec, param->type,
            param->subtype, param->extra_params))
      fs_codec_remove_feedback_parameter (new_codec, item);

    item = nextitem;
  }
}

static void
negotiate_stream_codec (CodecAssociation *old_ca, FsCodec *remote_codec,
    gboolean multi_stream, FsCodec **nego_codec, FsCodec **nego_send_codec)
{
  if (multi_stream)
    *nego_codec = sdp_negotiate_codec (old_ca->codec, FS_PARAM_TYPE_ALL,
        remote_codec, FS_PARAM_TYPE_SEND | FS_PARAM_TYPE_SEND_AVOID_NEGO);
  else
    *nego_codec = sdp_negotiate_codec (old_ca->codec, FS_PARAM_TYPE_ALL,
        remote_codec, FS_PARAM_TYPE_SEND);

  if (*nego_codec)
  {
    if (multi_stream)
      *nego_send_codec = sdp_negotiate_codec (
          old_ca->send_codec,
          FS_PARAM_TYPE_BOTH | FS_PARAM_TYPE_SEND_AVOID_NEGO,
          remote_codec, FS_PARAM_TYPE_SEND | FS_PARAM_TYPE_SEND_AVOID_NEGO);
    else
      *nego_send_codec = sdp_negotiate_codec (
          old_ca->send_codec, FS_PARAM_TYPE_BOTH,
          remote_codec, FS_PARAM_TYPE_SEND | FS_PARAM_TYPE_SEND_AVOID_NEGO);

    /* If send codec can't be negotiated, try another one */
    if (!*nego_send_codec)
    {
      fs_codec_destroy (*nego_codec);
      *nego_codec = NULL;
    }
  }

  if (*nego_codec)
  {
    intersect_feedback_params (*nego_codec, old_ca->codec);
    intersect_feedback_params (*nego_send_codec, old_ca->send_codec);

    if (multi_stream)
    {
      (*nego_codec)->minimum_reporting_interval =
          old_ca->codec->minimum_reporting_interval;
      (*nego_send_codec)->minimum_reporting_interval =
          old_ca->send_codec->minimum_reporting_interval;
    }
  }
}

/**
 * negotiate_stream_codecs:
 * @remote_codecs: Remote codecs for the stream
 * @current_codec_assocations: The current list of #CodecAssociation
 * @multi_stream: %TRUE if there is more than one stream.
 *
 * This function performs codec negotiation for a single stream. It does an
 * intersection of the current codecs and the remote codecs.
 *
 * Returns: a #GList of #CodecAssociation
 */

GList *
negotiate_stream_codecs (
    const GList *remote_codecs,
    GList *current_codec_associations,
    gboolean multi_stream)
{
  GList *new_codec_associations = NULL;
  const GList *rcodec_e = NULL;
  GList *item = NULL;

  GST_DEBUG ("Negotiating stream codecs (for %s)",
      multi_stream ? "a single stream" : "multiple streams");

  for (rcodec_e = remote_codecs;
       rcodec_e;
       rcodec_e = g_list_next (rcodec_e)) {
    FsCodec *remote_codec = rcodec_e->data;
    FsCodec *nego_codec = NULL;
    FsCodec *nego_send_codec = NULL;
    CodecAssociation *old_ca = NULL;

    gchar *tmp = fs_codec_to_string (remote_codec);
    GST_DEBUG ("Remote codec %s", tmp);
    g_free (tmp);

    /* First lets try the codec that is in the same PT */

    old_ca = lookup_codec_association_by_pt_list (current_codec_associations,
        remote_codec->id, FALSE);

    if (old_ca) {
      GST_DEBUG ("Have local codec in the same PT, lets try it first");
      negotiate_stream_codec (old_ca, remote_codec, multi_stream,
          &nego_codec, &nego_send_codec);
    }

    if (!nego_codec) {

      for (item = current_codec_associations;
           item;
           item = g_list_next (item))
      {
        old_ca = item->data;

        if (old_ca->disable || old_ca->reserved)
          continue;

        negotiate_stream_codec (old_ca, remote_codec, multi_stream,
            &nego_codec, &nego_send_codec);

        if (nego_codec)
        {
          /* If we have multiple streams with codecs,
           * then priorize the local IDs */
          if (multi_stream)
            nego_send_codec->id = nego_codec->id = old_ca->codec->id;

          break;
        }
      }
    }

    if (nego_codec) {
      CodecAssociation *new_ca = g_slice_new0 (CodecAssociation);
      gchar *tmp;

      new_ca->need_config = old_ca->need_config;
      new_ca->codec = nego_codec;
      new_ca->send_codec = nego_send_codec;
      new_ca->blueprint = old_ca->blueprint;
      new_ca->send_profile = g_strdup (old_ca->send_profile);
      new_ca->recv_profile = g_strdup (old_ca->recv_profile);

      tmp = fs_codec_to_string (nego_codec);
      GST_DEBUG ("Negotiated codec %s", tmp);
      g_free (tmp);

      new_codec_associations = g_list_append (new_codec_associations,
          new_ca);
    } else {
      gchar *tmp = fs_codec_to_string (remote_codec);
      CodecAssociation *new_ca = g_slice_new0 (CodecAssociation);
      GST_DEBUG ("Could not find a valid intersection... for codec %s",
          tmp);
      g_free (tmp);

      new_ca->codec = fs_codec_copy (remote_codec);
      new_ca->disable = TRUE;

      new_codec_associations = g_list_append (new_codec_associations, new_ca);
    }
  }

  /*
   * Check if there is a non-disabled codec left that we can use
   * for sending
   */
  for (item = new_codec_associations;
       item;
       item = g_list_next (item))
  {
    CodecAssociation *ca = item->data;

    if (codec_association_is_valid_for_sending (ca, TRUE))
      return new_codec_associations;
  }

  /* Else we destroy when and return NULL.. ie .. an error */
  codec_association_list_destroy (new_codec_associations);

  return NULL;
}

static void
keep_config_from_old_codec (FsCodec *new_codec, FsCodec *old_codec)
{
  GList *item;

  for (item = old_codec->optional_params; item; item = item->next)
  {
    FsCodecParameter *oldparam = item->data;
    if (!fs_codec_get_optional_parameter (new_codec, oldparam->name, NULL)
        && codec_has_config_data_named (new_codec, oldparam->name))
      fs_codec_add_optional_parameter (new_codec, oldparam->name,
          oldparam->value);
  }
}

static gboolean
match_send_codec_no_pt (CodecAssociation *old_ca, gpointer user_data)
{
  FsCodec *old_send_codec;
  FsCodec *tmpcodec = NULL;
  CodecAssociation *new_ca = user_data;
  gboolean ret;

  if (old_ca->disable || old_ca->reserved || !old_ca->send_codec)
    return FALSE;

  if (new_ca->send_codec->id == old_ca->send_codec->id)
  {
    old_send_codec = old_ca->send_codec;
  }
  else
  {
    tmpcodec = old_send_codec = fs_codec_copy (old_ca->send_codec);
    old_send_codec->id = new_ca->codec->id;
  }

  ret = fs_codec_are_equal (old_send_codec, new_ca->send_codec);
  fs_codec_destroy (tmpcodec);

  return ret;
}

/**
 * finish_codec_negotiation:
 * @old_codec_associations: The previous list of negotiated #CodecAssociation
 * @new_codec_associations: The new list of negotiated #CodecAssociation,
 *   will be modified by the negotiation
 *
 * This function performs the last step of the codec negotiation after the
 * intersection will all of the remote codecs has been done. It will keep
 * old codecs in case the other end does the non-standard thing and sends
 * using the PT we offered instead of using the negotiated result.
 * It also adds a marker to the list for every previously disabled codec so
 * they're not re-used.
 *
 * It also keeps the old discovered codec parameters if the other parameters
 * are the same.
 *
 * Returns: a modified list of #CodecAssociation
 */

GList *
finish_codec_negotiation (
    GList *old_codec_associations,
    GList *new_codec_associations)
{
  int i;
  GList *item;

  /* Now, lets fill all of the PTs that were previously used in the session
   * even if they are not currently used, so they can't be re-used
   */

  for (i=0; i < 128; i++)
  {
    CodecAssociation *local_ca = NULL;

    /* We can skip ids where something already exists */
    if (lookup_codec_association_by_pt_list (new_codec_associations, i, TRUE))
      continue;

    /* We check if our local table (our offer) and if we offered
     * something, we add it. Some broken implementation (like Tandberg's)
     * send packets on PTs that they did not put in their response
     */
    local_ca = lookup_codec_association_by_pt_list (old_codec_associations,
        i, FALSE);
    if (local_ca) {
      CodecAssociation *new_ca = codec_association_copy (local_ca);
      new_ca->recv_only = TRUE;
      new_codec_associations = g_list_append (new_codec_associations, new_ca);
    }
  }

  for (item = new_codec_associations; item; item = g_list_next (item))
  {
    CodecAssociation *new_ca = item->data;
    CodecAssociation *old_ca = NULL;

    if (new_ca->disable || new_ca->reserved || new_ca->recv_only)
    {
      new_ca->need_config = FALSE;
      continue;
    }

    old_ca = lookup_codec_association_custom_internal (old_codec_associations,
        TRUE, match_send_codec_no_pt, new_ca);
    if (old_ca)
      keep_config_from_old_codec (new_ca->codec, old_ca->codec);

    new_ca->need_config = codec_needs_config (new_ca->codec);
  }

  return new_codec_associations;
}


static CodecAssociation *
lookup_codec_association_by_pt_list (GList *codec_associations, gint pt,
                                     gboolean want_disabled)
{
  while (codec_associations)
  {
    if (codec_associations->data)
    {
      CodecAssociation *ca = codec_associations->data;
      if (ca->codec->id == pt &&
          (want_disabled || (!ca->disable && !ca->reserved)))
        return ca;
    }
    codec_associations = g_list_next (codec_associations);
  }

  return NULL;
}


/**
 * lookup_codec_association_by_codec:
 * @codec_associations: a #GList of CodecAssociation
 * @pt: a payload-type number
 *
 * Finds the first #CodecAssociation that matches the payload type
 *
 * Returns: a #CodecAssociation
 */

CodecAssociation *
lookup_codec_association_by_pt (GList *codec_associations, gint pt)
{
  return lookup_codec_association_by_pt_list (codec_associations, pt, FALSE);
}

/**
 * lookup_codec_association_by_codec:
 * @codec_associations: a #GList of #CodecAssociation
 * @codec: The #FsCodec to look for
 *
 * Finds the first #CodecAssociation that matches the #FsCodec
 *
 * Returns: a #CodecAssociation
 */

CodecAssociation *
lookup_codec_association_by_codec (GList *codec_associations, FsCodec *codec)
{
  while (codec_associations)
  {
    if (codec_associations->data)
    {
      CodecAssociation *ca = codec_associations->data;
      if (fs_codec_are_equal (ca->codec, codec))
        return ca;
    }
    codec_associations = g_list_next (codec_associations);
  }

  return NULL;
}

/**
 * codec_association_list_destroy:
 * @list: a #GList of #CodecAssociation
 *
 * Frees a #GList of #CodecAssociation
 */

void
codec_association_list_destroy (GList *list)
{
  g_list_foreach (list, (GFunc) _codec_association_destroy, NULL);
  g_list_free (list);
}


static CodecAssociation *
codec_association_copy (CodecAssociation *ca)
{
  CodecAssociation *newca = g_slice_new (CodecAssociation);

  g_return_val_if_fail (ca, NULL);

  memcpy (newca, ca, sizeof(CodecAssociation));
  newca->codec = fs_codec_copy (ca->codec);
  newca->send_codec = fs_codec_copy (ca->send_codec);
  newca->send_profile = g_strdup (ca->send_profile);
  newca->recv_profile = g_strdup (ca->recv_profile);

  return newca;
}

GList *
codec_associations_to_codecs_internal (GList *codec_associations,
    gboolean include_config, gboolean send_codecs)
{
  GList *codecs = NULL;
  GList *item = NULL;

  for (item = g_list_first (codec_associations);
       item;
       item = g_list_next (item))
  {
    CodecAssociation *ca = item->data;
    if (!ca->disable && !ca->reserved && !ca->recv_only && ca->codec)
    {
      FsCodec *codec = NULL;

      if (send_codecs)
        codec = fs_codec_copy (ca->send_codec);
      else if (include_config)
        codec = fs_codec_copy (ca->codec);
      else
        codec = codec_copy_filtered (ca->codec, FS_PARAM_TYPE_CONFIG);

      codecs = g_list_append (codecs, codec);
    }
  }

  return codecs;
}


/**
 * codec_associations_to_codecs:
 * @codec_associations: a #GList of #CodecAssociation
 * @include_config: whether to include the config data
 *
 * Returns a #GList of the #FsCodec that are inside the list of associations
 * excluding those that are disabled or otherwise receive-only. It copies
 * the #FsCodec structures.
 *
 * Returns: a #GList of #FsCodec
 */
GList *
codec_associations_to_codecs (GList *codec_associations,
    gboolean include_config)
{
  return codec_associations_to_codecs_internal (codec_associations,
      include_config, FALSE);
}



/**
 * codec_associations_to_send_codecs
 * @codec_associations: a #GList of #CodecAssociation
 *
 * Returns a #GList of the #FsCodec that are inside the list of associations
 * excluding those that are disabled or otherwise receive-only. It copies
 * the #FsCodec structures. These codecs are to be used for sending
 *
 * Returns: a #GList of #FsCodec
 */
GList *
codec_associations_to_send_codecs (GList *codec_associations)
{
  return codec_associations_to_codecs_internal (codec_associations,
      FALSE, TRUE);
}

gboolean
codec_association_is_valid_for_sending (CodecAssociation *ca,
    gboolean needs_codecbin)
{
  if (ca->send_codec &&
      !ca->disable &&
      !ca->reserved &&
      !ca->recv_only &&
      (!needs_codecbin ||
          (ca->blueprint &&
              codec_blueprint_has_factory (ca->blueprint, FS_DIRECTION_SEND)) ||
          ca->send_profile))
    return TRUE;
  else
    return FALSE;
}


static CodecAssociation *
lookup_codec_association_custom_internal (GList *codec_associations,
    gboolean want_disabled, CAFindFunc func, gpointer user_data)
{
  GList *item;

  g_return_val_if_fail (func, NULL);

  for (item = codec_associations;
       item;
       item = g_list_next (item))
  {
    CodecAssociation *ca = item->data;
    if ((ca->disable && !want_disabled) || ca->reserved)
      continue;

    if (func (ca, user_data))
      return ca;
  }

  return NULL;
}


CodecAssociation *
lookup_codec_association_custom (GList *codec_associations,
    CAFindFunc func, gpointer user_data)
{

  return lookup_codec_association_custom_internal (codec_associations, FALSE,
      func, user_data);
}


/**
 * codec_association_list_are_equal
 * @list1: a #GList of #FsCodec
 * @list2: a #GList of #FsCodec
 *
 * Compares the non-disabled #FsCodec of two lists of #CodecAssociation
 *
 * Returns: TRUE if they are identical, FALSE otherwise
 */

gboolean
codec_associations_list_are_equal (GList *list1, GList *list2)
{
  for (;list1 && list2;
       list1 = g_list_next (list1), list2 = g_list_next (list2))
  {
    CodecAssociation *ca1 = NULL;
    CodecAssociation *ca2 = NULL;

    /* Skip disabled codecs */
    while (list1) {
      ca1 = list1->data;
      if (!ca1->disable || !ca1->reserved)
        break;
      list1 = g_list_next (list1);
    }
    while (list2) {
      ca2 = list2->data;
      if (!ca2->disable || !ca2->reserved)
        break;
      list2 = g_list_next (list2);
    }

    if (list1 == NULL || list2 == NULL)
      break;

    /* We must emit the notification if the recv-only status
     * of a codec has changed
     */
    if (ca1->recv_only != ca2->recv_only)
      return FALSE;

    if (!fs_codec_are_equal (ca1->codec, ca2->codec))
      return FALSE;
  }

  if (list1 == NULL && list2 == NULL)
    return TRUE;
  else
    return FALSE;
}


/**
 * lookup_codec_association_by_codec_for_sending
 * @codec_associations: a #GList of #CodecAssociation
 * @codec: The #FsCodec to look for
 *
 * Finds the first #CodecAssociation that matches the #FsCodec and that is
 * valid for sending, the config data inside both are ignored.
 *
 * Returns: a #CodecAssociation
 */

CodecAssociation *
lookup_codec_association_by_codec_for_sending (GList *codec_associations,
    FsCodec *codec)
{
  GList *item;
  CodecAssociation *res = NULL;
  FsCodec *tmpcodec = codec_copy_filtered (codec, FS_PARAM_TYPE_CONFIG);

  for (item = codec_associations; item; item = g_list_next (item))
  {
    CodecAssociation *ca = item->data;

    if (codec_association_is_valid_for_sending (ca, FALSE) &&
        fs_codec_are_equal (ca->send_codec, tmpcodec))
    {
      res = ca;
      break;
    }
  }

  fs_codec_destroy (tmpcodec);

  return res;
}

static FsRtpHeaderExtension *
get_extension (GList *hdrexts, const gchar *uri, guint id)
{
  GList *item;

  for (item = hdrexts; item; item = item->next)
  {
    FsRtpHeaderExtension *hdrext = item->data;

    if ((!uri || !g_ascii_strcasecmp (hdrext->uri, uri)) &&
        (id == G_MAXUINT || hdrext->id == id))
      return hdrext;
  }

  return NULL;
}


static inline gboolean
read_bit (guint8 *array, guint8 bit)
{
  return array[bit/8] & (1 << (bit%8));
}

static inline void
write_bit (guint8 *array, guint8 bit)
{
  array[bit/8] |= 1 << (bit%8);
}

GList *
create_local_header_extensions (GList *hdrexts_old, GList *hdrexts_prefs,
    guint8 *used_ids)
{
  GList *hdrexts_new = fs_rtp_header_extension_list_copy (hdrexts_prefs);
  GList *item;

  for (item = hdrexts_new; item; item = item->next)
  {
    FsRtpHeaderExtension *hdrext = item->data;
    FsRtpHeaderExtension *hdrext_existing = get_extension (hdrexts_old,
        hdrext->uri, G_MAXUINT);

    if (hdrext_existing &&
        hdrext_existing->id < 256 &&
        !get_extension (hdrexts_prefs, NULL, hdrext->id))
      hdrext->id = hdrext_existing->id;
  }

  for (item = hdrexts_new; item; item = item->next)
  {
    FsRtpHeaderExtension *hdrext = item->data;
    if (hdrext->id < 256)
      write_bit (used_ids, hdrext->id);
  }

  return hdrexts_new;
}

static void
renumber_hdrext (GList *hdrexts, guint old_id, guint new_id)
{
   GList *item;

   for (item = hdrexts; item; item = item->next)
   {
     FsRtpHeaderExtension *hdrext = item->data;

     if (hdrext->id == old_id)
       hdrext->id = new_id;
   }
}

GList *
negotiate_stream_header_extensions (GList *hdrexts, GList *hdrexts_stream,
    gboolean favor_remote, guint8 *used_ids)
{
  GList *item;

  if (!hdrexts)
    return NULL;

  for (item = hdrexts_stream; item; item = item->next)
  {
    FsRtpHeaderExtension *hdrext_stream = item->data;
    if (hdrext_stream->id < 256)
      write_bit (used_ids, hdrext_stream->id);
  }

  for (item = hdrexts; item;)
  {
    FsRtpHeaderExtension *hdrext = item->data;
    FsRtpHeaderExtension *hdrext_stream = get_extension (hdrexts_stream,
        hdrext->uri, G_MAXUINT);
    GList *next = item->next;

    if (hdrext_stream)
    {
      /* Use the minimum direction */
      hdrext->direction &= hdrext_stream->direction;
      /* If favor remotes, then we use the remote one for all matching
       * extensions, we want to preserve duplicates, modules will pick
       * the one they prefer if they have a preference
       */
      if (favor_remote)
        renumber_hdrext (hdrexts, hdrext->id, hdrext_stream->id);
    }
    else
    {
      hdrexts = g_list_delete_link (hdrexts, item);
      fs_rtp_header_extension_destroy (hdrext);
    }

    item = next;
  }

  return hdrexts;
}

static GList *
hdrext_list_remove_by_id (GList *hdrexts, guint id)
{
  GList *item;

  for (item = hdrexts; item; item = item->next)
  {
      FsRtpHeaderExtension *hdrext = item->data;

      if (hdrext->id == id)
      {
        GList *next = item->next;

        hdrexts = g_list_delete_link (hdrexts, item);
        fs_rtp_header_extension_destroy (hdrext);
        item = next;
      }
    }

  return hdrexts;
}

GList *
finish_header_extensions_nego (GList *hdrexts, guint8 *used_ids)
{
  GList *item;
  guint min = 1;

  for (item = hdrexts; item;)
  {
    FsRtpHeaderExtension *hdrext = item->data;

    if (hdrext->id >= 256)
    {

      /* Find the next available ID */
      for (; min < 256; min++)
        if (!read_bit (used_ids, min))
          break;

      if (min < 256)
      {
        /* We have a valid ID, remove any other extension with the same ID */
        /* and then use it */
        item = hdrext_list_remove_by_id (item->next, hdrext->id);
        hdrext->id = min;
        write_bit (used_ids, min);
        min++;
      }
      else
      {
        /* We lost, we used all slots between 1 and 255... forget this extension
         */
        GList *next = item->next;

        hdrexts = g_list_delete_link (hdrexts, item);
        fs_rtp_header_extension_destroy (hdrext);
        item = next;
      }
    }
    else
    {
      /* Item already has a valid number, keep as-is */
      item = item->next;
    }
  }

  return hdrexts;

}

void
codec_preference_destroy (CodecPreference *cp)
{
  fs_codec_destroy (cp->codec);

  gst_caps_replace (&cp->input_caps, NULL);
  gst_caps_replace (&cp->output_caps, NULL);

  g_slice_free (CodecPreference, cp);
}
