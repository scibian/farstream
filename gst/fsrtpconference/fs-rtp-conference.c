/*
 * Farstream - Farstream RTP Conference Implementation
 *
 * Copyright 2007 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * Copyright 2007 Nokia Corp.
 *
 * fs-rtp-conference.c - RTP implementation for Farstream Conference Gstreamer
 *                       Elements
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

/**
 * SECTION:element-fsrtpconference
 * @short_description: Farstream RTP Conference Gstreamer Elements
 *
 * This is the core gstreamer element for a RTP conference. It must be added
 * to your pipeline before anything else is done. Then you create the session,
 * participants and streams according to the #FsConference interface.
 *
 * The various sdes property allow you to set the content of the SDES packet
 * in the sent RTCP reports.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fs-rtp-conference.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs-rtp-session.h"
#include "fs-rtp-stream.h"
#include "fs-rtp-participant.h"


GST_DEBUG_CATEGORY (fsrtpconference_debug);
GST_DEBUG_CATEGORY (fsrtpconference_disco);
GST_DEBUG_CATEGORY (fsrtpconference_nego);
#define GST_CAT_DEFAULT fsrtpconference_debug

/* Signals */
enum
{
  LAST_SIGNAL
};

/* Properties */
enum
{
  PROP_0,
  PROP_SDES,
};


