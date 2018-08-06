/*
 * Farstream - Farstream libnice Stream Transmitter
 *
 * Copyright 2007 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * Copyright 2007 Nokia Corp.
 *
 * fs-nice-stream-transmitter.c - A Farstream libnice stream transmitter
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
 * SECTION:fs-nice-stream-transmitter
 * @short_description: A stream transmitter object for ICE using libnice
 * @see_also: #FsRawUdpStreamTransmitter
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "fs-nice-stream-transmitter.h"
#include "fs-nice-transmitter.h"
#include "fs-nice-agent.h"

#include <farstream/fs-conference.h>

#include <gst/gst.h>

#include <string.h>
#include <sys/types.h>

#define GST_CAT_DEFAULT fs_nice_transmitter_debug

/* Signals */
enum
{
  LAST_SIGNAL
};

/* props */
enum
{
  PROP_0,
  PROP_SENDING,
  PROP_PREFERRED_LOCAL_CANDIDATES,
  PROP_STUN_IP,
  PROP_STUN_PORT,
  PROP_CONTROLLING_MODE,
  PROP_STREAM_ID,
  PROP_COMPATIBILITY_MODE,
  PROP_ASSOCIATE_ON_SOURCE,
  PROP_RELAY_INFO,
  PROP_MIN_PORT,
  PROP_MAX_PORT,
  PROP_ICE_TCP,
  PROP_ICE_UDP,
  PROP_RELIABLE,
  PROP_DEBUG,
  PROP_SEND_COMPONENT_MUX
};

struct _FsNiceStreamTransmitterPrivate
{
  FsNiceTransmitter *transmitter;

  FsNiceAgent *agent;

  guint stream_id;

  guint min_port;
  guint max_port;

  gchar *stun_ip;
  guint stun_port;

  gboolean controlling_mode;
  gboolean ice_udp;
  gboolean ice_tcp;
  gboolean reliable;
  gboolean send_component_mux;

  guint compatibility_mode;

  GMutex mutex;

  GList *preferred_local_candidates;

  gulong state_changed_handler_id;
  gulong gathering_done_handler_id;
  gulong new_selected_pair_handler_id;
  gulong new_candidate_handler_id;

  gulong tos_changed_handler_id;

  GPtrArray *relay_info;

  volatile gint associate_on_source;

  gboolean *component_has_been_ready; /* only from NiceAgent main thread */

  /* Everything below is protected by the mutex */

  gboolean sending;

  gboolean forced_candidates;
  GList *remote_candidates;
  GList *local_candidates;

  /* These are fixed and must be identical in the latest draft */
  gchar *username;
  gchar *password;

  gboolean gathered;

  NiceGstStream *gststream;
};

#define FS_NICE_STREAM_TRANSMITTER_GET_PRIVATE(o)  \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), FS_TYPE_NICE_STREAM_TRANSMITTER, \
                                FsNiceStreamTransmitterPrivate))

#define FS_NICE_STREAM_TRANSMITTER_LOCK(o)   g_mutex_lock (&(o)->priv->mutex)
#define FS_NICE_STREAM_TRANSMITTER_UNLOCK(o) g_mutex_unlock (&(o)->priv->mutex)


static void fs_nice_stream_transmitter_class_init (FsNiceStreamTransmitterClass *klass);
static void fs_nice_stream_transmitter_init (FsNiceStreamTransmitter *self);
static void fs_nice_stream_transmitter_dispose (GObject *object);
static void fs_nice_stream_transmitter_finalize (GObject *object);

static void fs_nice_stream_transmitter_get_property (GObject *object,
                                                guint prop_id,
                                                GValue *value,
                                                GParamSpec *pspec);
static void fs_nice_stream_transmitter_set_property (GObject *object,
                                                guint prop_id,
                                                const GValue *value,
                                                GParamSpec *pspec);

static gboolean fs_nice_stream_transmitter_add_remote_candidates (
    FsStreamTransmitter *streamtransmitter, GList *candidates,
    GError **error);
static gboolean fs_nice_stream_transmitter_force_remote_candidates (
    FsStreamTransmitter *streamtransmitter,
    GList *remote_candidates,
    GError **error);
static gboolean fs_nice_stream_transmitter_gather_local_candidates (
    FsStreamTransmitter *streamtransmitter,
    GError **error);
static void fs_nice_stream_transmitter_stop (
    FsStreamTransmitter *streamtransmitter);

static void agent_state_changed (NiceAgent *agent,
    guint stream_id,
    guint component_id,
    guint state,
    gpointer user_data);
static void agent_gathering_done (NiceAgent *agent, guint stream_id,
    gpointer user_data);
static void agent_new_selected_pair (NiceAgent *agent,
    guint stream_id,
    guint component_id,
    NiceCandidate *l_candidate,
    NiceCandidate *r_candidate,
    gpointer user_data);
static void agent_new_candidate (NiceAgent *agent,
    NiceCandidate *candidate,
    gpointer user_data);

static GstPadProbeReturn known_buffer_have_buffer_handler (GstPad *pad,
    GstPadProbeInfo *info,
    gpointer user_data);


static GObjectClass *parent_class = NULL;
// static guint signals[LAST_SIGNAL] = { 0 };

static GType type = 0;


GType
fs_nice_stream_transmitter_get_type (void)
{
  return type;
}

GType
fs_nice_stream_transmitter_register_type (FsPlugin *module G_GNUC_UNUSED)
{
  static const GTypeInfo info = {
    sizeof (FsNiceStreamTransmitterClass),
    NULL,
    NULL,
    (GClassInitFunc) fs_nice_stream_transmitter_class_init,
    NULL,
    NULL,
    sizeof (FsNiceStreamTransmitter),
    0,
    (GInstanceInitFunc) fs_nice_stream_transmitter_init
  };

  type = g_type_register_static (FS_TYPE_STREAM_TRANSMITTER,
      "FsNiceStreamTransmitter", &info, 0);

  return type;
}

