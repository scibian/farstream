/*
 * Farstream - Farstream Transmitter
 *
 * Copyright 2007 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * Copyright 2007 Nokia Corp.
 *
 * fs-transmitter.c - A Farstream Transmitter gobject (base implementation)
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
 * SECTION:fs-transmitter
 * @short_description: A transmitter object linked to a session
 *
 * This object is the base implementation of a Farstream Transmitter.
 * It needs to be derived and implement by a Farstream transmitter. A
 * Farstream Transmitter provides a GStreamer network sink and source to be used
 * for the Farstream Session. It creates #FsStreamTransmitter objects which are
 * used to set the different per-stream properties
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fs-transmitter.h"

#include <gst/gst.h>

#include "fs-plugin.h"
#include "fs-conference.h"
#include "fs-private.h"

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
  PROP_GST_SINK,
  PROP_GST_SRC,
  PROP_COMPONENTS,
  PROP_TYPE_OF_SERVICE,
  PROP_DO_TIMESTAMP,
};

/*
struct _FsTransmitterPrivate
{
};
*/

G_DEFINE_ABSTRACT_TYPE(FsTransmitter, fs_transmitter, G_TYPE_OBJECT);

#define FS_TRANSMITTER_GET_PRIVATE(o)  \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), FS_TYPE_TRANSMITTER, FsTransmitterPrivate))

static void fs_transmitter_get_property (GObject *object,
                                         guint prop_id,
                                         GValue *value,
                                         GParamSpec *pspec);
static void fs_transmitter_set_property (GObject *object,
                                         guint prop_id,
                                         const GValue *value,
                                         GParamSpec *pspec);

static guint signals[LAST_SIGNAL] = { 0 };


