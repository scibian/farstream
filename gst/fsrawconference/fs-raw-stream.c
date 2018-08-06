/*
 * Farstream - Farstream Raw Stream
 *
 * Copyright 2008 Richard Spiers <richard.spiers@gmail.com>
 * Copyright 2007 Nokia Corp.
 * Copyright 2007-2010 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 *  @author: Youness Alaoui <youness.alaoui@collabora.co.uk>
 *  @author: Mike Ruprecht <mike.ruprecht@collabora.co.uk>
 *
 * fs-raw-stream.c - A Farstream Raw Stream gobject
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
 * SECTION:fs-raw-stream
 * @short_description: A raw stream in a #FsRawSession in a #FsRawConference
 *
 * This list of remote codecs set on this stream should contain one or two
 * codecs. The first codec in this list represents the codec the remote side
 * will be sending. The second codec, if given, represents what should be
 * sent to the remote side. If only one codec is passed, and the codec to
 * send to the remote side hasn't yet been chosen, it will use the first
 * and only codec in the list.
 *
 * The codec content of the codec are ignored except for the "encoding_name"
 * parameter which has to be a valid caps string that can be parsed with
 * gst_caps_to_string() to produce fixed caps.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fs-raw-stream.h"
#include "fs-raw-session.h"

#include <string.h>
#include <unistd.h>

#include <gst/gst.h>


#define GST_CAT_DEFAULT fsrawconference_debug

/* Signals */
enum
{
  LAST_SIGNAL
};

/* props */
enum
{
  PROP_0,
  PROP_DIRECTION,
  PROP_PARTICIPANT,
  PROP_SESSION,
  PROP_CONFERENCE,
  PROP_STREAM_TRANSMITTER,
  PROP_REMOTE_CODECS,
};



struct _FsRawStreamPrivate
{
  FsRawConference *conference;
  FsRawSession *session;
  FsRawParticipant *participant;
  FsStreamDirection direction;
  FsStreamTransmitter *stream_transmitter;

  GList *remote_codecs;

  gulong local_candidates_prepared_handler_id;
  gulong new_active_candidate_pair_handler_id;
  gulong new_local_candidate_handler_id;
  gulong error_handler_id;
  gulong state_changed_handler_id;

  stream_get_new_stream_transmitter_cb get_new_stream_transmitter_cb;
  gpointer user_data;

  GMutex mutex; /* protects the conference */

  gboolean disposed;

#ifdef DEBUG_MUTEXES
  guint count;
#endif
};


G_DEFINE_TYPE(FsRawStream, fs_raw_stream, FS_TYPE_STREAM);

#define FS_RAW_STREAM_GET_PRIVATE(o)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((o), FS_TYPE_RAW_STREAM, FsRawStreamPrivate))

#ifdef DEBUG_MUTEXES

#define FS_RAW_STREAM_LOCK(stream) \
  do { \
    g_mutex_lock (&FS_RAW_STREAM (stream)->priv->mutex);   \
    g_assert (FS_RAW_STREAM (stream)->priv->count == 0);  \
    FS_RAW_STREAM (stream)->priv->count++;                \
  } while (0);
#define FS_RAW_STREAM_UNLOCK(stream) \
  do { \
    g_assert (FS_RAW_STREAM (stream)->priv->count == 1);  \
    FS_RAW_STREAM (stream)->priv->count--;                \
    g_mutex_unlock (&FS_RAW_STREAM (stream)->priv->mutex); \
  } while (0);
#define FS_RAW_STREAM_GET_LOCK(stream) \
  (FS_RAW_STREAM (stream)->priv->mutex)
#else
#define FS_RAW_STREAM_LOCK(stream) \
  g_mutex_lock (&(stream)->priv->mutex)
#define FS_RAW_STREAM_UNLOCK(stream) \
  g_mutex_unlock (&(stream)->priv->mutex)
#define FS_RAW_STREAM_GET_LOCK(stream) \
  (&(stream)->priv->mutex)
#endif

static void fs_raw_stream_dispose (GObject *object);
static void fs_raw_stream_finalize (GObject *object);