static void
fs_nice_stream_transmitter_class_init (FsNiceStreamTransmitterClass *klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  FsStreamTransmitterClass *streamtransmitterclass =
    FS_STREAM_TRANSMITTER_CLASS (klass);

  parent_class = g_type_class_peek_parent (klass);

  gobject_class->set_property = fs_nice_stream_transmitter_set_property;
  gobject_class->get_property = fs_nice_stream_transmitter_get_property;
  gobject_class->dispose = fs_nice_stream_transmitter_dispose;
  gobject_class->finalize = fs_nice_stream_transmitter_finalize;

  streamtransmitterclass->add_remote_candidates =
    fs_nice_stream_transmitter_add_remote_candidates;
  streamtransmitterclass->force_remote_candidates =
    fs_nice_stream_transmitter_force_remote_candidates;
  streamtransmitterclass->gather_local_candidates =
    fs_nice_stream_transmitter_gather_local_candidates;
  streamtransmitterclass->stop =
    fs_nice_stream_transmitter_stop;

  g_type_class_add_private (klass, sizeof (FsNiceStreamTransmitterPrivate));

  g_object_class_override_property (gobject_class, PROP_SENDING, "sending");
  g_object_class_override_property (gobject_class,
      PROP_PREFERRED_LOCAL_CANDIDATES, "preferred-local-candidates");
  g_object_class_override_property (gobject_class, PROP_ASSOCIATE_ON_SOURCE,
      "associate-on-source");

  g_object_class_install_property (gobject_class, PROP_STUN_IP,
      g_param_spec_string (
          "stun-ip",
          "STUN server",
          "The STUN server used to obtain server-reflexive candidates",
          NULL,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_STUN_PORT,
      g_param_spec_uint (
          "stun-port",
          "STUN server port",
          "The STUN server used to obtain server-reflexive candidates",
          0, 65536,
          3478,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_CONTROLLING_MODE,
      g_param_spec_boolean (
          "controlling-mode",
          "ICE controlling mode",
          "Whether the agent is in controlling mode",
          TRUE,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ICE_UDP,
      g_param_spec_boolean (
          "ice-udp",
          "ICE UDP",
          "Whether the agent gathers UDP candidates",
          TRUE,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_ICE_TCP,
      g_param_spec_boolean (
          "ice-tcp",
          "ICE TCP",
          "Whether the agent gathers TCP candidates",
          TRUE,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_RELIABLE,
      g_param_spec_boolean (
          "reliable",
          "reliable mode",
          "Whether the agent is reliable",
          FALSE,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_STREAM_ID,
      g_param_spec_uint (
          "stream-id",
          "The id of the stream",
          "The id of the stream according to libnice",
          0, G_MAXINT,
          0,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_COMPATIBILITY_MODE,
      g_param_spec_uint (
          "compatibility-mode",
          "The compability-mode",
          "The id of the stream according to libnice",
          NICE_COMPATIBILITY_DRAFT19, NICE_COMPATIBILITY_LAST,
          NICE_COMPATIBILITY_DRAFT19,
          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  /**
   * FsNiceStreamTransmitter:relay-info:
   *
   * This is a #GPtrArray containing one or more #GstStructure.
   *
   * The fields in the structure are:
   *  <informaltable>
   *   <tr><th colspan="2">Required</th></tr>
   *   <tr>
   *     <td nowrap="nowrap">(gchar*)</td>
   *     <td nowrap="nowrap">ip</td>
   *     <td>The IP address of the TURN server</td>
   *   </tr>
   *   <tr>
   *     <td nowrap="nowrap">(guint)</td>
   *     <td nowrap="nowrap">port</td>
   *     <td>The port of the TURN server</td>
   *   </tr>
   *   <tr>
   *     <td nowrap="nowrap">(gchar*)</td>
   *     <td nowrap="nowrap">username</td>
   *   </tr>
   *   <tr>
   *     <td nowrap="nowrap">(gchar*)</td>
   *     <td nowrap="nowrap">password</td>
   *   </tr>
   *   <tr><th colspan="2">Optional</th></tr>
   *   <tr>
   *    <td nowrap="nowrap">(gchar *)</td>
   *    <td nowrap="nowrap">relay-type</td>
   *    <td>The type of TURN server, can use "udp", "tcp" or "tls".
   *        Defaults to "udp" if not specified.</td>
   *   </tr>
   *   <tr>
   *    <td nowrap="nowrap">(guint)</td>
   *    <td nowrap="nowrap">component</td>
   *    <td>The component this TURN server and creditials will be used for.
   *    If no component is specified, it will be used for all components where
   *    no per-component details were specified.
   *    This is useful if you want to specify different short term creditial
   *    username/password combinations for Google and MSN compatibility modes.
   *    </td>
   *   </tr>
   *  </informaltable>
   *
   * Example:
   * |[
   GPtrArray *relay_info = g_ptr_array_new_full (1, (GDestroyNotify) gst_structure_free);
   g_ptr_array_add (relay_info,
      gst_structure_new ("aa",
          "ip", G_TYPE_STRING, "127.0.0.1",
          "port", G_TYPE_UINT, 7654,
          "username", G_TYPE_STRING, "blah",
          "password", G_TYPE_STRING, "blah2",
          "relay-type", G_TYPE_STRING, "udp",
          NULL));
   |]
   *
   */

  g_object_class_install_property (gobject_class, PROP_RELAY_INFO,
      g_param_spec_boxed (
          "relay-info",
          "Information for the TURN server",
          "ip/port/username/password/relay-type/component of the TURN servers"
          " in a GPtrArray of GstStructures",
          G_TYPE_PTR_ARRAY,
          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DEBUG,
      g_param_spec_boolean (
          "debug",
          "Enable debug messages",
          "Whether the agent should enable libnice and stun debug messages",
          FALSE,
          G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MIN_PORT,
      g_param_spec_uint (
          "min-port",
          "Minimal listen port",
          "Minimal port number for allocating host candidates."
          " 0 means use any port",
          0, 65535,
          0,
          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_MAX_PORT,
      g_param_spec_uint (
          "max-port",
          "Maximal listen port",
          "Maximal port number for allocating host candidates."
          " It should apply that min-port < max-port; otherwise, any port is"
          " used, just as when the value is 0",
          0, 65535,
          0,
          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_SEND_COMPONENT_MUX,
      g_param_spec_boolean (
          "send-component-mux",
          "Send component mux",
          "Whether to mux all components on the same component as component 1",
          FALSE,
          G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

}

static void
fs_nice_stream_transmitter_init (FsNiceStreamTransmitter *self)
{
  /* member init */
  self->priv = FS_NICE_STREAM_TRANSMITTER_GET_PRIVATE (self);

  self->priv->sending = TRUE;
  g_mutex_init (&self->priv->mutex);

  self->priv->controlling_mode = TRUE;
  self->priv->ice_udp = TRUE;
  self->priv->ice_tcp = TRUE;
  self->priv->reliable = TRUE;
}

static void
fs_nice_stream_transmitter_dispose (GObject *object)
{
  FsNiceStreamTransmitter *self = FS_NICE_STREAM_TRANSMITTER (object);

  fs_nice_stream_transmitter_stop (FS_STREAM_TRANSMITTER_CAST (object));

  FS_NICE_STREAM_TRANSMITTER_LOCK (self);
  if (self->priv->state_changed_handler_id)
    g_signal_handler_disconnect (self->priv->agent->agent,
        self->priv->state_changed_handler_id);
  self->priv->state_changed_handler_id = 0;

  if (self->priv->gathering_done_handler_id)
    g_signal_handler_disconnect (self->priv->agent->agent,
        self->priv->gathering_done_handler_id);
  self->priv->gathering_done_handler_id = 0;

  if (self->priv->new_selected_pair_handler_id)
    g_signal_handler_disconnect (self->priv->agent->agent,
        self->priv->new_selected_pair_handler_id);
  self->priv->new_selected_pair_handler_id = 0;

  if (self->priv->new_candidate_handler_id)
    g_signal_handler_disconnect (self->priv->agent->agent,
        self->priv->new_candidate_handler_id);
  self->priv->new_candidate_handler_id = 0;

  if (self->priv->tos_changed_handler_id)
    g_signal_handler_disconnect (self->priv->transmitter,
        self->priv->tos_changed_handler_id);
  self->priv->tos_changed_handler_id = 0;

  if (self->priv->agent)
  {
    g_object_unref (self->priv->agent);
    self->priv->agent = NULL;
  }
  FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);

  if (self->priv->transmitter)
  {
    g_object_unref (self->priv->transmitter);
    self->priv->transmitter = NULL;
  }

  parent_class->dispose (object);
}

static void
fs_nice_stream_transmitter_stop (FsStreamTransmitter *streamtransmitter)
{
  FsNiceStreamTransmitter *self =
    FS_NICE_STREAM_TRANSMITTER (streamtransmitter);
  NiceGstStream *gststream;
  guint stream_id;


  FS_NICE_STREAM_TRANSMITTER_LOCK (self);
  gststream = self->priv->gststream;
  self->priv->gststream = NULL;
  stream_id = self->priv->stream_id;
  /* We can't unset the stream id because it gets messy fast, just leave it as
   * is, all calls should fail anyway
   */
  FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);

  if (gststream)
    fs_nice_transmitter_free_gst_stream (self->priv->transmitter, gststream);
  if (stream_id)
    nice_agent_remove_stream (self->priv->agent->agent, stream_id);
}


static void
fs_nice_stream_transmitter_finalize (GObject *object)
{
  FsNiceStreamTransmitter *self = FS_NICE_STREAM_TRANSMITTER (object);

  fs_candidate_list_destroy (self->priv->preferred_local_candidates);

  fs_candidate_list_destroy (self->priv->remote_candidates);
  fs_candidate_list_destroy (self->priv->local_candidates);

  if (self->priv->relay_info)
    g_ptr_array_unref (self->priv->relay_info);

  g_free (self->priv->stun_ip);

  g_mutex_clear (&self->priv->mutex);

  g_free (self->priv->username);
  g_free (self->priv->password);

  g_free (self->priv->component_has_been_ready);

  parent_class->finalize (object);
}

static void
fs_nice_stream_transmitter_get_property (GObject *object,
                                           guint prop_id,
                                           GValue *value,
                                           GParamSpec *pspec)
{
  FsNiceStreamTransmitter *self = FS_NICE_STREAM_TRANSMITTER (object);

  switch (prop_id)
  {
    case PROP_SENDING:
      FS_NICE_STREAM_TRANSMITTER_LOCK (self);
      g_value_set_boolean (value, self->priv->sending);
      FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
      break;
    case PROP_PREFERRED_LOCAL_CANDIDATES:
      g_value_set_boxed (value, self->priv->preferred_local_candidates);
      break;
    case PROP_STUN_IP:
      if (self->priv->agent)
        g_object_get_property (G_OBJECT (self->priv->agent->agent),
            g_param_spec_get_name (pspec), value);
      else
        g_value_set_string (value, self->priv->stun_ip);
      break;
    case PROP_STUN_PORT:
      if (self->priv->agent)
        g_object_get_property (G_OBJECT (self->priv->agent->agent),
            g_param_spec_get_name (pspec), value);
      else
        g_value_set_uint (value, self->priv->stun_port);
      break;
    case PROP_CONTROLLING_MODE:
      if (self->priv->agent)
        g_object_get_property (G_OBJECT (self->priv->agent->agent),
            g_param_spec_get_name (pspec), value);
      else
        g_value_set_boolean (value, self->priv->controlling_mode);
      break;
    case PROP_ICE_UDP:
      if (self->priv->agent)
        g_object_get_property (G_OBJECT (self->priv->agent->agent),
            g_param_spec_get_name (pspec), value);
      else
        g_value_set_boolean (value, self->priv->ice_udp);
      break;
    case PROP_ICE_TCP:
      if (self->priv->agent)
        g_object_get_property (G_OBJECT (self->priv->agent->agent),
            g_param_spec_get_name (pspec), value);
      else
        g_value_set_boolean (value, self->priv->ice_tcp);
      break;
    case PROP_RELIABLE:
      if (self->priv->agent)
        g_object_get_property (G_OBJECT (self->priv->agent->agent),
            g_param_spec_get_name (pspec), value);
      else
        g_value_set_boolean (value, self->priv->reliable);
      break;
    case PROP_STREAM_ID:
      FS_NICE_STREAM_TRANSMITTER_LOCK (self);
      g_value_set_uint (value, self->priv->stream_id);
      FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
      break;
    case PROP_COMPATIBILITY_MODE:
      g_value_set_uint (value, self->priv->compatibility_mode);
      break;
    case PROP_ASSOCIATE_ON_SOURCE:
      g_value_set_boolean (value,
          g_atomic_int_get (&self->priv->associate_on_source));
      break;
    case PROP_SEND_COMPONENT_MUX:
      g_value_set_boolean (value, self->priv->send_component_mux);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
fs_nice_stream_transmitter_set_property (GObject *object,
                                           guint prop_id,
                                           const GValue *value,
                                           GParamSpec *pspec)
{
  FsNiceStreamTransmitter *self = FS_NICE_STREAM_TRANSMITTER (object);

  switch (prop_id)
  {
    case PROP_SENDING:
      FS_NICE_STREAM_TRANSMITTER_LOCK (self);
      self->priv->sending = g_value_get_boolean (value);
      if (self->priv->gststream)
        fs_nice_transmitter_set_sending (self->priv->transmitter,
            self->priv->gststream, g_value_get_boolean (value));
      FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
      break;
    case PROP_PREFERRED_LOCAL_CANDIDATES:
      self->priv->preferred_local_candidates = g_value_dup_boxed (value);
      break;
    case PROP_STUN_IP:
      self->priv->stun_ip = g_value_dup_string (value);
      break;
    case PROP_STUN_PORT:
      self->priv->stun_port = g_value_get_uint (value);
      break;
    case PROP_CONTROLLING_MODE:
      self->priv->controlling_mode = g_value_get_boolean (value);
      if (self->priv->transmitter && self->priv->agent)
        g_object_set_property (G_OBJECT (self->priv->agent->agent),
            g_param_spec_get_name (pspec), value);
      break;
    case PROP_ICE_UDP:
      self->priv->ice_udp = g_value_get_boolean (value);
      if (self->priv->transmitter && self->priv->agent)
        g_object_set_property (G_OBJECT (self->priv->agent->agent),
            g_param_spec_get_name (pspec), value);
      break;
    case PROP_ICE_TCP:
      self->priv->ice_tcp = g_value_get_boolean (value);
      if (self->priv->transmitter && self->priv->agent)
        g_object_set_property (G_OBJECT (self->priv->agent->agent),
            g_param_spec_get_name (pspec), value);
      break;
    case PROP_RELIABLE:
      self->priv->reliable = g_value_get_boolean (value);
      break;
    case PROP_COMPATIBILITY_MODE:
      self->priv->compatibility_mode = g_value_get_uint (value);
      break;
    case PROP_ASSOCIATE_ON_SOURCE:
      g_atomic_int_set (&self->priv->associate_on_source,
          g_value_get_boolean (value));
      break;
    case PROP_RELAY_INFO:
      self->priv->relay_info = g_value_dup_boxed (value);
      break;
    case PROP_MIN_PORT:
      self->priv->min_port = g_value_get_uint (value);
      break;
    case PROP_MAX_PORT:
      self->priv->max_port = g_value_get_uint (value);
      break;
    case PROP_DEBUG:
      if (g_value_get_boolean (value)) {
        nice_debug_enable (TRUE);
      } else {
        nice_debug_disable (TRUE);
      }
      break;
    case PROP_SEND_COMPONENT_MUX:
      self->priv->send_component_mux = g_value_get_boolean (value);
      if (self->priv->gststream != NULL)
        fs_nice_transmitter_set_send_component_mux (self->priv->transmitter,
            self->priv->gststream, self->priv->send_component_mux);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}


static NiceCandidateType
fs_candidate_type_to_nice_candidate_type (FsCandidateType type)
{
  switch (type)
  {
    case FS_CANDIDATE_TYPE_HOST:
      return NICE_CANDIDATE_TYPE_HOST;
    case FS_CANDIDATE_TYPE_SRFLX:
      return NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE;
    case FS_CANDIDATE_TYPE_PRFLX:
      return NICE_CANDIDATE_TYPE_PEER_REFLEXIVE;
    case FS_CANDIDATE_TYPE_RELAY:
      return NICE_CANDIDATE_TYPE_RELAYED;
    default:
      GST_WARNING ("Invalid candidate type %d, defaulting to type host", type);
      return NICE_CANDIDATE_TYPE_HOST;
  }
}

static NiceCandidateTransport
fs_network_protocol_to_nice_candidate_protocol (FsNetworkProtocol proto)
{
  switch (proto)
  {
    case FS_NETWORK_PROTOCOL_UDP:
      return NICE_CANDIDATE_TRANSPORT_UDP;
    case FS_NETWORK_PROTOCOL_TCP_ACTIVE:
      return NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE;
    case FS_NETWORK_PROTOCOL_TCP_PASSIVE:
      return NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE;
    case FS_NETWORK_PROTOCOL_TCP_SO:
      return NICE_CANDIDATE_TRANSPORT_TCP_SO;
    default:
      GST_WARNING ("Invalid Fs network protocol type %u", proto);
      return NICE_CANDIDATE_TRANSPORT_UDP;
  }
}

static NiceCandidate *
fs_candidate_to_nice_candidate (FsNiceStreamTransmitter *self,
    FsCandidate *candidate)
{
  NiceCandidate *nc = nice_candidate_new (
      fs_candidate_type_to_nice_candidate_type (candidate->type));

  nc->transport =
    fs_network_protocol_to_nice_candidate_protocol (candidate->proto);
  nc->priority = candidate->priority;
  nc->stream_id = self->priv->stream_id;
  nc->component_id = candidate->component_id;
  if (candidate->foundation != NULL)
    strncpy (nc->foundation, candidate->foundation,
       NICE_CANDIDATE_MAX_FOUNDATION - 1);

  nc->username = g_strdup(candidate->username);
  nc->password = g_strdup(candidate->password);


  if (candidate->ip == NULL)
    goto error;
  if (!nice_address_set_from_string (&nc->addr, candidate->ip))
    goto error;
  nice_address_set_port (&nc->addr, candidate->port);

  if (candidate->base_ip && candidate->base_port)
  {
    if (!nice_address_set_from_string (&nc->base_addr, candidate->base_ip))
      goto error;
    nice_address_set_port (&nc->base_addr, candidate->base_port);
  }

  return nc;

 error:
  nice_candidate_free (nc);
  return NULL;
}


static gboolean
fs_nice_stream_transmitter_add_remote_candidates (
    FsStreamTransmitter *streamtransmitter,
    GList *candidates,
    GError **error)
{
  FsNiceStreamTransmitter *self =
    FS_NICE_STREAM_TRANSMITTER (streamtransmitter);
  GList  *item;
  GSList *nice_candidates = NULL;
  gint c;
  const gchar *username;
  const gchar *password;

  if (!candidates)
  {
    GST_DEBUG ("NULL candidates passed, lets do an ICE restart");
    FS_NICE_STREAM_TRANSMITTER_LOCK (self);
    if (self->priv->remote_candidates)
      fs_candidate_list_destroy (self->priv->remote_candidates);
    self->priv->remote_candidates = NULL;
    self->priv->forced_candidates = FALSE;
    g_free (self->priv->username);
    g_free (self->priv->password);
    self->priv->username = NULL;
    self->priv->password = NULL;
    FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
    nice_agent_restart (self->priv->agent->agent);
    return TRUE;
  }

  FS_NICE_STREAM_TRANSMITTER_LOCK (self);

  username = self->priv->username;
  password = self->priv->password;

  /* Validate candidates */
  for (item = candidates;
       item;
       item = g_list_next (item))
  {
    FsCandidate *candidate = item->data;

    if (!candidate->ip)
    {
      FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "Candidate MUST have an IP address");
      return FALSE;
    }

    if (candidate->component_id == 0 ||
        candidate->component_id > self->priv->transmitter->components)
    {
      FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "Candidate MUST have a component id between 1 and %d, %d is invalid",
          self->priv->transmitter->components, candidate->component_id);
      return FALSE;
    }

    if (candidate->type == FS_CANDIDATE_TYPE_MULTICAST)
    {
      FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "libnice transmitter does not accept multicast candidates");
      return FALSE;
    }

    if (!candidate->username)
    {
      FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "Invalid remote candidates passed, does not have a username");
      return FALSE;
    }

    if (self->priv->compatibility_mode != NICE_COMPATIBILITY_GOOGLE &&
        !candidate->password)
    {
      FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "Invalid remote candidates passed, does not have a password");
      return FALSE;
    }

    if (self->priv->compatibility_mode != NICE_COMPATIBILITY_GOOGLE &&
        self->priv->compatibility_mode != NICE_COMPATIBILITY_MSN &&
        self->priv->compatibility_mode != NICE_COMPATIBILITY_OC2007)
    {
      if (!username)
      {
        username = candidate->username;
      }
      else if (strcmp (username, candidate->username))
      {
        FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
        g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
            "Invalid remote candidates passed, does not have the right"
            " username");
        return FALSE;
      }

      if (!password)
      {
        password = candidate->password;
      }
      else if (strcmp (password, candidate->password))
      {
        FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
        g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
            "Invalid remote candidates passed, does not have the right"
            " password");
        return FALSE;
      }
    }
  }

  if (!self->priv->username)
    self->priv->username = g_strdup (username);
  if (!self->priv->password)
    self->priv->password = g_strdup (password);

  if (self->priv->forced_candidates)
  {
    FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
    g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
        "Candidates have been forced, can't set remote candidates");
    return FALSE;
  }

  if (!self->priv->gathered)
  {
    self->priv->remote_candidates = g_list_concat (
        self->priv->remote_candidates,
        fs_candidate_list_copy (candidates));
    FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
    return TRUE;
  }

  if (self->priv->compatibility_mode != NICE_COMPATIBILITY_GOOGLE &&
      self->priv->compatibility_mode != NICE_COMPATIBILITY_MSN &&
      self->priv->compatibility_mode != NICE_COMPATIBILITY_OC2007)
  {
    username = g_strdup (username);
    password = g_strdup (password);
    FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);

    if (!nice_agent_set_remote_credentials (self->priv->agent->agent,
            self->priv->stream_id, username, password))
    {
      g_free ((gchar*) username);
      g_free ((gchar*) password);
      g_set_error (error, FS_ERROR, FS_ERROR_INTERNAL,
          "Could not set the security credentials");
      return FALSE;
    }
    g_free ((gchar*) username);
    g_free ((gchar*) password);
  }
  else
  {
    FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
  }

  for (c = 1; c <= self->priv->transmitter->components; c++)
  {
    for (item = candidates;
         item;
         item = g_list_next (item))
    {
      FsCandidate *candidate = item->data;

      if (candidate->component_id == c)
      {
        NiceCandidate *nc = fs_candidate_to_nice_candidate (self, candidate);

        if (!nc)
          goto error;

        nice_candidates = g_slist_append (nice_candidates, nc);
      }
    }

    nice_agent_set_remote_candidates (self->priv->agent->agent,
        self->priv->stream_id, c, nice_candidates);

    g_slist_foreach (nice_candidates, (GFunc)nice_candidate_free, NULL);
    g_slist_free (nice_candidates);
    nice_candidates = NULL;
  }

  return TRUE;
 error:

  g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
      "Invalid remote candidates passed");
  g_slist_foreach (nice_candidates, (GFunc) nice_candidate_free, NULL);
  g_slist_free (nice_candidates);

  return FALSE;
}

static gboolean
fs_nice_stream_transmitter_force_remote_candidates_act (
    FsNiceStreamTransmitter *self,
    GList *remote_candidates)
{
  gboolean res = TRUE;
  GList *item = NULL;

  for (item = remote_candidates;
       item && res;
       item = g_list_next (item))
  {
    FsCandidate *candidate = item->data;
    NiceCandidate *nc = fs_candidate_to_nice_candidate (self, candidate);

    res &= nice_agent_set_selected_remote_candidate (self->priv->agent->agent,
        self->priv->stream_id, candidate->component_id, nc);
    nice_candidate_free (nc);
  }

  return res;
}

static gboolean
fs_nice_stream_transmitter_force_remote_candidates (
    FsStreamTransmitter *streamtransmitter,
    GList *remote_candidates,
    GError **error)
{
  FsNiceStreamTransmitter *self =
    FS_NICE_STREAM_TRANSMITTER (streamtransmitter);
  GList *item = NULL;
  gboolean res = TRUE;
  gboolean *done;

  done = g_new0(gboolean, self->priv->transmitter->components);

  memset (done, 0, self->priv->transmitter->components * sizeof (gboolean));

  if (self->priv->stream_id == 0)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
        "Can not call this function before gathering local candidates");
    res = FALSE;
    goto out;
  }

  /* First lets check that we have valid candidates */

  for (item = remote_candidates; item; item = g_list_next (item))
  {
    FsCandidate *candidate = item->data;

    if (candidate->component_id < 1 ||
        candidate->component_id > self->priv->transmitter->components)
    {
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "The component on this candidate is wrong");
      res = FALSE;
      goto out;
    }

    if (done[candidate->component_id-1])
    {
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "You can set only one candidate per component");
      res = FALSE;
      goto out;
    }
    done[candidate->component_id-1] = TRUE;
  }

  FS_NICE_STREAM_TRANSMITTER_LOCK (self);
  self->priv->forced_candidates = TRUE;
  if (self->priv->gathered)
  {
    FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
    res = fs_nice_stream_transmitter_force_remote_candidates_act (self,
        remote_candidates);
  }
  else
  {
    if (self->priv->remote_candidates)
      fs_candidate_list_destroy (self->priv->remote_candidates);
    self->priv->remote_candidates = fs_candidate_list_copy (remote_candidates);
    FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
  }

  if (!res)
    g_set_error (error, FS_ERROR, FS_ERROR_INTERNAL,
        "Unknown error while selecting remote candidates");

out:
  g_free(done);
  return res;
}

static FsCandidateType
nice_candidate_type_to_fs_candidate_type (NiceCandidateType type)
{
  switch (type)
  {
    case NICE_CANDIDATE_TYPE_HOST:
      return FS_CANDIDATE_TYPE_HOST;
    case NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE:
      return FS_CANDIDATE_TYPE_SRFLX;
    case NICE_CANDIDATE_TYPE_PEER_REFLEXIVE:
      return FS_CANDIDATE_TYPE_PRFLX;
    case NICE_CANDIDATE_TYPE_RELAYED:
      return FS_CANDIDATE_TYPE_RELAY;
    default:
      GST_WARNING ("Invalid candidate type %d, defaulting to type host", type);
      return FS_CANDIDATE_TYPE_HOST;
  }
}

static FsNetworkProtocol
nice_candidate_transport_to_fs_network_protocol (NiceCandidateTransport trans)
{
  switch (trans)
  {
    case NICE_CANDIDATE_TRANSPORT_UDP:
      return FS_NETWORK_PROTOCOL_UDP;
    case NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE:
      return FS_NETWORK_PROTOCOL_TCP_PASSIVE;
    case NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE:
      return FS_NETWORK_PROTOCOL_TCP_ACTIVE;
    case NICE_CANDIDATE_TRANSPORT_TCP_SO:
      return FS_NETWORK_PROTOCOL_TCP_SO;
    default:
      GST_WARNING ("Invalid Nice network transport type %u", trans);
      return FS_NETWORK_PROTOCOL_UDP;
  }
}

static FsCandidate *
nice_candidate_to_fs_candidate (NiceAgent *agent, NiceCandidate *nicecandidate,
    gboolean local)
{
  FsCandidate *fscandidate;
  gchar *ipaddr = g_malloc (INET6_ADDRSTRLEN);

  nice_address_to_string (&nicecandidate->addr, ipaddr);

  fscandidate = fs_candidate_new (
      nicecandidate->foundation,
      nicecandidate->component_id,
      nice_candidate_type_to_fs_candidate_type (nicecandidate->type),
      nice_candidate_transport_to_fs_network_protocol (
          nicecandidate->transport),
      ipaddr,
      nice_address_get_port (&nicecandidate->addr));

  if (nice_address_is_valid (&nicecandidate->base_addr) &&
      nicecandidate->type != NICE_CANDIDATE_TYPE_HOST)
  {
    nice_address_to_string (&nicecandidate->base_addr, ipaddr);
    fscandidate->base_ip = ipaddr;
    fscandidate->base_port = nice_address_get_port (&nicecandidate->base_addr);
  }
  else
  {
    g_free (ipaddr);
    ipaddr = NULL;
  }

  fscandidate->username = g_strdup (nicecandidate->username);
  fscandidate->password = g_strdup (nicecandidate->password);
  fscandidate->priority = nicecandidate->priority;

  if (local && fscandidate->username == NULL && fscandidate->password == NULL)
  {
    gchar *username = NULL, *password = NULL;
    nice_agent_get_local_credentials (agent, nicecandidate->stream_id,
        &username, &password);
    fscandidate->username = username;
    fscandidate->password = password;

    if (username == NULL || password == NULL)
    {
      GST_WARNING ("The stream has no credentials??");
    }
  }



  return fscandidate;
}


static gboolean
candidate_list_are_equal (GList *list1, GList *list2)
{
  for (;
       list1 && list2;
       list1 = list1->next, list2 = list2->next)
  {
    FsCandidate *cand1 = list1->data;
    FsCandidate *cand2 = list2->data;

    if (strcmp (cand1->ip, cand2->ip))
        return FALSE;
  }

  return TRUE;
}

static void
weak_agent_removed (gpointer user_data, GObject *where_the_object_was)
{
  GList *agents = NULL;
  FsParticipant *participant = user_data;

  FS_PARTICIPANT_DATA_LOCK (participant);

  agents = g_object_get_data (G_OBJECT (participant), "nice-agents");
  agents = g_list_remove (agents, where_the_object_was);
  g_object_set_data (G_OBJECT (participant), "nice-agents", agents);

  FS_PARTICIPANT_DATA_UNLOCK (participant);

  g_object_unref (participant);
}

static gboolean
fs_nice_stream_transmitter_set_relay_info (FsNiceStreamTransmitter *self,
    const GstStructure *s, guint component_id, GError **error)
{
  const gchar *username, *password, *ip;
  const gchar *relay_type_string;
  NiceRelayType relay_type = NICE_RELAY_TYPE_TURN_UDP;
  guint port;
  gboolean has_port;

  ip = gst_structure_get_string (s, "ip");
  has_port = gst_structure_get_uint (s, "port",  &port);
  username = gst_structure_get_string (s, "username");
  password = gst_structure_get_string (s, "password");
  relay_type_string = gst_structure_get_string (s, "relay-type");

  if (!ip || !has_port || !username || !password || port > 65535)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
        "Need to pass an ip, port, username and password for a relay");
    return FALSE;
  }

  if (relay_type_string)
  {
    if (!g_ascii_strcasecmp(relay_type_string, "tcp"))
      relay_type = NICE_RELAY_TYPE_TURN_TCP;
    else if (!g_ascii_strcasecmp(relay_type_string, "tls"))
      relay_type = NICE_RELAY_TYPE_TURN_TLS;
  }

  nice_agent_set_relay_info(self->priv->agent->agent,
      self->priv->stream_id, component_id, ip, port, username, password,
      relay_type);

  return TRUE;
}



