/* GStreamer
 * Copyright (C) 2006 Stefan Kost <ensonic@users.sf.net>
 *
 * gstpreset.c: helper interface for element presets
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
 * SECTION:gstpreset
 * @short_description: helper interface for element presets
 *
 * This interface offers methods to query and manipulate parameter preset sets.
 * A preset is a bunch of property settings, together with meta data and a name.
 * The name of a preset serves as key for subsequent method calls to manipulate
 * single presets.
 * All instances of one type will share the list of presets. The list is created
 * on demand, if presets are not used, the list is not created.
 *
 * The interface comes with a default implementation that serves most plugins.
 * Wrapper plugins will override most methods to implement support for the
 * native preset format of those wrapped plugins.
 * One method that is useful to be overridden is gst_preset_get_property_names().
 * With that one can control which properties are saved and in which order.
 */
/* FIXME:
 * - non racyness
 *   - we need to avoid two instances writing the preset file
 *     -> flock(fileno()), http://www.ecst.csuchico.edu/~beej/guide/ipc/flock.html
 *     -> open exclusive
 *     -> better save the new file to a tempfile and then rename?
 *   - we like to know when any other instance makes changes to the keyfile
 *     - then ui can be updated
 *     - and we make sure that we don't lose edits
 *   -> its the same problem actually, once for inside a process, once system-
 *      wide
 *     - can we use a lock inside a names shared memory segment?
 *
 * - need to add support for GstChildProxy
 *   we can do this in a next iteration, the format is flexible enough
 *   http://www.buzztard.org/index.php/Preset_handling_interface
 *
 * - should there be a 'preset-list' property to get the preset list
 *   (and to connect a notify:: to to listen for changes)
 *   we could use gnome_vfs_monitor_add() to monitor the user preset_file.
 *
 * - should there be a 'preset-name' property so that we can set a preset via
 *   gst-launch, or should we handle this with special syntax in gst-launch:
 *   gst-launch element preset:<preset-name> property=value ...
 *   - this would alloow to hanve preset-bundles too (a preset on bins that
 *     specifies presets for children
 *
 * - GstChildProxy suport
 *   - if we stick with GParamSpec **_list_properties()
 *     we need to use g_param_spec_set_qdata() to specify the instance on each GParamSpec
 *     OBJECT_LOCK(obj);  // ChildProxy needs GstIterator support
 *     num=gst_child_proxy_get_children_count(obj);
 *     for(i=0;i<num;i++) {
 *       child=gst_child_proxy_get_child_by_index(obj,i);
 *       // v1 ----
 *       g_object_class_list_properties(child,&num);
 *       // foreach prop
 *       //   g_param_spec_set_qdata(prop, quark, (gpointer)child);
 *       //   add to result
 *       // v2 ----
 *       // children have to implement preset-iface too tag the returned GParamSpec* with the owner
 *       props=gst_preset_list_properties(child);
 *       // add props to result
 *     }
 *     OBJECT_UNLOCK(obj);
 *
 */

#include "gst_private.h"

#include "gstpreset.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <glib/gstdio.h>

#define GST_CAT_DEFAULT preset_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

/* defines for keyfile usage, this group contains the element type name and
 * version these presets belong to. */
#define PRESET_HEADER "_presets_"

/* keys of the preset header section */
#define PRESET_HEADER_ELEMENT_NAME "element-name"
#define PRESET_HEADER_VERSION "version"

static GQuark preset_user_path_quark = 0;
static GQuark preset_system_path_quark = 0;
static GQuark preset_quark = 0;

/*static GQuark property_list_quark = 0;*/

/* default iface implementation */

static gboolean gst_preset_default_save_presets_file (GstPreset * preset);

/*
 * preset_get_paths:
 * @preset: a #GObject that implements #GstPreset
 * @preset_user_path: location for path or %NULL
 * @preset_system_path: location for path or %NULL
 *
 * Fetch the preset_path for user local and system wide settings. Don't free
 * after use.
 *
 * Returns: %FALSE if no paths could be found.
 */
