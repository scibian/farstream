/*
 * Farstream - Farstream RTP Special Codec
 *
 * Copyright 2007 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * Copyright 2007 Nokia Corp.
 *
 * fs-rtp-special-source.c - A Farstream RTP Special Source gobject
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

#include "fs-rtp-special-source.h"

#include <farstream/fs-conference.h>

#include "fs-rtp-conference.h"
#include "fs-rtp-codec-negotiation.h"

#include "fs-rtp-dtmf-event-source.h"
#include "fs-rtp-dtmf-sound-source.h"

#define GST_CAT_DEFAULT fsrtpconference_debug

/*
 * SECTION:fs-rtp-special-source
 * @short_description: Base class to abstract how special sources are handled
 *
 * This class defines how special sources can be handled, it is the base
 * for DMTF and CN sources.
 *
 */


/* props */
enum
{
  PROP_0,
};

struct _FsRtpSpecialSourcePrivate {
  gboolean disposed;

  GstElement *outer_bin;
  GstElement *rtpmuxer;

  GstPad *muxer_request_pad;
  GstElement *src;

  GThread *stop_thread;

  fs_rtp_special_source_stopped_callback stopped_callback;
  gpointer stopped_data;

  /* Protects the content of this struct after object has been disposed of */
  GMutex mutex;
};

static GList *classes = NULL;

G_DEFINE_ABSTRACT_TYPE(FsRtpSpecialSource, fs_rtp_special_source,
    G_TYPE_OBJECT);

#define FS_RTP_SPECIAL_SOURCE_GET_PRIVATE(o)                                 \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), FS_TYPE_RTP_SPECIAL_SOURCE,             \
   FsRtpSpecialSourcePrivate))


#define FS_RTP_SPECIAL_SOURCE_LOCK(src)   g_mutex_lock (&(src)->priv->mutex)
#define FS_RTP_SPECIAL_SOURCE_UNLOCK(src) g_mutex_unlock (&(src)->priv->mutex)

static void fs_rtp_special_source_dispose (GObject *object);
static void fs_rtp_special_source_finalize (GObject *object);

static FsRtpSpecialSource *
fs_rtp_special_source_new (FsRtpSpecialSourceClass *klass,
    GList **negotiated_codec_associations,
    GMutex *mutex,
    FsCodec *selected_codec,
    GstElement *bin,
    GstElement *rtpmuxer);


static FsCodec* fs_rtp_special_source_class_get_codec (
    FsRtpSpecialSourceClass *klass,
    GList *negotiated_codec_associations,
    FsCodec *selected_codec);

static gpointer
register_classes (gpointer data)
{
  GList *my_classes = NULL;

  my_classes = g_list_prepend (my_classes,
      g_type_class_ref (FS_TYPE_RTP_DTMF_EVENT_SOURCE));
  my_classes = g_list_prepend (my_classes,
      g_type_class_ref (FS_TYPE_RTP_DTMF_SOUND_SOURCE));

  return my_classes;
}

static void
fs_rtp_special_sources_init (void)
{
  static GOnce my_once = G_ONCE_INIT;

  classes = g_once (&my_once, register_classes, NULL);
}

static void
fs_rtp_special_source_class_init (FsRtpSpecialSourceClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  gobject_class->dispose = fs_rtp_special_source_dispose;
  gobject_class->finalize = fs_rtp_special_source_finalize;

  g_type_class_add_private (klass, sizeof (FsRtpSpecialSourcePrivate));
}

static void
fs_rtp_special_source_init (FsRtpSpecialSource *self)
{
  self->priv = FS_RTP_SPECIAL_SOURCE_GET_PRIVATE (self);
  self->priv->disposed = FALSE;

  g_mutex_init (&self->priv->mutex);
}

/**
 * stop_source_thread:
 * @data: a pointer to the current #FsRtpSpecialSource
 *
 * This functioin will lock on the source's state change until its release
 * and only then let the source be disposed of
 */