static void
tos_changed (GObject *transmitter, GParamSpec *param,
    FsNiceStreamTransmitter *self)
{
  guint tos;

  g_object_get (transmitter, "tos", &tos, NULL);
  nice_agent_set_stream_tos (self->priv->agent->agent, self->priv->stream_id,
      tos);
}

static gboolean
fs_nice_stream_transmitter_build (FsNiceStreamTransmitter *self,
    FsParticipant *participant,
    GError **error)
{
  GList *item;
  GList *agents  = NULL;
  FsNiceAgent *agent = NULL;
  gint i;

  /* Before going any further, check that the list of candidates are ok */

  for (item = g_list_first (self->priv->preferred_local_candidates);
       item;
       item = g_list_next (item))
  {
    FsCandidate *cand = item->data;

    if (cand->ip == NULL)
    {
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "You have to set an ip on your preferred candidate");
      return FALSE;
    }

    if (cand->port || cand->component_id)
    {
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "You can not set a port or component id"
          " for the preferred nice candidate");
      return FALSE;
    }

    if (cand->type != FS_CANDIDATE_TYPE_HOST)
    {
      g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
          "You can only set preferred candidates of type host");
      return FALSE;
    }
  }

  /* Now if we have a relayinfo, lets verify that its ok */

  if (self->priv->relay_info)
  {
    for (i = 0; i < self->priv->relay_info->len; i++)
    {
      const GstStructure *s = g_ptr_array_index (self->priv->relay_info, i);

      if (!s)
      {
        g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
            "Element %d of the relay-info GPtrArray is NULL",
            i);
        return FALSE;
      }

      if (!gst_structure_has_field_typed (s, "ip", G_TYPE_STRING))
      {
        g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
            "Element %d of the relay-info does not have an ip as a string", i);
        return FALSE;
      }

      if (!gst_structure_has_field_typed (s, "port", G_TYPE_UINT))
      {
        g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
            "Element %d of the relay-info does not have a port as a guint", i);
        return FALSE;
      }

      if (gst_structure_has_field (s, "username") &&
          !gst_structure_has_field_typed (s, "username", G_TYPE_STRING))
      {
        g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
            "Element %d of the relay-info has a username that is not a string",
            i);
        return FALSE;
      }

      if (gst_structure_has_field (s, "password") &&
          !gst_structure_has_field_typed (s, "password", G_TYPE_STRING))
      {
        g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
            "Element %d of the relay-info has a password that is not a string",
            i);
        return FALSE;
      }

      if (gst_structure_has_field (s, "relay-type") &&
          !gst_structure_has_field_typed (s, "relay-type",
              G_TYPE_STRING))
      {
        g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
            "Element %d of the relay-info a relay-type"
            " that is not a string", i);
        return FALSE;
      }

      if (gst_structure_has_field (s, "component") &&
          !gst_structure_has_field_typed (s, "component",
              G_TYPE_UINT))
      {
        g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
            "Element %d of the relay-info has a component that is not a uint",
            i);
        return FALSE;
      }
    }
  }


  /* First find if there is already a matching agent */

  FS_PARTICIPANT_DATA_LOCK (participant);

  agents = g_object_get_data (G_OBJECT (participant), "nice-agents");

  for (item = g_list_first (agents);
       item;
       item = g_list_next (item))
  {
    guint stun_port;
    gchar *stun_server;
    guint compatibility;

    agent = item->data;

    g_object_get (agent->agent,
        "stun-server", &stun_server,
        "stun-server-port", &stun_port,
        "compatibility", &compatibility,
        NULL);

    /*
     * Check if the agent matches our requested criteria
     */
    if (compatibility == self->priv->compatibility_mode &&
        stun_port == self->priv->stun_port &&
        (stun_server == self->priv->stun_ip ||
            (stun_server && self->priv->stun_ip &&
                !strcmp (stun_server, self->priv->stun_ip))))
    {
      GList *prefs = NULL;

      g_object_get (G_OBJECT (agent),
          "preferred-local-candidates", &prefs,
          NULL);

      if (candidate_list_are_equal (prefs,
              self->priv->preferred_local_candidates))
      {
        fs_candidate_list_destroy (prefs);
        g_free (stun_server);
        break;
      }
      fs_candidate_list_destroy (prefs);
    }
    g_free (stun_server);
  }


  /* In this case we need to build a new agent */
  if (item == NULL)
  {
    agent = fs_nice_agent_new (self->priv->compatibility_mode,
        self->priv->preferred_local_candidates, self->priv->reliable,
        error);

    if (!agent)
        return FALSE;

    if (self->priv->stun_ip && self->priv->stun_port)
      g_object_set (agent->agent,
          "stun-server", self->priv->stun_ip,
          "stun-server-port", self->priv->stun_port,
          NULL);

    g_object_set (agent->agent,
        "controlling-mode", self->priv->controlling_mode,
        "ice-udp", self->priv->ice_udp,
        "ice-tcp", self->priv->ice_tcp,
        NULL);

    agents = g_list_prepend (agents, agent);
    g_object_set_data (G_OBJECT (participant), "nice-agents", agents);
    g_object_weak_ref (G_OBJECT (agent), weak_agent_removed, participant);
    g_object_ref (participant);

    self->priv->agent = agent;
  } else {
    self->priv->agent = g_object_ref (agent);
  }

  FS_PARTICIPANT_DATA_UNLOCK (participant);

  self->priv->component_has_been_ready = g_new0 (gboolean,
      self->priv->transmitter->components);

  self->priv->stream_id = nice_agent_add_stream (
      self->priv->agent->agent,
      self->priv->transmitter->components);

  if (self->priv->stream_id == 0)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
        "Could not create libnice stream");
    return FALSE;
  }

  /* if we have a relay- info, lets set it */
  if (self->priv->relay_info)
  {
    gint c;
    for (c = 1; c <= self->priv->transmitter->components; c++)
    {
      gboolean relay_info_set = FALSE;

      for (i = 0; i < self->priv->relay_info->len; i++)
      {
        const GstStructure *s = g_ptr_array_index (self->priv->relay_info, i);
        guint component_id;

        if (gst_structure_get_uint (s, "component", &component_id) &&
            component_id == c)
        {
          if (!fs_nice_stream_transmitter_set_relay_info (self, s, c, error))
            return FALSE;
          relay_info_set = TRUE;
        }
      }

      if (!relay_info_set)
      {
        for (i = 0; i < self->priv->relay_info->len; i++)
        {
          const GstStructure *s = g_ptr_array_index (self->priv->relay_info, i);

          if (!gst_structure_has_field (s, "component"))
            if (!fs_nice_stream_transmitter_set_relay_info (self, s, c, error))
              return FALSE;
        }
      }
    }
  }

  /* Set a port range if it has been specified. */
  if (self->priv->min_port && (self->priv->min_port < self->priv->max_port))
  {
    gint c;
    for (c = 1; c <= self->priv->transmitter->components; c++)
    {
      nice_agent_set_port_range (self->priv->agent->agent,
          self->priv->stream_id, c, self->priv->min_port, self->priv->max_port);
    }
  }

  self->priv->state_changed_handler_id = g_signal_connect_object (agent->agent,
      "component-state-changed", G_CALLBACK (agent_state_changed), self, 0);
  self->priv->gathering_done_handler_id = g_signal_connect_object (agent->agent,
      "candidate-gathering-done", G_CALLBACK (agent_gathering_done), self, 0);
  self->priv->new_selected_pair_handler_id = g_signal_connect_object (
      agent->agent, "new-selected-pair-full",
      G_CALLBACK (agent_new_selected_pair),
      self, 0);
  self->priv->new_candidate_handler_id = g_signal_connect_object (agent->agent,
      "new-candidate-full", G_CALLBACK (agent_new_candidate), self, 0);
  self->priv->tos_changed_handler_id = g_signal_connect_object (
      self->priv->transmitter, "notify::tos", G_CALLBACK (tos_changed), self,
      0);

  tos_changed (G_OBJECT (self->priv->transmitter), NULL, self);

  self->priv->gststream = fs_nice_transmitter_add_gst_stream (
      self->priv->transmitter,
      self->priv->agent->agent,
      self->priv->stream_id,
      known_buffer_have_buffer_handler, self,
      error);
  if (self->priv->gststream == NULL)
    return FALSE;

  fs_nice_transmitter_set_send_component_mux (self->priv->transmitter,
      self->priv->gststream, self->priv->send_component_mux);

  GST_DEBUG ("Created a stream with %u components",
      self->priv->transmitter->components);

  return TRUE;
}