static gboolean
preset_get_paths (GstPreset * preset, const gchar ** preset_user_path,
    const gchar ** preset_system_path)
{
  GType type = G_TYPE_FROM_INSTANCE (preset);
  gchar *preset_path;
  const gchar *element_name;

  /* we use the element name when we must contruct the paths */
  element_name = G_OBJECT_TYPE_NAME (preset);
  GST_INFO_OBJECT (preset, "element_name: '%s'", element_name);

  if (preset_user_path) {
    /* preset user path requested, see if we have it cached in the qdata */
    if (!(preset_path = g_type_get_qdata (type, preset_user_path_quark))) {
      gchar *preset_dir;

      /* user presets go in '$HOME/.gstreamer-0.10/presets/GstSimSyn.prs' */
      preset_dir = g_build_filename (g_get_home_dir (),
          ".gstreamer-" GST_MAJORMINOR, "presets", NULL);
      GST_INFO_OBJECT (preset, "user_preset_dir: '%s'", preset_dir);
      preset_path =
          g_strdup_printf ("%s" G_DIR_SEPARATOR_S "%s.prs", preset_dir,
          element_name);
      GST_INFO_OBJECT (preset, "user_preset_path: '%s'", preset_path);
      /* create dirs */
      g_mkdir_with_parents (preset_dir, 0755);
      g_free (preset_dir);

      /* cache the preset path to the type */
      g_type_set_qdata (type, preset_user_path_quark, preset_path);
    }
    *preset_user_path = preset_path;
  }

  if (preset_system_path) {
    /* preset system path requested, see if we have it cached in the qdata */
    if (!(preset_path = g_type_get_qdata (type, preset_system_path_quark))) {
      gchar *preset_dir;

      /* system presets in '$GST_DATADIR/gstreamer-0.10/presets/GstAudioPanorama.prs' */
      preset_dir = g_build_filename (GST_DATADIR, "gstreamer-" GST_MAJORMINOR,
          "presets", NULL);
      GST_INFO_OBJECT (preset, "system_preset_dir: '%s'", preset_dir);
      preset_path = g_strdup_printf ("%s" G_DIR_SEPARATOR_S "%s.prs",
          preset_dir, element_name);
      GST_INFO_OBJECT (preset, "system_preset_path: '%s'", preset_path);
      /* create dirs */
      g_mkdir_with_parents (preset_dir, 0755);
      g_free (preset_dir);

      /* cache the preset path to the type */
      g_type_set_qdata (type, preset_system_path_quark, preset_path);
    }
    *preset_system_path = preset_path;
  }
  return TRUE;
}

static gboolean
preset_skip_property (GParamSpec * property)
{
  if (((property->flags & G_PARAM_READWRITE) != G_PARAM_READWRITE) ||
      (property->flags & G_PARAM_CONSTRUCT_ONLY))
    return TRUE;
  /* FIXME: skip GST_PARAM_NOT_PRESETABLE, see #522205 */
  return FALSE;
}

/* caller must free @preset_version after use */
static GKeyFile *
preset_open_and_parse_header (GstPreset * preset, const gchar * preset_path,
    gchar ** preset_version)
{
  GKeyFile *in;
  GError *error = NULL;
  gboolean res;
  const gchar *element_name;
  gchar *name;

  in = g_key_file_new ();

  res = g_key_file_load_from_file (in, preset_path,
      G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, &error);
  if (!res || error != NULL)
    goto load_error;

  /* element type name and preset name must match or we are dealing with a wrong
   * preset file */
  element_name = G_OBJECT_TYPE_NAME (preset);
  name =
      g_key_file_get_value (in, PRESET_HEADER, PRESET_HEADER_ELEMENT_NAME,
      NULL);

  if (!name || strcmp (name, element_name))
    goto wrong_name;

  g_free (name);

  /* get the version now so that the caller can check it */
  if (preset_version)
    *preset_version =
        g_key_file_get_value (in, PRESET_HEADER, PRESET_HEADER_VERSION, NULL);

  return in;

  /* ERRORS */
load_error:
  {
    GST_WARNING_OBJECT (preset, "Unable to read preset file %s: %s",
        preset_path, error->message);
    g_error_free (error);
    g_key_file_free (in);
    return NULL;
  }
wrong_name:
  {
    GST_WARNING_OBJECT (preset,
        "Wrong element name in preset file %s. Expected %s, got %s",
        preset_path, element_name, GST_STR_NULL (name));
    g_free (name);
    g_key_file_free (in);
    return NULL;
  }
}

