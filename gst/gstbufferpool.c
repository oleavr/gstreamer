/* GStreamer
 * Copyright (C) 2010 Wim Taymans <wim.taymans@gmail.com>
 *
 * gstbufferpool.c: GstBufferPool baseclass
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:gstbufferpool
 * @short_description: Pool for buffers
 * @see_also: #GstBuffer
 *
 */

#include "gst_private.h"
#include "glib-compat-private.h"

#include <errno.h>
#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif
#include <sys/types.h>

#include "gstinfo.h"
#include "gstquark.h"

#include "gstbufferpool.h"

GST_DEBUG_CATEGORY_STATIC (gst_buffer_pool_debug);
#define GST_CAT_DEFAULT gst_buffer_pool_debug

#define GST_BUFFER_POOL_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), GST_TYPE_BUFFER_POOL, GstBufferPoolPrivate))

#define GST_BUFFER_POOL_LOCK(pool)   (g_static_rec_mutex_lock(&pool->priv->rec_lock))
#define GST_BUFFER_POOL_UNLOCK(pool) (g_static_rec_mutex_unlock(&pool->priv->rec_lock))

struct _GstBufferPoolPrivate
{
  GStaticRecMutex rec_lock;
  guint size;
  guint min_buffers;
  guint max_buffers;
  guint prefix;
  guint align;
};

enum
{
  /* add more above */
  LAST_SIGNAL
};

static void gst_buffer_pool_finalize (GObject * object);

G_DEFINE_TYPE (GstBufferPool, gst_buffer_pool, GST_TYPE_OBJECT);

static gboolean default_start (GstBufferPool * pool);
static gboolean default_stop (GstBufferPool * pool);
static gboolean default_set_config (GstBufferPool * pool,
    GstStructure * config);
static GstFlowReturn default_alloc_buffer (GstBufferPool * pool,
    GstBuffer ** buffer, GstBufferPoolParams * params);
static GstFlowReturn default_acquire_buffer (GstBufferPool * pool,
    GstBuffer ** buffer, GstBufferPoolParams * params);
static void default_reset_buffer (GstBufferPool * pool, GstBuffer * buffer,
    GstBufferPoolParams * params);
static void default_free_buffer (GstBufferPool * pool, GstBuffer * buffer);
static void default_release_buffer (GstBufferPool * pool, GstBuffer * buffer);

static void
gst_buffer_pool_class_init (GstBufferPoolClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  g_type_class_add_private (klass, sizeof (GstBufferPoolPrivate));

  gobject_class->finalize = gst_buffer_pool_finalize;

  klass->start = default_start;
  klass->stop = default_stop;
  klass->set_config = default_set_config;
  klass->acquire_buffer = default_acquire_buffer;
  klass->reset_buffer = default_reset_buffer;
  klass->alloc_buffer = default_alloc_buffer;
  klass->release_buffer = default_release_buffer;
  klass->free_buffer = default_free_buffer;

  GST_DEBUG_CATEGORY_INIT (gst_buffer_pool_debug, "bufferpool", 0,
      "bufferpool debug");
}

static void
gst_buffer_pool_init (GstBufferPool * pool)
{
  pool->priv = GST_BUFFER_POOL_GET_PRIVATE (pool);

  g_static_rec_mutex_init (&pool->priv->rec_lock);

  pool->poll = gst_poll_new_timer ();
  pool->queue = gst_atomic_queue_new (10);
  pool->flushing = TRUE;
  pool->active = FALSE;
  pool->configured = FALSE;
  pool->started = FALSE;
  pool->config = gst_structure_new_id_empty (GST_QUARK (BUFFER_POOL_CONFIG));
  gst_buffer_pool_config_set (pool->config, NULL, 0, 0, 0, 0, 0);
  gst_poll_write_control (pool->poll);

  GST_DEBUG_OBJECT (pool, "created");
}

static void
gst_buffer_pool_finalize (GObject * object)
{
  GstBufferPool *pool;

  pool = GST_BUFFER_POOL_CAST (object);

  GST_DEBUG_OBJECT (pool, "finalize");

  gst_buffer_pool_set_active (pool, FALSE);
  gst_atomic_queue_unref (pool->queue);
  gst_poll_free (pool->poll);
  gst_structure_free (pool->config);
  g_static_rec_mutex_free (&pool->priv->rec_lock);

  G_OBJECT_CLASS (gst_buffer_pool_parent_class)->finalize (object);
}