static gboolean
fs_nice_stream_transmitter_gather_local_candidates (
    FsStreamTransmitter *streamtransmitter,
    GError **error)
{
  FsNiceStreamTransmitter *self =
    FS_NICE_STREAM_TRANSMITTER (streamtransmitter);

  GST_DEBUG ("Stream %u started", self->priv->stream_id);

  nice_agent_gather_candidates (self->priv->agent->agent,
      self->priv->stream_id);

  return TRUE;
}

static FsStreamState
nice_component_state_to_fs_stream_state (NiceComponentState state)
{
  switch (state)
  {
    case NICE_COMPONENT_STATE_DISCONNECTED:
      return FS_STREAM_STATE_DISCONNECTED;
    case NICE_COMPONENT_STATE_GATHERING:
      return FS_STREAM_STATE_GATHERING;
    case NICE_COMPONENT_STATE_CONNECTING:
      return FS_STREAM_STATE_CONNECTING;
    case NICE_COMPONENT_STATE_CONNECTED:
      return FS_STREAM_STATE_CONNECTED;
    case NICE_COMPONENT_STATE_READY:
      return FS_STREAM_STATE_READY;
    case NICE_COMPONENT_STATE_FAILED:
      return FS_STREAM_STATE_FAILED;
    default:
      GST_ERROR ("Invalid state %u", state);
      return FS_STREAM_STATE_FAILED;
  }
}