static void fs_raw_stream_get_property (GObject *object,
                                        guint prop_id,
                                        GValue *value,
                                        GParamSpec *pspec);
static void fs_raw_stream_set_property (GObject *object,
                                        guint prop_id,
                                        const GValue *value,
                                        GParamSpec *pspec);

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
static void _transmitter_error (
    FsStreamTransmitter *stream_transmitter,
    gint errorno,
    gchar *error_msg,
    gpointer user_data);
static void _state_changed (FsStreamTransmitter *stream_transmitter,
    guint component,
    FsStreamState state,
    gpointer user_data);

static gboolean fs_raw_stream_add_remote_candidates (FsStream *stream,
    GList *candidates,
    GError **error);
static gboolean fs_raw_stream_force_remote_candidates (FsStream *stream,
    GList *remote_candidates,
    GError **error);
static gboolean fs_raw_stream_set_remote_codecs (FsStream *stream,
    GList *remote_codecs,
    GError **error);
static gboolean fs_raw_stream_set_transmitter (FsStream *stream,
    const gchar *transmitter,
    GParameter *stream_transmitter_parameters,
    guint stream_transmitter_n_parameters,
    GError **error);

static void
fs_raw_stream_class_init (FsRawStreamClass *klass)
{
  GObjectClass *gobject_class;
  FsStreamClass *stream_class = FS_STREAM_CLASS (klass);

  gobject_class = (GObjectClass *) klass;

  gobject_class->set_property = fs_raw_stream_set_property;
  gobject_class->get_property = fs_raw_stream_get_property;
  gobject_class->dispose = fs_raw_stream_dispose;
  gobject_class->finalize = fs_raw_stream_finalize;

  stream_class->add_remote_candidates = fs_raw_stream_add_remote_candidates;
  stream_class->force_remote_candidates = fs_raw_stream_force_remote_candidates;
  stream_class->set_remote_codecs = fs_raw_stream_set_remote_codecs;
  stream_class->set_transmitter = fs_raw_stream_set_transmitter;

  g_type_class_add_private (klass, sizeof (FsRawStreamPrivate));

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
      PROP_REMOTE_CODECS,
      "remote-codecs");

  g_object_class_install_property (gobject_class,
      PROP_CONFERENCE,
      g_param_spec_object ("conference",
          "The Conference this stream refers to",
          "This is a conveniance pointer for the Conference",
          FS_TYPE_RAW_CONFERENCE,
          G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * FsRawStream:stream-transmitter:
   *
   * The #FsStreamTransmitter for this stream.
   *
   */
  g_object_class_install_property (gobject_class,
      PROP_STREAM_TRANSMITTER,
      g_param_spec_object ("stream-transmitter",
        "The transmitter use by the stream",
        "An FsStreamTransmitter used by this stream",
        FS_TYPE_STREAM_TRANSMITTER,
        G_PARAM_CONSTRUCT_ONLY | G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));
}

static void
fs_raw_stream_init (FsRawStream *self)
{
  /* member init */
  self->priv = FS_RAW_STREAM_GET_PRIVATE (self);

  self->priv->session = NULL;
  self->priv->participant = NULL;

  self->priv->direction = FS_DIRECTION_NONE;

  g_mutex_init (&self->priv->mutex);
}


static FsRawConference *
fs_raw_stream_get_conference (FsRawStream *self, GError **error)
{
  FsRawConference *conference;

  FS_RAW_STREAM_LOCK (self);
  conference = self->priv->conference;
  if (conference)
    g_object_ref (conference);
  FS_RAW_STREAM_UNLOCK (self);

  if (!conference)
    g_set_error (error, FS_ERROR, FS_ERROR_DISPOSED,
        "Called function after stream has been disposed");

  return conference;
}

static void
fs_raw_stream_dispose (GObject *obj)
{
  FsRawStream *self = FS_RAW_STREAM (obj);
  FsRawConference *conference;
  FsStreamTransmitter *st;

  FS_RAW_STREAM_LOCK (self);
  conference = self->priv->conference;
  self->priv->conference = NULL;
  FS_RAW_STREAM_UNLOCK (self);

  if (!conference)
    return;


  if (fs_raw_conference_is_internal_thread (conference))
  {
    g_critical ("You MUST call fs_stream_destroy() from your main thread, "
        "this FsStream may now be leaked");
    return;
  }

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
        self->priv->state_changed_handler_id);

    fs_stream_transmitter_stop (st);
    g_object_unref (st);
  }

  if (self->priv->participant)
  {
    g_object_unref (self->priv->participant);
    self->priv->participant = NULL;
  }

  if (self->priv->session)
  {
    fs_raw_session_remove_stream (self->priv->session, (FsStream *)self);

    g_object_unref (self->priv->session);
    self->priv->session = NULL;
  }

  gst_object_unref (conference);

  G_OBJECT_CLASS (fs_raw_stream_parent_class)->dispose (obj);
}