static guint64
preset_parse_version (const gchar * str_version)
{
  gint major, minor, micro, nano, num;

  major = minor = micro = nano = 0;

  /* parse version (e.g. 0.10.15.1) to guint64 */
  num = sscanf (str_version, "%d.%d.%d.%d", &major, &minor, &micro, &nano);
  /* make sure we have atleast "major.minor" */
  if (num > 1) {
    guint64 version;

    version = ((((major << 8 | minor) << 8) | micro) << 8) | nano;
    GST_DEBUG ("version %s -> %" G_GUINT64_FORMAT, str_version, version);
    return version;
  }
  return G_GUINT64_CONSTANT (0);
}

static void
preset_merge (GKeyFile * system, GKeyFile * user)
{
  gchar *str;
  gchar **groups, **keys;
  gsize i, j, num_groups, num_keys;

  /* copy file comment if there is any */
  if ((str = g_key_file_get_comment (user, NULL, NULL, NULL))) {
    g_key_file_set_comment (system, NULL, NULL, str, NULL);
    g_free (str);
  }

  /* get groups in user and copy into system */
  groups = g_key_file_get_groups (user, &num_groups);
  for (i = 0; i < num_groups; i++) {
    /* copy group comment if there is any */
    if ((str = g_key_file_get_comment (user, groups[i], NULL, NULL))) {
      g_key_file_set_comment (system, groups[i], NULL, str, NULL);
      g_free (str);
    }

    /* ignore private groups */
    if (groups[i][0] == '_')
      continue;

    /* if group already exists in system, remove and re-add keys from user */
    if (g_key_file_has_group (system, groups[i])) {
      g_key_file_remove_group (system, groups[i], NULL);
    }

    keys = g_key_file_get_keys (user, groups[i], &num_keys, NULL);
    for (j = 0; j < num_keys; j++) {
      /* copy key comment if there is any */
      if ((str = g_key_file_get_comment (user, groups[i], keys[j], NULL))) {
        g_key_file_set_comment (system, groups[i], keys[j], str, NULL);
        g_free (str);
      }
      str = g_key_file_get_value (user, groups[i], keys[j], NULL);
      g_key_file_set_value (system, groups[i], keys[j], str);
      g_free (str);
    }
    g_strfreev (keys);
  }
  g_strfreev (groups);
}

/* reads the user and system presets files and merges them together. This
 * function caches the GKeyFile on the element type. If there is no existing
 * preset file, a new in-memory GKeyFile will be created. */
static GKeyFile *
preset_get_keyfile (GstPreset * preset)
{
  GKeyFile *presets;
  GType type = G_TYPE_FROM_INSTANCE (preset);

  /* first see if the have a cached version for the type */
  if (!(presets = g_type_get_qdata (type, preset_quark))) {
    const gchar *preset_user_path, *preset_system_path;
    gchar *str_version_user = NULL, *str_version_system = NULL;
    gboolean updated_from_system = FALSE;
    GKeyFile *in_user, *in_system;

    preset_get_paths (preset, &preset_user_path, &preset_system_path);

    /* try to load the user and system presets, we do this to get the versions
     * of both files. */
    in_user = preset_open_and_parse_header (preset, preset_user_path,
        &str_version_user);
    in_system = preset_open_and_parse_header (preset, preset_system_path,
        &str_version_system);

    /* compare version to check for merge */
    if (in_system) {
      /* keep system presets if there is no user preset or when the system
       * version is higher than the user version. */
      if (!in_user) {
        presets = in_system;
      } else if (preset_parse_version (str_version_system) >
          preset_parse_version (str_version_user)) {
        presets = in_system;
        updated_from_system = TRUE;
      }
    }
    if (in_user) {
      if (updated_from_system) {
        /* merge user on top of system presets */
        preset_merge (presets, in_user);
        g_key_file_free (in_user);
      } else {
        /* keep user presets */
        presets = in_user;
      }
    }
    if (!in_user && !in_system) {
      /* we did not load a user or system presets file, create a new one */
      presets = g_key_file_new ();
      g_key_file_set_string (presets, PRESET_HEADER, PRESET_HEADER_ELEMENT_NAME,
          G_OBJECT_TYPE_NAME (preset));
    }

    g_free (str_version_user);
    g_free (str_version_system);

    /* attach the preset to the type */
    g_type_set_qdata (type, preset_quark, (gpointer) presets);

    if (updated_from_system) {
      gst_preset_default_save_presets_file (preset);
    }
  }
  return presets;
}

