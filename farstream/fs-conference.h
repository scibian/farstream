/*
 * Farstream - FsConference Class
 *
 * Copyright 2007 Collabora Ltd.
 *  @author: Philippe Kalaf <philippe.kalaf@collabora.co.uk>
 * Copyright 2007 Nokia Corp.
 *
 * fs-conference.h - Header file for farstream Conference base class
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

#ifndef __FS_CONFERENCE_H__
#define __FS_CONFERENCE_H__

#include <gst/gst.h>

#include <farstream/fs-session.h>
#include <farstream/fs-codec.h>
#include <farstream/fs-enumtypes.h>

G_BEGIN_DECLS


#define FS_TYPE_CONFERENCE \
  (fs_conference_get_type ())
#define FS_CONFERENCE(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),FS_TYPE_CONFERENCE,FsConference))
#define FS_CONFERENCE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),FS_TYPE_CONFERENCE,FsConferenceClass))
#define FS_CONFERENCE_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS((obj),FS_TYPE_CONFERENCE,FsConferenceClass))
#define FS_IS_CONFERENCE(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),FS_TYPE_CONFERENCE))
#define FS_IS_CONFERENCE_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),FS_TYPE_CONFERENCE))
#define FS_CONFERENCE_CAST(obj) \
  ((FsConference *)(obj))

typedef struct _FsConference FsConference;
typedef struct _FsConferenceClass FsConferenceClass;

/**
 * FsConference:
 *
 * The #FsConference structure, all the members are private
 */

struct _FsConference
{
  GstBin parent;

  /*< private >*/

  gpointer _padding[8];
};


/**
 * FsConferenceClass:
 * @parent: parent GstBin class
 * @new_session: virtual method to create a new conference session
 * @new_participant: virtual method to create a new participant
 *
 * #FsConferenceClass class structure.
 */
struct _FsConferenceClass {
  GstBinClass parent;

  /* virtual functions */
  FsSession *(* new_session) (FsConference *conference, FsMediaType media_type,
                              GError **error);

  FsParticipant *(* new_participant) (FsConference *conference,
      GError **error);

  /*< private > */
  gpointer _gst_reserved[GST_PADDING];
};

GType fs_conference_get_type (void);



/**
 * FsError:
 * @FS_ERROR_CONSTRUCTION: Error constructing some of the sub-elements, this
 * probably denotes an error in the installation of the gstreamer elements.
 * It is a fatal error.
 * @FS_ERROR_INVALID_ARGUMENTS: Invalid arguments to the function, this
 * is a programming error and should not be reported to the user
 * @FS_ERROR_INTERNAL: An internal error happened in Farstream, it may be in
 * an inconsistent state. The object from which this error comes should be
 * discarded.
 * @FS_ERROR_NETWORK: A network related error, this should probably be
 *  reported to the user.
 * @FS_ERROR_NOT_IMPLEMENTED: The optional functionality is not implemented by
 * this plugin.
 * @FS_ERROR_NEGOTIATION_FAILED: The codec negotiation has failed, this means
 * that there are no common codecs between the local and remote codecs.
 * @FS_ERROR_UNKNOWN_CODEC: Data is received on an unknown codec, this most
 * likely denotes an error on the remote side, the buffers will be ignored.
 * It can safely be ignored in most cases (but may result in a call with no
 * media received).
 * @FS_ERROR_NO_CODECS: There are no codecs detected for that media type.
 * @FS_ERROR_NO_CODECS_LEFT: All of the codecs have been disabled by the
 * codec preferences, one should try less strict codec preferences.
 * @FS_ERROR_CONNECTION_FAILED: Could not connect to the to remote party.
 * @FS_ERROR_DISPOSED: The object has been disposed.
 * @FS_ERROR_ALREADY_EXISTS: The object already exists
 *
 * This is the enum of error numbers that will come either on the "error"
 * signal, from the Gst Bus or for error in the FS_ERROR domain in GErrors
 */

typedef enum
{
  FS_ERROR_CONSTRUCTION = 1,
  FS_ERROR_INTERNAL,
  FS_ERROR_INVALID_ARGUMENTS = 100,
  FS_ERROR_NETWORK,
  FS_ERROR_NOT_IMPLEMENTED,
  FS_ERROR_NEGOTIATION_FAILED,
  FS_ERROR_UNKNOWN_CODEC,
  FS_ERROR_NO_CODECS,
  FS_ERROR_NO_CODECS_LEFT,
  FS_ERROR_CONNECTION_FAILED,
  FS_ERROR_DISPOSED,
  FS_ERROR_ALREADY_EXISTS
} FsError;

/**
 * FS_ERROR:
 *
 * This quark is used to denote errors coming from Farstream objects
 */

#define FS_ERROR (fs_error_quark ())

/**
 * FS_ERROR_IS_FATAL:
 * @error: a #FsError
 *
 * Tells the programmer if an error if fatal or not, if it returns %TRUE,
 * the error is fatal, and the object that created it should
 * be discarded. It returns %FALSE otherwise.
 */

#define FS_ERROR_IS_FATAL(error) \
  (error < 100)

GQuark fs_error_quark (void);

/* virtual class function wrappers */
FsSession *fs_conference_new_session (FsConference *conference,
                                      FsMediaType media_type,
                                      GError **error);

FsParticipant *fs_conference_new_participant (FsConference *conference,
    GError **error);

gboolean fs_parse_error (GObject *object,
    GstMessage *message,
    FsError *error,
    const gchar **error_msg);



G_END_DECLS

#endif /* __FS_CONFERENCE_H__ */