/**
 * gst_buffer_pool_new:
 *
 * Creates a new #GstBufferPool instance.
 *
 * Returns: (transfer full): a new #GstBufferPool instance
 */
GstBufferPool *
gst_buffer_pool_new (void)
{
  GstBufferPool *result;

  result = g_object_newv (GST_TYPE_BUFFER_POOL, 0, NULL);
  GST_DEBUG_OBJECT (result, "created new buffer pool");

  return result;
}

static GstFlowReturn
default_alloc_buffer (GstBufferPool * pool, GstBuffer ** buffer,
    GstBufferPoolParams * params)
{
  GstBufferPoolPrivate *priv = pool->priv;
  GstMemory *mem;

  *buffer = gst_buffer_new ();

  mem = gst_allocator_alloc (NULL, priv->size + priv->prefix, priv->align);
  gst_memory_resize (mem, priv->prefix, priv->size);
  gst_buffer_take_memory (*buffer, -1, mem);

  return GST_FLOW_OK;
}

static gboolean
mark_meta_pooled (GstBuffer * buffer, GstMeta ** meta, gpointer user_data)
{
  GST_META_FLAG_SET (*meta, GST_META_FLAG_POOLED);

  return TRUE;
}

/* the default implementation for preallocating the buffers
 * in the pool */
static gboolean
default_start (GstBufferPool * pool)
{
  guint i;
  GstBufferPoolPrivate *priv = pool->priv;
  GstBufferPoolClass *pclass;

  pclass = GST_BUFFER_POOL_GET_CLASS (pool);

  /* no alloc function, error */
  if (G_UNLIKELY (pclass->alloc_buffer == NULL))
    goto no_alloc;

  /* we need to prealloc buffers */
  for (i = 0; i < priv->min_buffers; i++) {
    GstBuffer *buffer;

    if (pclass->alloc_buffer (pool, &buffer, NULL) != GST_FLOW_OK)
      goto alloc_failed;

    gst_buffer_foreach_meta (buffer, mark_meta_pooled, pool);
    GST_LOG_OBJECT (pool, "prealloced buffer %d: %p", i, buffer);
    /* release to the queue, we call the vmethod directly, we don't need to do
     * the other refcount handling right now. */
    if (G_LIKELY (pclass->release_buffer))
      pclass->release_buffer (pool, buffer);
  }
  return TRUE;

  /* ERRORS */
no_alloc:
  {
    GST_WARNING_OBJECT (pool, "no alloc function");
    return FALSE;
  }
alloc_failed:
  {
    GST_WARNING_OBJECT (pool, "alloc function failed");
    return FALSE;
  }
}

/* must be called with the lock */
static gboolean
do_start (GstBufferPool * pool)
{
  if (!pool->started) {
    GstBufferPoolClass *pclass;

    pclass = GST_BUFFER_POOL_GET_CLASS (pool);

    GST_LOG_OBJECT (pool, "starting");
    /* start the pool, subclasses should allocate buffers and put them
     * in the queue */
    if (G_LIKELY (pclass->start)) {
      if (!pclass->start (pool))
        return FALSE;
    }
    pool->started = TRUE;
  }
  return TRUE;
}


static void
default_free_buffer (GstBufferPool * pool, GstBuffer * buffer)
{
  gst_buffer_unref (buffer);
}

/* must be called with the lock */
static gboolean
default_stop (GstBufferPool * pool)
{
  GstBuffer *buffer;
  GstBufferPoolClass *pclass;

  pclass = GST_BUFFER_POOL_GET_CLASS (pool);

  /* clear the pool */
  while ((buffer = gst_atomic_queue_pop (pool->queue))) {
    GST_LOG_OBJECT (pool, "freeing %p", buffer);
    gst_poll_read_control (pool->poll);

    if (G_LIKELY (pclass->free_buffer))
      pclass->free_buffer (pool, buffer);
  }
  return TRUE;
}

/* must be called with the lock */
static gboolean
do_stop (GstBufferPool * pool)
{
  if (pool->started) {
    GstBufferPoolClass *pclass;

    pclass = GST_BUFFER_POOL_GET_CLASS (pool);

    GST_LOG_OBJECT (pool, "stopping");
    if (G_LIKELY (pclass->stop)) {
      if (!pclass->stop (pool))
        return FALSE;
    }
    pool->started = FALSE;
  }
  return TRUE;
}