static void
fs_transmitter_class_init (FsTransmitterClass *klass)
{
  GObjectClass *gobject_class;

  _fs_conference_init_debug ();

  gobject_class = (GObjectClass *) klass;

  gobject_class->set_property = fs_transmitter_set_property;
  gobject_class->get_property = fs_transmitter_get_property;



  /**
   * FsTransmitter:gst-src:
   *
   * A network source #GstElement to be used by the #FsSession
   * This element MUST provide a source pad named "src_%u" per component.
   * These pads number must start at 1 (the %u corresponds to the component
   * number).
   * These pads MUST be static pads.
   *
   */
  g_object_class_install_property (gobject_class,
      PROP_GST_SRC,
      g_param_spec_object ("gst-src",
        "The network source",
        "A source GstElement to be used by a FsSession",
        GST_TYPE_ELEMENT,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * FsTransmitter:gst-sink:
   *
   * A network source #GstElement to be used by the #FsSession
   * These element's sink must have async=FALSE
   * This element MUST provide a pad named "sink_\%u" per component.
   * These pads number must start at 1 (the \%u corresponds to the component
   * number).
   * These pads MUST be static pads.
   *
   */
  g_object_class_install_property (gobject_class,
      PROP_GST_SINK,
      g_param_spec_object ("gst-sink",
        "The network source",
        "A source GstElement to be used by a FsSession",
        GST_TYPE_ELEMENT,
        G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  /**
   * FsTransmitter:components:
   *
   * The number of components to create
   */
  g_object_class_install_property (gobject_class,
      PROP_COMPONENTS,
      g_param_spec_uint ("components",
        "Number of componnets",
        "The number of components to create",
        1, 255, 1,
        G_PARAM_CONSTRUCT_ONLY | G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * FsTransmitter:tos:
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
   * FsTransmitter:do-timestamp:
   *
   * Apply current stream time to buffers or provide buffers without
   * timestamps. Must be set before creating a stream transmitter.
   */
  g_object_class_install_property (gobject_class,
      PROP_DO_TIMESTAMP,
      g_param_spec_boolean ("do-timestamp",
          "Do Timestamp",
          "Apply current stream time to buffers",
          TRUE,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * FsTransmitter::error:
   * @self: #FsTransmitter that emitted the signal
   * @errorno: The number of the error
   * @error_msg: Error message to be displayed to user
   *
   * This signal is emitted in any error condition
   *
   */
  signals[ERROR_SIGNAL] = g_signal_new ("error",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL, NULL,
      G_TYPE_NONE, 2, FS_TYPE_ERROR, G_TYPE_STRING);

  //g_type_class_add_private (klass, sizeof (FsTransmitterPrivate));
}

static void
fs_transmitter_init (FsTransmitter *self)
{
  // self->priv = FS_TRANSMITTER_GET_PRIVATE (self);
}

static void
fs_transmitter_get_property (GObject *object,
                             guint prop_id,
                             GValue *value,
                             GParamSpec *pspec)
{
  GST_WARNING ("Subclass %s of FsTransmitter does not override the %s property"
      " getter",
      G_OBJECT_TYPE_NAME(object),
      g_param_spec_get_name (pspec));
}

static void
fs_transmitter_set_property (GObject *object,
                             guint prop_id,
                             const GValue *value,
                             GParamSpec *pspec)
{
  GST_WARNING ("Subclass %s of FsTransmitter does not override the %s property"
      " setter",
      G_OBJECT_TYPE_NAME(object),
      g_param_spec_get_name (pspec));
}


/**
 * fs_transmitter_new_stream_transmitter:
 * @transmitter: a #FsTranmitter
 * @participant: the #FsParticipant for which the #FsStream using this
 * new #FsStreamTransmitter is created
 * @n_parameters: The number of parameters to pass to the newly created
 * #FsStreamTransmitter
 * @parameters: an array of #GParameter
 * @error: location of a #GError, or NULL if no error occured
 *
 * This function will create a new #FsStreamTransmitter element for a
 * specific participant for this #FsTransmitter
 *
 * Returns: (transfer full): a new #FsStreamTransmitter, or NULL if there is an
 *  error
 */

FsStreamTransmitter *
fs_transmitter_new_stream_transmitter (FsTransmitter *transmitter,
                                       FsParticipant *participant,
                                       guint n_parameters,
                                       GParameter *parameters,
                                       GError **error)
{
  FsTransmitterClass *klass;

  g_return_val_if_fail (transmitter, NULL);
  g_return_val_if_fail (FS_IS_TRANSMITTER (transmitter), NULL);
  klass = FS_TRANSMITTER_GET_CLASS (transmitter);
  g_return_val_if_fail (klass->new_stream_transmitter, NULL);


  return klass->new_stream_transmitter (transmitter, participant,
      n_parameters, parameters, error);

  return NULL;
}

/**
 * fs_transmitter_new:
 * @type: The type of transmitter to create
 * @components: The number of components to create
 * @tos: The Type of Service of the socket, max is 255
 * @error: location of a #GError, or NULL if no error occured
 *
 * This function creates a new transmitter of the requested type.
 * It will load the appropriate plugin as required.
 *
 * Returns: a newly-created #FsTransmitter of the requested type
 *    (or NULL if there is an error)
 */

FsTransmitter *
fs_transmitter_new (const gchar *type,
    guint components,
    guint tos,
    GError **error)
{
  FsTransmitter *self = NULL;

  g_return_val_if_fail (type != NULL, NULL);
  g_return_val_if_fail (tos <= 255, NULL);

  self = FS_TRANSMITTER (fs_plugin_create (type, "transmitter", error,
          "components", components,
          "tos", tos,
          NULL));

  if (!self)
    return NULL;

  if (self->construction_error) {
    g_propagate_error(error, self->construction_error);
    g_object_unref (self);
    self = NULL;
  }

  return self;
}

/**
 * fs_transmitter_get_stream_transmitter_type:
 * @transmitter: A #FsTransmitter object
 *
 * This function returns the GObject type for the stream transmitter.
 * This is meant for bindings that need to introspect the type of arguments
 * that can be passed to the _new_stream_transmitter.
 *
 * Returns: the #GType
 */

GType
fs_transmitter_get_stream_transmitter_type (FsTransmitter *transmitter)
{
  FsTransmitterClass *klass;

  g_return_val_if_fail (transmitter, 0);
  g_return_val_if_fail (FS_IS_TRANSMITTER (transmitter), 0);
  klass = FS_TRANSMITTER_GET_CLASS (transmitter);
  g_return_val_if_fail (klass->get_stream_transmitter_type, 0);

  return klass->get_stream_transmitter_type (transmitter);
}


/**
 * fs_transmitter_emit_error:
 * @transmitter: #FsTransmitter on which to emit the error signal
 * @error_no: The number of the error
 * @error_msg: Error message to be displayed to user
 *
 * This function emit the "error" signal on a #FsTransmitter, it should
 * only be called by subclasses.
 */
void
fs_transmitter_emit_error (FsTransmitter *transmitter,
    gint error_no,
    const gchar *error_msg)
{
  g_signal_emit (transmitter, signals[ERROR_SIGNAL], 0, error_no,
      error_msg);
}

/**
 * fs_transmitter_list_available:
 *
 * Get the list of all available transmitters
 *
 * Returns: (transfer full): a newly allocated array of strings containing the
 * list of all available transmitters or %NULL if there are none. It should
 *  be freed with g_strfreev().
 */

char **
fs_transmitter_list_available (void)
{
  return fs_plugin_list_available ("transmitter");
}