static gpointer
stop_source_thread (gpointer data)
{
  FsRtpSpecialSource *self = FS_RTP_SPECIAL_SOURCE (data);

  gst_element_set_locked_state (self->priv->src, TRUE);
  gst_element_set_state (self->priv->src, GST_STATE_NULL);

  FS_RTP_SPECIAL_SOURCE_LOCK (self);
  if (self->priv->muxer_request_pad)
  {
    gst_element_release_request_pad (self->priv->rtpmuxer,
        self->priv->muxer_request_pad);
    gst_object_unref (self->priv->muxer_request_pad);
  }
  self->priv->muxer_request_pad = NULL;

  gst_bin_remove (GST_BIN (self->priv->outer_bin), self->priv->src);
  self->priv->src = NULL;
  FS_RTP_SPECIAL_SOURCE_UNLOCK (self);

  if (self->priv->stopped_callback)
    self->priv->stopped_callback (self, self->priv->stopped_data);

  g_object_unref (self);

  return NULL;
}

static gboolean
fs_rtp_special_source_is_stopping (FsRtpSpecialSource *self)
{
  gboolean stopping;

  FS_RTP_SPECIAL_SOURCE_LOCK (self);
  stopping = !!self->priv->stop_thread;
  FS_RTP_SPECIAL_SOURCE_UNLOCK (self);

  return stopping;
}

/*
 * Returns TRUE if the source is already stopped
 */

static gboolean
fs_rtp_special_source_stop_locked (FsRtpSpecialSource *self)
{

  if (self->priv->src)
  {
    if (self->priv->stop_thread)
    {
      GST_DEBUG ("stopping thread for special source already running");
      return TRUE;
    }

    g_object_ref (self);
    self->priv->stop_thread = g_thread_new ("special-source-stop",
        stop_source_thread, self);
    g_thread_unref (self->priv->stop_thread);

    return TRUE;
  }
  else
  {
    self->priv->stop_thread = GUINT_TO_POINTER (1);
    return FALSE;
  }
}

static void
fs_rtp_special_source_dispose (GObject *object)
{
  FsRtpSpecialSource *self = FS_RTP_SPECIAL_SOURCE (object);

  if (self->priv->disposed)
    return;


  FS_RTP_SPECIAL_SOURCE_LOCK (self);

  if (self->priv->disposed)
  {
    FS_RTP_SPECIAL_SOURCE_UNLOCK (self);
    return;
  }


  if (fs_rtp_special_source_stop_locked (self))
  {
    FS_RTP_SPECIAL_SOURCE_UNLOCK (self);
    return;
  }

  if (self->priv->rtpmuxer)
  {
    gst_object_unref (self->priv->rtpmuxer);
    self->priv->rtpmuxer = NULL;
  }

  if (self->priv->outer_bin)
  {
    gst_object_unref (self->priv->outer_bin);
    self->priv->outer_bin = NULL;
  }

  self->priv->disposed = TRUE;

  FS_RTP_SPECIAL_SOURCE_UNLOCK (self);

  G_OBJECT_CLASS (fs_rtp_special_source_parent_class)->dispose (object);
}


static void
fs_rtp_special_source_finalize (GObject *object)
{
  FsRtpSpecialSource *self = FS_RTP_SPECIAL_SOURCE (object);

  if (self->priv->rtpmuxer)
  {
    gst_object_unref (self->priv->rtpmuxer);
    self->priv->rtpmuxer = NULL;
  }

  if (self->priv->outer_bin)
  {
    gst_object_unref (self->priv->outer_bin);
    self->priv->outer_bin = NULL;
  }

  if (self->codec)
    fs_codec_destroy (self->codec);
  self->codec = NULL;

  g_mutex_clear (&self->priv->mutex);

  G_OBJECT_CLASS (fs_rtp_special_source_parent_class)->finalize (object);
}

static GList*
fs_rtp_special_source_class_add_blueprint (FsRtpSpecialSourceClass *klass,
    GList *blueprints)
{
  if (klass->add_blueprint)
    return klass->add_blueprint (klass, blueprints);
  else
    GST_CAT_DEBUG (fsrtpconference_disco,
        "Class %s has no add_blueprint function", G_OBJECT_CLASS_NAME(klass));

  return blueprints;
}