/**
 * gst_buffer_pool_set_active:
 * @pool: a #GstBufferPool
 * @active: the new active state
 *
 * Control the active state of @pool. When the pool is active, new calls to
 * gst_buffer_pool_acquire_buffer() will return with GST_FLOW_WRONG_STATE.
 *
 * Activating the bufferpool will preallocate all resources in the pool based on
 * the configuration of the pool.
 *
 * Deactivating will free the resources again when there are no outstanding
 * buffers. When there are outstanding buffers, they will be freed as soon as
 * they are all returned to the pool.
 *
 * Returns: %FALSE when the pool was not configured or when preallocation of the
 * buffers failed.
 */
gboolean
gst_buffer_pool_set_active (GstBufferPool * pool, gboolean active)
{
  gboolean res = TRUE;

  g_return_val_if_fail (GST_IS_BUFFER_POOL (pool), FALSE);

  GST_LOG_OBJECT (pool, "active %d", active);

  GST_BUFFER_POOL_LOCK (pool);
  /* just return if we are already in the right state */
  if (pool->active == active)
    goto was_ok;

  /* we need to be configured */
  if (!pool->configured)
    goto not_configured;

  if (active) {
    if (!do_start (pool))
      goto start_failed;

    /* unset the flushing state now */
    gst_poll_read_control (pool->poll);
    g_atomic_int_set (&pool->flushing, FALSE);
  } else {
    gint outstanding;

    /* set to flushing first */
    g_atomic_int_set (&pool->flushing, TRUE);
    gst_poll_write_control (pool->poll);

    /* when all buffers are in the pool, free them. Else they will be
     * freed when they are released */
    outstanding = g_atomic_int_get (&pool->outstanding);
    GST_LOG_OBJECT (pool, "outstanding buffers %d", outstanding);
    if (outstanding == 0) {
      if (!do_stop (pool))
        goto stop_failed;
    }
  }
  pool->active = active;
  GST_BUFFER_POOL_UNLOCK (pool);

  return res;

was_ok:
  {
    GST_DEBUG_OBJECT (pool, "pool was in the right state");
    GST_BUFFER_POOL_UNLOCK (pool);
    return TRUE;
  }
not_configured:
  {
    GST_ERROR_OBJECT (pool, "pool was not configured");
    GST_BUFFER_POOL_UNLOCK (pool);
    return FALSE;
  }
start_failed:
  {
    GST_ERROR_OBJECT (pool, "start failed");
    GST_BUFFER_POOL_UNLOCK (pool);
    return FALSE;
  }
stop_failed:
  {
    GST_WARNING_OBJECT (pool, "stop failed");
    GST_BUFFER_POOL_UNLOCK (pool);
    return FALSE;
  }
}

/**
 * gst_buffer_pool_is_active:
 * @pool: a #GstBufferPool
 *
 * Check if @pool is active. A pool can be activated with the
 * gst_buffer_pool_set_active() call.
 *
 * Returns: %TRUE when the pool is active.
 */
gboolean
gst_buffer_pool_is_active (GstBufferPool * pool)
{
  gboolean res;

  GST_BUFFER_POOL_LOCK (pool);
  res = pool->active;
  GST_BUFFER_POOL_UNLOCK (pool);

  return res;
}

static gboolean
default_set_config (GstBufferPool * pool, GstStructure * config)
{
  GstBufferPoolPrivate *priv = pool->priv;
  const GstCaps *caps;
  guint size, min_buffers, max_buffers;
  guint prefix, align;

  /* parse the config and keep around */
  if (!gst_buffer_pool_config_get (config, &caps, &size, &min_buffers,
          &max_buffers, &prefix, &align))
    goto wrong_config;

  GST_DEBUG_OBJECT (pool, "config %" GST_PTR_FORMAT, config);

  priv->size = size;
  priv->min_buffers = min_buffers;
  priv->max_buffers = max_buffers;
  priv->prefix = prefix;
  priv->align = align;

  return TRUE;

wrong_config:
  {
    GST_WARNING_OBJECT (pool, "invalid config %" GST_PTR_FORMAT, config);
    return FALSE;
  }
}