/* get a list of all supported preset names for an element */
static gchar **
gst_preset_default_get_preset_names (GstPreset * preset)
{
  GKeyFile *presets;
  gsize i, num_groups;
  gchar **groups;

  /* get the presets from the type */
  if (!(presets = preset_get_keyfile (preset)))
    goto no_presets;

  /* get the groups, which are also the preset names */
  if (!(groups = g_key_file_get_groups (presets, &num_groups)))
    goto no_groups;

  /* remove all private group names starting with '_' from the array */
  for (i = 0; i < num_groups; i++) {
    if (groups[i][0] == '_') {
      /* free private group */
      g_free (groups[i]);
      /* move last element of list down */
      num_groups--;
      /* move last element into removed element */
      groups[i] = groups[num_groups];
      groups[num_groups] = NULL;
    }
  }
  /* sort the array now */
  g_qsort_with_data (groups, num_groups, sizeof (gchar *),
      (GCompareDataFunc) strcmp, NULL);

  return groups;

  /* ERRORS */
no_presets:
  {
    GST_WARNING_OBJECT (preset, "Could not load presets");
    return NULL;
  }
no_groups:
  {
    GST_WARNING_OBJECT (preset, "Could not find preset groups");
    return NULL;
  }
}

/* get a list of all property names that are used for presets */
static gchar **
gst_preset_default_get_property_names (GstPreset * preset)
{
  GParamSpec **props;
  guint i, j, n_props;
  GObjectClass *gclass;
  gchar **result;

  gclass = G_OBJECT_CLASS (GST_ELEMENT_GET_CLASS (preset));

  /* get a list of normal properties. 
   * FIXME, change this for childproxy support. */
  props = g_object_class_list_properties (gclass, &n_props);
  if (!props)
    goto no_properties;

  /* allocate array big enough to hold the worst case, including a terminating
   * NULL pointer. */
  result = g_new (gchar *, n_props + 1);

  /* now filter out the properties that we can use for presets */
  GST_DEBUG_OBJECT (preset, "  filtering properties: %u", n_props);
  for (i = j = 0; i < n_props; i++) {
    if (preset_skip_property (props[i]))
      continue;

    /* copy and increment out pointer */
    result[j++] = g_strdup (props[i]->name);
  }
  result[j] = NULL;
  g_free (props);

  return result;

  /* ERRORS */
no_properties:
  {
    GST_INFO_OBJECT (preset, "object has no properties");
    return NULL;
  }
}

/* load the presets of @name for the instance @preset. Returns %FALSE if something
 * failed. */
static gboolean
gst_preset_default_load_preset (GstPreset * preset, const gchar * name)
{
  GKeyFile *presets;
  gchar **props;
  guint i;
  GObjectClass *gclass;

  /* get the presets from the type */
  if (!(presets = preset_get_keyfile (preset)))
    goto no_presets;

  /* get the preset name */
  if (!g_key_file_has_group (presets, name))
    goto no_group;

  GST_DEBUG_OBJECT (preset, "loading preset : '%s'", name);

  /* get the properties that we can configure in this element */
  if (!(props = gst_preset_get_property_names (preset)))
    goto no_properties;

  gclass = G_OBJECT_CLASS (GST_ELEMENT_GET_CLASS (preset));

  /* for each of the property names, find the preset parameter and try to
   * configure the property with its value */
  for (i = 0; props[i]; i++) {
    gchar *str;
    GValue gvalue = { 0, };
    GParamSpec *property;

    /* check if we have a settings for this element property */
    if (!(str = g_key_file_get_value (presets, name, props[i], NULL))) {
      /* the element has a property but the parameter is not in the keyfile */
      GST_WARNING_OBJECT (preset, "parameter '%s' not in preset", props[i]);
      continue;
    }

    GST_DEBUG_OBJECT (preset, "setting value '%s' for property '%s'", str,
        props[i]);

    /* FIXME, change for childproxy to get the property and element.  */
    if (!(property = g_object_class_find_property (gclass, props[i]))) {
      /* the parameter was in the keyfile, the element said it supported it but
       * then the property was not found in the element. This should not happen. */
      GST_WARNING_OBJECT (preset, "property '%s' not in object", props[i]);
      g_free (str);
      continue;
    }

    /* try to deserialize the property value from the keyfile and set it as
     * the object property */
    g_value_init (&gvalue, property->value_type);
    if (gst_value_deserialize (&gvalue, str)) {
      /* FIXME, change for childproxy support */
      g_object_set_property (G_OBJECT (preset), props[i], &gvalue);
    } else {
      GST_WARNING_OBJECT (preset,
          "deserialization of value '%s' for property '%s' failed", str,
          props[i]);
    }
    g_value_unset (&gvalue);
    g_free (str);
  }
  g_strfreev (props);

  return TRUE;

  /* ERRORS */
no_presets:
  {
    GST_WARNING_OBJECT (preset, "no presets");
    return FALSE;
  }
no_group:
  {
    GST_WARNING_OBJECT (preset, "no preset named '%s'", name);
    return FALSE;
  }
no_properties:
  {
    GST_INFO_OBJECT (preset, "no properties");
    return FALSE;
  }
}

