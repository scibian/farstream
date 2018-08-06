/*
 * Farstream - Farstream Session
 *
 * Copyright 2007 Collabora Ltd.
 *  @author: Philippe Kalaf <philippe.kalaf@collabora.co.uk>
 * Copyright 2007 Nokia Corp.
 *
 * fs-session.c - A Farstream Session gobject (base implementation)
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
 * SECTION:fs-session
 * @short_description: A session in a conference
 *
 * This object is the base implementation of a Farstream Session. It needs to be
 * derived and implemented by a farstream conference gstreamer element. A
 * Farstream session is defined in the same way as an RTP session. It can contain
 * one or more participants but represents only one media stream (i.e. One
 * session for video and one session for audio in an AV conference). Sessions
 * contained in the same conference will be synchronised together during
 * playback.
 *
 *
 * This will communicate asynchronous events to the user through #GstMessage
 * of type #GST_MESSAGE_ELEMENT sent over the #GstBus.
 *
 * <refsect2><title>The "<literal>farstream-send-codec-changed</literal>"
 *   message</title>
 * <table>
 *  <tr>
 *   <td><code>"session"</code></td>
 *   <td>#FsSession</td>
 *   <td>The session that emits the message</td>
 *  </tr>
 *  <tr>
 *   <td><code>"codec"</code></td>
 *   <td>#FsCodec</td>
 *   <td>The new send codec</td>
 *  </tr>
 *  <tr>
 *   <td><code>"secondary-codecs"</code></td>
 *   <td>#GList</td>
 *   <td>A #GList of #FsCodec (to be freed with fs_codec_list_destroy())
 *   </td>
 *  </tr>
 * </table>
 * <para>
 * This message is sent on the bus when the value of the
 * #FsSession:current-send-codec property changes.
 * </para>
 * </refsect2>
 * <refsect2><title>The "<literal>farstream-codecs-changed</literal>"
 *  message</title>
 * <table>
 *  <tr>
 *   <td><code>"session"</code></td>
 *   <td>#FsSession</td>
 *   <td>The session that emits the message</td>
 *  </tr>
 * </table>
 * <para>
 * This message is sent on the bus when the value of the
 * #FsSession:codecs or #FsSession:codecs-without-config properties change.
 * If one is using codecs that have configuration data that needs to be
 * transmitted reliably, one should fetch #FsSession:codecs, otherwise,
 * #FsSession:codecs-without-config should be enough.
 * </para>
 * </refsect2>
 * <refsect2><title>The "<literal>farstream-telephony-event-started</literal>"
 *  message</title>
 * <table>
 *  <tr>
 *   <td><code>"session"</code></td>
 *   <td>#FsSession</td>
 *   <td>The session that emits the message</td>
 *  </tr>
 *  <tr>
 *   <td><code>"method"</code></td>
 *   <td>#FsDTMFMethod</td>
 *   <td>The method used to send the DTMF</td>
 *  </tr>
 *  <tr>
 *   <td><code>"event"</code></td>
 *   <td>#FSDTMFEvent</td>
 *   <td>The event number</td>
 *  </tr>
 *  <tr>
 *   <td><code>"volume"</code></td>
 *   <td>guchar</td>
 *   <td>The volume of the event</td>
 *  </tr>
 * </table>
 * <para>
 * This message is emitted after a succesful call to
 * fs_session_start_telephony_event() to inform the application that the
 * telephony event has started.
 * </para>
 * </refsect2>
 * <refsect2><title>The "<literal>farstream-telephony-event-stopped</literal>"
 *  message</title>
 * <table>
 *  <tr>
 *   <td><code>"session"</code></td>
 *   <td>#FsSession</td>
 *   <td>The session that emits the message</td>
 *  </tr>
 *  <tr>
 *   <td><code>"method"</code></td>
 *   <td>#FsDTMFMethod</td>
 *   <td>The method used to send the DTMF</td>
 *  </tr>
 * </table>
 * <para>
 * This message is emitted after a succesful call to
 * fs_session_stop_telephony_event() to inform the application that the
 * telephony event has stopped.
 * </para>
 * </refsect2>
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fs-session.h"

#include <gst/gst.h>

#include "fs-conference.h"
#include "fs-codec.h"
#include "fs-enumtypes.h"
#include "fs-private.h"

#define GST_CAT_DEFAULT _fs_conference_debug

/* Signals */
enum
{
  ERROR_SIGNAL,
  LAST_SIGNAL
};