static void
fs_raw_stream_finalize (GObject *object)
{
  FsRawStream *self = FS_RAW_STREAM (object);

  fs_codec_list_destroy (self->priv->remote_codecs);

  g_mutex_clear (&self->priv->mutex);

  G_OBJECT_CLASS (fs_raw_stream_parent_class)->finalize (object);
}


static void
fs_raw_stream_get_property (GObject *object,
                            guint prop_id,
                            GValue *value,
                            GParamSpec *pspec)
{
  FsRawStream *self = FS_RAW_STREAM (object);
  FsRawConference *conference = fs_raw_stream_get_conference (self, NULL);

  if (!conference &&
      !(pspec->flags & (G_PARAM_CONSTRUCT_ONLY | G_PARAM_CONSTRUCT)))
    return;

  if (conference)
    GST_OBJECT_LOCK (conference);

  switch (prop_id)
  {
    case PROP_SESSION:
      g_value_set_object (value, self->priv->session);
      break;
    case PROP_PARTICIPANT:
      g_value_set_object (value, self->priv->participant);
      break;
    case PROP_DIRECTION:
      g_value_set_flags (value, self->priv->direction);
      break;
    case PROP_CONFERENCE:
      g_value_set_object (value, self->priv->conference);
      break;
    case PROP_REMOTE_CODECS:
      g_value_set_boxed (value, self->priv->remote_codecs);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  if (conference)
  {
    GST_OBJECT_UNLOCK (conference);
    gst_object_unref (conference);
  }
}

static void
fs_raw_stream_set_property (GObject *object,
                            guint prop_id,
                            const GValue *value,
                            GParamSpec *pspec)
{
  FsRawStream *self = FS_RAW_STREAM (object);
  FsRawConference *conference = fs_raw_stream_get_conference (self, NULL);

  if (!conference &&
      !(pspec->flags & (G_PARAM_CONSTRUCT_ONLY | G_PARAM_CONSTRUCT)))
    return;

  if (conference)
    GST_OBJECT_LOCK (conference);

  switch (prop_id)
  {
    case PROP_SESSION:
      self->priv->session = FS_RAW_SESSION (g_value_dup_object (value));
      break;
    case PROP_PARTICIPANT:
      self->priv->participant = FS_RAW_PARTICIPANT (g_value_dup_object (value));
      break;
    case PROP_DIRECTION:
      if (g_value_get_flags (value) != self->priv->direction)
      {
        FsStreamDirection direction = g_value_get_flags (value);
        FsStreamTransmitter *st;

        self->priv->direction = direction;
        st = self->priv->stream_transmitter;
        if (st)
          g_object_ref (st);

        if (conference)
          GST_OBJECT_UNLOCK (conference);
        if (st)
        {
          g_object_set (st, "sending",
              (direction & FS_DIRECTION_SEND) ? TRUE : FALSE, NULL);
          g_object_unref (st);
        }
        if (self->priv->session)
          fs_raw_session_update_direction (self->priv->session, direction);
        if (conference)
          GST_OBJECT_LOCK (conference);
      }
      break;
    case PROP_CONFERENCE:
      self->priv->conference = FS_RAW_CONFERENCE (g_value_dup_object (value));
      break;
    case PROP_STREAM_TRANSMITTER:
      self->priv->stream_transmitter = g_value_get_object (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

  if (conference)
  {
    GST_OBJECT_UNLOCK (conference);
    gst_object_unref (conference);
  }
}


static void
_local_candidates_prepared (FsStreamTransmitter *stream_transmitter,
    gpointer user_data)
{
  FsRawStream *self = FS_RAW_STREAM (user_data);
  GstElement *conf = GST_ELEMENT (fs_raw_stream_get_conference (self, NULL));

  if (!conf)
    return;

  gst_element_post_message (conf,
      gst_message_new_element (GST_OBJECT (conf),
          gst_structure_new ("farstream-local-candidates-prepared",
              "stream", FS_TYPE_STREAM, self,
              NULL)));

  gst_object_unref (conf);
}


static void
_new_active_candidate_pair (
    FsStreamTransmitter *stream_transmitter,
    FsCandidate *local_candidate,
    FsCandidate *remote_candidate,
    gpointer user_data)
{
  FsRawStream *self = FS_RAW_STREAM (user_data);
  GstElement *conf = GST_ELEMENT (fs_raw_stream_get_conference (self, NULL));

  if (!conf)
    return;

  gst_element_post_message (conf,
      gst_message_new_element (GST_OBJECT (conf),
          gst_structure_new ("farstream-new-active-candidate-pair",
              "stream", FS_TYPE_STREAM, self,
              "local-candidate", FS_TYPE_CANDIDATE, local_candidate,
              "remote-candidate", FS_TYPE_CANDIDATE, remote_candidate,
              NULL)));

  gst_object_unref (conf);
}


static void
_new_local_candidate (
    FsStreamTransmitter *stream_transmitter,
    FsCandidate *candidate,
    gpointer user_data)
{
  FsRawStream *self = FS_RAW_STREAM (user_data);
  GstElement *conf = GST_ELEMENT (fs_raw_stream_get_conference (self, NULL));

  if (!conf)
    return;

  gst_element_post_message (conf,
      gst_message_new_element (GST_OBJECT (conf),
          gst_structure_new ("farstream-new-local-candidate",
              "stream", FS_TYPE_STREAM, self,
              "candidate", FS_TYPE_CANDIDATE, candidate,
              NULL)));

  gst_object_unref (conf);
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
_state_changed (FsStreamTransmitter *stream_transmitter,
    guint component,
    FsStreamState state,
    gpointer user_data)
{
  FsRawStream *self = FS_RAW_STREAM (user_data);
  GstElement *conf = GST_ELEMENT (fs_raw_stream_get_conference (self, NULL));

  if (!conf)
    return;

  gst_element_post_message (conf,
      gst_message_new_element (GST_OBJECT (conf),
          gst_structure_new ("farstream-component-state-changed",
              "stream", FS_TYPE_STREAM, self,
              "component", G_TYPE_UINT, component,
              "state", FS_TYPE_STREAM_STATE, state,
              NULL)));

  gst_object_unref (conf);

  if (component == 1 && state == FS_STREAM_STATE_FAILED)
    fs_stream_emit_error (FS_STREAM (self), FS_ERROR_CONNECTION_FAILED,
        "Could not establish connection");
}

/**
 * fs_raw_stream_add_remote_candidate:
 */
static gboolean
fs_raw_stream_add_remote_candidates (FsStream *stream, GList *candidates,
                                     GError **error)
{
  FsRawStream *self = FS_RAW_STREAM (stream);
  FsRawConference *conference = fs_raw_stream_get_conference (self, error);
  FsStreamTransmitter *st = NULL;
  gboolean ret = FALSE;

  if (!conference)
    return FALSE;

  GST_OBJECT_LOCK (conference);
  if (self->priv->stream_transmitter)
    st = g_object_ref (self->priv->stream_transmitter);
  GST_OBJECT_UNLOCK (conference);

  if (st)
  {
    ret = fs_stream_transmitter_add_remote_candidates (st, candidates, error);
    g_object_unref (st);
  }

  gst_object_unref (conference);

  return ret;
}


/**
 * fs_raw_stream_force_remote_candidate:
 */
static gboolean
fs_raw_stream_force_remote_candidates (FsStream *stream,
    GList *candidates,
    GError **error)
{
  FsRawStream *self = FS_RAW_STREAM (stream);
  FsRawConference *conference = fs_raw_stream_get_conference (self, error);
  FsStreamTransmitter *st = NULL;
  gboolean ret = FALSE;

  if (!conference)
    return FALSE;

  GST_OBJECT_LOCK (conference);
  if (self->priv->stream_transmitter)
    st = g_object_ref (self->priv->stream_transmitter);
  GST_OBJECT_UNLOCK (conference);

  if (st)
  {
    ret = fs_stream_transmitter_force_remote_candidates (st, candidates, error);
    g_object_unref (st);
  }

  gst_object_unref (conference);

  return ret;
}


/**
 * fs_raw_stream_set_remote_codecs:
 * @stream: an #FsStream
 * @remote_codecs: a #GList of #FsCodec representing the remote codecs
 * @error: location of a #GError, or NULL if no error occured
 *
 * This function will set the list of remote codecs for this stream. This list
 * should contain one or two codecs. The first codec in this list represents
 * the codec the remote side will be sending. The second codec, if given,
 * represents what should be sent to the remote side. If only one codec is
 * passed, and the codec to send to the remote side hasn't yet been chosen,
 * it will use the first and only codec in the list. If the list isn't in this
 * format, @error will be set and %FALSE will be returned. The @remote_codecs
 * list will be copied so it must be free'd using fs_codec_list_destroy()
 * when done.
 *
 * Returns: %FALSE if the remote codecs couldn't be set.
 */
static gboolean
fs_raw_stream_set_remote_codecs (FsStream *stream,
    GList *remote_codecs,
    GError **error)
{
  FsRawStream *self = FS_RAW_STREAM (stream);
  GList *item = NULL;
  FsRawSession *session;
  FsRawConference *conf = fs_raw_stream_get_conference (self, error);
  gboolean is_new = TRUE;

  if (!conf)
    return FALSE;

  GST_OBJECT_LOCK (conf);
  session = self->priv->session;
  if (session)
    g_object_ref (session);
  GST_OBJECT_UNLOCK (conf);

  if (!session)
    {
      g_set_error (error, FS_ERROR, FS_ERROR_DISPOSED,
          "Called function after stream has been disposed");
      return FALSE;
    }

  if (remote_codecs == NULL) {
    g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
        "You can not set NULL remote codecs");
    goto error;
  }

  if (g_list_length (remote_codecs) > 2) {
    g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
        "Too many codecs passed");
    goto error;
  }

  for (item = g_list_first (remote_codecs); item; item = g_list_next (item))
  {
    FsCodec *codec = item->data;
    GstCaps *caps;

    if (!codec->encoding_name)
    {
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "The codec must have an encoding name");
      goto error;
    }

    caps = fs_raw_codec_to_gst_caps (codec);
    if (!caps)
    {
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "The encoding name for codec %s is not valid GstCaps",
          codec->encoding_name);
      goto error;
    }
    gst_caps_unref (caps);
  }

  GST_OBJECT_LOCK (conf);
  if (self->priv->remote_codecs)
  {
    is_new = !fs_codec_list_are_equal (self->priv->remote_codecs,
        remote_codecs);
    fs_codec_list_destroy (self->priv->remote_codecs);
  }
  self->priv->remote_codecs = fs_codec_list_copy (remote_codecs);
  GST_OBJECT_UNLOCK (conf);

  if (is_new)
    g_object_notify (G_OBJECT (stream), "remote-codecs");

  g_object_unref (session);
  g_object_unref (conf);
  return TRUE;

 error:

  g_object_unref (session);
  g_object_unref (conf);
  return FALSE;
}