/* save the presets file. A copy of the existing presets file is stored in a
 * .bak file */
static gboolean
gst_preset_default_save_presets_file (GstPreset * preset)
{
  GKeyFile *presets;
  const gchar *preset_path;
  GError *error = NULL;
  gchar *bak_file_name;
  gboolean backup = TRUE;
  gchar *data;
  gsize data_size;

  preset_get_paths (preset, &preset_path, NULL);

  /* get the presets from the type */
  if (!(presets = preset_get_keyfile (preset)))
    goto no_presets;

  GST_DEBUG_OBJECT (preset, "saving preset file: '%s'", preset_path);

  /* create backup if possible */
  bak_file_name = g_strdup_printf ("%s.bak", preset_path);
  if (g_file_test (bak_file_name, G_FILE_TEST_EXISTS)) {
    if (g_unlink (bak_file_name)) {
      backup = FALSE;
      GST_INFO_OBJECT (preset, "cannot remove old backup file : %s",
          bak_file_name);
    }
  }
  if (backup) {
    if (g_rename (preset_path, bak_file_name)) {
      GST_INFO_OBJECT (preset, "cannot backup file : %s -> %s", preset_path,
          bak_file_name);
    }
  }
  g_free (bak_file_name);

  /* update gstreamer version */
  g_key_file_set_string (presets, PRESET_HEADER, PRESET_HEADER_VERSION,
      PACKAGE_VERSION);

  /* get new contents, wee need this to save it */
  if (!(data = g_key_file_to_data (presets, &data_size, &error)))
    goto convert_failed;

  /* write presets */
  if (!g_file_set_contents (preset_path, data, data_size, &error))
    goto write_failed;

  g_free (data);

  return TRUE;

  /* ERRORS */
no_presets:
  {
    GST_WARNING_OBJECT (preset,
        "no presets, trying to unlink possibly existing preset file: '%s'",
        preset_path);
    g_unlink (preset_path);
    return FALSE;
  }
convert_failed:
  {
    GST_WARNING_OBJECT (preset, "can not get the keyfile contents: %s",
        error->message);
    g_error_free (error);
    g_free (data);
    return FALSE;
  }
write_failed:
  {
    GST_WARNING_OBJECT (preset, "Unable to store preset file %s: %s",
        preset_path, error->message);
    g_error_free (error);
    g_free (data);
    return FALSE;
  }
}

/* save the preset with the given name */
static gboolean
gst_preset_default_save_preset (GstPreset * preset, const gchar * name)
{
  GKeyFile *presets;
  gchar **props;
  guint i;
  GObjectClass *gclass;

  GST_INFO_OBJECT (preset, "saving new preset: %s", name);

  /* get the presets from the type */
  if (!(presets = preset_get_keyfile (preset)))
    goto no_presets;

  /* take copies of current gobject properties from preset */
  if (!(props = gst_preset_get_property_names (preset)))
    goto no_properties;

  gclass = G_OBJECT_CLASS (GST_ELEMENT_GET_CLASS (preset));

  /* loop over the object properties and store the property value in the
   * keyfile */
  for (i = 0; props[i]; i++) {
    GValue gvalue = { 0, };
    gchar *str;
    GParamSpec *property;

    /* FIXME, change for childproxy to get the property and element.  */
    if (!(property = g_object_class_find_property (gclass, props[i]))) {
      /* the element said it supported the property but then it does not have
       * that property. This should not happen. */
      GST_WARNING_OBJECT (preset, "property '%s' not in object", props[i]);
      continue;
    }

    g_value_init (&gvalue, property->value_type);
    /* FIXME, change for childproxy */
    g_object_get_property (G_OBJECT (preset), props[i], &gvalue);

    if ((str = gst_value_serialize (&gvalue))) {
      g_key_file_set_string (presets, name, props[i], (gpointer) str);
      g_free (str);
    } else {
      GST_WARNING_OBJECT (preset, "serialization for property '%s' failed",
          props[i]);
    }
    g_value_unset (&gvalue);
  }
  GST_INFO_OBJECT (preset, "  saved");
  g_strfreev (props);

  /* save updated version */
  return gst_preset_default_save_presets_file (preset);

  /* ERRORS */
no_presets:
  {
    GST_WARNING_OBJECT (preset, "no presets");
    return FALSE;
  }
no_properties:
  {
    GST_INFO_OBJECT (preset, "no properties");
    return FALSE;
  }
}