/**
 * gst_buffer_pool_set_config:
 * @pool: a #GstBufferPool
 * @config: (transfer full): a #GstStructure
 *
 * Set the configuration of the pool. The pool must be inactive and all buffers
 * allocated form this pool must be returned or else this function will do
 * nothing and return FALSE.
 *
 * @config is a #GstStructure that contains the configuration parameters for
 * the pool. A default and mandatory set of parameters can be configured with
 * gst_buffer_pool_config_set(). This function takes ownership of @config.
 *
 * Returns: TRUE when the configuration could be set.
 */
gboolean
gst_buffer_pool_set_config (GstBufferPool * pool, GstStructure * config)
{
  gboolean result;
  GstBufferPoolClass *pclass;

  g_return_val_if_fail (GST_IS_BUFFER_POOL (pool), FALSE);
  g_return_val_if_fail (config != NULL, FALSE);

  GST_BUFFER_POOL_LOCK (pool);
  /* can't change the settings when active */
  if (pool->active)
    goto was_active;

  /* we can't change when outstanding buffers */
  if (g_atomic_int_get (&pool->outstanding) != 0)
    goto have_outstanding;

  pclass = GST_BUFFER_POOL_GET_CLASS (pool);

  /* set the new config */
  if (G_LIKELY (pclass->set_config))
    result = pclass->set_config (pool, config);
  else
    result = FALSE;

  if (result) {
    if (pool->config)
      gst_structure_free (pool->config);
    pool->config = config;

    /* now we are configured */
    pool->configured = TRUE;
  }
  GST_BUFFER_POOL_UNLOCK (pool);

  return result;

  /* ERRORS */
was_active:
  {
    GST_WARNING_OBJECT (pool, "can't change config, we are active");
    GST_BUFFER_POOL_UNLOCK (pool);
    return FALSE;
  }
have_outstanding:
  {
    GST_WARNING_OBJECT (pool, "can't change config, have outstanding buffers");
    GST_BUFFER_POOL_UNLOCK (pool);
    return FALSE;
  }
}

/**
 * gst_buffer_pool_get_config:
 * @pool: a #GstBufferPool
 *
 * Get a copy of the current configuration of the pool. This configuration
 * can either be modified and used for the gst_buffer_pool_set_config() call
 * or it must be freed after usage.
 *
 * Returns: (transfer full): a copy of the current configuration of @pool. use
 * gst_structure_free() after usage or gst_buffer_pool_set_config().
 */
GstStructure *
gst_buffer_pool_get_config (GstBufferPool * pool)
{
  GstStructure *result;

  g_return_val_if_fail (GST_IS_BUFFER_POOL (pool), NULL);

  GST_BUFFER_POOL_UNLOCK (pool);
  result = gst_structure_copy (pool->config);
  GST_BUFFER_POOL_UNLOCK (pool);

  return result;
}

static const gchar *empty_option[] = { NULL };

/**
 * gst_buffer_pool_get_options:
 * @pool: a #GstBufferPool
 *
 * Get a NULL terminated array of string with supported bufferpool options for
 * @pool. An option would typically be enabled with
 * gst_buffer_pool_config_add_option().
 *
 * Returns: (array zero-terminated=1) (transfer none): a NULL terminated array of strings.
 */
const gchar **
gst_buffer_pool_get_options (GstBufferPool * pool)
{
  GstBufferPoolClass *pclass;
  const gchar **result;

  g_return_val_if_fail (GST_IS_BUFFER_POOL (pool), NULL);

  pclass = GST_BUFFER_POOL_GET_CLASS (pool);

  if (G_LIKELY (pclass->get_options)) {
    if ((result = pclass->get_options (pool)) == NULL)
      goto invalid_result;
  } else
    result = empty_option;

  return result;

  /* ERROR */
invalid_result:
  {
    g_warning ("bufferpool subclass returned NULL options");
    return empty_option;
  }
}

/**
 * gst_buffer_pool_has_option:
 * @pool: a #GstBufferPool
 * @option: an option
 *
 * Check if the bufferpool supports @option.
 *
 * Returns: a NULL terminated array of strings.
 */
gboolean
gst_buffer_pool_has_option (GstBufferPool * pool, const gchar * option)
{
  guint i;
  const gchar **options;

  g_return_val_if_fail (GST_IS_BUFFER_POOL (pool), FALSE);
  g_return_val_if_fail (option != NULL, FALSE);

  options = gst_buffer_pool_get_options (pool);

  for (i = 0; options[i]; i++) {
    if (g_str_equal (options[i], option))
      return TRUE;
  }
  return FALSE;
}

