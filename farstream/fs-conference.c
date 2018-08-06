/*
 * Farstream - GStreamer interfaces
 *
 * Copyright 2007-2011 Collabora Ltd.
 *  @author: Philippe Kalaf <philippe.kalaf@collabora.co.uk>
 *  @author: Olivier Crete <olivier.crete@collabora.com>
 * Copyright 2007-2011 Nokia Corp.
 *
 * fs-conference.c - GStreamer interface to be implemented by farstream
 *                         conference elements
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

#include "fs-conference.h"
#include "fs-session.h"
#include "fs-private.h"

/**
 * SECTION:fs-conference
 * @short_description: Interface for farstream conference elements
 *
 * A Farstream conference is a conversation space that takes place between 2 or
 * more participants. Each conference must have one or more Farstream sessions
 * that are associated to the conference participants.
 *
 *
 * This will communicate asynchronous events to the user through #GstMessage
 * of type #GST_MESSAGE_ELEMENT sent over the #GstBus.
 *
 * <refsect2><title>The "<literal>farstream-error</literal>" message</title>
 * |[
 * "src-object"       #GObject           The object (#FsConference, #FsSession or #FsStream) that emitted the error
 * "error-no"         #FsError           The Error number
 * "error-msg"        #gchar*            The error message
 * ]|
 * <para>
 * The message is sent on asynchronous errors.
 * </para>
 * </refsect2>
 *
 */


GST_DEBUG_CATEGORY (_fs_conference_debug);
#define GST_CAT_DEFAULT _fs_conference_debug


G_DEFINE_ABSTRACT_TYPE (FsConference, fs_conference, GST_TYPE_BIN)


GQuark
fs_error_quark (void)
{
  return g_quark_from_static_string ("fs-error");
}

void
_fs_conference_init_debug (void)
{
  GST_DEBUG_CATEGORY_INIT (_fs_conference_debug, "fsconference", 0,
      "farstream base conference library");
}

static void
fs_conference_class_init (FsConferenceClass * klass)
{
  _fs_conference_init_debug ();
}

static void
fs_conference_init (FsConference *conf)
{
  GST_DEBUG_OBJECT (conf, "fs_conference_init");
}


static void
fs_conference_error (GObject *signal_src,
    GObject *error_src,
    FsError error_no,
    gchar *error_msg,
    FsConference *conf)
{
  GstMessage *gst_msg = NULL;
  GstStructure *error_struct = NULL;

  error_struct = gst_structure_new ("farstream-error",
      "src-object", G_TYPE_OBJECT, error_src,
      "error-no", FS_TYPE_ERROR, error_no,
      "error-msg", G_TYPE_STRING, error_msg,
      NULL);

  gst_msg = gst_message_new_element (GST_OBJECT (conf), error_struct);

  if (!gst_element_post_message (GST_ELEMENT (conf), gst_msg))
    GST_WARNING_OBJECT (conf, "Could not post error on bus");
}

/**
 * fs_conference_new_session:
 * @conference: #FsConference interface of a #GstElement
 * @media_type: #FsMediaType of the new session
 * @error: location of a #GError, or %NULL if no error occured
 *
 * Create a new Farstream session for the given conference.
 *
 * Returns: (transfer full): the new #FsSession that has been created.
 * The #FsSession must be unref'd by the user when closing the session.
 */

FsSession *
fs_conference_new_session (FsConference *conf,
    FsMediaType media_type,
    GError **error)
{
  FsConferenceClass *klass;
  FsSession *new_session = NULL;

  g_return_val_if_fail (conf, NULL);
  g_return_val_if_fail (FS_IS_CONFERENCE (conf), NULL);
  klass = FS_CONFERENCE_GET_CLASS (conf);
  g_return_val_if_fail (klass->new_session, NULL);

  new_session = klass->new_session (conf, media_type, error);

  if (!new_session)
    return NULL;

  /* Let's catch all session errors and send them over the GstBus */
  g_signal_connect_object (new_session, "error",
      G_CALLBACK (fs_conference_error), conf, 0);

  return new_session;
}

/**
 * fs_conference_new_participant:
 * @conference: #FsConference interface of a #GstElement
 * @error: location of a #GError, or %NULL if no error occured
 *
 * Create a new Farstream Participant for the type of the given conference.
 *
 * Returns: (transfer full): the new #FsParticipant that has been created.
 * The #FsParticipant is owned by the user and he must unref it when he is
 * done with it.
 */
FsParticipant *
fs_conference_new_participant (FsConference *conf,
    GError **error)
{
  FsConferenceClass *klass;

  g_return_val_if_fail (conf, NULL);
  g_return_val_if_fail (FS_IS_CONFERENCE (conf), NULL);
  klass = FS_CONFERENCE_GET_CLASS (conf);
  g_return_val_if_fail (klass->new_participant, NULL);

  return klass->new_participant (conf, error);
}


/**
 * fs_parse_error:
 * @object: a #GObject to match against the message
 * @message: a #GstMessage to parse
 * @error: (out): Returns the #FsError error number in
 * the message if not %NULL.
 * @error_msg: (out) (transfer none):Returns the error message if not %NULL
 *
 * Parses a "farstream-farstream" message and checks if it matches
 * the @object parameters.
 *
 * Returns: %TRUE if the message matches the object and is valid.
 */
gboolean
fs_parse_error (GObject *object,
    GstMessage *message,
    FsError *error,
    const gchar **error_msg)

{
  const GstStructure *s;
  const GValue *value;
  GObject *message_object;

  g_return_val_if_fail (object != NULL, FALSE);

  if (GST_MESSAGE_TYPE (message) != GST_MESSAGE_ELEMENT)
    return FALSE;

  s = gst_message_get_structure (message);

  if (!gst_structure_has_name (s, "farstream-error"))
    return FALSE;

  value = gst_structure_get_value (s, "src-object");
  if (!value || !G_VALUE_HOLDS (value, G_TYPE_OBJECT))
    return FALSE;
  message_object = g_value_get_object (value);

  if (object != message_object)
    return FALSE;

  value = gst_structure_get_value (s, "error-no");
  if (!value || !G_VALUE_HOLDS (value, FS_TYPE_ERROR))
    return FALSE;
  if (error)
    *error = g_value_get_enum (value);

  value = gst_structure_get_value (s, "error-msg");
  if (!value || !G_VALUE_HOLDS (value, G_TYPE_STRING))
    return FALSE;
  if (error_msg)
    *error_msg = g_value_get_string (value);

  return TRUE;
}