/* copies all keys and comments from one group to another, deleting the old
 * group. */
static gboolean
gst_preset_default_rename_preset (GstPreset * preset, const gchar * old_name,
    const gchar * new_name)
{
  GKeyFile *presets;
  gchar *str;
  gchar **keys;
  gsize i, num_keys;

  /* get the presets from the type */
  if (!(presets = preset_get_keyfile (preset)))
    goto no_presets;

  if (!g_key_file_has_group (presets, old_name))
    goto no_group;

  /* copy group comment if there is any */
  if ((str = g_key_file_get_comment (presets, old_name, NULL, NULL))) {
    g_key_file_set_comment (presets, new_name, NULL, str, NULL);
    g_free (str);
  }

  /* get all keys from the old group and copy them in the new group */
  keys = g_key_file_get_keys (presets, old_name, &num_keys, NULL);
  for (i = 0; i < num_keys; i++) {
    /* copy key comment if there is any */
    if ((str = g_key_file_get_comment (presets, old_name, keys[i], NULL))) {
      g_key_file_set_comment (presets, new_name, keys[i], str, NULL);
      g_free (str);
    }
    /* copy key value */
    str = g_key_file_get_value (presets, old_name, keys[i], NULL);
    g_key_file_set_value (presets, new_name, keys[i], str);
    g_free (str);
  }
  g_strfreev (keys);

  /* remove old group */
  g_key_file_remove_group (presets, old_name, NULL);

  /* save updated version */
  return gst_preset_default_save_presets_file (preset);

  /* ERRORS */
no_presets:
  {
    GST_WARNING_OBJECT (preset, "no presets");
    return FALSE;
  }
no_group:
  {
    GST_WARNING_OBJECT (preset, "no preset named %s", old_name);
    return FALSE;
  }
}

/* delete a group from the keyfile */
static gboolean
gst_preset_default_delete_preset (GstPreset * preset, const gchar * name)
{
  GKeyFile *presets;

  /* get the presets from the type */
  if (!(presets = preset_get_keyfile (preset)))
    goto no_presets;

  /* get the group */
  if (!g_key_file_has_group (presets, name))
    goto no_group;

  /* remove the group */
  g_key_file_remove_group (presets, name, NULL);

  /* save updated version */
  return gst_preset_default_save_presets_file (preset);

  /* ERRORS */
no_presets:
  {
    GST_WARNING_OBJECT (preset, "no presets");
    return FALSE;
  }
no_group:
  {
    GST_WARNING_OBJECT (preset, "no preset named %s", name);
    return FALSE;
  }
}

static gboolean
gst_preset_default_set_meta (GstPreset * preset, const gchar * name,
    const gchar * tag, const gchar * value)
{
  GKeyFile *presets;
  gchar *key;

  /* get the presets from the type */
  if (!(presets = preset_get_keyfile (preset)))
    goto no_presets;

  key = g_strdup_printf ("_meta/%s", tag);
  if (value && *value) {
    g_key_file_set_value (presets, name, key, value);
  } else {
    g_key_file_remove_key (presets, name, key, NULL);
  }
  g_free (key);

  /* save updated keyfile */
  return gst_preset_default_save_presets_file (preset);

  /* ERRORS */
no_presets:
  {
    GST_WARNING_OBJECT (preset, "no presets");
    return FALSE;
  }
}