/**
 * gst_buffer_pool_config_set:
 * @config: a #GstBufferPool configuration
 * @caps: caps for the buffers
 * @size: the size of each buffer, not including prefix
 * @min_buffers: the minimum amount of buffers to allocate.
 * @max_buffers: the maximum amount of buffers to allocate or 0 for unlimited.
 * @prefix: prefix each buffer with this many bytes
 * @align: alignment of the buffer data.
 *
 * Configure @config with the given parameters.
 */
void
gst_buffer_pool_config_set (GstStructure * config, const GstCaps * caps,
    guint size, guint min_buffers, guint max_buffers, guint prefix, guint align)
{
  g_return_if_fail (config != NULL);

  gst_structure_id_set (config,
      GST_QUARK (CAPS), GST_TYPE_CAPS, caps,
      GST_QUARK (SIZE), G_TYPE_UINT, size,
      GST_QUARK (MIN_BUFFERS), G_TYPE_UINT, min_buffers,
      GST_QUARK (MAX_BUFFERS), G_TYPE_UINT, max_buffers,
      GST_QUARK (PREFIX), G_TYPE_UINT, prefix,
      GST_QUARK (ALIGN), G_TYPE_UINT, align, NULL);
}

/**
 * gst_buffer_pool_config_add_option:
 * @config: a #GstBufferPool configuration
 * @option: an option to add
 *
 * Enabled the option in @config. This will instruct the @bufferpool to enable
 * the specified option on the buffers that it allocates.
 *
 * The supported options by @pool can be retrieved with gst_buffer_pool_get_options().
 */
void
gst_buffer_pool_config_add_option (GstStructure * config, const gchar * option)
{
  GValueArray *array;
  const GValue *value;
  GValue option_value = { 0 };
  gint i;

  g_return_if_fail (config != NULL);

  value = gst_structure_id_get_value (config, GST_QUARK (OPTIONS));
  if (value) {
    array = (GValueArray *) g_value_get_boxed (value);
  } else {
    GValue new_array_val = { 0, };

    array = g_value_array_new (0);

    g_value_init (&new_array_val, G_TYPE_VALUE_ARRAY);
    g_value_take_boxed (&new_array_val, array);

    gst_structure_id_take_value (config, GST_QUARK (OPTIONS), &new_array_val);
  }
  for (i = 0; i < array->n_values; i++) {
    value = g_value_array_get_nth (array, i);
    if (g_str_equal (option, g_value_get_string (value)))
      return;
  }
  g_value_init (&option_value, G_TYPE_STRING);
  g_value_set_string (&option_value, option);
  g_value_array_append (array, &option_value);
  g_value_unset (&option_value);
}

/**
 * gst_buffer_pool_config_n_options:
 * @config: a #GstBufferPool configuration
 *
 * Retrieve the number of values currently stored in the
 * options array of the @config structure.
 *
 * Returns: the options array size as a #guint.
 */
guint
gst_buffer_pool_config_n_options (GstStructure * config)
{
  GValueArray *array;
  const GValue *value;
  guint size = 0;

  g_return_val_if_fail (config != NULL, 0);

  value = gst_structure_id_get_value (config, GST_QUARK (OPTIONS));
  if (value) {
    array = (GValueArray *) g_value_get_boxed (value);
    size = array->n_values;
  }
  return size;
}

/**
 * gst_buffer_pool_config_get_option:
 * @config: a #GstBufferPool configuration
 * @index: position in the option array to read
 *
 * Parse an available @config and get the option
 * at @index of the options API array.
 *
 * Returns: a #gchar of the option at @index.
 */
const gchar *
gst_buffer_pool_config_get_option (GstStructure * config, guint index)
{
  const GValue *value;
  const gchar *ret = NULL;

  g_return_val_if_fail (config != NULL, 0);

  value = gst_structure_id_get_value (config, GST_QUARK (OPTIONS));
  if (value) {
    GValueArray *array;
    GValue *option_value;

    array = (GValueArray *) g_value_get_boxed (value);
    option_value = g_value_array_get_nth (array, index);

    if (option_value)
      ret = g_value_get_string (option_value);
  }
  return ret;
}

/**
 * gst_buffer_pool_config_has_option:
 * @config: a #GstBufferPool configuration
 * @option: an option
 *
 * Check if @config contains @option
 *
 * Returns: TRUE if the options array contains @option.
 */