static GList*
fs_rtp_special_source_class_negotiation_filter (FsRtpSpecialSourceClass *klass,
    GList *codec_associations)
{
  if (klass->negotiation_filter)
    return klass->negotiation_filter (klass, codec_associations);
  else
    GST_CAT_DEBUG (fsrtpconference_disco,
        "Class %s has no negotiation_filter function",
        G_OBJECT_CLASS_NAME(klass));

  return codec_associations;
}


/**
 * fs_rtp_special_sources_add_blueprints:
 * @blueprints: a #GList of #CodecBlueprint
 *
 * This function will add blueprints to the current list of blueprints based
 * on which elements are installed and on which codecs are already in the list
 * of blueprints.
 *
 * Returns: The updated #GList of #CodecBlueprint
 */

GList *
fs_rtp_special_sources_add_blueprints (GList *blueprints)
{
  GList *item = NULL;

  fs_rtp_special_sources_init ();

  for (item = g_list_first (classes);
       item;
       item = g_list_next (item))
  {
    FsRtpSpecialSourceClass *klass = item->data;

    blueprints = fs_rtp_special_source_class_add_blueprint (klass, blueprints);
  }

  return blueprints;
}

/**
 * fs_rtp_special_sources_negotiation_filter:
 * @codec_associations: A #GList of negotiation Codec Associations
 *
 * This will apply all of the source specific negotiation filters to the list
 * of just negotiated codec associations and modify it in the appropriate way.
 */

GList *
fs_rtp_special_sources_negotiation_filter (GList *codec_associations)
{
  GList *item = NULL;

  fs_rtp_special_sources_init ();

  for (item = g_list_first (classes);
       item;
       item = g_list_next (item))
  {
    FsRtpSpecialSourceClass *klass = item->data;

    codec_associations = fs_rtp_special_source_class_negotiation_filter (klass,
        codec_associations);
  }

  return codec_associations;
}

void
fs_rtp_special_sources_remove_finish (GList **extra_sources,
    GMutex *mutex,
    FsRtpSpecialSource *source)
{
  g_mutex_lock (mutex);
  *extra_sources = g_list_remove (*extra_sources, source);
  g_mutex_unlock (mutex);
  g_object_unref (source);
}


/**
 * fs_rtp_special_sources_remove:
 * @extra_sources: A pointer to the #GList returned by previous calls to this
 *  function
 * @negotiated_codec_associations: A pointer to the #GList of current negotiated
 * #CodecAssociation
 * @mutex: the mutex protecting the last two things
 * @selected_codec: A pointer to the currently selected codec for sending,
 *   but not send_codec
 *
 * This function removes any special source that are not compatible with the
 * currently selected send codec.
 *
 * Returns: %TRUE if a source was removed
 */
gboolean
fs_rtp_special_sources_remove (
    GList **extra_sources,
    GList **negotiated_codec_associations,
    GMutex *mutex,
    FsCodec *selected_codec,
    fs_rtp_special_source_stopped_callback stopped_callback,
    gpointer stopped_data)
{
  GList *klass_item = NULL;
  gboolean changed = FALSE;

  fs_rtp_special_sources_init ();

  for (klass_item = g_list_first (classes);
       klass_item;
       klass_item = g_list_next (klass_item))
  {
    FsRtpSpecialSourceClass *klass = klass_item->data;
    GList *obj_item;
    FsRtpSpecialSource *obj = NULL;

  restart:
    g_mutex_lock (mutex);

    /* Check if we already have an object for this type */
    for (obj_item = g_list_first (*extra_sources);
         obj_item;
         obj_item = g_list_next (obj_item))
    {
      obj = obj_item->data;
      if (G_OBJECT_TYPE(obj) == G_OBJECT_CLASS_TYPE(klass) &&
          !fs_rtp_special_source_is_stopping (obj))
        break;
    }

    if (obj_item)
    {
      FsCodec *telephony_codec = fs_rtp_special_source_class_get_codec (klass,
          *negotiated_codec_associations, selected_codec);

      if (!telephony_codec || !fs_codec_are_equal (telephony_codec, obj->codec))
      {
        FsRtpSpecialSource *self = FS_RTP_SPECIAL_SOURCE (obj);

        self->priv->stopped_callback = stopped_callback;
        self->priv->stopped_data = stopped_data;
        if (!fs_rtp_special_source_stop_locked (self))
        {
          *extra_sources = g_list_remove (*extra_sources, obj);
          changed = TRUE;
          g_mutex_unlock (mutex);
          g_object_unref (obj);
          goto restart;
        }
      }
    }

    g_mutex_unlock (mutex);
  }

  return changed;
}

