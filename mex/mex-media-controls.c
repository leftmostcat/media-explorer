/*
 * Mex - a media explorer
 *
 * Copyright © 2010, 2011 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU Lesser General Public License,
 * version 2.1, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses>
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "mex-media-controls.h"
#include "mex-view-model.h"
#include "mex-proxy.h"
#include "mex-content-tile.h"
#include "mex-queue-model.h"
#include "mex-shadow.h"
#include "mex-main.h"
#include "mex-player.h"
#include "mex-content-proxy.h"
#include "mex-aggregate-model.h"
#include "mex-metadata-utils.h"
#ifdef USE_PLAYER_CLUTTER_GST
#include <clutter-gst/clutter-gst.h>
#include <clutter-gst/clutter-gst-player.h>
#include <gst/gst.h>
#include <gst/tag/tag.h>
#endif
#include <glib/gi18n-lib.h>

static void mx_focusable_iface_init (MxFocusableIface *iface);

G_DEFINE_TYPE_WITH_CODE (MexMediaControls, mex_media_controls, MX_TYPE_WIDGET,
                         G_IMPLEMENT_INTERFACE (MX_TYPE_FOCUSABLE,
                                                mx_focusable_iface_init))

#define MEDIA_CONTROLS_PRIVATE(o) \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), MEX_TYPE_MEDIA_CONTROLS, MexMediaControlsPrivate))

enum
{
  PROP_0,

  PROP_MEDIA,
  PROP_PLAYING_QUEUE
};

struct _MexMediaControlsPrivate
{
  ClutterMedia *media;

  ClutterActor *vbox;
  ClutterActor *slider;

  ClutterScript *script;

  MxAction *play_pause_action;
  MxAction *stop_action;
  MxAction *add_to_queue_action;

  MexContent *content;

  guint key_press_timeout;
  guint long_press_activated : 1;
  guint increment : 1;
  guint key_press_count;

  GCompareDataFunc sort_func;
  gpointer sort_data;

  MexProxy *proxy;

  guint is_queue_model : 1;
  guint is_disabled    : 1;
  guint show_description : 1;

  MexModel   *model;
  MexViewModel *proxy_model;
};

enum
{
  STOPPED,

  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };


/* MxFocusableIface */

static MxFocusable *
mex_media_controls_move_focus (MxFocusable      *focusable,
                               MxFocusDirection  direction,
                               MxFocusable      *from)
{
  return NULL;
}

static MxFocusable *
mex_media_controls_accept_focus (MxFocusable *focusable,
                                 MxFocusHint  hint)
{
  MexMediaControlsPrivate *priv = MEX_MEDIA_CONTROLS (focusable)->priv;

  return mx_focusable_accept_focus (MX_FOCUSABLE (priv->vbox),
                                    MX_FOCUS_HINT_FIRST);
}

static void
mx_focusable_iface_init (MxFocusableIface *iface)
{
  iface->move_focus = mex_media_controls_move_focus;
  iface->accept_focus = mex_media_controls_accept_focus;
}

/* Actor implementation */