struct state_changed_signal_data
{
  FsNiceStreamTransmitter *self;
  guint component_id;
  FsStreamState fs_state;
};

static void
free_state_changed_signal_data (gpointer user_data)
{
  struct state_changed_signal_data *data = user_data;
  g_object_unref (data->self);
  g_slice_free (struct state_changed_signal_data, data);
}

static gboolean
state_changed_signal_idle (gpointer userdata)
{
  struct state_changed_signal_data *data = userdata;

  g_signal_emit_by_name (data->self, "state-changed", data->component_id,
      data->fs_state);
  return FALSE;
}

static void
agent_state_changed (NiceAgent *agent,
    guint stream_id,
    guint component_id,
    guint state,
    gpointer user_data)
{
  FsNiceStreamTransmitter *self = FS_NICE_STREAM_TRANSMITTER (user_data);
  FsStreamState fs_state;
  struct state_changed_signal_data *data;

  if (stream_id != self->priv->stream_id)
    return;

  g_return_if_fail (component_id > 0 &&
      component_id <= self->priv->transmitter->components);

  /* Ignore failed until we've connected, never time out because
   * of the dribbling case, more candidates could come later
   */
  if (state == NICE_COMPONENT_STATE_FAILED &&
      !self->priv->component_has_been_ready[component_id - 1])
    return;
  else if (state == NICE_COMPONENT_STATE_READY)
    self->priv->component_has_been_ready[component_id - 1] = TRUE;

  fs_state = nice_component_state_to_fs_stream_state (state);
  data = g_slice_new (struct state_changed_signal_data);

  GST_DEBUG ("Stream: %u Component %u has state %u",
      self->priv->stream_id, component_id, state);

  data->self = g_object_ref (self);
  data->component_id = component_id;
  data->fs_state = fs_state;
  fs_nice_agent_add_idle (self->priv->agent, state_changed_signal_idle,
      data, free_state_changed_signal_data);

  if (fs_state >= FS_STREAM_STATE_CONNECTED)
  {
    FS_NICE_STREAM_TRANSMITTER_LOCK (self);
    if (self->priv->gststream)
      fs_nice_transmitter_request_keyunit (self->priv->transmitter,
          self->priv->gststream, component_id);
    FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
  }
}