/**
 * fs_rtp_special_sources_create:
 * @current_extra_sources: A pointer to the #GList returned by previous calls
 * to this function
 * @negotiated_codec_associations: A pointer to the #GList of current negotiated
 * #CodecAssociation
 * @mutex: the mutex protecting the last two things
 * @selected_codec: The currently selected codec for sending (but not
 *    send_codec)
 * @bin: The #GstBin to add the stuff to
 * @rtpmuxer: The rtpmux element
 *
 * This function add special sources that don't already exist but are needed
 *
 * Returns: %TRUE if at least one source was added
 */
gboolean
fs_rtp_special_sources_create (
    GList **extra_sources,
    GList **negotiated_codec_associations,
    GMutex *mutex,
    FsCodec *selected_codec,
    GstElement *bin,
    GstElement *rtpmuxer)
{
  GList *klass_item = NULL;
  gboolean changed = FALSE;

  fs_rtp_special_sources_init ();

  g_mutex_lock (mutex);

  for (klass_item = g_list_first (classes);
       klass_item;
       klass_item = g_list_next (klass_item))
  {
    FsRtpSpecialSourceClass *klass = klass_item->data;
    GList *obj_item;
    FsRtpSpecialSource *obj = NULL;

    /* Check if we already have an object for this type */
    for (obj_item = g_list_first (*extra_sources);
         obj_item;
         obj_item = g_list_next (obj_item))
    {
      obj = obj_item->data;
      if (G_OBJECT_TYPE(obj) == G_OBJECT_CLASS_TYPE(klass) &&
          !fs_rtp_special_source_is_stopping (obj))
        break;
    }

    if (!obj_item &&
        fs_rtp_special_source_class_get_codec (klass,
            *negotiated_codec_associations, selected_codec))
    {
      g_mutex_unlock (mutex);
      obj = fs_rtp_special_source_new (klass, negotiated_codec_associations,
          mutex, selected_codec, bin, rtpmuxer);
      if (!obj)
      {
        GST_WARNING ("Failed to make new special source");
        return changed;
      }

      g_mutex_lock (mutex);

      /* Check again if we already have an object for this type */
      for (obj_item = g_list_first (*extra_sources);
           obj_item;
           obj_item = g_list_next (obj_item))
        if (G_OBJECT_TYPE(obj_item->data) == G_OBJECT_CLASS_TYPE(klass) &&
            !fs_rtp_special_source_is_stopping (obj_item->data))
          break;
      if (obj_item)
      {
        g_mutex_unlock (mutex);
        g_object_unref (obj);
        g_mutex_lock (mutex);
      }
      else
      {
        *extra_sources = g_list_prepend (*extra_sources, obj);
        changed = TRUE;
      }
    }
  }

  g_mutex_unlock (mutex);

  return changed;
}

