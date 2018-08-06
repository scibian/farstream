/*
 * Farstream - Farstream RTP Stream
 *
 * Copyright 2007 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * Copyright 2007 Nokia Corp.
 *
 * fs-rtp-stream.c - A Farstream RTP Stream gobject
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
 * SECTION:fs-rtp-stream
 * @short_description: A RTP stream in a #FsRtpSession in a #FsRtpConference
 *
 * This is the conjunction of a #FsRtpParticipant and a #FsRtpSession,
 * it is created by calling fs_session_new_stream() on a
 * #FsRtpSession.
 *
 * <refsect2><title>SRTP authentication & decryption</title>
 * <para>
 *
 * To tell #FsRtpStream to authenticate and decrypt the media it is
 * receiving using SRTP, one must set the parameters using a
 * #GstStructure named "FarstreamSRTP" and pass it to
 * fs_stream_set_decryption_parameters().
 *
 * The cipher, auth, and key must be specified, refer to the <link
 * linkend="FarstreamSRTP">FsRtpSession
 * documentation</link> for details.
 *
 * </para> </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fs-rtp-stream.h"

#include <gst/gst.h>

#include <farstream/fs-rtp.h>

/* Signals */
enum
{
  LAST_SIGNAL
};

/* props */
enum
{
  PROP_0,
#if 0
  /* TODO Do we really need this? */
  PROP_SOURCE_PADS,
#endif
  PROP_REMOTE_CODECS,
  PROP_NEGOTIATED_CODECS,
  PROP_CURRENT_RECV_CODECS,
  PROP_DIRECTION,
  PROP_PARTICIPANT,
  PROP_SESSION,
  PROP_RTP_HEADER_EXTENSIONS,
  PROP_DECRYPTION_PARAMETERS,
  PROP_SEND_RTCP_MUX,
  PROP_REQUIRE_ENCRYPTION
};

struct _FsRtpStreamPrivate
{
  FsRtpSession *session;
  FsStreamTransmitter *stream_transmitter;

  FsStreamDirection direction;
  gboolean send_rtcp_mux;

  stream_new_remote_codecs_cb new_remote_codecs_cb;
  stream_known_source_packet_receive_cb known_source_packet_received_cb;
  stream_sending_changed_locked_cb sending_changed_locked_cb;
  stream_ssrc_added_cb ssrc_added_cb;
  stream_get_new_stream_transmitter_cb get_new_stream_transmitter_cb;
  stream_decrypt_clear_locked_cb decrypt_clear_locked_cb;
  gpointer user_data_for_cb;

  /* protected by session lock */
  GstStructure *decryption_parameters;
  gboolean encrypted;

  gulong local_candidates_prepared_handler_id;
  gulong new_active_candidate_pair_handler_id;
  gulong new_local_candidate_handler_id;
  gulong error_handler_id;
  gulong known_source_packet_received_handler_id;
  gulong state_changed_handler_id;

  GMutex mutex;
};


G_DEFINE_TYPE(FsRtpStream, fs_rtp_stream, FS_TYPE_STREAM);

#define FS_RTP_STREAM_GET_PRIVATE(o)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((o), FS_TYPE_RTP_STREAM, FsRtpStreamPrivate))

static void fs_rtp_stream_dispose (GObject *object);
static void fs_rtp_stream_finalize (GObject *object);

static void fs_rtp_stream_get_property (GObject *object,
                                    guint prop_id,
                                    GValue *value,
                                    GParamSpec *pspec);
static void fs_rtp_stream_set_property (GObject *object,
                                    guint prop_id,
                                    const GValue *value,
                                    GParamSpec *pspec);

static gboolean fs_rtp_stream_add_remote_candidates (FsStream *stream,
                                                     GList *candidates,
                                                     GError **error);
static gboolean fs_rtp_stream_force_remote_candidates (FsStream *stream,
    GList *remote_candidates,
    GError **error);

static gboolean fs_rtp_stream_set_remote_codecs (FsStream *stream,
                                                 GList *remote_codecs,
                                                 GError **error);

static gboolean fs_rtp_stream_set_transmitter (FsStream *stream,
    const gchar *transmitter,
    GParameter *stream_transmitter_parameters,
    guint stream_transmitter_n_parameters,
    GError **error);

static void fs_rtp_stream_add_id (FsStream *stream, guint id);

static gboolean fs_rtp_stream_set_decryption_parameters (FsStream *stream,
    GstStructure *parameters, GError **error);

static void _local_candidates_prepared (
    FsStreamTransmitter *stream_transmitter,
    gpointer user_data);
static void _new_active_candidate_pair (
    FsStreamTransmitter *stream_transmitter,
    FsCandidate *candidate1,
    FsCandidate *candidate2,
    gpointer user_data);
static void _new_local_candidate (
    FsStreamTransmitter *stream_transmitter,
    FsCandidate *candidate,
    gpointer user_data);
static void
_known_source_packet_received (FsStreamTransmitter *st,
    guint component,
    GstBuffer *buffer,
    FsRtpStream *self);
static void _transmitter_error (
    FsStreamTransmitter *stream_transmitter,
    gint errorno,
    gchar *error_msg,
    gpointer user_data);
static void _substream_codec_changed (FsRtpSubStream *substream,
    FsRtpStream *stream);
static void _state_changed (FsStreamTransmitter *stream_transmitter,
    guint component,
    FsStreamState state,
    gpointer user_data);

// static guint signals[LAST_SIGNAL] = { 0 };