static void
mex_media_controls_get_property (GObject    *object,
                                 guint       property_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  MexMediaControls *self = MEX_MEDIA_CONTROLS (object);

  switch (property_id)
    {
    case PROP_MEDIA:
      g_value_set_object (value, mex_media_controls_get_media (self));
      break;

    case PROP_PLAYING_QUEUE:
      g_value_set_boolean (value, mex_media_controls_get_playing_queue (self));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
mex_media_controls_set_property (GObject      *object,
                                 guint         property_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  MexMediaControls *self = MEX_MEDIA_CONTROLS (object);

  switch (property_id)
    {
    case PROP_MEDIA:
      mex_media_controls_set_media (self, g_value_get_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
mex_media_controls_dispose (GObject *object)
{
  MexMediaControls *self = MEX_MEDIA_CONTROLS (object);
  MexMediaControlsPrivate *priv = self->priv;

  if (priv->media)
    mex_media_controls_set_media (self, NULL);

  if (priv->content)
    {
      g_object_unref (priv->content);
      priv->content = NULL;
    }

  if (priv->model)
    {
      g_object_unref (priv->model);
      priv->model = NULL;
    }

  if (priv->proxy)
    {
      g_object_unref (priv->proxy);
      priv->proxy = NULL;
    }

  if (priv->proxy_model)
    {
      g_object_unref (priv->proxy_model);
      priv->proxy_model = NULL;
    }

  if (priv->script)
    {
      g_object_unref (priv->script);
      priv->script = NULL;
    }

  if (priv->vbox)
    {
      clutter_actor_destroy (priv->vbox);
      priv->vbox = NULL;
    }

  G_OBJECT_CLASS (mex_media_controls_parent_class)->dispose (object);
}

static void
mex_media_controls_finalize (GObject *object)
{
  G_OBJECT_CLASS (mex_media_controls_parent_class)->finalize (object);
}

static void
mex_media_controls_get_preferred_width (ClutterActor *actor,
                                        gfloat        for_height,
                                        gfloat       *min_width_p,
                                        gfloat       *nat_width_p)
{
  MxPadding padding;
  MexMediaControlsPrivate *priv = MEX_MEDIA_CONTROLS (actor)->priv;

  mx_widget_get_padding (MX_WIDGET (actor), &padding);
  if (for_height >= 0)
    for_height = MAX (0, for_height - padding.top - padding.bottom);

  clutter_actor_get_preferred_width (priv->vbox,
                                     for_height,
                                     min_width_p,
                                     nat_width_p);

  if (min_width_p)
    *min_width_p += padding.left + padding.right;
  if (nat_width_p)
    *nat_width_p += padding.left + padding.right;
}

static void
mex_media_controls_get_preferred_height (ClutterActor *actor,
                                        gfloat        for_width,
                                        gfloat       *min_height_p,
                                        gfloat       *nat_height_p)
{
  MxPadding padding;
  MexMediaControlsPrivate *priv = MEX_MEDIA_CONTROLS (actor)->priv;

  mx_widget_get_padding (MX_WIDGET (actor), &padding);
  if (for_width >= 0)
    for_width = MAX (0, for_width - padding.left - padding.right);

  clutter_actor_get_preferred_height (priv->vbox,
                                     for_width,
                                     min_height_p,
                                     nat_height_p);

  if (min_height_p)
    *min_height_p += padding.top + padding.bottom;
  if (nat_height_p)
    *nat_height_p += padding.top + padding.bottom;
}

static void
mex_media_controls_allocate (ClutterActor           *actor,
                             const ClutterActorBox  *box,
                             ClutterAllocationFlags  flags)
{
  ClutterActorBox child_box;
  MexMediaControlsPrivate *priv = MEX_MEDIA_CONTROLS (actor)->priv;

  CLUTTER_ACTOR_CLASS (mex_media_controls_parent_class)->
    allocate (actor, box, flags);

  mx_widget_get_available_area (MX_WIDGET (actor), box, &child_box);
  clutter_actor_allocate (priv->vbox, &child_box, flags);
}

static void
mex_media_controls_paint (ClutterActor *actor)
{
  MexMediaControlsPrivate *priv = MEX_MEDIA_CONTROLS (actor)->priv;

  CLUTTER_ACTOR_CLASS (mex_media_controls_parent_class)->paint (actor);

  clutter_actor_paint (priv->vbox);
}

static void
mex_media_controls_pick (ClutterActor       *actor,
                         const ClutterColor *color)
{
  MexMediaControlsPrivate *priv = MEX_MEDIA_CONTROLS (actor)->priv;

  CLUTTER_ACTOR_CLASS (mex_media_controls_parent_class)->pick (actor, color);

  clutter_actor_paint (priv->vbox);
}

static void
mex_media_controls_map (ClutterActor *actor)
{
  MexMediaControlsPrivate *priv = MEX_MEDIA_CONTROLS (actor)->priv;

  CLUTTER_ACTOR_CLASS (mex_media_controls_parent_class)->map (actor);

  clutter_actor_map (priv->vbox);
}

static void
mex_media_controls_unmap (ClutterActor *actor)
{
  MexMediaControlsPrivate *priv = MEX_MEDIA_CONTROLS (actor)->priv;

  CLUTTER_ACTOR_CLASS (mex_media_controls_parent_class)->unmap (actor);

  clutter_actor_unmap (priv->vbox);
  g_object_set (G_OBJECT (priv->proxy_model), "model", NULL, NULL);
  if (priv->content)
    {
      g_object_unref (priv->content);
      priv->content = NULL;
    }
  if (priv->model)
    {
      g_object_unref (priv->model);
      priv->model = NULL;
    }
}

static void
mex_media_controls_class_init (MexMediaControlsClass *klass)
{
  GParamSpec *pspec;

  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  ClutterActorClass *actor_class = CLUTTER_ACTOR_CLASS (klass);

  g_type_class_add_private (klass, sizeof (MexMediaControlsPrivate));

  object_class->get_property = mex_media_controls_get_property;
  object_class->set_property = mex_media_controls_set_property;
  object_class->dispose = mex_media_controls_dispose;
  object_class->finalize = mex_media_controls_finalize;

  actor_class->get_preferred_width = mex_media_controls_get_preferred_width;
  actor_class->get_preferred_height = mex_media_controls_get_preferred_height;
  actor_class->allocate = mex_media_controls_allocate;
  actor_class->paint = mex_media_controls_paint;
  actor_class->pick = mex_media_controls_pick;
  actor_class->map = mex_media_controls_map;
  actor_class->unmap = mex_media_controls_unmap;

  pspec = g_param_spec_object ("media",
                               "Media",
                               "The ClutterMedia object the controls apply to.",
                               G_TYPE_OBJECT,
                               G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_property (object_class, PROP_MEDIA, pspec);


  signals[STOPPED] = g_signal_new ("stopped",
                                   G_TYPE_FROM_CLASS (klass),
                                   G_SIGNAL_RUN_LAST,
                                   0, NULL, NULL,
                                   g_cclosure_marshal_VOID__VOID,
                                   G_TYPE_NONE, 0);
}

static void
mex_media_controls_play_cb (MxButton         *toggle,
                            MexMediaControls *self)
{
  MexMediaControlsPrivate *priv = self->priv;

  if (priv->media)
    clutter_media_set_playing (priv->media,
                               !clutter_media_get_playing (priv->media));
}

static void
mex_media_controls_stop_cb (MxButton         *toggle,
                            MexMediaControls *self)
{
  MexMediaControlsPrivate *priv = self->priv;

  if (priv->media)
    clutter_media_set_playing (priv->media, FALSE);

  g_signal_emit (self, signals[STOPPED], 0);
}

static void
slider_value_changed_cb (MxSlider         *slider,
                         GParamSpec       *pspec,
                         MexMediaControls *controls)
{
  MexMediaControlsPrivate *priv = controls->priv;

  if (priv->media)
    clutter_media_set_progress (priv->media,
                                mx_slider_get_value (MX_SLIDER (priv->slider)));
}

static gboolean
key_press_timeout_cb (gpointer data)
{
  MexMediaControlsPrivate *priv = MEX_MEDIA_CONTROLS (data)->priv;
  gdouble duration;
  gdouble change;
  gfloat progress;

  priv->long_press_activated = TRUE;

  priv->key_press_count++;

  duration = clutter_media_get_duration (priv->media);
  progress = clutter_media_get_progress (priv->media);

  if (priv->key_press_count >= 10)
    change = 60;
  else
    change = 10;

  if (priv->increment)
    progress = MIN (1.0, ((duration * progress) + change) / duration);
  else
    progress = MAX (0.0, ((duration * progress) - change) / duration);

  clutter_media_set_progress (priv->media, progress);

  return TRUE;
}

static gboolean
slider_captured_event (MxSlider         *slider,
                       ClutterEvent     *event,
                       MexMediaControls *controls)
{
  MexMediaControlsPrivate *priv = controls->priv;


  /* cancel the long press timeout when a key is released */
  if (event->type == CLUTTER_KEY_RELEASE)
    {
      if (priv->key_press_timeout)
        {
          g_source_remove (priv->key_press_timeout);
          priv->key_press_timeout = 0;
          priv->long_press_activated = FALSE;

          priv->key_press_count = 0;
        }
    }

  if (event->type != CLUTTER_KEY_PRESS)
    return FALSE;

  /* handle just left and right key events */
  if (event->key.keyval == CLUTTER_KEY_Left)
    priv->increment = FALSE;
  else if (event->key.keyval == CLUTTER_KEY_Right)
    priv->increment  = TRUE;
  else
    return FALSE;


  /* start the key press timeout if required */
  if (!priv->key_press_timeout)
    {
      priv->long_press_activated = FALSE;

      priv->key_press_timeout = g_timeout_add (250, key_press_timeout_cb,
                                               controls);
      key_press_timeout_cb (controls);
    }

  return TRUE;
}

static void
mex_media_controls_update_header (MexMediaControls *self)
{
  MexMediaControlsPrivate *priv = self->priv;
  ClutterActor *label, *info;

  label = (ClutterActor*) clutter_script_get_object (priv->script,
                                                     "title-label");

  mx_label_set_text (MX_LABEL (label),
                     mex_content_get_metadata (priv->content,
                                               MEX_CONTENT_METADATA_TITLE));

  info = (ClutterActor *) clutter_script_get_object (priv->script,
                                                     "description-label");

  mx_label_set_text (MX_LABEL (info),
                     mex_content_get_metadata (priv->content,
                                               MEX_CONTENT_METADATA_SYNOPSIS));
}

static void
mex_media_controls_replace_content (MexMediaControls *self,
                                    MexContent       *content)
{
  MexPlayer *player;
  MxScrollable *related_box;
  MxAdjustment *adjustment;
  gdouble upper;

  MexMediaControlsPrivate *priv = self->priv;

  player = mex_player_get_default ();

  mex_content_view_set_content (MEX_CONTENT_VIEW (player), content);

  if (priv->content)
    g_object_unref (priv->content);
  priv->content = g_object_ref_sink (content);
  mex_media_controls_update_header (self);

  mex_push_focus ((MxFocusable*) clutter_script_get_object (priv->script,
                                              "play-pause-button"));

  related_box = (MxScrollable *)clutter_script_get_object (priv->script,
                                                           "related-box");
  mx_scrollable_get_adjustments (MX_SCROLLABLE (related_box),
                                 &adjustment, NULL);

  mx_adjustment_get_values (adjustment, NULL, NULL, &upper,
                            NULL, NULL, NULL);

  mx_adjustment_set_value (adjustment, upper);
  mx_scrollable_set_adjustments (MX_SCROLLABLE (related_box),
                                 adjustment,
                                 NULL);
}

static gboolean
mex_media_controls_key_press_event (ClutterActor    *actor,
                                    ClutterKeyEvent *event,
                                    gpointer         user_data)
{
  MexMediaControls *self = MEX_MEDIA_CONTROLS (actor);

  if (self->priv->key_press_timeout)
    return FALSE;

  if (MEX_KEY_INFO (event->keyval))
    {
      ClutterActor *info_box;
      gfloat info_box_h, info_box_w, controls_h;

      info_box = (ClutterActor*) clutter_script_get_object (self->priv->script,
                                                            "info-box");

      controls_h = clutter_actor_get_height (actor);
      info_box_w = clutter_actor_get_width (info_box);

      CLUTTER_ACTOR_CLASS (G_OBJECT_GET_CLASS (info_box))->get_preferred_height (info_box,
                                                                                 info_box_w,
                                                                                 NULL,
                                                                                 &info_box_h);

      if (clutter_actor_get_height (info_box) > 0)
        {
          clutter_actor_set_opacity (info_box, 0);
          clutter_actor_set_height (info_box, 0);
          clutter_actor_set_height (actor, controls_h - info_box_h);
        }
      else
        {
          clutter_actor_set_opacity (info_box, 0xff);
          clutter_actor_set_height (info_box, info_box_h);
          clutter_actor_set_height (actor, controls_h + info_box_h);
        }

      return TRUE;
    }

  return FALSE;
}

static gboolean
key_press_event_cb (ClutterActor    *actor,
                    ClutterKeyEvent *event,
                    gpointer         user_data)
{
  MexMediaControls *self = MEX_MEDIA_CONTROLS (user_data);

  if (MEX_KEY_OK (event->keyval))
    {
      MexContent *content =
        mex_content_view_get_content (MEX_CONTENT_VIEW (actor));
      mex_media_controls_replace_content (self, content);

      return TRUE;
    }

  return FALSE;
}

static gboolean
button_release_event_cb (ClutterActor       *actor,
                         ClutterButtonEvent *event,
                         gpointer            user_data)
{
  MexMediaControls *self = MEX_MEDIA_CONTROLS (user_data);
  MexContent *content = mex_content_view_get_content (MEX_CONTENT_VIEW (actor));

  mex_media_controls_replace_content (self, content);

  return TRUE;
}

static void
tile_created_cb (MexProxy *proxy,
                 GObject  *content,
                 GObject  *object,
                 gpointer  controls)
{
  const gchar *mime_type;

  /* filter out folders */
  mime_type = mex_content_get_metadata (MEX_CONTENT (content),
                                        MEX_CONTENT_METADATA_MIMETYPE);

  if (g_strcmp0 (mime_type, "x-grl/box") == 0
      || g_strcmp0 (mime_type, "x-mex/group") == 0)
    {
      g_signal_stop_emission_by_name (proxy, "object-created");
      return;
    }

  mex_tile_set_important (MEX_TILE (object), TRUE);
  clutter_actor_set_reactive (CLUTTER_ACTOR (object), TRUE);

  g_object_set (object, "thumb-height", 140, "thumb-width", 250, NULL);

  g_signal_connect (object, "key-press-event", G_CALLBACK (key_press_event_cb),
                    controls);
  g_signal_connect (object, "button-release-event",
                    G_CALLBACK (button_release_event_cb), controls);
}

static void
show_hide_subtitle_selector (MxButton         *button,
                             MexMediaControls *controls)
{
  MexMediaControlsPrivate *priv = controls->priv;
  ClutterActor *table;

  table = (ClutterActor *) clutter_script_get_object (priv->script, "subtitle-selector");

  if (CLUTTER_ACTOR_IS_VISIBLE (table))
    clutter_actor_hide (table);
  else
    {
      clutter_actor_show (table);
      mex_push_focus (MX_FOCUSABLE (table));
    }
}

static void
mex_media_controls_init (MexMediaControls *self)
{
  ClutterScript *script;
  GError *err = NULL;
  ClutterActor *related_box, *subtitle_button;
  gchar *tmp;

  MexMediaControlsPrivate *priv = self->priv = MEDIA_CONTROLS_PRIVATE (self);

  g_signal_connect (self, "key-press-event",
                    G_CALLBACK (mex_media_controls_key_press_event), NULL);

  priv->script = script = clutter_script_new ();

  tmp = g_build_filename (mex_get_data_dir (), "json", "media-controls.json",
                          NULL);
  clutter_script_load_from_file (script, tmp, &err);
  g_free (tmp);

  if (err)
    g_error ("Could not load media controls interface: %s", err->message);

  priv->vbox =
    (ClutterActor*) clutter_script_get_object (script, "media-controls");
  clutter_actor_set_parent (priv->vbox, CLUTTER_ACTOR (self));


  /* slider setup */
  priv->slider = (ClutterActor*) clutter_script_get_object (script, "slider");
  g_signal_connect (priv->slider, "notify::value",
                    G_CALLBACK (slider_value_changed_cb), self);
  g_signal_connect (priv->slider, "captured-event",
                    G_CALLBACK (slider_captured_event), self);

  priv->play_pause_action =
    (MxAction*) clutter_script_get_object (script, "play-pause-action");

  priv->stop_action =
   (MxAction*) clutter_script_get_object (script, "stop-action");

  priv->add_to_queue_action =
   (MxAction*) clutter_script_get_object (script, "add-to-queue-action");

  g_signal_connect (priv->play_pause_action, "activated",
                    G_CALLBACK (mex_media_controls_play_cb), self);
  g_signal_connect (priv->stop_action, "activated",
                    G_CALLBACK (mex_media_controls_stop_cb), self);
#if 0
  g_signal_connect (priv->add_to_queue_action, "activated",
                    G_CALLBACK (mex_media_controls_add_to_queue_cb), self);
#endif

  /* proxy setup */

  priv->proxy_model = MEX_VIEW_MODEL (mex_view_model_new (NULL));
  /* FIXME: Set an arbitrary 200-item limit as we can't handle large
   *        amounts of actors without massive slow-down.
   */
  mex_view_model_set_limit (priv->proxy_model, 200);

  related_box = (ClutterActor *) clutter_script_get_object (priv->script,
                                                            "related-box");
  priv->proxy = mex_content_proxy_new (MEX_MODEL (priv->proxy_model),
                                       CLUTTER_CONTAINER (related_box),
                                       MEX_TYPE_CONTENT_TILE);

  g_signal_connect (priv->proxy, "object-created", G_CALLBACK (tile_created_cb),
                    self);

  priv->is_disabled = TRUE;

  /* subtitles */
  subtitle_button = (ClutterActor *) clutter_script_get_object (priv->script,
                                                                "select-subtitles");
  g_signal_connect (subtitle_button, "clicked", G_CALLBACK (show_hide_subtitle_selector),
                    self);
}

ClutterActor *
mex_media_controls_new (void)
{
  return g_object_new (MEX_TYPE_MEDIA_CONTROLS, NULL);
}

static void
mex_media_controls_notify_can_seek_cb (ClutterMedia     *media,
                                       GParamSpec       *pspec,
                                       MexMediaControls *self)
{
  MexMediaControlsPrivate *priv = self->priv;
  gboolean can_seek = clutter_media_get_can_seek (media);

  mx_widget_set_disabled (MX_WIDGET (priv->slider), !can_seek);
}

static void
mex_media_controls_notify_playing_cb (ClutterMedia     *media,
                                      GParamSpec       *pspec,
                                      MexMediaControls *self)
{
  MexMediaControlsPrivate *priv = self->priv;
  MxStylable *button;
  const gchar *name;

  if (clutter_media_get_playing (media))
    name = "MediaPause";
  else
    name = "MediaPlay";

  button = MX_STYLABLE (clutter_script_get_object (priv->script,
                                                   "play-pause-button"));

  mx_stylable_set_style_class (button, name);
}

static void
mex_media_controls_show_description (MexMediaControls *self,
                                     gboolean          show)
{
  MexMediaControlsPrivate *priv = self->priv;
  MxLabel *label;
  ClutterActor *play_pause_button, *stop_button;
  const gchar *text;

  label = (MxLabel*) clutter_script_get_object (priv->script, "progress-label");

  play_pause_button =
    (ClutterActor*) clutter_script_get_object (priv->script,
                                               "play-pause-button");
  stop_button =
    (ClutterActor*) clutter_script_get_object (priv->script,
                                               "stop-button");

  if (show)
    {
      clutter_actor_hide (priv->slider);
      clutter_actor_hide (play_pause_button);
      clutter_actor_hide (stop_button);

      if (priv->content)
        text = mex_content_get_metadata (priv->content,
                                         MEX_CONTENT_METADATA_SYNOPSIS);
      else
        text = NULL;

      mx_label_set_text (label, (text) ? text : "");
    }
  else
    {
      mx_label_set_text (label, "");
      clutter_actor_show (priv->slider);
      clutter_actor_show (play_pause_button);
      clutter_actor_show (stop_button);
    }

  priv->show_description = show;
}

static void
mex_media_controls_notify_progress_cb (ClutterMedia     *media,
                                       GParamSpec       *pspec,
                                       MexMediaControls *self)
{
  MexMediaControlsPrivate *priv = self->priv;
  MxLabel *label;
  gchar *text;
  gdouble length;
  gfloat progress;

  if (priv->show_description)
    return;

  progress = clutter_media_get_progress (media);
  length = clutter_media_get_duration (media);

  g_signal_handlers_block_by_func (priv->slider, slider_value_changed_cb, self);
  mx_slider_set_value (MX_SLIDER (priv->slider), progress);
  g_signal_handlers_unblock_by_func (priv->slider, slider_value_changed_cb,
                                     self);

  text = mex_metadata_utils_create_progress_string (progress, length);

  label = (MxLabel*) clutter_script_get_object (priv->script, "progress-label");
  mx_label_set_text (label, text);
  g_free (text);
}

static void
free_string_list (GList *l)
{
  while (l)
    {
      g_free (l->data);
      l = g_list_delete_link (l, l);
    }
}

static gchar *
get_stream_description (GstTagList *tags,
                        gint        track_num)
{
  gchar *description = NULL;

  if (tags)
    {
      gst_tag_list_get_string (tags, GST_TAG_LANGUAGE_CODE, &description);

      if (description)
        {
          const gchar *language = gst_tag_get_language_name (description);

          if (language)
            {
              g_free (description);
              description = g_strdup (language);
            }
        }

      if (!description)
        gst_tag_list_get_string (tags, GST_TAG_CODEC, &description);
    }

  if (!description)
    {
      /* In this context Tracks is either an audio track or a subtitles
       * track */
      description = g_strdup_printf (_("Track %d"), track_num);
    }

  return description;
}

static GList *
get_streams_descriptions (GList *tags_list)
{
  GList *descriptions = NULL, *l;
  gint track_num = 1;

  for (l = tags_list; l; l = g_list_next (l))
    {
      GstTagList *tags = l->data;
      gchar *description;

      description = get_stream_description (tags, track_num);
      track_num++;

      descriptions = g_list_prepend (descriptions, description);
    }

  return g_list_reverse (descriptions);
}

static void
set_subtitle (MxButton         *button,
              MexMediaControls *self)
{
  gint i;

  i = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (button),
                                          "subtitle-track"));

  clutter_gst_player_set_subtitle_track (CLUTTER_GST_PLAYER (self->priv->media),
                                         i);
  clutter_actor_hide ((ClutterActor *) clutter_script_get_object (self->priv->script,
                                                                  "subtitle-selector"));
}

static void
on_media_subtitle_tracks_changed (ClutterMedia     *media,
                                  GParamSpec       *pspsec,
                                  MexMediaControls *self)
{
#ifdef USE_PLAYER_CLUTTER_GST
  ClutterGstPlayer *player = CLUTTER_GST_PLAYER (media);
  MexMediaControlsPrivate *priv = self->priv;
  GList *tracks, *l, *descriptions;
  gint n_tracks;
  ClutterActor *table, *subtitle_button, *button;
  gint row, column, i;

  table = (ClutterActor *) clutter_script_get_object (priv->script, "subtitle-selector");
  subtitle_button = (ClutterActor *) clutter_script_get_object (priv->script,
                                                                "select-subtitles");

  tracks = clutter_gst_player_get_subtitle_tracks (player);

  clutter_container_foreach (CLUTTER_CONTAINER (table),
                             (ClutterCallback) clutter_actor_destroy, NULL);

  /* no need to display the subtitle combo box if there's no subtitles */
  n_tracks = g_list_length (tracks);
  if (n_tracks == 0)
    {
      clutter_actor_hide (subtitle_button);
      return;
    }

  /* Add a "None" option to disable subtitles */
  /* TRANSLATORS: In this context, None is used to disable subtitles in the
   * list of choices for subtitles */
  button = mx_button_new_with_label (_("None"));
  g_object_set_data (G_OBJECT (button), "subtitle-track", GINT_TO_POINTER (0));
  g_signal_connect (button, "clicked", G_CALLBACK (set_subtitle), self);
  g_object_set (button, "min-width", 180.0, NULL);

  clutter_actor_add_child (table, button);

  /* TRANSLATORS: In this context, track is a subtitles track */
  descriptions = get_streams_descriptions (tracks);
  row = 1;
  column = 0;
  i = 0;
  for (l = descriptions; l; l = g_list_next (l))
    {
      gchar *description = l->data;

      button = mx_button_new_with_label (description);
      g_object_set (button, "min-width", 180.0, NULL);
      mx_table_insert_actor (MX_TABLE (table), button, row, column);
      g_object_set_data (G_OBJECT (button), "subtitle-track",
                         GINT_TO_POINTER (i));
      g_signal_connect (button, "clicked", G_CALLBACK (set_subtitle), self);

      if (++row > 2)
        {
          column++;
          row = 0;
        }
      i++;
    }
  free_string_list (descriptions);

  clutter_actor_show (subtitle_button);
#endif
}

static void
mex_media_controls_notify_download_cb (ClutterMedia     *media,
				       gdouble           start,
				       gdouble           stop,
				       MexMediaControls *self)
{
  MexMediaControlsPrivate *priv = self->priv;

  mx_slider_set_buffer_value (MX_SLIDER (priv->slider), stop);
}

void
mex_media_controls_set_media (MexMediaControls *self,
                              ClutterMedia     *media)
{
  MexMediaControlsPrivate *priv;

  g_return_if_fail (MEX_IS_MEDIA_CONTROLS (self));
  g_return_if_fail (!media || CLUTTER_IS_MEDIA (media));

  priv = self->priv;
  if (priv->media != media)
    {
      if (priv->media)
        {
          mex_media_controls_set_disabled (self, TRUE);

          g_object_unref (priv->media);
          priv->media = NULL;
        }

      if (media)
        {
          priv->media = g_object_ref (media);

          mex_media_controls_set_disabled (self, FALSE);
        }

      g_object_notify (G_OBJECT (self), "media");
    }
}

ClutterMedia *
mex_media_controls_get_media (MexMediaControls *self)
{
  g_return_val_if_fail (MEX_IS_MEDIA_CONTROLS (self), NULL);
  return self->priv->media;
}

gboolean
mex_media_controls_get_playing_queue (MexMediaControls *self)
{
  return self->priv->is_queue_model;
}

MexContent *
mex_media_controls_get_content (MexMediaControls *self)
{
  return self->priv->content;
}

void
mex_media_controls_set_content (MexMediaControls *self,
                                MexContent       *content,
                                MexModel         *context)
{
  MexMediaControlsPrivate *priv = self->priv;
  gboolean show_description;

  g_return_if_fail (MEX_IS_CONTENT (content));

  if (priv->model == context)
    {
      if (priv->content == content)
        return;

      if (priv->content)
        g_object_unref (priv->content);
      if (content)
        priv->content = g_object_ref_sink (content);

      mex_media_controls_focus_content (self, priv->content);
      mex_media_controls_update_header (self);
      return;
    }

  if (priv->model)
    {
      g_object_unref (priv->model);
      priv->model = NULL;
    }
  if (context)
    priv->model = g_object_ref_sink (context);
  if (priv->content)
    {
      g_object_unref (priv->content);
      priv->content = NULL;
    }
  if (content)
    priv->content = g_object_ref_sink (content);
  priv->is_queue_model = FALSE;

  mex_media_controls_update_header (self);


  /* We may not have a context if we're launched by something like SetUri*/
  if (context)
    {
      MexModel *orig_model = NULL;

      /* disconnect the proxy while the view model sorts it's data, since the
       * proxy cannot re-sort */
      mex_proxy_set_model (priv->proxy, NULL);

      g_object_set (G_OBJECT (priv->proxy_model), "model", context, NULL);

      mex_view_model_set_start_content (priv->proxy_model, priv->content);
      mex_view_model_set_loop (priv->proxy_model, TRUE);

      /* reset the model on proxy to ensure all items are in the correct order */
      mex_proxy_set_model (priv->proxy, MEX_MODEL (priv->proxy_model));

      if (g_str_has_prefix (mex_content_get_metadata (priv->content,
                                                      MEX_CONTENT_METADATA_MIMETYPE),
                            "audio/"))
        {
          /* treat models with audio in them as queue models, i.e. advance to
           * the next item automatically */
          priv->is_queue_model = TRUE;
        }

      /* Work out if the context was a queue FIXME unreliable */
      /* From coloumn context = MexViewModel MexAggregateModel MexQueueModel */
      /* From grid  context = MexViewModel MexQueueModel */

      orig_model = mex_model_get_model (context);
      if (MEX_IS_QUEUE_MODEL (orig_model))
        priv->is_queue_model = TRUE;
      else if (MEX_IS_AGGREGATE_MODEL (orig_model))
        {
          MexModel *real_model;
          real_model =
            mex_aggregate_model_get_model_for_content (MEX_AGGREGATE_MODEL (orig_model), content);
          if (MEX_IS_QUEUE_MODEL (real_model))
            priv->is_queue_model = TRUE;
        }
    }
  /* show the description rather than the seek bar for certain content */
  show_description = !g_strcmp0 ("x-mex/tv",
                                 mex_content_get_metadata (priv->content,
                                                           MEX_CONTENT_METADATA_MIMETYPE));

  mex_media_controls_show_description(self, show_description);
}

void
mex_media_controls_focus_content (MexMediaControls *self,
                                  MexContent       *content)
{
  MexMediaControlsPrivate *priv = self->priv;
  ClutterContainer *container;
  GList *children, *l;

  container = CLUTTER_CONTAINER (clutter_script_get_object (priv->script,
                                                            "related-box"));

  children = clutter_container_get_children (container);

  for (l = children; l; l = g_list_next (l))
    {
      if (mex_content_view_get_content (l->data) == content)
        {
          mex_push_focus (l->data);
          return;
        }
    }

  return;
}

/**
  * mex_media_controls_get_enqueued:
  * @controls: The MexMediaControls widget
  * @current_content: MexContent that the player is currently playing
  *
  * If the media controls has been given a queue model then return the next
  * MexContent in the queue model.
  *
  * Return value: The next content in the queue or NULL
  */
MexContent *
mex_media_controls_get_enqueued (MexMediaControls *controls,
                                 MexContent *current_content)
{
  MexMediaControlsPrivate *priv;
  MexModel *queue;
  MexContent *content = NULL;

  if (!MEX_IS_MEDIA_CONTROLS (controls) || !MEX_IS_CONTENT (current_content))
    return NULL;

  priv = controls->priv;

  if (priv->is_queue_model == FALSE)
    return NULL;

  queue = mex_proxy_get_model (priv->proxy);
  if (queue)
    {
      gint idx, length;

      idx = mex_model_index (queue, current_content);
      length = mex_model_get_length (queue);

      if (idx++ > length)
       return NULL;

      content = mex_model_get_content (queue, idx);
    }

  return content;
}

void
mex_media_controls_set_disabled (MexMediaControls *self,
                                 gboolean          disabled)
{
  MexMediaControlsPrivate *priv;

  g_return_if_fail (MEX_IS_MEDIA_CONTROLS (self));

  priv = self->priv;

  if (!priv->media)
    return;

  if (priv->is_disabled == disabled)
    return;

  if (disabled)
    {
      g_signal_handlers_disconnect_by_func (priv->media,
                                            mex_media_controls_notify_can_seek_cb,
                                            self);
      g_signal_handlers_disconnect_by_func (priv->media,
                                            mex_media_controls_notify_playing_cb,
                                            self);
      g_signal_handlers_disconnect_by_func (priv->media,
                                            mex_media_controls_notify_progress_cb,
                                            self);
#ifndef USE_PLAYER_DBUS
      g_signal_handlers_disconnect_by_func (priv->media,
                                            mex_media_controls_notify_download_cb,
                                            self);
#endif /* !USE_PLAYER_DBUS */
      g_signal_handlers_disconnect_by_func (priv->media,
                                            on_media_subtitle_tracks_changed,
                                            self);
    }
  else
    {
      g_signal_connect (priv->media, "notify::can-seek",
                        G_CALLBACK (mex_media_controls_notify_can_seek_cb),
                        self);
      g_signal_connect (priv->media, "notify::playing",
                        G_CALLBACK (mex_media_controls_notify_playing_cb),
                        self);
      g_signal_connect (priv->media, "notify::progress",
                        G_CALLBACK (mex_media_controls_notify_progress_cb),
                        self);
#ifndef USE_PLAYER_DBUS
      g_signal_connect (priv->media, "download-buffering",
                        G_CALLBACK (mex_media_controls_notify_download_cb),
                        self);
#endif /* !USE_PLAYER_DBUS */

      g_signal_connect (priv->media, "notify::subtitle-tracks",
                        G_CALLBACK (on_media_subtitle_tracks_changed), self);

      mex_media_controls_notify_can_seek_cb (priv->media, NULL, self);
      mex_media_controls_notify_playing_cb (priv->media, NULL, self);
      mex_media_controls_notify_progress_cb (priv->media, NULL, self);
      mex_media_controls_notify_download_cb (priv->media, 0.0, 0.0, self);
      on_media_subtitle_tracks_changed (priv->media, NULL, self);
    }

  priv->is_disabled = disabled;
}