static FsRtpSpecialSource *
fs_rtp_special_source_new (FsRtpSpecialSourceClass *klass,
    GList **negotiated_codec_associations,
    GMutex *mutex,
    FsCodec *selected_codec,
    GstElement *bin,
    GstElement *rtpmuxer)
{
  FsRtpSpecialSource *source = NULL;
  GstPad *pad = NULL;

  g_return_val_if_fail (klass, NULL);
  g_return_val_if_fail (klass->build, NULL);
  g_return_val_if_fail (GST_IS_BIN (bin), NULL);
  g_return_val_if_fail (GST_IS_ELEMENT (rtpmuxer), NULL);

  source = g_object_new (G_OBJECT_CLASS_TYPE (klass),
      NULL);
  g_return_val_if_fail (source, NULL);

  g_mutex_lock (mutex);

  source->priv->rtpmuxer = gst_object_ref (rtpmuxer);
  source->priv->outer_bin = gst_object_ref (bin);
  source->priv->src = klass->build (source, *negotiated_codec_associations,
      selected_codec);

  g_mutex_unlock (mutex);

  if (!source->priv->src)
    goto error;

  if (!gst_bin_add (GST_BIN (source->priv->outer_bin), source->priv->src))
  {
    GST_ERROR ("Could not add bin to outer bin");
    gst_object_unref (source->priv->src);
    source->priv->src = NULL;
    goto error;
  }

  source->priv->muxer_request_pad = gst_element_get_request_pad (rtpmuxer,
      "priority_sink_%u");
  if (!source->priv->muxer_request_pad)
    source->priv->muxer_request_pad = gst_element_get_request_pad (rtpmuxer,
        "sink_%u");

  if (!source->priv->muxer_request_pad)
  {
    GST_ERROR ("Could not get request pad from muxer");
    goto error_added;
  }

  pad = gst_element_get_static_pad (source->priv->src, "src");

  if (GST_PAD_LINK_FAILED (gst_pad_link (pad, source->priv->muxer_request_pad)))
  {
    GST_ERROR ("Could not link rtpdtmfsrc src to muxer sink");
    gst_object_unref (pad);
    goto error_added;
  }
  gst_object_unref (pad);

  if (!gst_element_sync_state_with_parent (source->priv->src))
  {
    GST_ERROR ("Could not sync capsfilter state with its parent");
    goto error_added;
  }

  return source;

 error_added:
  gst_element_set_state (source->priv->src, GST_STATE_NULL);
  gst_bin_remove (GST_BIN (source->priv->outer_bin), source->priv->src);
  source->priv->src = NULL;

 error:
  g_object_unref (source);

  return NULL;
}

GList *
fs_rtp_special_sources_destroy (GList *current_extra_sources)
{

  while (current_extra_sources)
  {
    FsRtpSpecialSource *self = current_extra_sources->data;

    self->priv->stopped_callback = NULL;
    g_object_unref (self);
    current_extra_sources = g_list_remove (current_extra_sources, self);
  }

  return NULL;
}

/**
 * fs_rtp_special_source_class_get_codec:
 *
 * Returns the codec that will be selected by this source if it is used
 *
 * Returns: The codec or %NULL. This returns the codec, not a copy
 */
static FsCodec*
fs_rtp_special_source_class_get_codec (FsRtpSpecialSourceClass *klass,
    GList *negotiated_codec_associations,
    FsCodec *selected_codec)
{
  if (klass->get_codec)
    return klass->get_codec (klass, negotiated_codec_associations,
        selected_codec);

  return NULL;
}

/**
 * fs_rtp_special_sources_get_codecs_locked:
 * @special_sources: The #GList of special sources
 * @codec_associations: The #GList of current codec associations
 *
 * Gets the list of the codecs that are used by special sources, excluding
 * the main codec
 *
 * Returns: a #GList of #FsCodec
 */

GList *
fs_rtp_special_sources_get_codecs_locked (GList *special_sources,
    GList *codec_associations, FsCodec *main_codec)
{
  GQueue result = G_QUEUE_INIT;

  for (; special_sources; special_sources = special_sources->next)
  {
    FsRtpSpecialSource *source = special_sources->data;

    if (fs_rtp_special_source_is_stopping (source))
      continue;

    if (main_codec->id != source->codec->id)
    {
      CodecAssociation *ca =
        lookup_codec_association_by_pt (codec_associations, source->codec->id);

      g_queue_push_tail (&result, fs_codec_copy (ca->codec));
    }
  }

  return result.head;
}

gboolean
fs_rtp_special_sources_claim_message_locked (GList *special_sources,
    GstMessage *message)
{
  GList *item;

  for (item = special_sources; item; item = item->next)
  {
    FsRtpSpecialSource *source = item->data;

    if (gst_object_has_ancestor (GST_OBJECT (GST_MESSAGE_SRC (message)),
            GST_OBJECT (source->priv->src)))
      return TRUE;
  }

  return FALSE;
}