static void
fs_rtp_stream_class_init (FsRtpStreamClass *klass)
{
  GObjectClass *gobject_class;
  FsStreamClass *stream_class = FS_STREAM_CLASS (klass);

  gobject_class = (GObjectClass *) klass;

  gobject_class->set_property = fs_rtp_stream_set_property;
  gobject_class->get_property = fs_rtp_stream_get_property;
  gobject_class->dispose = fs_rtp_stream_dispose;
  gobject_class->finalize = fs_rtp_stream_finalize;

  stream_class->add_remote_candidates = fs_rtp_stream_add_remote_candidates;
  stream_class->set_remote_codecs = fs_rtp_stream_set_remote_codecs;
  stream_class->force_remote_candidates = fs_rtp_stream_force_remote_candidates;
  stream_class->add_id = fs_rtp_stream_add_id;
  stream_class->set_transmitter = fs_rtp_stream_set_transmitter;
  stream_class->set_decryption_parameters =
      fs_rtp_stream_set_decryption_parameters;

  g_type_class_add_private (klass, sizeof (FsRtpStreamPrivate));

  g_object_class_override_property (gobject_class,
                                    PROP_REMOTE_CODECS,
                                    "remote-codecs");
  g_object_class_override_property (gobject_class,
                                    PROP_NEGOTIATED_CODECS,
                                    "negotiated-codecs");
  g_object_class_override_property (gobject_class,
                                    PROP_CURRENT_RECV_CODECS,
                                    "current-recv-codecs");
  g_object_class_override_property (gobject_class,
                                    PROP_DIRECTION,
                                    "direction");
  g_object_class_override_property (gobject_class,
                                    PROP_PARTICIPANT,
                                    "participant");
  g_object_class_override_property (gobject_class,
                                    PROP_SESSION,
                                    "session");
  g_object_class_override_property (gobject_class,
                                    PROP_DECRYPTION_PARAMETERS,
                                    "decryption-parameters");
  g_object_class_override_property (gobject_class,
                                    PROP_REQUIRE_ENCRYPTION,
                                    "require-encryption");

  g_object_class_install_property (gobject_class,
      PROP_RTP_HEADER_EXTENSIONS,
      g_param_spec_boxed ("rtp-header-extensions",
          "RTP Header extension desired by participant in this stream",
          "GList of RTP Header extensions that the participant for this stream"
          " would like to use",
          FS_TYPE_RTP_HEADER_EXTENSION_LIST,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_SEND_RTCP_MUX,
      g_param_spec_boolean ("send-rtcp-mux",
          "Send RTCP muxed with on the same RTP connection",
          "Send RTCP muxed with on the same RTP connection",
          FALSE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
fs_rtp_stream_init (FsRtpStream *self)
{
  /* member init */
  self->priv = FS_RTP_STREAM_GET_PRIVATE (self);

  self->priv->session = NULL;
  self->participant = NULL;
  self->priv->stream_transmitter = NULL;

  g_mutex_init (&self->priv->mutex);

  self->priv->direction = FS_DIRECTION_NONE;
}

static FsRtpSession *
fs_rtp_stream_get_session (FsRtpStream *self, GError **error)
{
  FsRtpSession *session;

  g_mutex_lock (&self->priv->mutex);
  session = self->priv->session;
  if (session)
    g_object_ref (session);
  g_mutex_unlock (&self->priv->mutex);

  if (!session)
    g_set_error (error, FS_ERROR, FS_ERROR_DISPOSED,
        "Called function after stream has been disposed");

  return session;
}


static FsStreamTransmitter *
fs_rtp_stream_get_stream_transmitter (FsRtpStream *self, GError **error)
{
  FsRtpSession *session = fs_rtp_stream_get_session (self, error);
  FsStreamTransmitter *st = NULL;

  if (!session)
    return NULL;

  FS_RTP_SESSION_LOCK (session);
  st = self->priv->stream_transmitter;
  if (st)
    g_object_ref (st);
  FS_RTP_SESSION_UNLOCK (session);

  if (!st)
    g_set_error (error, FS_ERROR, FS_ERROR_DISPOSED,
        "Stream transmitter not set (or stream has been disposed)");

  g_object_unref (session);
  return st;
}

static void
fs_rtp_stream_dispose (GObject *object)
{
  FsRtpStream *self = FS_RTP_STREAM (object);
  FsStreamTransmitter *st;
  FsRtpParticipant *participant;
  FsRtpSession *session = fs_rtp_stream_get_session (self, NULL);

  if (!session)
    return;

  g_mutex_lock (&self->priv->mutex);
  self->priv->session = NULL;
  g_mutex_unlock (&self->priv->mutex);

  FS_RTP_SESSION_LOCK (session);

  if (self->priv->sending_changed_locked_cb &&
      self->priv->direction & FS_DIRECTION_SEND)
    self->priv->sending_changed_locked_cb (self, FALSE,
        self->priv->user_data_for_cb);

  participant = self->participant;
  self->participant = NULL;

  st = self->priv->stream_transmitter;
  self->priv->stream_transmitter = NULL;

  if (st)
  {
    g_signal_handler_disconnect (st,
        self->priv->local_candidates_prepared_handler_id);
    g_signal_handler_disconnect (st,
        self->priv->new_active_candidate_pair_handler_id);
    g_signal_handler_disconnect (st,
        self->priv->new_local_candidate_handler_id);
    g_signal_handler_disconnect (st,
        self->priv->error_handler_id);
    g_signal_handler_disconnect (st,
        self->priv->known_source_packet_received_handler_id);
    g_signal_handler_disconnect (st,
        self->priv->state_changed_handler_id);

    FS_RTP_SESSION_UNLOCK (session);
    fs_stream_transmitter_stop (st);
    g_object_unref (st);
    FS_RTP_SESSION_LOCK (session);
  }

  while (self->substreams)
  {
    FsRtpSubStream *substream = self->substreams->data;
    self->substreams = g_list_remove (self->substreams, substream);
    FS_RTP_SESSION_UNLOCK (session);
    fs_rtp_sub_stream_stop (substream);
    g_object_unref (substream);
    FS_RTP_SESSION_LOCK (session);
  }

  FS_RTP_SESSION_UNLOCK (session);

  g_object_unref (participant);
  g_object_unref (session);
  g_object_unref (session);

  G_OBJECT_CLASS (fs_rtp_stream_parent_class)->dispose (object);
}

static void
fs_rtp_stream_finalize (GObject *object)
{
  FsRtpStream *self = FS_RTP_STREAM (object);

  fs_codec_list_destroy (self->remote_codecs);
  fs_codec_list_destroy (self->negotiated_codecs);

  if (self->priv->decryption_parameters)
    gst_structure_free (self->priv->decryption_parameters);

  g_mutex_clear (&self->priv->mutex);

  G_OBJECT_CLASS (fs_rtp_stream_parent_class)->finalize (object);
}

static gboolean
_codec_list_has_codec (GList *list, FsCodec *codec)
{
  for (; list; list = g_list_next (list))
  {
    FsCodec *listcodec = list->data;
    if (fs_codec_are_equal (codec, listcodec))
      return TRUE;
  }

  return FALSE;
}

static void
fs_rtp_stream_get_property (GObject *object,
                            guint prop_id,
                            GValue *value,
                            GParamSpec *pspec)
{
  FsRtpStream *self = FS_RTP_STREAM (object);
  FsRtpSession *session = fs_rtp_stream_get_session (self, NULL);

  if (!session)
    return;

  switch (prop_id) {
    case PROP_REMOTE_CODECS:
      FS_RTP_SESSION_LOCK (session);
      g_value_set_boxed (value, self->remote_codecs);
      FS_RTP_SESSION_UNLOCK (session);
      break;
    case PROP_NEGOTIATED_CODECS:
      FS_RTP_SESSION_LOCK (session);
      g_value_set_boxed (value, self->negotiated_codecs);
      FS_RTP_SESSION_UNLOCK (session);
      break;
    case PROP_SESSION:
      g_value_set_object (value, session);
      break;
    case PROP_PARTICIPANT:
      FS_RTP_SESSION_LOCK (session);
      g_value_set_object (value, self->participant);
      FS_RTP_SESSION_UNLOCK (session);
      break;
    case PROP_DIRECTION:
      g_value_set_flags (value, self->priv->direction);
      break;
    case PROP_CURRENT_RECV_CODECS:
      {
        GList *codeclist = NULL;
        GList *substream_item;

        FS_RTP_SESSION_LOCK (session);
        for (substream_item = g_list_first (self->substreams);
             substream_item;
             substream_item = g_list_next (substream_item))
        {
          FsRtpSubStream *substream = substream_item->data;

          if (substream->codec)
          {
            if (!_codec_list_has_codec (codeclist, substream->codec))
              codeclist = g_list_append (codeclist,
                  fs_codec_copy (substream->codec));
          }
        }

        g_value_take_boxed (value, codeclist);
        FS_RTP_SESSION_UNLOCK (session);
      }
      break;
    case PROP_RTP_HEADER_EXTENSIONS:
      FS_RTP_SESSION_LOCK (session);
      g_value_set_boxed (value, self->hdrext);
      FS_RTP_SESSION_UNLOCK (session);
      break;
    case PROP_DECRYPTION_PARAMETERS:
      FS_RTP_SESSION_LOCK (session);
      g_value_set_boxed (value, self->priv->decryption_parameters);
      FS_RTP_SESSION_UNLOCK (session);
      break;
    case PROP_SEND_RTCP_MUX:
      FS_RTP_SESSION_LOCK (session);
      if (self->priv->stream_transmitter == NULL ||
          g_object_class_find_property (
              G_OBJECT_GET_CLASS (self->priv->stream_transmitter),
              "send-component-mux") != NULL)
        g_value_set_boolean (value, self->priv->send_rtcp_mux);
      else
        g_value_set_boolean (value, FALSE);
      FS_RTP_SESSION_UNLOCK (session);
      break;
    case PROP_REQUIRE_ENCRYPTION:
      FS_RTP_SESSION_LOCK (session);
      g_value_set_boolean (value, fs_rtp_stream_requires_crypto_locked (self));
      FS_RTP_SESSION_UNLOCK (session);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  g_object_unref (session);
}

static void
fs_rtp_stream_set_property (GObject *object,
                            guint prop_id,
                            const GValue *value,
                            GParamSpec *pspec)
{
  FsRtpStream *self = FS_RTP_STREAM (object);
  GList *item;

  switch (prop_id) {
    case PROP_SESSION:
      self->priv->session = FS_RTP_SESSION (g_value_dup_object (value));
      break;
    case PROP_PARTICIPANT:
      self->participant = FS_RTP_PARTICIPANT (g_value_dup_object (value));
      break;
    case PROP_DIRECTION:
      {
        FsStreamTransmitter *st = NULL;
        GList *copy = NULL;
        FsRtpSession *session = fs_rtp_stream_get_session (self, NULL);
        FsStreamDirection dir;

        if (!session)
        {
          self->priv->direction = g_value_get_flags (value);
          return;
        }

        FS_RTP_SESSION_LOCK (session);
        if (self->priv->sending_changed_locked_cb &&
            (self->priv->direction & FS_DIRECTION_SEND) !=
            (g_value_get_flags (value) & FS_DIRECTION_SEND))
          self->priv->sending_changed_locked_cb (self,
              g_value_get_flags (value) & FS_DIRECTION_SEND,
              self->priv->user_data_for_cb);

        dir = self->priv->direction = g_value_get_flags (value);
        FS_RTP_SESSION_UNLOCK (session);
        st = fs_rtp_stream_get_stream_transmitter (self, NULL);
        if (st)
        {
          g_object_set (self->priv->stream_transmitter, "sending",
              dir & FS_DIRECTION_SEND, NULL);
          g_object_unref (st);
        }

        FS_RTP_SESSION_LOCK (session);
        copy = g_list_copy (g_list_first (self->substreams));
        g_list_foreach (copy, (GFunc) g_object_ref, NULL);
        FS_RTP_SESSION_UNLOCK (session);

        for (item = copy;  item; item = g_list_next (item))
          g_object_set (G_OBJECT (item->data),
              "receiving", ((dir & FS_DIRECTION_RECV) != 0),
              NULL);
        g_list_foreach (copy, (GFunc) g_object_unref, NULL);
        g_list_free (copy);
        g_object_unref (session);
      }
      break;
    case PROP_RTP_HEADER_EXTENSIONS:
      {
        FsRtpSession *session = fs_rtp_stream_get_session (self, NULL);
        if (session)
        {
          FS_RTP_SESSION_LOCK (session);
          fs_rtp_header_extension_list_destroy (self->hdrext);
          self->hdrext = g_value_dup_boxed (value);
          FS_RTP_SESSION_UNLOCK (session);
          /* The callbadck can not fail because it does not change
           * the codecs
           */
          self->priv->new_remote_codecs_cb (NULL, NULL, NULL,
              self->priv->user_data_for_cb);
          g_object_unref (session);
        }
      }
      break;
    case PROP_SEND_RTCP_MUX:
      {
        FsRtpSession *session = fs_rtp_stream_get_session (self, NULL);

        if (session) {
          FS_RTP_SESSION_LOCK (session);
          self->priv->send_rtcp_mux = g_value_get_boolean (value);
          if (self->priv->stream_transmitter != NULL &&
              g_object_class_find_property (
                  G_OBJECT_GET_CLASS (self->priv->stream_transmitter),
                  "send-component-mux") != NULL)
            g_object_set (self->priv->stream_transmitter,
                "send-component-mux", self->priv->send_rtcp_mux, NULL);
          FS_RTP_SESSION_UNLOCK (session);
        }
      }
      break;
    case PROP_REQUIRE_ENCRYPTION:
      {
        FsRtpSession *session = fs_rtp_stream_get_session (self, NULL);

        if (session) {
          FS_RTP_SESSION_LOCK (session);

          if (self->priv->encrypted != g_value_get_boolean (value))
          {
            self->priv->encrypted = g_value_get_boolean (value);

            if (!self->priv->decrypt_clear_locked_cb (self,
                    self->priv->user_data_for_cb)) {
              g_warning ("Can't set encryption because srtpdec is not"
                  " installed");
              self->priv->encrypted = FALSE;
            }
          }
          FS_RTP_SESSION_UNLOCK (session);
        }
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

}


/**
 * fs_rtp_stream_add_remote_candidate:
 */
static gboolean
fs_rtp_stream_add_remote_candidates (FsStream *stream, GList *candidates,
                                     GError **error)
{
  FsRtpStream *self = FS_RTP_STREAM (stream);
  FsStreamTransmitter *st = fs_rtp_stream_get_stream_transmitter (self, error);
  gboolean ret = FALSE;

  if (!st)
    return FALSE;

  ret = fs_stream_transmitter_add_remote_candidates (st, candidates, error);

  g_object_unref (st);
  return ret;
}

/**
 * fs_rtp_stream_force_remote_candidates
 *
 * Implement FsStream -> force_remote_candidates
 * by calling the same function in the stream transmittrer
 */

static gboolean
fs_rtp_stream_force_remote_candidates (FsStream *stream,
    GList *remote_candidates,
    GError **error)
{
  FsRtpStream *self = FS_RTP_STREAM (stream);
  FsStreamTransmitter *st = fs_rtp_stream_get_stream_transmitter (self, error);
  gboolean ret = FALSE;

  if (!st)
    return FALSE;


  ret = fs_stream_transmitter_force_remote_candidates (
      self->priv->stream_transmitter, remote_candidates,
      error);

  g_object_unref (st);
  return ret;
}


/**
 * fs_rtp_stream_set_remote_codecs:
 * @stream: an #FsStream
 * @remote_codecs: a #GList of #FsCodec representing the remote codecs
 * @error: location of a #GError, or NULL if no error occured
 *
 * This function will set the list of remote codecs for this stream. If
 * the given remote codecs couldn't be negotiated with the list of local
 * codecs or already negotiated codecs for the corresponding #FsSession, @error
 * will be set and %FALSE will be returned. The @remote_codecs list will be
 * copied so it must be free'd using fs_codec_list_destroy() when done.
 *
 * Returns: %FALSE if the remote codecs couldn't be set.
 */
static gboolean
fs_rtp_stream_set_remote_codecs (FsStream *stream,
                                 GList *remote_codecs, GError **error)
{
  FsRtpStream *self = FS_RTP_STREAM (stream);
  GList *item = NULL;
  FsMediaType media_type;
  FsRtpSession *session = fs_rtp_stream_get_session (self, error);

  if (!session)
    return FALSE;

  if (remote_codecs == NULL) {
    g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
      "You can not set NULL remote codecs");
    goto error;
  }

  g_object_get (session, "media-type", &media_type, NULL);

  for (item = g_list_first (remote_codecs); item; item = g_list_next (item))
  {
    FsCodec *codec = item->data;

    if (!codec->encoding_name)
    {
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "The codec must have an encoding name");
      goto error;
    }
    if (codec->id < 0 || codec->id > 128)
    {
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "The codec id must be between 0 ans 128 for %s",
          codec->encoding_name);
      goto error;
    }
    if (codec->media_type != media_type)
    {
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "The media type for codec %s is not %s", codec->encoding_name,
          fs_media_type_to_string (media_type));
      goto error;
    }
  }

  if (self->priv->new_remote_codecs_cb (self, remote_codecs, error,
          self->priv->user_data_for_cb))
  {
    gboolean is_new = TRUE;

    FS_RTP_SESSION_LOCK (session);
    if (self->remote_codecs)
    {
      is_new = !fs_codec_list_are_equal (self->remote_codecs, remote_codecs);
      fs_codec_list_destroy (self->remote_codecs);
    }
    self->remote_codecs = fs_codec_list_copy (remote_codecs);
    FS_RTP_SESSION_UNLOCK (session);

    if (is_new)
      g_object_notify (G_OBJECT (stream), "remote-codecs");
  } else {
    goto error;
  }

  g_object_unref (session);
  return TRUE;

 error:

  g_object_unref (session);
  return FALSE;
}

/**
 * fs_rtp_stream_new:
 * @session: The #FsRtpSession this stream is a child of
 * @participant: The #FsRtpParticipant this stream is for
 * @direction: the initial #FsDirection for this stream
 * @new_remote_codecs_cb: Callback called when the remote codecs change
 * (ie when fs_rtp_stream_set_remote_codecs() is called). One must hold
 * the session lock across calls.
 * @known_source_packet_received: Callback called when a packet from a
 * known source is receive.
 * @sending_changed_locked_cb: Callback called when the sending status of
 *  this stream changes
 * @user_data: User data for the callbacks.
 * This function create a new stream
 *
 * Returns: the newly created string or NULL on error
 */

FsRtpStream *
fs_rtp_stream_new (FsRtpSession *session,
    FsRtpParticipant *participant,
    FsStreamDirection direction,
    stream_new_remote_codecs_cb new_remote_codecs_cb,
    stream_known_source_packet_receive_cb known_source_packet_received_cb,
    stream_sending_changed_locked_cb sending_changed_locked_cb,
    stream_ssrc_added_cb ssrc_added_cb,
    stream_get_new_stream_transmitter_cb get_new_stream_transmitter_cb,
    stream_decrypt_clear_locked_cb decrypt_clear_locked_cb,
    gpointer user_data_for_cb)
{
  FsRtpStream *self;

  g_return_val_if_fail (session, NULL);
  g_return_val_if_fail (participant, NULL);
  g_return_val_if_fail (new_remote_codecs_cb, NULL);
  g_return_val_if_fail (known_source_packet_received_cb, NULL);

  self = g_object_new (FS_TYPE_RTP_STREAM,
    "session", session,
    "participant", participant,
    "direction", direction,
    NULL);

  self->priv->new_remote_codecs_cb = new_remote_codecs_cb;
  self->priv->known_source_packet_received_cb = known_source_packet_received_cb;
  self->priv->sending_changed_locked_cb = sending_changed_locked_cb;
  self->priv->ssrc_added_cb = ssrc_added_cb;
  self->priv->get_new_stream_transmitter_cb = get_new_stream_transmitter_cb;
  self->priv->decrypt_clear_locked_cb = decrypt_clear_locked_cb;

  self->priv->user_data_for_cb = user_data_for_cb;

  return self;
}


static void
_local_candidates_prepared (FsStreamTransmitter *stream_transmitter,
    gpointer user_data)
{
  FsRtpStream *self = FS_RTP_STREAM (user_data);
  GstElement *conf = NULL;
  FsRtpSession *session = fs_rtp_stream_get_session (self, NULL);

  if (!session)
    return;

  g_object_get (session, "conference", &conf, NULL);

  if (!conf) {
    g_object_unref (session);
    return;
  }

  gst_element_post_message (conf,
      gst_message_new_element (GST_OBJECT (conf),
          gst_structure_new ("farstream-local-candidates-prepared",
              "stream", FS_TYPE_STREAM, self,
              NULL)));

  gst_object_unref (conf);
  g_object_unref (session);
}


static void
_new_active_candidate_pair (
    FsStreamTransmitter *stream_transmitter,
    FsCandidate *local_candidate,
    FsCandidate *remote_candidate,
    gpointer user_data)
{
  FsRtpStream *self = FS_RTP_STREAM (user_data);
  FsRtpSession *session = fs_rtp_stream_get_session (self, NULL);
  GstElement *conf = NULL;

  if (!session)
    return;

  g_object_get (session, "conference", &conf, NULL);

  if (!conf) {
    g_object_unref (session);
    return;
  }

  gst_element_post_message (conf,
      gst_message_new_element (GST_OBJECT (conf),
          gst_structure_new ("farstream-new-active-candidate-pair",
              "stream", FS_TYPE_STREAM, self,
              "local-candidate", FS_TYPE_CANDIDATE, local_candidate,
              "remote-candidate", FS_TYPE_CANDIDATE, remote_candidate,
              NULL)));

  gst_object_unref (conf);
  g_object_unref (session);
}


static void
_new_local_candidate (
    FsStreamTransmitter *stream_transmitter,
    FsCandidate *candidate,
    gpointer user_data)
{
  FsRtpStream *self = FS_RTP_STREAM (user_data);
  FsRtpSession *session = fs_rtp_stream_get_session (self, NULL);
  GstElement *conf = NULL;

  if (!session)
    return;

  g_object_get (session, "conference", &conf, NULL);

  if (!conf) {
    g_object_unref (session);
    return;
  }

  gst_element_post_message (conf,
      gst_message_new_element (GST_OBJECT (conf),
          gst_structure_new ("farstream-new-local-candidate",
              "stream", FS_TYPE_STREAM, self,
              "candidate", FS_TYPE_CANDIDATE, candidate,
              NULL)));

  gst_object_unref (conf);
  g_object_unref (session);
}

static void
_transmitter_error (
    FsStreamTransmitter *stream_transmitter,
    gint errorno,
    gchar *error_msg,
    gpointer user_data)
{
  FsStream *stream = FS_STREAM (user_data);

  fs_stream_emit_error (stream, errorno, error_msg);
}

static void
_known_source_packet_received (FsStreamTransmitter *st,
    guint component,
    GstBuffer *buffer,
    FsRtpStream *self)
{
  self->priv->known_source_packet_received_cb (self, component, buffer,
      self->priv->user_data_for_cb);
}

static void
_state_changed (FsStreamTransmitter *stream_transmitter,
    guint component,
    FsStreamState state,
    gpointer user_data)
{
  FsRtpStream *self = FS_RTP_STREAM (user_data);
  FsRtpSession *session = fs_rtp_stream_get_session (self, NULL);
  GstElement *conf = NULL;

  if (!session)
    return;

  g_object_get (session, "conference", &conf, NULL);

  if (!conf) {
    g_object_unref (session);
    return;
  }

  gst_element_post_message (conf,
      gst_message_new_element (GST_OBJECT (conf),
          gst_structure_new ("farstream-component-state-changed",
              "stream", FS_TYPE_STREAM, self,
              "component", G_TYPE_UINT, component,
              "state", FS_TYPE_STREAM_STATE, state,
              NULL)));

  gst_object_unref (conf);
  g_object_unref (session);

  if (component == 1 && state == FS_STREAM_STATE_FAILED)
    fs_stream_emit_error (FS_STREAM (self), FS_ERROR_CONNECTION_FAILED,
        "Could not establish connection on the RTP component");
}

static void
_substream_src_pad_added (FsRtpSubStream *substream, GstPad *pad,
                          FsCodec *codec, gpointer user_data)
{
  FsStream *stream = FS_STREAM (user_data);

  fs_stream_emit_src_pad_added (stream, pad, codec);
}

static void
_substream_error (FsRtpSubStream *substream,
    gint errorno,
    gchar *error_msg,
    gchar *debug_msg,
    gpointer user_data)
{
  FsStream *stream = FS_STREAM (user_data);

  fs_stream_emit_error (stream, errorno, error_msg);
}


static void
_substream_unlinked (FsRtpSubStream *substream, gpointer user_data)
{
  FsRtpStream *stream = FS_RTP_STREAM (user_data);
  FsRtpSession *session =  fs_rtp_stream_get_session (stream, NULL);

  if (!session)
    return;

  FS_RTP_SESSION_LOCK (session);
  stream->substreams = g_list_remove (stream->substreams,
      substream);
  FS_RTP_SESSION_UNLOCK (session);

  fs_rtp_sub_stream_stop (substream);

  g_object_unref (substream);
  g_object_unref (session);
}


/**
 * fs_rtp_stream_add_substream_unlock:
 * @stream: a #FsRtpStream
 * @substream: the #FsRtpSubStream to associate with this stream
 *
 * This functions associates a substream with this stream
 *
 * You must enter this function with the session lock held and it will release
 * it.
 *
 * Returns: TRUE on success, FALSE on failure
 */
gboolean
fs_rtp_stream_add_substream_unlock (FsRtpStream *stream,
    FsRtpSubStream *substream,
    GError **error)
{
  gboolean ret = TRUE;
  FsRtpSession *session = fs_rtp_stream_get_session (stream, error);

  if (!session)
    return FALSE;

  stream->substreams = g_list_prepend (stream->substreams,
      substream);
  g_object_set (substream,
      "stream", stream,
      "receiving", ((stream->priv->direction & FS_DIRECTION_RECV) != 0),
      NULL);

  g_signal_connect_object (substream, "unlinked",
      G_CALLBACK (_substream_unlinked), stream, 0);
  g_signal_connect_object (substream, "src-pad-added",
      G_CALLBACK (_substream_src_pad_added), stream, 0);
  g_signal_connect_object (substream, "codec-changed",
      G_CALLBACK (_substream_codec_changed), stream, 0);
  g_signal_connect_object (substream, "error",
      G_CALLBACK (_substream_error), stream, 0);

  fs_rtp_sub_stream_verify_codec_locked (substream);

  /* Only announce a pad if it has a codec attached to it */
  if (substream->codec)
    ret = fs_rtp_sub_stream_add_output_ghostpad_unlock (substream, error);
  else
    FS_RTP_SESSION_UNLOCK (session);

  g_object_unref (session);

  return ret;
}

/**
 *  _substream_codec_changed
 * @substream: The #FsRtpSubStream that may have a new receive codec
 * @stream: a #FsRtpStream
 *
 * This function checks if the specified substream introduces a new codec
 * not present in another substream and if it does, it emits a GstMessage
 * and the notify signal
 */

static void
_substream_codec_changed (FsRtpSubStream *substream,
    FsRtpStream *stream)
{
  GList *substream_item = NULL;
  GList *codeclist = NULL;
  FsRtpSession *session = fs_rtp_stream_get_session (stream, NULL);

  if (!session)
    return;

  FS_RTP_SESSION_LOCK (session);

  if (!substream->codec)
  {
    FS_RTP_SESSION_UNLOCK (session);
    g_object_unref (session);
    return;
  }

  codeclist = g_list_prepend (NULL, fs_codec_copy (substream->codec));

  for (substream_item = stream->substreams;
       substream_item;
       substream_item = g_list_next (substream_item))
  {
    FsRtpSubStream *othersubstream = substream_item->data;

    if (othersubstream != substream)
    {
      if (othersubstream->codec)
      {
        if (fs_codec_are_equal (substream->codec, othersubstream->codec))
          break;

        if (!_codec_list_has_codec (codeclist, othersubstream->codec))
          codeclist = g_list_append (codeclist,
              fs_codec_copy (othersubstream->codec));
      }
    }
  }

  FS_RTP_SESSION_UNLOCK (session);

  if (substream_item == NULL)
  {
    GstElement *conf = NULL;

    g_object_notify (G_OBJECT (stream), "current-recv-codecs");

    g_object_get (session, "conference", &conf, NULL);

    gst_element_post_message (conf,
        gst_message_new_element (GST_OBJECT (conf),
            gst_structure_new ("farstream-recv-codecs-changed",
                "stream", FS_TYPE_STREAM, stream,
                "codecs", FS_TYPE_CODEC_LIST, codeclist,
                NULL)));

    gst_object_unref (conf);
  }

  fs_codec_list_destroy (codeclist);
  g_object_unref (session);
}

/**
 * fs_rtp_stream_set_negotiated_codecs_unlock
 * @stream: a #FsRtpStream
 * @codecs: The #GList of #FsCodec to set for the negotiated-codecs property
 *
 * This function sets the value of the FsStream:negotiated-codecs property.
 * Unlike most other functions in this element, it TAKES the reference to the
 * codecs, so you have to give it its own copy.
 *
 * You must enter this function with the session lock held and it will release
 * it.
 */
void
fs_rtp_stream_set_negotiated_codecs_unlock (FsRtpStream *stream,
    GList *codecs)
{
  FsRtpSession *session = fs_rtp_stream_get_session (stream, NULL);

  if (!session)
    return;

  if (fs_codec_list_are_equal (stream->negotiated_codecs, codecs))
  {
    fs_codec_list_destroy (codecs);
    FS_RTP_SESSION_UNLOCK (session);
    g_object_unref (session);
    return;
  }

  if (stream->negotiated_codecs)
    fs_codec_list_destroy (stream->negotiated_codecs);

  stream->negotiated_codecs = codecs;

  FS_RTP_SESSION_UNLOCK (session);

  g_object_notify (G_OBJECT (stream), "negotiated-codecs");

  g_object_unref (session);
}

static void
fs_rtp_stream_add_id (FsStream *stream, guint id)
{
  FsRtpStream *self = FS_RTP_STREAM (stream);
  FsRtpSession *session = fs_rtp_stream_get_session (self, NULL);

  if (!session)
    return;

  if (self->priv->ssrc_added_cb)
    self->priv->ssrc_added_cb (self, id, self->priv->user_data_for_cb);

  g_object_unref (session);
}

static gboolean
fs_rtp_stream_set_transmitter (FsStream *stream,
    const gchar *transmitter,
    GParameter *stream_transmitter_parameters,
    guint stream_transmitter_n_parameters,
    GError **error)
{
  FsStreamTransmitter *st = NULL;
  FsRtpStream *self = FS_RTP_STREAM (stream);
  FsRtpSession *session = fs_rtp_stream_get_session (self, error);

  if (!session)
    return FALSE;

  FS_RTP_SESSION_LOCK (session);
  if (self->priv->stream_transmitter)
  {
    FS_RTP_SESSION_UNLOCK (session);
    g_object_unref (session);
    return FALSE;
  }
  FS_RTP_SESSION_UNLOCK (session);

  st = self->priv->get_new_stream_transmitter_cb (self,
      FS_PARTICIPANT (self->participant), transmitter,
      stream_transmitter_parameters, stream_transmitter_n_parameters, error,
      self->priv->user_data_for_cb);

  if (!st)
  {
    g_object_unref (session);
    return FALSE;
  }


  g_object_set (st, "sending",
    self->priv->direction & FS_DIRECTION_SEND, NULL);

  self->priv->local_candidates_prepared_handler_id =
    g_signal_connect_object (st,
        "local-candidates-prepared",
        G_CALLBACK (_local_candidates_prepared),
        self, 0);
  self->priv->new_active_candidate_pair_handler_id =
    g_signal_connect_object (st,
        "new-active-candidate-pair",
        G_CALLBACK (_new_active_candidate_pair),
        self, 0);
  self->priv->new_local_candidate_handler_id =
    g_signal_connect_object (st,
        "new-local-candidate",
        G_CALLBACK (_new_local_candidate),
        self, 0);
  self->priv->error_handler_id =
    g_signal_connect_object (st,
        "error",
        G_CALLBACK (_transmitter_error),
        self, 0);
  self->priv->known_source_packet_received_handler_id =
    g_signal_connect_object (st,
        "known-source-packet-received",
        G_CALLBACK (_known_source_packet_received),
        self, 0);
  self->priv->state_changed_handler_id =
    g_signal_connect_object (st,
        "state-changed",
        G_CALLBACK (_state_changed),
        self, 0);


  FS_RTP_SESSION_LOCK (session);
  self->priv->stream_transmitter = st;
  if (self->priv->direction & FS_DIRECTION_SEND)
    self->priv->sending_changed_locked_cb (self,
        self->priv->direction & FS_DIRECTION_SEND,
        self->priv->user_data_for_cb);
  if (g_object_class_find_property (G_OBJECT_GET_CLASS (st),
          "send-component-mux") != NULL)
    g_object_set (st, "send-component-mux", self->priv->send_rtcp_mux, NULL);
  FS_RTP_SESSION_UNLOCK (session);

  if (!fs_stream_transmitter_gather_local_candidates (st, error))
  {

    FS_RTP_SESSION_LOCK (session);
    self->priv->stream_transmitter = NULL;
    FS_RTP_SESSION_UNLOCK (session);
    g_object_unref (st);
    g_object_unref (session);
    return FALSE;
  }

  g_object_unref (session);
  return TRUE;
}

static gint
parse_enum (const gchar *name, const gchar *value, GError **error)
{
  GstElementFactory *factory;
  GstPluginFeature *loaded_feature;
  GType srtpenc_type;
  GObjectClass *srtpenc_class;
  GParamSpec *spec;
  GParamSpecEnum *enumspec;
  GEnumValue *enumvalue;

  if (value == NULL)
    goto error;

  factory = gst_element_factory_find ("srtpenc");
  if (!factory)
    goto error_not_installed;

  loaded_feature = gst_plugin_feature_load (GST_PLUGIN_FEATURE (factory));
  gst_object_unref (factory);
  factory = GST_ELEMENT_FACTORY (loaded_feature);

  srtpenc_type = gst_element_factory_get_element_type (factory);
  gst_object_unref (factory);
  if (srtpenc_type == 0)
    goto error_not_installed;

  srtpenc_class = g_type_class_ref (srtpenc_type);
  if (!srtpenc_class)
    goto error_not_installed;

  spec = g_object_class_find_property (srtpenc_class, name);
  g_type_class_unref (srtpenc_class);
  if (!spec)
    goto error_internal;

  if (!G_IS_PARAM_SPEC_ENUM (spec))
    goto error_internal;
  enumspec = G_PARAM_SPEC_ENUM (spec);

  enumvalue = g_enum_get_value_by_nick (enumspec->enum_class, value);
  if (enumvalue)
    return enumvalue->value;

  enumvalue = g_enum_get_value_by_name (enumspec->enum_class, value);
  if (enumvalue)
    return enumvalue->value;

error:
  g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
      "Invalid %s value: %s", name, value);
  return -1;

error_not_installed:
  g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
      "Can't find srtpenc, no encryption possible");
  return -1;

error_internal:
  g_set_error (error, FS_ERROR, FS_ERROR_INTERNAL,
      "Can't find srtpenc %s property or is not a GEnum type!", name);
  return -1;
}


gboolean
validate_srtp_parameters (GstStructure *parameters,
    gint *srtp_cipher, gint *srtcp_cipher, gint *srtp_auth, gint *srtcp_auth,
    GstBuffer **key, guint *replay_window, GError **error)
{
  gint cipher = 0; /* 0 is null cipher, no encryption */
  gint auth = -1;

  *key = NULL;
  *srtp_cipher = -1;
  *srtcp_cipher = -1;
  *srtp_auth = -1;
  *srtcp_auth = -1;
  *replay_window = 128;

  if (parameters)
  {
    const GValue *v = NULL;
    const gchar *tmp;

    if (!gst_structure_has_name (parameters, "FarstreamSRTP"))
    {
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "The only structure accepted is FarstreamSRTP");
      return FALSE;
    }
    if ((tmp = gst_structure_get_string (parameters, "cipher")))
    {
      cipher = parse_enum ("rtp-cipher", tmp, error);
      if (cipher == -1)
        return FALSE;
    }
    if ((tmp = gst_structure_get_string (parameters, "rtp-cipher")))
    {
      *srtp_cipher = parse_enum ("rtp-cipher", tmp, error);
      if (*srtp_cipher == -1)
        return FALSE;
    }
    if ((tmp = gst_structure_get_string (parameters, "rtcp-cipher")))
    {
      *srtcp_cipher = parse_enum ("rtcp-cipher", tmp, error);
      if (*srtcp_cipher == -1)
        return FALSE;
    }
    if ((tmp = gst_structure_get_string (parameters, "auth")))
    {
      auth = parse_enum ("rtp-auth", tmp, error);
      if (auth == -1)
        return FALSE;
    }
    if ((tmp = gst_structure_get_string (parameters, "rtp-auth")))
    {
      *srtp_auth = parse_enum ("rtp-auth", tmp, error);
      if (*srtp_auth == -1)
        return FALSE;
    }
    if ((tmp = gst_structure_get_string (parameters, "rtcp-auth")))
    {
      *srtcp_auth = parse_enum ("rtcp-auth", tmp, error);
      if (*srtcp_auth == -1)
        return FALSE;
    }

    if (*srtp_cipher == -1)
      *srtp_cipher = cipher;
    if (*srtcp_cipher == -1)
      *srtcp_cipher = cipher;

    if (*srtp_auth == -1)
      *srtp_auth = auth;
    if (*srtcp_auth == -1)
      *srtcp_auth = auth;
    if (*srtp_auth == -1 || *srtcp_auth == -1)
    {
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "At least the authentication MUST be set, \"auth\" or \"rtp-auth\""
          " and \"rtcp-auth\" are required.");
      return FALSE;
    }

    v = gst_structure_get_value (parameters, "key");
    if (!v)
    {
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "The argument \"key\" is required.");
      return FALSE;
    }
    if (!GST_VALUE_HOLDS_BUFFER (v) || gst_value_get_buffer (v) == NULL)
    {
       g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
           "The argument \"key\" MUST hold a GstBuffer.");
       return FALSE;
    }
    *key = gst_value_get_buffer (v);

    if (gst_structure_get_uint (parameters, "replay-window-size",
            replay_window))
    {
      if (*replay_window < 64 || *replay_window >= 32768)
      {
        g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
            "Reply window size must be between 64 and 32768");
        return FALSE;
      }
    }
  } else {
    *srtp_cipher = *srtcp_cipher = *srtcp_auth = *srtp_auth = 0; /* 0 is NULL */
  }

  return TRUE;
}