/**
 * fs_raw_stream_new:
 * @session: The #FsRawSession this stream is a child of
 * @participant: The #FsRawParticipant this stream is for
 * @direction: the initial #FsDirection for this stream
 *
 *
 * This function create a new stream
 *
 * Returns: the newly created string or NULL on error
 */

FsRawStream *
fs_raw_stream_new (FsRawSession *session,
    FsRawParticipant *participant,
    FsStreamDirection direction,
    FsRawConference *conference,
    stream_get_new_stream_transmitter_cb get_new_stream_transmitter_cb,
    gpointer user_data)
{
  FsRawStream *self;

  g_return_val_if_fail (session, NULL);
  g_return_val_if_fail (participant, NULL);

  self = g_object_new (FS_TYPE_RAW_STREAM,
      "session", session,
      "participant", participant,
      "direction", direction,
      "conference", conference,
      NULL);

  self->priv->get_new_stream_transmitter_cb = get_new_stream_transmitter_cb;
  self->priv->user_data = user_data;

  return self;
}


static gboolean
fs_raw_stream_set_transmitter (FsStream *stream,
    const gchar *transmitter,
    GParameter *stream_transmitter_parameters,
    guint stream_transmitter_n_parameters,
    GError **error)
{
  FsRawStream *self = FS_RAW_STREAM (stream);
  FsRawConference *conf = fs_raw_stream_get_conference (self, error);
  FsStreamTransmitter *st;
  FsRawSession *session = NULL;

  if (!conf)
    return FALSE;

  GST_OBJECT_LOCK (conf);
  if (self->priv->stream_transmitter)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_ALREADY_EXISTS,
        "Stream transmitter already set");
    GST_OBJECT_UNLOCK (conf);
    goto error;
  }

  session = g_object_ref (self->priv->session);
  GST_OBJECT_UNLOCK (conf);

  st = self->priv->get_new_stream_transmitter_cb (self, transmitter,
      FS_PARTICIPANT (self->priv->participant), stream_transmitter_parameters,
      stream_transmitter_n_parameters,error, self->priv->user_data);

  if (!st)
    goto error;

  GST_OBJECT_LOCK (conf);
  self->priv->stream_transmitter = st;
  GST_OBJECT_UNLOCK (conf);


  g_object_set (self->priv->stream_transmitter, "sending",
      (self->priv->direction & FS_DIRECTION_SEND) ? TRUE : FALSE, NULL);

  self->priv->local_candidates_prepared_handler_id =
      g_signal_connect_object (self->priv->stream_transmitter,
          "local-candidates-prepared",
          G_CALLBACK (_local_candidates_prepared),
          self, 0);
  self->priv->new_active_candidate_pair_handler_id =
      g_signal_connect_object (self->priv->stream_transmitter,
          "new-active-candidate-pair",
          G_CALLBACK (_new_active_candidate_pair),
          self, 0);
  self->priv->new_local_candidate_handler_id =
      g_signal_connect_object (self->priv->stream_transmitter,
          "new-local-candidate",
          G_CALLBACK (_new_local_candidate),
          self, 0);
  self->priv->error_handler_id =
      g_signal_connect_object (self->priv->stream_transmitter,
          "error",
          G_CALLBACK (_transmitter_error),
          self, 0);
  self->priv->state_changed_handler_id =
      g_signal_connect_object (self->priv->stream_transmitter,
          "state-changed",
          G_CALLBACK (_state_changed),
          self, 0);

  if (!fs_stream_transmitter_gather_local_candidates (
        self->priv->stream_transmitter, error))
  {
    GST_OBJECT_LOCK (conf);
    self->priv->stream_transmitter = NULL;
    GST_OBJECT_UNLOCK (conf);

    g_signal_handler_disconnect (st,
        self->priv->local_candidates_prepared_handler_id);
    g_signal_handler_disconnect (st,
        self->priv->new_active_candidate_pair_handler_id);
    g_signal_handler_disconnect (st,
        self->priv->new_local_candidate_handler_id);
    g_signal_handler_disconnect (st,
        self->priv->error_handler_id);
    g_signal_handler_disconnect (st,
        self->priv->state_changed_handler_id);

    fs_stream_transmitter_stop (st);
    /* We want to remove the transmitter, but not the stream */
    fs_raw_session_remove_stream (session, NULL);
    goto error;
  }

  g_object_unref (conf);
  g_object_unref (session);

  g_object_notify (G_OBJECT (self), "remote-codecs");
  g_object_notify (G_OBJECT (self), "direction");
  return TRUE;

error:
  if (session)
    g_object_unref (session);
  g_object_unref (conf);

  return FALSE;
}