struct candidate_signal_data
{
  FsNiceStreamTransmitter *self;
  const gchar *signal_name;
  FsCandidate *candidate1;
  FsCandidate *candidate2;
};

static void
free_candidate_signal_data (gpointer user_data)
{
  struct candidate_signal_data *data = user_data;
  fs_candidate_destroy (data->candidate1);
  if (data->candidate2)
    fs_candidate_destroy (data->candidate2);
  g_object_unref (data->self);
  g_slice_free (struct candidate_signal_data, data);
}

static gboolean
agent_candidate_signal_idle (gpointer userdata)
{
  struct candidate_signal_data *data = userdata;

  g_signal_emit_by_name (data->self, data->signal_name, data->candidate1,
                         data->candidate2);
  return FALSE;
}


static void
agent_new_selected_pair (NiceAgent *agent,
    guint stream_id,
    guint component_id,
    NiceCandidate *l_candidate,
    NiceCandidate *r_candidate,
    gpointer user_data)
{
  FsNiceStreamTransmitter *self = FS_NICE_STREAM_TRANSMITTER (user_data);
  FsCandidate *local = NULL;
  FsCandidate *remote = NULL;
  struct candidate_signal_data *data;

  if (stream_id != self->priv->stream_id)
    return;

  local = nice_candidate_to_fs_candidate (agent, l_candidate, TRUE);
  remote = nice_candidate_to_fs_candidate (agent, r_candidate, FALSE);

  data = g_slice_new (struct candidate_signal_data);
  data->self = g_object_ref (self);
  data->signal_name = "new-active-candidate-pair";
  data->candidate1 = local;
  data->candidate2 = remote;
  fs_nice_agent_add_idle (self->priv->agent, agent_candidate_signal_idle,
      data, free_candidate_signal_data);
}