static gboolean
fs_rtp_stream_set_decryption_parameters (FsStream *stream,
    GstStructure *parameters, GError **error)
{
  FsRtpStream *self = FS_RTP_STREAM (stream);
  GstBuffer *key;
  gint rtp_cipher;
  gint rtcp_cipher;
  gint rtp_auth;
  gint rtcp_auth;
  guint replay_window_size;
  FsRtpSession *session;
  gboolean ret = FALSE;

  g_return_val_if_fail (FS_IS_RTP_STREAM (stream), FALSE);
  g_return_val_if_fail (parameters == NULL ||
      GST_IS_STRUCTURE (parameters), FALSE);

  if (!validate_srtp_parameters (parameters, &rtp_cipher, &rtcp_cipher,
          &rtp_auth, &rtcp_auth, &key, &replay_window_size, error))
    return FALSE;

  session = fs_rtp_stream_get_session (self, error);
  if (!session)
    return FALSE;


  FS_RTP_SESSION_LOCK (session);
  if (self->priv->decryption_parameters != parameters &&
      (!parameters || !self->priv->decryption_parameters ||
          !gst_structure_is_equal (self->priv->decryption_parameters,
              parameters)))
  {
    if (!self->priv->decrypt_clear_locked_cb (self,
            self->priv->user_data_for_cb))
    {
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "Can't set encryption because srtpdec is not installed");
      goto done;
    }

    if (self->priv->decryption_parameters)
      gst_structure_free (self->priv->decryption_parameters);

    if (parameters)
      self->priv->decryption_parameters = gst_structure_copy (parameters);
    else
      self->priv->decryption_parameters = NULL;

 }

  ret = TRUE;