static GstStaticPadTemplate fs_rtp_conference_sink_template =
  GST_STATIC_PAD_TEMPLATE ("sink_%u",
                           GST_PAD_SINK,
                           GST_PAD_SOMETIMES,
                           GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate fs_rtp_conference_src_template =
  GST_STATIC_PAD_TEMPLATE ("src_%u_%u_%u",
                           GST_PAD_SRC,
                           GST_PAD_SOMETIMES,
                           GST_STATIC_CAPS_ANY);


#define FS_RTP_CONFERENCE_GET_PRIVATE(obj) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((obj), FS_TYPE_RTP_CONFERENCE, FsRtpConferencePrivate))

struct _FsRtpConferencePrivate
{
  gboolean disposed;

  /* Protected by GST_OBJECT_LOCK */
  GList *sessions;
  guint sessions_cookie;
  guint max_session_id;

  GList *participants;

  /* Array of all internal threads, as GThreads */
  GPtrArray *threads;
};

G_DEFINE_TYPE (FsRtpConference, fs_rtp_conference, FS_TYPE_CONFERENCE);

static void fs_rtp_conference_get_property (GObject *object,
    guint prop_id,
    GValue *value,
    GParamSpec *pspec);
static void fs_rtp_conference_set_property (GObject *object,
    guint prop_id,
    const GValue *value,
    GParamSpec *pspec);

static void fs_rtp_conference_finalize (GObject *object);
static FsSession *fs_rtp_conference_new_session (FsConference *conf,
                                                 FsMediaType media_type,
                                                 GError **error);
static FsParticipant *fs_rtp_conference_new_participant (FsConference *conf,
    GError **error);

static FsRtpSession *fs_rtp_conference_get_session_by_id_locked (
    FsRtpConference *self, guint session_id);
static FsRtpSession *fs_rtp_conference_get_session_by_id (
    FsRtpConference *self, guint session_id);

static GstCaps *_rtpbin_request_pt_map (GstElement *element,
    guint session_id,
    guint pt,
    gpointer user_data);
static void _rtpbin_pad_added (GstElement *rtpbin,
    GstPad *new_pad,
    gpointer user_data);
static void _rtpbin_on_bye_ssrc (GstElement *rtpbin,
    guint session_id,
    guint ssrc,
    gpointer user_data);
static void _rtpbin_on_ssrc_validated (GstElement *rtpbin,
    guint session_id,
    guint ssrc,
    gpointer user_data);

static void
_remove_session (gpointer user_data,
    GObject *where_the_object_was);
static void
_remove_participant (gpointer user_data,
    GObject *where_the_object_was);


static void fs_rtp_conference_handle_message (
    GstBin * bin,
    GstMessage * message);

static GstStateChangeReturn fs_rtp_conference_change_state (
    GstElement *element,
    GstStateChange transition);



static void
fs_rtp_conference_dispose (GObject * object)
{
  FsRtpConference *self = FS_RTP_CONFERENCE (object);
  GList *item;

  if (self->priv->disposed)
    return;

  if (self->rtpbin) {
    gst_object_unref (self->rtpbin);
    self->rtpbin = NULL;
  }

  for (item = g_list_first (self->priv->sessions);
       item;
       item = g_list_next (item))
    g_object_weak_unref (G_OBJECT (item->data), _remove_session, self);
  g_list_free (self->priv->sessions);
  self->priv->sessions = NULL;
  self->priv->sessions_cookie++;

  for (item = g_list_first (self->priv->participants);
       item;
       item = g_list_next (item))
    g_object_weak_unref (G_OBJECT (item->data), _remove_participant, self);
  g_list_free (self->priv->participants);
  self->priv->participants = NULL;

  self->priv->disposed = TRUE;

  G_OBJECT_CLASS (fs_rtp_conference_parent_class)->dispose (object);
}


static void
fs_rtp_conference_finalize (GObject * object)
{
  FsRtpConference *self = FS_RTP_CONFERENCE (object);

  /* Peek will always succeed here because we 'refed the class in the _init */
  g_type_class_unref (g_type_class_peek (FS_TYPE_RTP_SUB_STREAM));

  g_ptr_array_free (self->priv->threads, TRUE);

  G_OBJECT_CLASS (fs_rtp_conference_parent_class)->finalize (object);
}

static void
fs_rtp_conference_class_init (FsRtpConferenceClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *gstelement_class = GST_ELEMENT_CLASS (klass);
  FsConferenceClass *baseconf_class = FS_CONFERENCE_CLASS (klass);
  GstBinClass *gstbin_class = GST_BIN_CLASS (klass);

  g_type_class_add_private (klass, sizeof (FsRtpConferencePrivate));

  GST_DEBUG_CATEGORY_INIT (fsrtpconference_debug, "fsrtpconference", 0,
      "Farstream RTP Conference Element");
  GST_DEBUG_CATEGORY_INIT (fsrtpconference_disco, "fsrtpconference_disco",
      0, "Farstream RTP Codec Discovery");
  GST_DEBUG_CATEGORY_INIT (fsrtpconference_nego, "fsrtpconference_nego",
      0, "Farstream RTP Codec Negotiation");

  gst_element_class_add_pad_template (gstelement_class,
            gst_static_pad_template_get (&fs_rtp_conference_sink_template));
  gst_element_class_add_pad_template (gstelement_class,
            gst_static_pad_template_get (&fs_rtp_conference_src_template));

  gst_element_class_set_metadata (gstelement_class,
      "Farstream RTP Conference",
      "Generic/Bin/RTP",
      "A Farstream RTP Conference",
      "Olivier Crete <olivier.crete@collabora.co.uk>");

  baseconf_class->new_session =
    GST_DEBUG_FUNCPTR (fs_rtp_conference_new_session);
  baseconf_class->new_participant =
    GST_DEBUG_FUNCPTR (fs_rtp_conference_new_participant);

  gstbin_class->handle_message =
    GST_DEBUG_FUNCPTR (fs_rtp_conference_handle_message);

  gstelement_class->change_state =
    GST_DEBUG_FUNCPTR (fs_rtp_conference_change_state);

  gobject_class->finalize = GST_DEBUG_FUNCPTR (fs_rtp_conference_finalize);
  gobject_class->dispose = GST_DEBUG_FUNCPTR (fs_rtp_conference_dispose);
  gobject_class->set_property =
    GST_DEBUG_FUNCPTR (fs_rtp_conference_set_property);
  gobject_class->get_property =
    GST_DEBUG_FUNCPTR (fs_rtp_conference_get_property);

  g_object_class_install_property (gobject_class, PROP_SDES,
      g_param_spec_boxed ("sdes", "SDES Items for this conference",
          "SDES items to use for sessions in this conference",
          GST_TYPE_STRUCTURE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
fs_rtp_conference_init (FsRtpConference *conf)
{
  GST_DEBUG_OBJECT (conf, "fs_rtp_conference_init");

  conf->priv = FS_RTP_CONFERENCE_GET_PRIVATE (conf);

  conf->priv->disposed = FALSE;
  conf->priv->max_session_id = 1;

  conf->priv->threads = g_ptr_array_new ();

  conf->rtpbin = gst_element_factory_make ("rtpbin", NULL);

  if (!conf->rtpbin) {
    GST_ERROR_OBJECT (conf, "Could not create Rtpbin element");
    return;
  }

  if (!gst_bin_add (GST_BIN (conf), conf->rtpbin)) {
    GST_ERROR_OBJECT (conf, "Could not add Rtpbin element");
    gst_object_unref (conf->rtpbin);
    conf->rtpbin = NULL;
    return;
  }

  gst_object_ref (conf->rtpbin);

  g_signal_connect (conf->rtpbin, "request-pt-map",
                    G_CALLBACK (_rtpbin_request_pt_map), conf);
  g_signal_connect (conf->rtpbin, "pad-added",
                    G_CALLBACK (_rtpbin_pad_added), conf);
  g_signal_connect (conf->rtpbin, "on-bye-ssrc",
                    G_CALLBACK (_rtpbin_on_bye_ssrc), conf);
  g_signal_connect (conf->rtpbin, "on-ssrc-validated",
                    G_CALLBACK (_rtpbin_on_ssrc_validated), conf);

  /* We have to ref the class here because the class initialization
   * in GLib is not thread safe
   * http://bugzilla.gnome.org/show_bug.cgi?id=349410
   * http://bugzilla.gnome.org/show_bug.cgi?id=64764
   */
  g_type_class_ref (FS_TYPE_RTP_SUB_STREAM);
}

static void
fs_rtp_conference_get_property (GObject *object,
    guint prop_id,
    GValue *value,
    GParamSpec *pspec)
{
  FsRtpConference *self = FS_RTP_CONFERENCE (object);

  if (!self->rtpbin)
    return;

  switch (prop_id)
  {
    case PROP_SDES:
      g_object_get_property (G_OBJECT (self->rtpbin), "sdes", value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
fs_rtp_conference_set_property (GObject *object,
    guint prop_id,
    const GValue *value,
    GParamSpec *pspec)
{
  FsRtpConference *self = FS_RTP_CONFERENCE (object);

  if (!self->rtpbin)
    return;

  switch (prop_id)
  {
    case PROP_SDES:
      g_object_set_property (G_OBJECT (self->rtpbin), "sdes", value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstCaps *
_rtpbin_request_pt_map (GstElement *element, guint session_id,
                                         guint pt, gpointer user_data)
{
  FsRtpConference *self = FS_RTP_CONFERENCE (user_data);
  FsRtpSession *session = NULL;
  GstCaps *caps = NULL;

  session = fs_rtp_conference_get_session_by_id (self, session_id);

  if (session) {
    caps = fs_rtp_session_request_pt_map (session, pt);
    g_object_unref (session);
  } else {
    GST_WARNING_OBJECT (self,"Rtpbin %p tried to request the caps for "
                       " payload type %u for non-existent session %u",
                       element, pt, session_id);
  }

  return caps;
}

static void
_rtpbin_pad_added (GstElement *rtpbin, GstPad *new_pad,
  gpointer user_data)
{
  FsRtpConference *self = FS_RTP_CONFERENCE (user_data);
  gchar *name;

  GST_DEBUG_OBJECT (self, "pad %s:%s added", GST_DEBUG_PAD_NAME (new_pad));

  name = gst_pad_get_name (new_pad);

  if (g_str_has_prefix (name, "recv_rtp_src_"))
  {
    guint session_id, ssrc, pt;

    if (sscanf (name, "recv_rtp_src_%u_%u_%u",
            &session_id, &ssrc, &pt) == 3 && ssrc <= G_MAXUINT32)
    {
      FsRtpSession *session =
        fs_rtp_conference_get_session_by_id (self, session_id);

      if (session)
      {
        fs_rtp_session_new_recv_pad (session, new_pad, ssrc, pt);
        g_object_unref (session);
      }
    }
  }

  g_free (name);
}

static void
_rtpbin_on_bye_ssrc (GstElement *rtpbin,
    guint session_id,
    guint ssrc,
    gpointer user_data)
{
  FsRtpConference *self = FS_RTP_CONFERENCE (user_data);
  FsRtpSession *session =
    fs_rtp_conference_get_session_by_id (self, session_id);

  if (session)
  {
    fs_rtp_session_bye_ssrc (session, ssrc);

    g_object_unref (session);
  }
}

/**
 * fs_rtp_conference_get_session_by_id_locked
 * @self: The #FsRtpConference
 * @session_id: The session id
 *
 * Gets the #FsRtpSession from a list of sessions or NULL if it doesnt exist
 * You have to hold the GST_OBJECT_LOCK to call this function.
 *
 * Return value: A #FsRtpSession (unref after use) or NULL if it doesn't exist
 */
static FsRtpSession *
fs_rtp_conference_get_session_by_id_locked (FsRtpConference *self,
                                            guint session_id)
{
  GList *item = NULL;

  for (item = g_list_first (self->priv->sessions);
       item;
       item = g_list_next (item)) {
    FsRtpSession *session = item->data;

    if (session->id == session_id) {
      g_object_ref (session);
      break;
    }
  }

  if (item)
    return FS_RTP_SESSION (item->data);
  else
    return NULL;
}

/**
 * fs_rtp_conference_get_session_by_id
 * @self: The #FsRtpConference
 * @session_id: The session id
 *
 * Gets the #FsRtpSession from a list of sessions or NULL if it doesnt exist
 *
 * Return value: A #FsRtpSession (unref after use) or NULL if it doesn't exist
 */
static FsRtpSession *
fs_rtp_conference_get_session_by_id (FsRtpConference *self, guint session_id)
{
  FsRtpSession *session = NULL;

  GST_OBJECT_LOCK (self);
  session = fs_rtp_conference_get_session_by_id_locked (self, session_id);
  GST_OBJECT_UNLOCK (self);

  return session;
}

static void
_remove_session (gpointer user_data,
                 GObject *where_the_object_was)
{
  FsRtpConference *self = FS_RTP_CONFERENCE (user_data);

  GST_OBJECT_LOCK (self);
  self->priv->sessions =
    g_list_remove_all (self->priv->sessions, where_the_object_was);
  self->priv->sessions_cookie++;
  GST_OBJECT_UNLOCK (self);
}

static void
_remove_participant (gpointer user_data,
                 GObject *where_the_object_was)
{
  FsRtpConference *self = FS_RTP_CONFERENCE (user_data);

  GST_OBJECT_LOCK (self);
  self->priv->participants =
    g_list_remove_all (self->priv->participants, where_the_object_was);
  GST_OBJECT_UNLOCK (self);
}


static FsSession *
fs_rtp_conference_new_session (FsConference *conf,
                               FsMediaType media_type,
                               GError **error)
{
  FsRtpConference *self = FS_RTP_CONFERENCE (conf);
  FsSession *new_session = NULL;
  guint id;

  if (!self->rtpbin)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not create Rtpbin");
    return NULL;
  }

  GST_OBJECT_LOCK (self);
  do {
    id = self->priv->max_session_id++;
  } while (fs_rtp_conference_get_session_by_id_locked (self, id));
  GST_OBJECT_UNLOCK (self);

  new_session = FS_SESSION_CAST (fs_rtp_session_new (media_type, self, id,
     error));

  if (!new_session) {
    return NULL;
  }

  GST_OBJECT_LOCK (self);
  self->priv->sessions = g_list_append (self->priv->sessions, new_session);
  self->priv->sessions_cookie++;
  GST_OBJECT_UNLOCK (self);

  g_object_weak_ref (G_OBJECT (new_session), _remove_session, self);

  return new_session;
}


static FsParticipant *
fs_rtp_conference_new_participant (FsConference *conf,
    GError **error)
{
  FsRtpConference *self = FS_RTP_CONFERENCE (conf);
  FsParticipant *new_participant = NULL;

  if (!self->rtpbin)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not create Rtpbin");
    return NULL;
  }

  new_participant = FS_PARTICIPANT_CAST (fs_rtp_participant_new ());


  GST_OBJECT_LOCK (self);
  self->priv->participants = g_list_append (self->priv->participants,
      new_participant);
  GST_OBJECT_UNLOCK (self);

  g_object_weak_ref (G_OBJECT (new_participant), _remove_participant, self);

  return new_participant;
}

static void
fs_rtp_conference_handle_message (
    GstBin * bin,
    GstMessage * message)
{
  FsRtpConference *self = FS_RTP_CONFERENCE (bin);

  if (!self->rtpbin)
    goto out;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ELEMENT:
    {
      const GstStructure *s = gst_message_get_structure (message);

      /* we change the structure name and add the session ID to it */
      if (gst_structure_has_name (s, "application/x-rtp-source-sdes") &&
          gst_structure_has_field_typed (s, "session", G_TYPE_UINT) &&
          gst_structure_has_field_typed (s, "ssrc", G_TYPE_UINT) &&
          gst_structure_has_field_typed (s, "cname", G_TYPE_STRING))
      {
        guint session_id;
        guint ssrc;
        const GValue *val;
        FsRtpSession *session;
        const gchar *cname;

        val = gst_structure_get_value (s, "session");
        session_id = g_value_get_uint (val);

        val = gst_structure_get_value (s, "ssrc");
        ssrc = g_value_get_uint (val);

        cname = gst_structure_get_string (s, "cname");

        if (!ssrc || !cname)
        {
          GST_WARNING_OBJECT (self,
              "Got GstRTPBinSDES without a ssrc or a cname (ssrc:%u cname:%p)",
              ssrc, cname);
          break;
        }

        session = fs_rtp_conference_get_session_by_id (self, session_id);

        if (session) {
          fs_rtp_session_associate_ssrc_cname (session, ssrc, cname);
          g_object_unref (session);
        } else {
          GST_WARNING_OBJECT (self,"Our RtpBin announced a new association"
              "for non-existent session %u for ssrc: %u and cname %s",
              session_id, ssrc, cname);
        }
      }
      else if (gst_structure_has_name (s, "dtmf-event-processed") ||
          gst_structure_has_name (s, "dtmf-event-dropped"))
      {
        GList *item;
        guint cookie;


        GST_OBJECT_LOCK (self);
      restart:
        cookie = self->priv->sessions_cookie;
        for (item = self->priv->sessions; item; item = item->next)
        {
          GST_OBJECT_UNLOCK (self);
          if (fs_rtp_session_handle_dtmf_event_message (item->data, message))
          {
            gst_message_unref (message);
            message = NULL;
            goto out;
          }
          GST_OBJECT_LOCK (self);
          if (cookie != self->priv->sessions_cookie)
            goto restart;
        }
        GST_OBJECT_UNLOCK (self);

      }
    }
    break;
    case GST_MESSAGE_STREAM_STATUS:
    {
      GstStreamStatusType type;
      guint i;

      gst_message_parse_stream_status (message, &type, NULL);

      switch (type)
      {
        case GST_STREAM_STATUS_TYPE_ENTER:
          GST_OBJECT_LOCK (self);
          for (i = 0; i < self->priv->threads->len; i++)
          {
            if (g_ptr_array_index (self->priv->threads, i) ==
                g_thread_self ())
              goto done;
          }
          g_ptr_array_add (self->priv->threads, g_thread_self ());
        done:
          GST_OBJECT_UNLOCK (self);
          break;

        case GST_STREAM_STATUS_TYPE_LEAVE:
          GST_OBJECT_LOCK (self);
          while (g_ptr_array_remove_fast (self->priv->threads,
                  g_thread_self ()));
          GST_OBJECT_UNLOCK (self);
          break;

        default:
          /* Do nothing */
          break;
      }
    }
      break;
    default:
      break;
  }

 out:
  /* forward all messages to the parent */
  if (message)
    GST_BIN_CLASS (fs_rtp_conference_parent_class)->handle_message (bin,
        message);
}

static GstStateChangeReturn
fs_rtp_conference_change_state (GstElement *element, GstStateChange transition)
{
  FsRtpConference *self = FS_RTP_CONFERENCE (element);
  GstStateChangeReturn result;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!self->rtpbin)
      {
        GST_ERROR_OBJECT (element, "Could not create the RtpBin subelement");
        result = GST_STATE_CHANGE_FAILURE;
        goto failure;
      }
      break;
    default:
      break;
  }

  if ((result =
          GST_ELEMENT_CLASS (fs_rtp_conference_parent_class)->change_state (
            element, transition)) == GST_STATE_CHANGE_FAILURE)
    goto failure;

  return result;

 failure:
  {
    GST_ERROR_OBJECT (element, "parent failed state change");
    return result;
  }
}



/**
 * fs_codec_to_gst_caps
 * @codec: A #FsCodec to be converted
 *
 * This function converts a #FsCodec to a fixed #GstCaps with media type
 * application/x-rtp.
 *
 * Return value: A newly-allocated #GstCaps or %NULL if the codec was %NULL
 */

GstCaps *
fs_codec_to_gst_caps (const FsCodec *codec)
{
  GstCaps *caps;
  GstStructure *structure;
  GList *item;

  if (codec == NULL)
    return NULL;

  caps = gst_caps_new_empty_simple ("application/x-rtp");
  structure = gst_caps_get_structure (caps, 0);

  if (codec->encoding_name)
  {
    gchar *encoding_name = g_ascii_strup (codec->encoding_name, -1);

    gst_structure_set (structure,
        "encoding-name", G_TYPE_STRING, encoding_name,
        NULL);
    g_free (encoding_name);
  }

  if (codec->clock_rate)
    gst_structure_set (structure,
      "clock-rate", G_TYPE_INT, codec->clock_rate, NULL);

  if (fs_media_type_to_string (codec->media_type))
    gst_structure_set (structure, "media", G_TYPE_STRING,
      fs_media_type_to_string (codec->media_type), NULL);

  if (codec->id >= 0 && codec->id < 128)
    gst_structure_set (structure, "payload", G_TYPE_INT, codec->id, NULL);

  if (codec->channels)
  {
    gchar tmp[11];

    snprintf (tmp, 11, "%u", codec->channels);
    gst_structure_set (structure,
        "channels", G_TYPE_INT, codec->channels,
        "encoding-params", G_TYPE_STRING, tmp,
        NULL);
  }

  for (item = codec->optional_params;
       item;
       item = g_list_next (item))
  {
    FsCodecParameter *param = item->data;
    gchar *lower_name = g_ascii_strdown (param->name, -1);

    if (!strcmp (lower_name, "ptime") || !strcmp (lower_name, "maxptime"))
      gst_structure_set (structure, lower_name, G_TYPE_UINT,
          atoi (param->value), NULL);
    else
      gst_structure_set (structure, lower_name, G_TYPE_STRING, param->value,
          NULL);
    g_free (lower_name);
  }

  for (item = codec->feedback_params;
       item;
       item = g_list_next (item))
  {
    FsFeedbackParameter *param = item->data;
    gchar *lower_type = g_ascii_strdown (param->type, -1);
    gchar *rtcpfb_name;

    if (param->subtype[0])
    {
      gchar *lower_subt = g_ascii_strdown (param->subtype, -1);
      rtcpfb_name = g_strdup_printf ("rtcp-fb-%s-%s", lower_type, lower_subt);
      g_free (lower_subt);
    }
    else
    {
      rtcpfb_name = g_strdup_printf ("rtcp-fb-%s", lower_type);
    }

    gst_structure_set (structure,
        rtcpfb_name, G_TYPE_STRING, param->extra_params, NULL);
    g_free (lower_type);
    g_free (rtcpfb_name);
  }

  return caps;
}

static void
_rtpbin_on_ssrc_validated (GstElement *rtpbin,
    guint session_id,
    guint ssrc,
    gpointer user_data)
{
  FsRtpConference *self = FS_RTP_CONFERENCE (user_data);
  FsRtpSession *session =
    fs_rtp_conference_get_session_by_id (self, session_id);

  if (session)
  {
    fs_rtp_session_ssrc_validated (session, ssrc);

    g_object_unref (session);
  }
}

gboolean
fs_rtp_conference_is_internal_thread (FsRtpConference *self)
{
  guint i;
  gboolean ret = FALSE;

  GST_OBJECT_LOCK (self);
  for (i = 0; i < self->priv->threads->len; i++)
  {
    if (g_ptr_array_index (self->priv->threads, i) == g_thread_self ())
    {
      ret = TRUE;
      break;
    }
  }
  GST_OBJECT_UNLOCK (self);

  return ret;
}