gboolean
gst_buffer_pool_config_has_option (GstStructure * config, const gchar * option)
{
  const GValue *value;

  g_return_val_if_fail (config != NULL, 0);

  value = gst_structure_id_get_value (config, GST_QUARK (OPTIONS));
  if (value) {
    GValueArray *array;
    GValue *option_value;
    gint i;

    array = (GValueArray *) g_value_get_boxed (value);
    for (i = 0; i < array->n_values; i++) {
      option_value = g_value_array_get_nth (array, i);
      if (g_str_equal (option, g_value_get_string (option_value)))
        return TRUE;
    }
  }
  return FALSE;
}

/**
 * gst_buffer_pool_config_get:
 * @config: (transfer none): a #GstBufferPool configuration
 * @caps: (out): the caps of buffers
 * @size: (out): the size of each buffer, not including prefix
 * @min_buffers: (out): the minimum amount of buffers to allocate.
 * @max_buffers: (out): the maximum amount of buffers to allocate or 0 for unlimited.
 * @prefix: (out): prefix each buffer with this many bytes
 * @align: (out): alignment of the buffer data.
 *
 * Get the configuration values from @config.
 */
gboolean
gst_buffer_pool_config_get (GstStructure * config, const GstCaps ** caps,
    guint * size, guint * min_buffers, guint * max_buffers, guint * prefix,
    guint * align)
{
  g_return_val_if_fail (config != NULL, FALSE);

  return gst_structure_id_get (config,
      GST_QUARK (CAPS), GST_TYPE_CAPS, caps,
      GST_QUARK (SIZE), G_TYPE_UINT, size,
      GST_QUARK (MIN_BUFFERS), G_TYPE_UINT, min_buffers,
      GST_QUARK (MAX_BUFFERS), G_TYPE_UINT, max_buffers,
      GST_QUARK (PREFIX), G_TYPE_UINT, prefix,
      GST_QUARK (ALIGN), G_TYPE_UINT, align, NULL);
}

static GstFlowReturn
default_acquire_buffer (GstBufferPool * pool, GstBuffer ** buffer,
    GstBufferPoolParams * params)
{
  GstFlowReturn result;
  GstBufferPoolClass *pclass;
  GstBufferPoolPrivate *priv = pool->priv;

  pclass = GST_BUFFER_POOL_GET_CLASS (pool);

  while (TRUE) {
    if (G_UNLIKELY (g_atomic_int_get (&pool->flushing)))
      goto flushing;

    /* try to get a buffer from the queue */
    *buffer = gst_atomic_queue_pop (pool->queue);
    if (G_LIKELY (*buffer)) {
      gst_poll_read_control (pool->poll);
      result = GST_FLOW_OK;
      GST_LOG_OBJECT (pool, "acquired buffer %p", *buffer);
      break;
    }

    /* no buffer */
    if (priv->max_buffers == 0) {
      /* no max_buffers, we allocate some more */
      if (G_LIKELY (pclass->alloc_buffer)) {
        result = pclass->alloc_buffer (pool, buffer, params);
        if (result == GST_FLOW_OK && *buffer)
          gst_buffer_foreach_meta (*buffer, mark_meta_pooled, pool);
        else
          result = GST_FLOW_ERROR;
      } else
        result = GST_FLOW_NOT_SUPPORTED;
      GST_LOG_OBJECT (pool, "alloc buffer %p", *buffer);
      break;
    }

    /* check if we need to wait */
    if (params && (params->flags & GST_BUFFER_POOL_FLAG_DONTWAIT)) {
      GST_LOG_OBJECT (pool, "no more buffers");
      result = GST_FLOW_EOS;
      break;
    }

    /* now wait */
    GST_LOG_OBJECT (pool, "waiting for free buffers");
    gst_poll_wait (pool->poll, GST_CLOCK_TIME_NONE);
  }

  return result;

  /* ERRORS */
flushing:
  {
    GST_DEBUG_OBJECT (pool, "we are flushing");
    return GST_FLOW_WRONG_STATE;
  }
}

static inline void
dec_outstanding (GstBufferPool * pool)
{
  if (g_atomic_int_dec_and_test (&pool->outstanding)) {
    /* all buffers are returned to the pool, see if we need to free them */
    if (g_atomic_int_get (&pool->flushing)) {
      /* take the lock so that set_active is not run concurrently */
      GST_BUFFER_POOL_LOCK (pool);
      /* recheck the flushing state in the lock, the pool could have been
       * set to active again */
      if (g_atomic_int_get (&pool->flushing))
        do_stop (pool);

      GST_BUFFER_POOL_UNLOCK (pool);
    }
  }
}