done:
  FS_RTP_SESSION_UNLOCK (session);
  g_object_unref (session);

  return ret;
}

GstCaps *
fs_rtp_stream_get_srtp_caps_locked (FsRtpStream *self)
{
  const gchar *srtp_cipher;
  const gchar *srtcp_cipher;
  const gchar *srtp_auth;
  const gchar *srtcp_auth;
  const GValue *v;
  GstBuffer *key;

  /* This is always TRUE for now, but when we expand to DTLS-SRTP, it may
   * not be.
   */
  if (!self->priv->decryption_parameters ||
      !gst_structure_has_name (self->priv->decryption_parameters,
          "FarstreamSRTP"))
  {
    /* Return NULL (drop packets) if encrypted, otherwise return
     * the NULL codec.
     */
    if (self->priv->encrypted)
      return NULL;
    else
      return gst_caps_new_simple ("application/x-srtp",
          "srtp-cipher", G_TYPE_STRING, "null",
          "srtcp-cipher", G_TYPE_STRING, "null",
          "srtp-auth", G_TYPE_STRING, "null",
          "srtcp-auth", G_TYPE_STRING, "null",
          NULL);
  }

  srtp_cipher = gst_structure_get_string (self->priv->decryption_parameters,
      "rtp-cipher");
  if (!srtp_cipher)
    srtp_cipher = gst_structure_get_string (self->priv->decryption_parameters,
        "cipher");
  if (!srtp_cipher)
    srtp_cipher = "null";

  srtcp_cipher = gst_structure_get_string (self->priv->decryption_parameters,
      "rtcp-cipher");
  if (!srtcp_cipher)
    srtcp_cipher = gst_structure_get_string (self->priv->decryption_parameters,
        "cipher");
  if (!srtcp_cipher)
    srtcp_cipher = "null";

  srtp_auth = gst_structure_get_string (self->priv->decryption_parameters,
      "rtp-auth");
  if (!srtp_auth)
    srtp_auth = gst_structure_get_string (self->priv->decryption_parameters,
        "auth");
  if (!srtp_auth)
    srtp_auth = "null";

  srtcp_auth = gst_structure_get_string (self->priv->decryption_parameters,
      "rtcp-auth");
  if (!srtcp_auth)
    srtcp_auth = gst_structure_get_string (self->priv->decryption_parameters,
        "auth");
  if (!srtcp_auth)
    srtcp_auth = "null";

  v = gst_structure_get_value (self->priv->decryption_parameters, "key");
  key = gst_value_get_buffer (v);

  return gst_caps_new_simple ("application/x-srtp",
      "srtp-key", GST_TYPE_BUFFER, key,
      "srtp-cipher", G_TYPE_STRING, srtp_cipher,
      "srtcp-cipher", G_TYPE_STRING, srtcp_cipher,
      "srtp-auth", G_TYPE_STRING, srtp_auth,
      "srtcp-auth", G_TYPE_STRING, srtcp_auth,
      NULL);
}

gboolean
fs_rtp_stream_requires_crypto_locked (FsRtpStream *self)
{
  return self->priv->encrypted;
}