/* the caller must free @value after usage */
static gboolean
gst_preset_default_get_meta (GstPreset * preset, const gchar * name,
    const gchar * tag, gchar ** value)
{
  GKeyFile *presets;
  gchar *key;

  /* get the presets from the type */
  if (!(presets = preset_get_keyfile (preset)))
    goto no_presets;

  key = g_strdup_printf ("_meta/%s", tag);
  *value = g_key_file_get_value (presets, name, key, NULL);
  g_free (key);

  return TRUE;

  /* ERRORS */
no_presets:
  {
    GST_WARNING_OBJECT (preset, "no presets");
    *value = NULL;
    return FALSE;
  }
}

/* wrapper */

/**
 * gst_preset_get_preset_names:
 * @preset: a #GObject that implements #GstPreset
 *
 * Get a copy of preset names as a NULL terminated string array.
 *
 * Returns: list with names, ue g_strfreev() after usage.
 *
 * Since: 0.10.20
 */
gchar **
gst_preset_get_preset_names (GstPreset * preset)
{
  g_return_val_if_fail (GST_IS_PRESET (preset), NULL);

  return (GST_PRESET_GET_INTERFACE (preset)->get_preset_names (preset));
}

/**
 * gst_preset_get_property_names:
 * @preset: a #GObject that implements #GstPreset
 *
 * Get a the names of the GObject properties that can be used for presets.
 *
 * Returns: an array of property names which should be freed with g_strfreev() after use.
 *
 * Since: 0.10.20
 */
gchar **
gst_preset_get_property_names (GstPreset * preset)
{
  g_return_val_if_fail (GST_IS_PRESET (preset), NULL);

  return (GST_PRESET_GET_INTERFACE (preset)->get_property_names (preset));
}

/**
 * gst_preset_load_preset:
 * @preset: a #GObject that implements #GstPreset
 * @name: preset name to load
 *
 * Load the given preset.
 *
 * Returns: %TRUE for success, %FALSE if e.g. there is no preset with that @name
 *
 * Since: 0.10.20
 */
gboolean
gst_preset_load_preset (GstPreset * preset, const gchar * name)
{
  g_return_val_if_fail (GST_IS_PRESET (preset), FALSE);
  g_return_val_if_fail (name, FALSE);

  return (GST_PRESET_GET_INTERFACE (preset)->load_preset (preset, name));
}

/**
 * gst_preset_save_preset:
 * @preset: a #GObject that implements #GstPreset
 * @name: preset name to save
 *
 * Save the current object settings as a preset under the given name. If there
 * is already a preset by this @name it will be overwritten.
 *
 * Returns: %TRUE for success, %FALSE
 *
 * Since: 0.10.20
 */
gboolean
gst_preset_save_preset (GstPreset * preset, const gchar * name)
{
  g_return_val_if_fail (GST_IS_PRESET (preset), FALSE);
  g_return_val_if_fail (name, FALSE);

  return (GST_PRESET_GET_INTERFACE (preset)->save_preset (preset, name));
}

/**
 * gst_preset_rename_preset:
 * @preset: a #GObject that implements #GstPreset
 * @old_name: current preset name
 * @new_name: new preset name
 *
 * Renames a preset. If there is already a preset by the @new_name it will be
 * overwritten.
 *
 * Returns: %TRUE for success, %FALSE if e.g. there is no preset with @old_name
 *
 * Since: 0.10.20
 */
gboolean
gst_preset_rename_preset (GstPreset * preset, const gchar * old_name,
    const gchar * new_name)
{
  g_return_val_if_fail (GST_IS_PRESET (preset), FALSE);
  g_return_val_if_fail (old_name, FALSE);
  g_return_val_if_fail (new_name, FALSE);

  return (GST_PRESET_GET_INTERFACE (preset)->rename_preset (preset, old_name,
          new_name));
}

/**
 * gst_preset_delete_preset:
 * @preset: a #GObject that implements #GstPreset
 * @name: preset name to remove
 *
 * Delete the given preset.
 *
 * Returns: %TRUE for success, %FALSE if e.g. there is no preset with that @name
 *
 * Since: 0.10.20
 */
gboolean
gst_preset_delete_preset (GstPreset * preset, const gchar * name)
{
  g_return_val_if_fail (GST_IS_PRESET (preset), FALSE);
  g_return_val_if_fail (name, FALSE);

  return (GST_PRESET_GET_INTERFACE (preset)->delete_preset (preset, name));
}