/* props */
enum
{
  PROP_0,
  PROP_CONFERENCE,
  PROP_MEDIA_TYPE,
  PROP_ID,
  PROP_SINK_PAD,
  PROP_CODEC_PREFERENCES,
  PROP_CODECS,
  PROP_CODECS_WITHOUT_CONFIG,
  PROP_CURRENT_SEND_CODEC,
  PROP_TYPE_OF_SERVICE,
  PROP_ALLOWED_SRC_CAPS,
  PROP_ALLOWED_SINK_CAPS,
  PROP_ENCRYPTION_PARAMETERS
};

/*
struct _FsSessionPrivate
{
};

#define FS_SESSION_GET_PRIVATE(o)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((o), FS_TYPE_SESSION, FsSessionPrivate))
*/

G_DEFINE_ABSTRACT_TYPE(FsSession, fs_session, G_TYPE_OBJECT)

static void fs_session_get_property (GObject *object,
                                     guint prop_id,
                                     GValue *value,
                                     GParamSpec *pspec);
static void fs_session_set_property (GObject *object,
                                     guint prop_id,
                                     const GValue *value,
                                     GParamSpec *pspec);

static guint signals[LAST_SIGNAL] = { 0 };

static void
fs_session_class_init (FsSessionClass *klass)
{
  GObjectClass *gobject_class;

  gobject_class = (GObjectClass *) klass;

  gobject_class->set_property = fs_session_set_property;
  gobject_class->get_property = fs_session_get_property;


  /**
   * FsSession:conference:
   *
   * The #FsConference parent of this session. This property is a
   * construct param and is read-only.
   *
   */
  g_object_class_install_property (gobject_class,
    PROP_CONFERENCE,
    g_param_spec_object ("conference",
      "The FsConference",
      "The Conference this stream refers to",
      FS_TYPE_CONFERENCE,
      G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * FsSession:media-type:
   *
   * The media-type of the session. This is either Audio, Video or both.
   * This is a constructor parameter that cannot be changed.
   *
   */
  g_object_class_install_property (gobject_class,
      PROP_MEDIA_TYPE,
      g_param_spec_enum ("media-type",
        "The media type of the session",
        "An enum that specifies the media type of the session",
        FS_TYPE_MEDIA_TYPE,
        FS_MEDIA_TYPE_AUDIO,
        G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * FsSession:id:
   *
   * The ID of the session, the first number of the pads linked to this session
   * will be this id
   *
   */
  g_object_class_install_property (gobject_class,
      PROP_ID,
      g_param_spec_uint ("id",
        "The ID of the session",
        "This ID is used on pad related to this session",
        0, G_MAXUINT, 0,
        G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * FsSession:sink-pad:
   *
   * The Gstreamer sink pad that must be used to send media data on this
   * session. User must unref this GstPad when done with it.
   *
   */
  g_object_class_install_property (gobject_class,
      PROP_SINK_PAD,
      g_param_spec_object ("sink-pad",
        "A gstreamer sink pad for this session",
        "A pad used for sending data on this session",
        GST_TYPE_PAD,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * FsSession:codec-preferences: (type GLib.List(FsCodec)) (transfer full)
   *
   * This is the current preferences list for the local codecs. It is
   * set by the user to specify the codec options and priorities. The user may
   * change its value with fs_session_set_codec_preferences() at any time
   * during a session. It is a #GList of #FsCodec.
   * The user must free this codec list using fs_codec_list_destroy() when done.
   *
   * The payload type may be a valid dynamic PT (96-127), %FS_CODEC_ID_DISABLE
   * or %FS_CODEC_ID_ANY. If the encoding name is "reserve-pt", then the
   * payload type of the codec will be "reserved" and not be used by any
   * dynamically assigned payload type.
   */
  g_object_class_install_property (gobject_class,
      PROP_CODEC_PREFERENCES,
      g_param_spec_boxed ("codec-preferences",
        "List of user preferences for the codecs",
        "A GList of FsCodecs that allows user to set his codec options and"
        " priorities",
        FS_TYPE_CODEC_LIST,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * FsSession:codecs: (type GLib.List(FsCodec)) (transfer full)
   *
   * This is the list of codecs used for this session. It will include the
   * codecs and payload type used to receive media on this session. It will
   * also include any configuration parameter that must be transmitted reliably
   * for the other end to decode the content.
   *
   * It may change when the codec preferences are set, when codecs are set
   * on a #FsStream in this session, when a #FsStream is destroyed or
   * asynchronously when new config data is discovered.
   *
   * If any configuration parameter needs to be discovered, this property
   * will be %NULL until they have been discovered. One can always get
   * the codecs from #FsSession:codecs-without-config.
   * The "farstream-codecs-changed" message will be emitted whenever the value
   * of this property changes.
   *
   * It is a #GList of #FsCodec. User must free this codec list using
   * fs_codec_list_destroy() when done.
   */
  g_object_class_install_property (gobject_class,
      PROP_CODECS,
      g_param_spec_boxed ("codecs",
        "List of codecs",
        "A GList of FsCodecs indicating the codecs for this session",
        FS_TYPE_CODEC_LIST,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * FsSession:codecs-without-config: (type GLib.List(FsCodec)) (transfer full)
   *
   * This is the same list of codecs as #FsSession:codecs without
   * the configuration information that describes the data sent. It is suitable
   * for configurations where a list of codecs is shared by many senders.
   * If one is using codecs such as Theora, Vorbis or H.264 that require
   * such information to be transmitted, the configuration data should be
   * included in the stream and retransmitted regularly.
   *
   * It may change when the codec preferences are set, when codecs are set
   * on a #FsStream in this session, when a #FsStream is destroyed or
   * asynchronously when new config data is discovered.
   *
   * The "farstream-codecs-changed" message will be emitted whenever the value
   * of this property changes.
   *
   * It is a #GList of #FsCodec. User must free this codec list using
   * fs_codec_list_destroy() when done.
   */
  g_object_class_install_property (gobject_class,
      PROP_CODECS_WITHOUT_CONFIG,
      g_param_spec_boxed ("codecs-without-config",
          "List of codecs without the configuration data",
          "A GList of FsCodecs indicating the codecs for this session without "
          "any configuration data",
          FS_TYPE_CODEC_LIST,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * FsSession:current-send-codec:
   *
   * Indicates the currently active send codec. A user can change the active
   * send codec by calling fs_session_set_send_codec(). The send codec could
   * also be automatically changed by Farstream. This property is an
   * #FsCodec. User must free the codec using fs_codec_destroy() when done.
   * The "farstream-send-codec-changed" message is emitted on the bus when
   * the value of this property changes.
   */
  g_object_class_install_property (gobject_class,
      PROP_CURRENT_SEND_CODEC,
      g_param_spec_boxed ("current-send-codec",
        "Current active send codec",
        "An FsCodec indicating the currently active send codec",
        FS_TYPE_CODEC,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * FsSession:tos:
   *
   * Sets the IP ToS field (and if possible the IPv6 TCLASS field
   */
  g_object_class_install_property (gobject_class,
      PROP_TYPE_OF_SERVICE,
      g_param_spec_uint ("tos",
          "IP Type of Service",
          "The IP Type of Service to set on sent packets",
          0, 255, 0,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * FsSession:allowed-sink-caps:
   *
   * These are the #GstCaps that can be fed into the session,
   * they are used to filter the codecs to only those that can
   * accepted those caps as input.
   */
  g_object_class_install_property (gobject_class,
      PROP_ALLOWED_SINK_CAPS,
      g_param_spec_boxed ("allowed-sink-caps",
        "Allowed sink caps",
        "GstCaps that can be fed into the session",
        GST_TYPE_CAPS,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * FsSession:allowed-src-caps:
   *
   * These are the #GstCaps that the session can produce,
   * they are used to filter the codecs to only those that can
   * accepted those caps as output.
   */
  g_object_class_install_property (gobject_class,
      PROP_ALLOWED_SRC_CAPS,
      g_param_spec_boxed ("allowed-src-caps",
        "Allowed source caps",
        "GstCaps that the session can produce",
        GST_TYPE_CAPS,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * FsSession:encryption-parameters:
   *
   * Retrieves previously set encryption parameters
   */
  g_object_class_install_property (gobject_class,
      PROP_ENCRYPTION_PARAMETERS,
      g_param_spec_boxed ("encryption-parameters",
          "Encryption parameters",
          "Parameters used to encrypt the stream",
          GST_TYPE_STRUCTURE,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));


  /**
   * FsSession::error:
   * @self: #FsSession that emitted the signal
   * @object: The #Gobject that emitted the signal
   * @error_no: The number of the error
   * @error_msg: Error message
   *
   * This signal is emitted in any error condition, it can be emitted on any
   * thread. Applications should listen to the GstBus for errors.
   *
   */
  signals[ERROR_SIGNAL] = g_signal_new ("error",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0, NULL, NULL, NULL,
      G_TYPE_NONE, 3, G_TYPE_OBJECT, FS_TYPE_ERROR, G_TYPE_STRING);
}

static void
fs_session_init (FsSession *self)
{
  /* member init */
  // self->priv = FS_SESSION_GET_PRIVATE (self);
}

static void
fs_session_get_property (GObject *object,
                         guint prop_id,
                         GValue *value,
                         GParamSpec *pspec)
{
  switch (prop_id) {
    case PROP_ENCRYPTION_PARAMETERS:
      g_value_set_boxed (value, NULL);
      /* Not having parameters is valid, in this case set nothing */
      break;
    default:
      GST_WARNING ("Subclass %s of FsSession does not override the %s property"
          " getter",
          G_OBJECT_TYPE_NAME(object),
          g_param_spec_get_name (pspec));
      break;
  }
}

static void
fs_session_set_property (GObject *object,
                         guint prop_id,
                         const GValue *value,
                         GParamSpec *pspec)
{
  GST_WARNING ("Subclass %s of FsSession does not override the %s property"
      " setter",
      G_OBJECT_TYPE_NAME(object),
      g_param_spec_get_name (pspec));
}

static void
fs_session_error_forward (GObject *signal_src,
                          FsError error_no, gchar *error_msg,
                          FsSession *session)
{
  /* We just need to forward the error signal including a ref to the stream
   * object (signal_src) */
  g_signal_emit (session, signals[ERROR_SIGNAL], 0, signal_src, error_no,
      error_msg);
}

/**
 * fs_session_new_stream:
 * @session: a #FsSession
 * @participant: #FsParticipant of a participant for the new stream
 * @direction: #FsStreamDirection describing the direction of the new stream that will
 * be created for this participant
 * @error: location of a #GError, or %NULL if no error occured
 *
 * This function creates a stream for the given participant into the active session.
 *
 * Returns: (transfer full): the new #FsStream that has been created.
 * User must unref the #FsStream when the stream is ended. If an error occured,
 * returns NULL.
 */
FsStream *
fs_session_new_stream (FsSession *session,
    FsParticipant *participant,
    FsStreamDirection direction,
    GError **error)
{
  FsSessionClass *klass;
  FsStream *new_stream = NULL;

  g_return_val_if_fail (session, NULL);
  g_return_val_if_fail (FS_IS_SESSION (session), NULL);
  klass = FS_SESSION_GET_CLASS (session);
  g_return_val_if_fail (klass->new_stream, NULL);

  new_stream = klass->new_stream (session, participant, direction, error);

  if (!new_stream)
    return NULL;

  /* Let's catch all stream errors and forward them */
  g_signal_connect_object (new_stream, "error",
      G_CALLBACK (fs_session_error_forward), session, 0);

  return new_stream;
}

/**
 * fs_session_start_telephony_event:
 * @session: a #FsSession
 * @event: A #FsStreamDTMFEvent or another number defined at
 * http://www.iana.org/assignments/audio-telephone-event-registry
 * @volume: The volume in dBm0 without the negative sign. Should be between
 * 0 and 36. Higher values mean lower volume
 *
 * This function will start sending a telephony event (such as a DTMF
 * tone) on the #FsSession. You have to call the function
 * fs_session_stop_telephony_event() to stop it.
 *
 * If this function returns %TRUE, a "farstream-telephony-event-started" will
 * always be emitted when the event is actually played out.
 *
 * Returns: %TRUE if sucessful, it can return %FALSE if the #FsStream
 * does not support this telephony event.
 */
gboolean
fs_session_start_telephony_event (FsSession *session, guint8 event,
                                  guint8 volume)
{
  FsSessionClass *klass;

  g_return_val_if_fail (session, FALSE);
  g_return_val_if_fail (FS_IS_SESSION (session), FALSE);
  klass = FS_SESSION_GET_CLASS (session);

  if (klass->start_telephony_event) {
    return klass->start_telephony_event (session, event, volume);
  } else {
    GST_WARNING ("start_telephony_event not defined in class");
  }
  return FALSE;
}

/**
 * fs_session_stop_telephony_event:
 * @session: an #FsSession
 *
 * This function will stop sending a telephony event started by
 * fs_session_start_telephony_event(). If the event was being sent
 * for less than 50ms, it will be sent for 50ms minimum. If the
 * duration was a positive and the event is not over, it will cut it
 * short.
 *
 * If this function returns %TRUE, a "farstream-telephony-event-stopped" will
 * always be emitted when the event is actually stopped.

 * Returns: %TRUE if sucessful, it can return %FALSE if the #FsSession
 * does not support telephony events or if no telephony event is being sent
 */
gboolean
fs_session_stop_telephony_event (FsSession *session)
{
  FsSessionClass *klass;

  g_return_val_if_fail (session, FALSE);
  g_return_val_if_fail (FS_IS_SESSION (session), FALSE);
  klass = FS_SESSION_GET_CLASS (session);

  if (klass->stop_telephony_event) {
    return klass->stop_telephony_event (session);
  } else {
    GST_WARNING ("stop_telephony_event not defined in class");
  }
  return FALSE;
}

/**
 * fs_session_set_send_codec:
 * @session: a #FsSession
 * @send_codec: a #FsCodec representing the codec to send
 * @error: location of a #GError, or %NULL if no error occured
 *
 * This function will set the currently being sent codec for all streams in this
 * session. The given #FsCodec must be taken directly from the #codecs
 * property of the session. If the given codec is not in the codecs
 * list, @error will be set and %FALSE will be returned. The @send_codec will be
 * copied so it must be free'd using fs_codec_destroy() when done.
 *
 * Returns: %FALSE if the send codec couldn't be set.
 */
gboolean
fs_session_set_send_codec (FsSession *session, FsCodec *send_codec,
                           GError **error)
{
  FsSessionClass *klass;

  g_return_val_if_fail (session, FALSE);
  g_return_val_if_fail (FS_IS_SESSION (session), FALSE);
  klass = FS_SESSION_GET_CLASS (session);

  if (klass->set_send_codec) {
    return klass->set_send_codec (session, send_codec, error);
  } else {
    GST_WARNING ("set_send_codec not defined in class");
    g_set_error (error, FS_ERROR, FS_ERROR_NOT_IMPLEMENTED,
      "set_send_codec not defined in class");
  }
  return FALSE;
}

/**
 * fs_session_set_codec_preferences:
 * @session: a #FsSession
 * @codec_preferences: (element-type FsCodec) (allow-none): a #GList of #FsCodec with the
 *   desired configuration
 * @error: location of a #GError, or %NULL if no error occured
 *
 * Set the list of desired codec preferences. The user may
 * change this value during an ongoing session. Note that doing this can cause
 * the codecs to change. Therefore this requires the user to fetch
 * the new codecs and renegotiate them with the peers. It is a #GList
 * of #FsCodec. The changes are immediately effective.
 * The function does not take ownership of the list.
 *
 * The payload type may be a valid dynamic PT (96-127), %FS_CODEC_ID_DISABLE
 * or %FS_CODEC_ID_ANY. If the encoding name is "reserve-pt", then the
 * payload type of the codec will be "reserved" and not be used by any
 * dynamically assigned payload type.
 *
 * If the list of specifications would invalidate all codecs, an error will
 * be returned.
 *
 * Returns: %TRUE on success, %FALSE on error.
 */
gboolean
fs_session_set_codec_preferences (FsSession *session,
    GList *codec_preferences,
    GError **error)
{
  FsSessionClass *klass;

  g_return_val_if_fail (session, FALSE);
  g_return_val_if_fail (FS_IS_SESSION (session), FALSE);
  klass = FS_SESSION_GET_CLASS (session);

  if (klass->set_codec_preferences) {
    return klass->set_codec_preferences (session, codec_preferences, error);
  } else {
    GST_WARNING ("set_send_preferences not defined in class");
    g_set_error (error, FS_ERROR, FS_ERROR_NOT_IMPLEMENTED,
        "set_codec_preferences not defined in class");
  }
  return FALSE;
}

/**
 * fs_session_emit_error:
 * @session: #FsSession on which to emit the error signal
 * @error_no: The number of the error of type #FsError
 * @error_msg: Error message
 *
 * This function emit the "error" signal on a #FsSession, it should only be
 * called by subclasses.
 */
void
fs_session_emit_error (FsSession *session,
    gint error_no,
    const gchar *error_msg)
{
  g_signal_emit (session, signals[ERROR_SIGNAL], 0, session, error_no,
      error_msg);
}

/**
 * fs_session_list_transmitters:
 * @session: A #FsSession
 *
 * Get the list of all available transmitters for this session.
 *
 * Returns: (transfer full): a newly-allocagted %NULL terminated array of
 * named of transmitters or %NULL if no transmitter is needed for this type of
 * session. It should be freed with g_strfreev().
 */

gchar **
fs_session_list_transmitters (FsSession *session)
{
  FsSessionClass *klass;

  g_return_val_if_fail (session, NULL);
  g_return_val_if_fail (FS_IS_SESSION (session), NULL);
  klass = FS_SESSION_GET_CLASS (session);

  if (klass->list_transmitters) {
    return klass->list_transmitters (session);
  } else {
    return NULL;
  }
}


/**
 * fs_session_get_stream_transmitter_type:
 * @session: A #FsSession
 * @transmitter: The name of the transmitter
 *
 * Returns the GType of the stream transmitter, bindings can use it
 * to validate/convert the parameters passed to fs_session_new_stream().
 *
 * Returns: The #GType of the stream transmitter
 */
GType
fs_session_get_stream_transmitter_type (FsSession *session,
    const gchar *transmitter)
{
  FsSessionClass *klass;

  g_return_val_if_fail (session, 0);
  g_return_val_if_fail (FS_IS_SESSION (session), 0);
  klass = FS_SESSION_GET_CLASS (session);

  if (klass->get_stream_transmitter_type)
    return klass->get_stream_transmitter_type (session, transmitter);

  return 0;
}

/**
 * fs_session_codecs_need_resend:
 * @session: a #FsSession
 * @old_codecs: (element-type FsCodec) (transfer none) (allow-none):
 *  Codecs previously retrieved from the #FsSession:codecs property
 * @new_codecs: (element-type FsCodec) (transfer none) (allow-none):
 *   Codecs recently retrieved from the #FsSession:codecs property
 *
 * Some codec updates need to be reliably transmitted to the other side
 * because they contain important parameters required to decode the media.
 * Other codec updates, caused by user action, don't.
 *
 * Returns: (element-type FsCodec) (transfer full): A new #GList of
 *  #FsCodec that need to be resent or %NULL if there are none. This
 *  list must be freed with fs_codec_list_destroy().
 */
GList *
fs_session_codecs_need_resend (FsSession *session,
    GList *old_codecs, GList *new_codecs)
{
  FsSessionClass *klass;

  g_return_val_if_fail (session, 0);
  g_return_val_if_fail (FS_IS_SESSION (session), 0);
  klass = FS_SESSION_GET_CLASS (session);

  if (klass->codecs_need_resend)
    return klass->codecs_need_resend (session, old_codecs, new_codecs);

  return NULL;
}

/**
 * fs_session_set_encryption_parameters:
 * @session: a #FsSession
 * @parameters: (transfer none) (allow-none): a #GstStructure containing the
 *   encryption  parameters or %NULL to disable encryption
 * @error: the location where to store a #GError or %NULL
 *
 * Sets encryption parameters. The exact parameters depend on the type of
 * plugin being used.
 *
 * Returns: %TRUE if the encryption parameters could be set, %FALSE otherwise
 * Since: UNRELEASED
 */
gboolean
fs_session_set_encryption_parameters (FsSession *session,
    GstStructure *parameters, GError **error)
{
  FsSessionClass *klass;

  g_return_val_if_fail (session, FALSE);
  g_return_val_if_fail (FS_IS_SESSION (session), FALSE);
  klass = FS_SESSION_GET_CLASS (session);

  if (klass->set_encryption_parameters)
    return klass->set_encryption_parameters (session, parameters, error);

  g_set_error (error, FS_ERROR, FS_ERROR_NOT_IMPLEMENTED,
      "Does not support encryption");

  return FALSE;
}

/**
 * fs_session_destroy:
 * @session: a #FsSession
 *
 * This will cause the session to remove all links to other objects and to
 * remove itself from the #FsConference, it will also destroy all #FsStream
 * inside this #FsSession Once a #FsSession has been destroyed, it
 * can not be used anymore.
 *
 * It is strongly recommended to call this function from the main thread because
 * releasing the application's reference to a session.
 */

void
fs_session_destroy (FsSession *session)
{
  g_return_if_fail (session);
  g_return_if_fail (FS_IS_SESSION (session));

  g_object_run_dispose (G_OBJECT (session));
}

static gboolean
check_message (GstMessage *message,
    FsSession *session,
    const gchar *message_name)
{
  const GstStructure *s;
  const GValue *value;
  FsSession *message_session;

  if (GST_MESSAGE_TYPE (message) != GST_MESSAGE_ELEMENT)
    return FALSE;

  s = gst_message_get_structure (message);

  if (!gst_structure_has_name (s, message_name))
    return FALSE;

  value = gst_structure_get_value (s, "session");
  if (!value || !G_VALUE_HOLDS (value, FS_TYPE_SESSION))
    return FALSE;
  message_session = g_value_get_object (value);

  if (session != message_session)
    return FALSE;

  return TRUE;
}

/**
 * fs_session_parse_send_codec_changed:
 * @session: a #FsSession to match against the message
 * @message: a #GstMessage to parse
 * @codec: (out) (transfer none): Returns the #FsCodec in the message if not
 *   %NULL.
 * @secondary_codecs: (out) (transfer none) (element-type FsCodec):
 *  Returns a #GList of #FsCodec of the message if not %NULL
 *
 * Parses a "farstream-send-codec-changed" message and checks if it matches
 * the @session parameters.
 *
 * Returns: %TRUE if the message matches the session and is valid.
 */
gboolean
fs_session_parse_send_codec_changed ( FsSession *session,
    GstMessage *message,
    FsCodec **codec,
    GList **secondary_codecs)
{
  const GstStructure *s;
  const GValue *value;

  g_return_val_if_fail (session != NULL, FALSE);

  if (!check_message (message, session, "farstream-send-codec-changed"))
    return FALSE;

  s = gst_message_get_structure (message);

  value = gst_structure_get_value (s, "codec");
  if (!value || !G_VALUE_HOLDS (value, FS_TYPE_CODEC))
    return FALSE;
  if (codec)
    *codec = g_value_get_boxed (value);

  value = gst_structure_get_value (s, "secondary-codecs");
  if (!value || !G_VALUE_HOLDS (value, FS_TYPE_CODEC_LIST))
    return FALSE;
  if (secondary_codecs)
    *secondary_codecs = g_value_get_boxed (value);

  return TRUE;
}


/**
 * fs_session_parse_codecs_changed:
 * @session: a #FsSession to match against the message
 * @message: a #GstMessage to parse
 *
 * Parses a "farstream-codecs-changed" message and checks if it matches
 * the @session parameters.
 *
 * Returns: %TRUE if the message matches the session and is valid.
 */
gboolean
fs_session_parse_codecs_changed (FsSession *session,
    GstMessage *message)
{
  g_return_val_if_fail (session != NULL, FALSE);

  return check_message (message, session, "farstream-codecs-changed");
}

/**
 * fs_session_parse_telephony_event_started:
 * @session: a #FsSession to match against the message
 * @message: a #GstMessage to parse
 * @method: (out): Returns the #FsDTMFMethod in the message if not %NULL.
 * @event: (out): Returns the #FsDTMFEvent in the message if not %NULL.
 * @volume: (out): Returns the volume in the message if not %NULL.
 *
 * Parses a "farstream-telephony-event-started" message and checks if it matches
 * the @session parameters.
 *
 * Returns: %TRUE if the message matches the session and is valid.
 */
gboolean
fs_session_parse_telephony_event_started (FsSession *session,
    GstMessage *message,
    FsDTMFMethod *method, FsDTMFEvent *event,
    guint8 *volume)
{
  const GstStructure *s;
  const GValue *value;

  g_return_val_if_fail (session != NULL, FALSE);

  if (!check_message (message, session, "farstream-telephony-event-started"))
    return FALSE;

  s = gst_message_get_structure (message);

  if (!gst_structure_has_field_typed (s, "method", FS_TYPE_DTMF_METHOD))
    return FALSE;
  if (method)
    gst_structure_get_enum (s, "method", FS_TYPE_DTMF_METHOD, (gint*) method);

  if (!gst_structure_has_field_typed (s, "event", FS_TYPE_DTMF_EVENT))
    return FALSE;
  if (event)
    gst_structure_get_enum (s, "event", FS_TYPE_DTMF_EVENT, (gint*) event);

  value = gst_structure_get_value (s, "volume");
  if (!value || !G_VALUE_HOLDS (value, G_TYPE_UCHAR))
    return FALSE;
  if (volume)
    *volume = g_value_get_uchar (value);

  return TRUE;
}


/**
 * fs_session_parse_telephony_event_stopped:
 * @session: a #FsSession to match against the message
 * @message: a #GstMessage to parse
 * @method: (out): Returns the #FsDTMFMethod in the message if not %NULL.
 *
 * Parses a "farstream-telephony-event-stopped" message and checks if it matches
 * the @session parameters.
 *
 * Returns: %TRUE if the message matches the session and is valid.
 */
gboolean
fs_session_parse_telephony_event_stopped (FsSession *session,
    GstMessage *message,
     FsDTMFMethod *method)
{
  const GstStructure *s;

  g_return_val_if_fail (session != NULL, FALSE);

  if (!check_message (message, session, "farstream-telephony-event-stopped"))
    return FALSE;

  s = gst_message_get_structure (message);

  if (!gst_structure_has_field_typed (s, "method", FS_TYPE_DTMF_METHOD))
    return FALSE;
  if (method)
    gst_structure_get_enum (s, "method", FS_TYPE_DTMF_METHOD, (gint*) method);

  return TRUE;
}

/**
 * fs_session_set_allowed_caps:
 * @session: a #FsSession
 * @sink_caps: (allow-none): Caps for the sink pad or %NULL
 * @src_caps: (allow-none): Caps for the src pad or %NULL
 * @error: the location where a #GError can be stored or %NULL
 *
 * Sets the allowed caps for the sink and source pads for this #FsSession.
 * Only codecs that can take the input specified by the @sink_caps and
 * can produce output as specified by the @src_caps will be produced
 * in the #FsSession:codecs property and so only those will be negotiated.
 *
 * If %NULL is passed to either @src_caps or @sink_caps, it is not changed.
 *
 * The default is "video/x-raw" for a video stream, "audio/x-raw" for an audio
 * stream and "ANY" for an application stream.
 *
 * The values can be retrived using the #FsSession:allowed-src-caps and
 * #FsSession:allowed-sink-caps properties.
 *
 * Returns: %TRUE if the new filter caps were acceptable.
 *
 * Since: UNRELEASED
 */
gboolean
fs_session_set_allowed_caps (FsSession *session, GstCaps *sink_caps,
    GstCaps *src_caps, GError **error)
{
  FsSessionClass *klass;

  g_return_val_if_fail (FS_IS_SESSION (session), FALSE);

  if (sink_caps == NULL && src_caps == NULL)
    return TRUE;

  klass = FS_SESSION_GET_CLASS (session);

  if (klass->set_allowed_caps)
    return klass->set_allowed_caps (session, sink_caps, src_caps, error);

  g_set_error (error, FS_ERROR, FS_ERROR_NOT_IMPLEMENTED,
      "set_allowed_caps is not implemented");

  return FALSE;
}