static void
agent_new_candidate (NiceAgent *agent,
    NiceCandidate *candidate,
    gpointer user_data)
{
  FsNiceStreamTransmitter *self = FS_NICE_STREAM_TRANSMITTER (user_data);
  FsCandidate *fscandidate = NULL;

  if (candidate->stream_id != self->priv->stream_id)
    return;

  GST_DEBUG ("New candidate found");

  fscandidate = nice_candidate_to_fs_candidate (agent, candidate, TRUE);

  FS_NICE_STREAM_TRANSMITTER_LOCK (self);
  if (!self->priv->gathered)
  {
    /* Nice doesn't do connchecks while gathering, so don't tell the upper
     * layers about the candidates untill gathering is finished.
     * Also older versions of farstream would fail the connection right away
     * when the first candidate given failed immediately (e.g. ipv6 on a
     * non-ipv6 capable host, so we order ipv6 candidates after ipv4 ones */

     if (strchr (fscandidate->ip, ':'))
      self->priv->local_candidates = g_list_append
        (self->priv->local_candidates, fscandidate);
    else
      self->priv->local_candidates = g_list_prepend
        (self->priv->local_candidates, fscandidate);
    FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
  }
  else
  {
    struct candidate_signal_data *data =
      g_slice_new (struct candidate_signal_data);

    FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);

    data->self = g_object_ref (self);
    data->signal_name = "new-local-candidate";
    data->candidate1 = fscandidate;
    data->candidate2 = NULL;
    fs_nice_agent_add_idle (self->priv->agent, agent_candidate_signal_idle,
      data, free_candidate_signal_data);
  }
}