/**
 * gst_preset_set_meta:
 * @preset: a #GObject that implements #GstPreset
 * @name: preset name
 * @tag: meta data item name
 * @value: new value
 *
 * Sets a new @value for an existing meta data item or adds a new item. Meta
 * data @tag names can be something like e.g. "comment". Supplying %NULL for the
 * @value will unset an existing value.
 *
 * Returns: %TRUE for success, %FALSE if e.g. there is no preset with that @name
 *
 * Since: 0.10.20
 */
gboolean
gst_preset_set_meta (GstPreset * preset, const gchar * name, const gchar * tag,
    const gchar * value)
{
  g_return_val_if_fail (GST_IS_PRESET (preset), FALSE);
  g_return_val_if_fail (name, FALSE);
  g_return_val_if_fail (tag, FALSE);

  return GST_PRESET_GET_INTERFACE (preset)->set_meta (preset, name, tag, value);
}

/**
 * gst_preset_get_meta:
 * @preset: a #GObject that implements #GstPreset
 * @name: preset name
 * @tag: meta data item name
 * @value: value
 *
 * Gets the @value for an existing meta data @tag. Meta data @tag names can be
 * something like e.g. "comment". Returned values need to be released when done.
 *
 * Returns: %TRUE for success, %FALSE if e.g. there is no preset with that @name
 * or no value for the given @tag
 *
 * Since: 0.10.20
 */
gboolean
gst_preset_get_meta (GstPreset * preset, const gchar * name, const gchar * tag,
    gchar ** value)
{
  g_return_val_if_fail (GST_IS_PRESET (preset), FALSE);
  g_return_val_if_fail (name, FALSE);
  g_return_val_if_fail (tag, FALSE);
  g_return_val_if_fail (value, FALSE);

  return GST_PRESET_GET_INTERFACE (preset)->get_meta (preset, name, tag, value);
}

/* class internals */

static void
gst_preset_class_init (GstPresetInterface * iface)
{
  iface->get_preset_names = gst_preset_default_get_preset_names;
  iface->get_property_names = gst_preset_default_get_property_names;

  iface->load_preset = gst_preset_default_load_preset;
  iface->save_preset = gst_preset_default_save_preset;
  iface->rename_preset = gst_preset_default_rename_preset;
  iface->delete_preset = gst_preset_default_delete_preset;

  iface->set_meta = gst_preset_default_set_meta;
  iface->get_meta = gst_preset_default_get_meta;
}

static void
gst_preset_base_init (gpointer g_class)
{
  static gboolean initialized = FALSE;

  if (!initialized) {
    /* init default implementation */
    GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, "preset",
        GST_DEBUG_FG_WHITE | GST_DEBUG_BG_BLACK, "preset interface");

    /* create quarks for use with g_type_{g,s}et_qdata() */
    preset_quark = g_quark_from_static_string ("GstPreset::presets");
    preset_user_path_quark =
        g_quark_from_static_string ("GstPreset::user_path");
    preset_system_path_quark =
        g_quark_from_static_string ("GstPreset::system_path");

#if 0
    property_list_quark = g_quark_from_static_string ("GstPreset::properties");

    /* create interface properties, each element would need to override this
     *   g_object_class_override_property(gobject_class, PROP_PRESET_NAME, "preset-name");
     * and in _set_property() do
     *   case PROP_PRESET_NAME: {
     *     gchar *name = g_value_get_string (value);
     *     if (name)
     *       gst_preset_load_preset(preset, name);
     *   } break;
     */
    g_object_interface_install_property (g_class,
        g_param_spec_string ("preset-name",
            "preset-name property",
            "load given preset", NULL, G_PARAM_WRITABLE));
#endif

    initialized = TRUE;
  }
}

GType
gst_preset_get_type (void)
{
  static volatile gsize type = 0;

  if (g_once_init_enter (&type)) {
    GType _type;
    const GTypeInfo info = {
      sizeof (GstPresetInterface),
      (GBaseInitFunc) gst_preset_base_init,     /* base_init */
      NULL,                     /* base_finalize */
      (GClassInitFunc) gst_preset_class_init,   /* class_init */
      NULL,                     /* class_finalize */
      NULL,                     /* class_data */
      0,
      0,                        /* n_preallocs */
      NULL                      /* instance_init */
    };
    _type = g_type_register_static (G_TYPE_INTERFACE, "GstPreset", &info, 0);
    g_once_init_leave (&type, _type);
  }
  return type;
}