static gboolean
remove_meta_unpooled (GstBuffer * buffer, GstMeta ** meta, gpointer user_data)
{
  if (!GST_META_FLAG_IS_SET (*meta, GST_META_FLAG_POOLED))
    *meta = NULL;
  return TRUE;
}

static void
default_reset_buffer (GstBufferPool * pool, GstBuffer * buffer,
    GstBufferPoolParams * params)
{
  GST_BUFFER_FLAGS (buffer) = 0;

  GST_BUFFER_PTS (buffer) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DTS (buffer) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION (buffer) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_OFFSET (buffer) = GST_BUFFER_OFFSET_NONE;
  GST_BUFFER_OFFSET_END (buffer) = GST_BUFFER_OFFSET_NONE;

  /* remove all metadata without the POOLED flag */
  gst_buffer_foreach_meta (buffer, remove_meta_unpooled, pool);
}

/**
 * gst_buffer_pool_acquire_buffer:
 * @pool: a #GstBufferPool
 * @buffer: (out): a location for a #GstBuffer
 * @params: (transfer none) (allow-none) parameters.
 *
 * Acquire a buffer from @pool. @buffer should point to a memory location that
 * can hold a pointer to the new buffer.
 *
 * @params can be NULL or contain optional parameters to influence the allocation.
 *
 * Returns: a #GstFlowReturn such as GST_FLOW_WRONG_STATE when the pool is
 * inactive.
 */
GstFlowReturn
gst_buffer_pool_acquire_buffer (GstBufferPool * pool, GstBuffer ** buffer,
    GstBufferPoolParams * params)
{
  GstBufferPoolClass *pclass;
  GstFlowReturn result;

  g_return_val_if_fail (GST_IS_BUFFER_POOL (pool), GST_FLOW_ERROR);
  g_return_val_if_fail (buffer != NULL, GST_FLOW_ERROR);

  pclass = GST_BUFFER_POOL_GET_CLASS (pool);

  /* assume we'll have one more outstanding buffer we need to do that so
   * that concurrent set_active doesn't clear the buffers */
  g_atomic_int_inc (&pool->outstanding);

  if (G_LIKELY (pclass->acquire_buffer))
    result = pclass->acquire_buffer (pool, buffer, params);
  else
    result = GST_FLOW_NOT_SUPPORTED;

  if (G_LIKELY (result == GST_FLOW_OK)) {
    /* all buffers from the pool point to the pool and have the refcount of the
     * pool incremented */
    (*buffer)->pool = gst_object_ref (pool);
    /* now reset the buffer when needed */
    if (G_LIKELY (pclass->reset_buffer))
      pclass->reset_buffer (pool, *buffer, params);
  } else {
    dec_outstanding (pool);
  }

  return result;
}

static void
default_release_buffer (GstBufferPool * pool, GstBuffer * buffer)
{
  /* keep it around in our queue */
  GST_LOG_OBJECT (pool, "released buffer %p", buffer);
  gst_atomic_queue_push (pool->queue, buffer);
  gst_poll_write_control (pool->poll);
}

/**
 * gst_buffer_pool_release_buffer:
 * @pool: a #GstBufferPool
 * @buffer: (transfer none): a #GstBuffer
 *
 * Release @buffer to @pool. @buffer should have previously been allocated from
 * @pool with gst_buffer_pool_acquire_buffer().
 *
 * This function is usually called automatically when the last ref on @buffer
 * disappears.
 */
void
gst_buffer_pool_release_buffer (GstBufferPool * pool, GstBuffer * buffer)
{
  GstBufferPoolClass *pclass;

  g_return_if_fail (GST_IS_BUFFER_POOL (pool));
  g_return_if_fail (buffer != NULL);

  /* check that the buffer is ours, all buffers returned to the pool have the
   * pool member set to NULL and the pool refcount decreased */
  if (!G_ATOMIC_POINTER_COMPARE_AND_EXCHANGE (&buffer->pool, pool, NULL))
    return;

  pclass = GST_BUFFER_POOL_GET_CLASS (pool);

  if (G_LIKELY (pclass->release_buffer))
    pclass->release_buffer (pool, buffer);

  dec_outstanding (pool);

  /* decrease the refcount that the buffer had to us */
  gst_object_unref (pool);
}