static gboolean
agent_gathering_done_idle (gpointer data)
{
  FsNiceStreamTransmitter *self = data;
  GList *remote_candidates = NULL;
  GList *local_candidates = NULL;
  gboolean forced_candidates;

  FS_NICE_STREAM_TRANSMITTER_LOCK (self);
  if (self->priv->gathered)
  {
    FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);
    return FALSE;
  }

  self->priv->gathered = TRUE;
  remote_candidates = self->priv->remote_candidates;
  self->priv->remote_candidates = NULL;
  local_candidates = self->priv->local_candidates;
  self->priv->local_candidates = NULL;
  forced_candidates = self->priv->forced_candidates;
  FS_NICE_STREAM_TRANSMITTER_UNLOCK (self);

  GST_DEBUG ("Candidates gathered for stream %u", self->priv->stream_id);

  if (local_candidates)
  {
    GList *l;

    for (l = local_candidates ; l != NULL; l = g_list_next (l))
      g_signal_emit_by_name (self, "new-local-candidate", l->data);
    fs_candidate_list_destroy (local_candidates);
  }

  g_signal_emit_by_name (self, "local-candidates-prepared");

  if (remote_candidates)
  {
    if (forced_candidates)
    {
      if (!fs_nice_stream_transmitter_force_remote_candidates_act (self,
              remote_candidates))
      {
        fs_stream_transmitter_emit_error (FS_STREAM_TRANSMITTER (self),
            FS_ERROR_INTERNAL,
            "Error setting delayed forced remote candidates");
      }
    }
    else
    {
      GError *error = NULL;

      if (self->priv->compatibility_mode != NICE_COMPATIBILITY_GOOGLE &&
          self->priv->compatibility_mode != NICE_COMPATIBILITY_MSN &&
          self->priv->compatibility_mode != NICE_COMPATIBILITY_OC2007)
      {
        if (!nice_agent_set_remote_credentials (self->priv->agent->agent,
                self->priv->stream_id, self->priv->username,
                self->priv->password))
        {
          fs_stream_transmitter_emit_error (FS_STREAM_TRANSMITTER (self),
              FS_ERROR_INTERNAL,
              "Could not set the security credentials");
          fs_candidate_list_destroy (remote_candidates);
          return FALSE;
        }
      }


      if (!fs_nice_stream_transmitter_add_remote_candidates (
              FS_STREAM_TRANSMITTER_CAST (self),
              remote_candidates, &error))
      {
        fs_stream_transmitter_emit_error (FS_STREAM_TRANSMITTER (self),
            error->code, error->message);
      }
      g_clear_error (&error);
    }

    fs_candidate_list_destroy (remote_candidates);
  }

  return FALSE;
}


static void
agent_gathering_done (NiceAgent *agent, guint stream_id, gpointer user_data)
{
  FsNiceStreamTransmitter *self = FS_NICE_STREAM_TRANSMITTER (user_data);

  if (stream_id != self->priv->stream_id)
    return;

  fs_nice_agent_add_idle (self->priv->agent, agent_gathering_done_idle,
      g_object_ref (self), g_object_unref);
}


FsNiceStreamTransmitter *
fs_nice_stream_transmitter_newv (FsNiceTransmitter *transmitter,
    FsParticipant *participant,
    guint n_parameters,
    GParameter *parameters,
    GError **error)
{
  FsNiceStreamTransmitter *streamtransmitter = NULL;

  if (!participant || !FS_IS_PARTICIPANT (participant))
  {
    g_set_error (error, FS_ERROR, FS_ERROR_INVALID_ARGUMENTS,
        "You need a valid participant");
    return NULL;
  }

  streamtransmitter = g_object_newv (FS_TYPE_NICE_STREAM_TRANSMITTER,
    n_parameters, parameters);

  if (!streamtransmitter)
  {
    g_set_error (error, FS_ERROR, FS_ERROR_CONSTRUCTION,
      "Could not build the stream transmitter");
    return NULL;
  }

  streamtransmitter->priv->transmitter = g_object_ref (transmitter);

  if (!fs_nice_stream_transmitter_build (streamtransmitter, participant, error))
  {
    g_object_unref (streamtransmitter);
    return NULL;
  }

  return streamtransmitter;
}


static GstPadProbeReturn
known_buffer_have_buffer_handler (GstPad *pad, GstPadProbeInfo *info,
    gpointer user_data)
{
  FsNiceStreamTransmitter *self = FS_NICE_STREAM_TRANSMITTER (user_data);
  guint component_id;
  GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER (info);

  if (!g_atomic_int_get (&self->priv->associate_on_source))
    return GST_PAD_PROBE_OK;

  component_id = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (pad),
          "component-id"));

  g_signal_emit_by_name (self, "known-source-packet-received", component_id,
      buffer);

  return GST_PAD_PROBE_OK;
}
