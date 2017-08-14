/*
 * Copyright (c) 2017 gnome-mpv
 *
 * This file is part of GNOME MPV.
 *
 * GNOME MPV is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * GNOME MPV is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNOME MPV.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <string.h>
#include <glib/gstdio.h>

#include "gmpv_player.h"
#include "gmpv_mpv_wrapper.h"
#include "gmpv_def.h"

struct _GmpvPlayer
{
	GmpvMpv parent;
	GPtrArray *playlist;
	GPtrArray *metadata;
	GPtrArray *track_list;
	gboolean init_vo_config;
	gchar *tmp_input_config;
};

struct _GmpvPlayerClass
{
	GmpvMpvClass parent_class;
};

static void finalize(GObject *object);
static void mpv_event_handler(GmpvMpv *mpv, gint event_id, gpointer event_data);
static void mpv_property_changed_handler(	GmpvMpv *mpv,
						const gchar *name,
						gpointer value );
static void apply_default_options(GmpvMpv *mpv);
static void initialize(GmpvMpv *mpv);
static void load_file(GmpvMpv *mpv, const gchar *uri, gboolean append);
static void reset(GmpvMpv *mpv);
static void load_input_conf(GmpvPlayer *player, const gchar *input_conf);
static void load_config_file(GmpvMpv *mpv);
static void load_input_config_file(GmpvPlayer *player);
static void load_scripts(GmpvPlayer *player);
static GmpvTrack *parse_track_entry(mpv_node_list *node);
static void add_file_to_playlist(GmpvPlayer *player, const gchar *uri);
static void load_from_playlist(GmpvPlayer *player);
static GmpvPlaylistEntry *parse_playlist_entry(mpv_node_list *node);
static void update_playlist(GmpvPlayer *player);
static void update_metadata(GmpvPlayer *player);
static void update_track_list(GmpvPlayer *player);

G_DEFINE_TYPE(GmpvPlayer, gmpv_player, GMPV_TYPE_MPV)

static void finalize(GObject *object)
{
	GmpvPlayer *player = GMPV_PLAYER(object);

	if(player->tmp_input_config)
	{
		g_unlink(player->tmp_input_config);
	}

	g_free(player->tmp_input_config);
	g_ptr_array_free(player->playlist, TRUE);
	g_ptr_array_free(player->metadata, TRUE);
	g_ptr_array_free(player->track_list, TRUE);

	G_OBJECT_CLASS(gmpv_player_parent_class)->finalize(object);
}

static void mpv_event_handler(GmpvMpv *mpv, gint event_id, gpointer event_data)
{
	if(event_id == MPV_EVENT_START_FILE)
	{
		gboolean vo_configured = FALSE;

		gmpv_mpv_get_property(	mpv,
					"vo-configured",
					MPV_FORMAT_FLAG,
					&vo_configured );

		/* If the vo is not configured yet, save the content of mpv's
		 * playlist. This will be loaded again when the vo is
		 * configured.
		 */
		if(!vo_configured)
		{
			update_playlist(GMPV_PLAYER(mpv));
		}
	}

	GMPV_MPV_CLASS(gmpv_player_parent_class)
		->mpv_event(mpv, event_id, event_data);
}

static void mpv_property_changed_handler(	GmpvMpv *mpv,
						const gchar *name,
						gpointer value )
{
	GmpvPlayer *player = GMPV_PLAYER(mpv);

	if(g_strcmp0(name, "pause") == 0)
	{
		gboolean idle_active = FALSE;
		gboolean pause = value?*((int *)value):TRUE;

		gmpv_mpv_get_property(	mpv,
					"idle-active",
					MPV_FORMAT_FLAG,
					&idle_active );

		if(idle_active && !pause && !player->init_vo_config)
		{
			load_from_playlist(player);
		}
	}
	else if(g_strcmp0(name, "playlist") == 0)
	{
		gint64 playlist_count = 0;
		gboolean idle_active = FALSE;
		gboolean was_empty = FALSE;

		gmpv_mpv_get_property(	mpv,
					"playlist-count",
					MPV_FORMAT_INT64,
					&playlist_count );
		gmpv_mpv_get_property(	mpv,
					"idle-active",
					MPV_FORMAT_FLAG,
					&idle_active );

		was_empty = (player->playlist->len == 0);

		if(!idle_active)
		{
			update_playlist(player);
		}

		/* Check if we're transitioning from empty playlist to non-empty
		 * playlist.
		 */
		if(was_empty && player->playlist->len > 0)
		{
			gmpv_mpv_set_property_flag(mpv, "pause", FALSE);
		}
	}
	else if(g_strcmp0(name, "metadata") == 0)
	{
		update_metadata(player);
	}
	else if(g_strcmp0(name, "track-list") == 0)
	{
		update_track_list(player);
	}
	else if(g_strcmp0(name, "vo-configured") == 0)
	{
		if(player->init_vo_config)
		{
			player->init_vo_config = FALSE;
			load_scripts(player);
			load_from_playlist(player);
		}
	}

	GMPV_MPV_CLASS(gmpv_player_parent_class)
		->mpv_property_changed(mpv, name, value);
}

static void apply_default_options(GmpvMpv *mpv)
{
	gchar *config_dir = get_config_dir_path();
	gchar *watch_dir = get_watch_dir_path();

	const struct
	{
		const gchar *name;
		const gchar *value;
	}
	options[] = {	{"vo", "opengl,vdpau,vaapi,xv,x11,opengl-cb,"},
			{"osd-level", "1"},
			{"softvol", "yes"},
			{"force-window", "immediate"},
			{"input-default-bindings", "yes"},
			{"audio-client-name", ICON_NAME},
			{"title", "${media-title}"},
			{"autofit-larger", "75%"},
			{"window-scale", "1"},
			{"pause", "yes"},
			{"ytdl", "yes"},
			{"load-scripts", "no"},
			{"osd-bar", "no"},
			{"input-cursor", "no"},
			{"cursor-autohide", "no"},
			{"softvol-max", "100"},
			{"config", "no"},
			{"config-dir", config_dir},
			{"watch-later-directory", watch_dir},
			{"screenshot-template", "gnome-mpv-shot%n"},
			{NULL, NULL} };

	for(gint i = 0; options[i].name; i++)
	{
		g_debug(	"Applying default option --%s=%s",
				options[i].name,
				options[i].value );

		gmpv_mpv_set_option_string(	mpv,
						options[i].name,
						options[i].value );
	}

	g_free(config_dir);
	g_free(watch_dir);
}

static void initialize(GmpvMpv *mpv)
{
	apply_default_options(mpv);
	load_config_file(mpv);
	load_input_config_file(GMPV_PLAYER(mpv));

	GMPV_MPV_CLASS(gmpv_player_parent_class)->initialize(mpv);
}

static void load_file(GmpvMpv *mpv, const gchar *uri, gboolean append)
{
	GmpvPlayer *player = GMPV_PLAYER(mpv);
	gboolean ready = FALSE;
	gboolean idle_active = FALSE;

	g_object_get(mpv, "ready", &ready, NULL);
	gmpv_mpv_get_property(	mpv,
				"idle-active",
				MPV_FORMAT_FLAG,
				&idle_active );

	if(idle_active || !ready)
	{
		if(!append)
		{
			g_ptr_array_set_size(player->playlist, 0);
		}

		add_file_to_playlist(player, uri);
	}
	else
	{
		GMPV_MPV_CLASS(gmpv_player_parent_class)
			->load_file(mpv, uri, append);
	}
}

static void reset(GmpvMpv *mpv)
{
	gboolean idle_active = FALSE;
	gint64 playlist_pos = 0;

	gmpv_mpv_get_property(	mpv,
				"idle-active",
				MPV_FORMAT_FLAG,
				&idle_active );
	gmpv_mpv_get_property(	mpv,
				"playlist-pos",
				MPV_FORMAT_INT64,
				&playlist_pos );

	GMPV_MPV_CLASS(gmpv_player_parent_class)->reset(mpv);

	if(!idle_active)
	{
		load_from_playlist(GMPV_PLAYER(mpv));
	}

	if(playlist_pos > 0)
	{
		gmpv_mpv_set_property(	mpv,
					"playlist-pos",
					MPV_FORMAT_INT64,
					&playlist_pos );
	}
}

static void load_input_conf(GmpvPlayer *player, const gchar *input_conf)
{
	const gchar *default_keybinds[] = DEFAULT_KEYBINDS;
	gchar *tmp_path;
	FILE *tmp_file;
	gint tmp_fd;

	if(player->tmp_input_config)
	{
		g_unlink(player->tmp_input_config);
		g_free(player->tmp_input_config);
	}

	tmp_fd = g_file_open_tmp(NULL, &tmp_path, NULL);
	tmp_file = fdopen(tmp_fd, "w");
	player->tmp_input_config = tmp_path;

	if(!tmp_file)
	{
		g_error("Failed to open temporary input config file");
	}

	for(gint i = 0; default_keybinds[i]; i++)
	{
		const gsize len = strlen(default_keybinds[i]);
		gsize write_size = fwrite(default_keybinds[i], len, 1, tmp_file);
		gint rc = fputc('\n', tmp_file);

		if(write_size != 1 || rc != '\n')
		{
			g_error(	"Error writing default keybindings to "
					"temporary input config file" );
		}
	}

	g_debug("Using temporary input config file: %s", tmp_path);
	gmpv_mpv_set_option_string(GMPV_MPV(player), "input-conf", tmp_path);

	if(input_conf && strlen(input_conf) > 0)
	{
		const gsize buf_size = 65536;
		void *buf = g_malloc(buf_size);
		FILE *src_file = g_fopen(input_conf, "r");
		gsize read_size = buf_size;

		if(!src_file)
		{
			g_warning(	"Cannot open input config file: %s",
					input_conf );
		}

		while(src_file && read_size == buf_size)
		{
			read_size = fread(buf, 1, buf_size, src_file);

			if(read_size != fwrite(buf, 1, read_size, tmp_file))
			{
				g_error(	"Error writing requested input "
						"config file to temporary "
						"input config file" );
			}
		}

		g_info("Loaded input config file: %s", input_conf);

		g_free(buf);
	}

	fclose(tmp_file);
}

static void load_config_file(GmpvMpv *mpv)
{
	GSettings *settings = g_settings_new(CONFIG_ROOT);

	if(g_settings_get_boolean(settings, "mpv-config-enable"))
	{
		gchar *mpv_conf =	g_settings_get_string
					(settings, "mpv-config-file");

		g_info("Loading config file: %s", mpv_conf);
		gmpv_mpv_load_config_file(mpv, mpv_conf);

		g_free(mpv_conf);
	}

	g_object_unref(settings);
}

static void load_input_config_file(GmpvPlayer *player)
{
	GSettings *settings = g_settings_new(CONFIG_ROOT);
	gchar *input_conf = NULL;

	if(g_settings_get_boolean(settings, "mpv-input-config-enable"))
	{
		input_conf =	g_settings_get_string
				(settings, "mpv-input-config-file");

		g_info("Loading input config file: %s", input_conf);
	}

	load_input_conf(player, input_conf);

	g_free(input_conf);
	g_object_unref(settings);
}

static void load_scripts(GmpvPlayer *player)
{
	gchar *path = get_scripts_dir_path();
	GDir *dir = g_dir_open(path, 0, NULL);

	if(dir)
	{
		const gchar *name;

		do
		{
			gchar *full_path;

			name = g_dir_read_name(dir);
			full_path = g_build_filename(path, name, NULL);

			if(g_file_test(full_path, G_FILE_TEST_IS_REGULAR))
			{
				const gchar *cmd[]
					= {"load-script", full_path, NULL};

				g_info("Loading script: %s", full_path);
				gmpv_mpv_command(GMPV_MPV(player), cmd);
			}

			g_free(full_path);
		}
		while(name);

		g_dir_close(dir);
	}
	else
	{
		g_warning("Failed to open scripts directory: %s", path);
	}

	g_free(path);
}

static GmpvTrack *parse_track_entry(mpv_node_list *node)
{
	GmpvTrack *entry = gmpv_track_new();

	for(gint i = 0; i < node->num; i++)
	{
		if(g_strcmp0(node->keys[i], "type") == 0)
		{
			const gchar *type = node->values[i].u.string;

			if(g_strcmp0(type, "audio") == 0)
			{
				entry->type = TRACK_TYPE_AUDIO;
			}
			else if(g_strcmp0(type, "video") == 0)
			{
				entry->type = TRACK_TYPE_VIDEO;
			}
			else if(g_strcmp0(type, "sub") == 0)
			{
				entry->type = TRACK_TYPE_SUBTITLE;
			}
		}
		else if(g_strcmp0(node->keys[i], "title") == 0)
		{
			entry->title = g_strdup(node->values[i].u.string);
		}
		else if(g_strcmp0(node->keys[i], "lang") == 0)
		{
			entry->lang = g_strdup(node->values[i].u.string);
		}
		else if(g_strcmp0(node->keys[i], "id") == 0)
		{
			entry->id = node->values[i].u.int64;
		}
	}

	return entry;
}

static void add_file_to_playlist(GmpvPlayer *player, const gchar *uri)
{
	GmpvPlaylistEntry *entry = gmpv_playlist_entry_new(uri, NULL);

	g_ptr_array_add(player->playlist, entry);
}

static void load_from_playlist(GmpvPlayer *player)
{
	GmpvMpv *mpv = GMPV_MPV(player);
	GPtrArray *playlist = player->playlist;

	for(guint i = 0; playlist && i < playlist->len; i++)
	{
		GmpvPlaylistEntry *entry = g_ptr_array_index(playlist, i);

		/* Do not append on first iteration */
		GMPV_MPV_CLASS(gmpv_player_parent_class)
			->load_file(mpv, entry->filename, i != 0);
	}
}

static GmpvPlaylistEntry *parse_playlist_entry(mpv_node_list *node)
{
	const gchar *filename = NULL;
	const gchar *title = NULL;

	for(gint i = 0; i < node->num; i++)
	{
		if(g_strcmp0(node->keys[i], "filename") == 0)
		{
			filename = node->values[i].u.string;
		}
		else if(g_strcmp0(node->keys[i], "title") == 0)
		{
			title = node->values[i].u.string;
		}
	}

	return gmpv_playlist_entry_new(filename, title);
}

static void update_playlist(GmpvPlayer *player)
{
	const mpv_node_list *org_list;
	mpv_node playlist;

	g_ptr_array_set_size(player->playlist, 0);

	gmpv_mpv_get_property
		(GMPV_MPV(player), "playlist", MPV_FORMAT_NODE, &playlist);

	org_list = playlist.u.list;

	if(playlist.format == MPV_FORMAT_NODE_ARRAY)
	{
		for(gint i = 0; i < org_list->num; i++)
		{
			GmpvPlaylistEntry *entry;

			entry = parse_playlist_entry(org_list->values[i].u.list);
			g_ptr_array_add(player->playlist, entry);
		}

		mpv_free_node_contents(&playlist);
	}
}

static void update_metadata(GmpvPlayer *player)
{
	mpv_node_list *org_list = NULL;
	mpv_node metadata;

	g_ptr_array_set_size(player->metadata, 0);
	gmpv_mpv_get_property(	GMPV_MPV(player),
				"metadata",
				MPV_FORMAT_NODE,
				&metadata );
	org_list = metadata.u.list;

	if(metadata.format == MPV_FORMAT_NODE_MAP && org_list->num > 0)
	{
		for(gint i = 0; i < org_list->num; i++)
		{
			const gchar *key;
			mpv_node value;

			key = org_list->keys[i];
			value = org_list->values[i];

			if(value.format == MPV_FORMAT_STRING)
			{
				GmpvMetadataEntry *entry;

				entry =	gmpv_metadata_entry_new
					(key, value.u.string);

				g_ptr_array_add(player->metadata, entry);
			}
			else
			{
				g_warning(	"Ignored metadata field %s "
						"with unexpected format %d",
						key,
						value.format );
			}
		}

		mpv_free_node_contents(&metadata);
	}
}

static void update_track_list(GmpvPlayer *player)
{
	mpv_node_list *org_list = NULL;
	mpv_node track_list;

	g_ptr_array_set_size(player->track_list, 0);
	gmpv_mpv_get_property(	GMPV_MPV(player),
				"track-list",
				MPV_FORMAT_NODE,
				&track_list );
	org_list = track_list.u.list;

	if(track_list.format == MPV_FORMAT_NODE_ARRAY)
	{
		for(gint i = 0; i < org_list->num; i++)
		{
			GmpvTrack *entry =	parse_track_entry
						(org_list->values[i].u.list);

			g_ptr_array_add(player->track_list, entry);
		}

		mpv_free_node_contents(&track_list);
	}
}

static void gmpv_player_class_init(GmpvPlayerClass *klass)
{
	GmpvMpvClass *mpv_class = GMPV_MPV_CLASS(klass);
	GObjectClass *obj_class = G_OBJECT_CLASS(klass);

	mpv_class->mpv_event = mpv_event_handler;
	mpv_class->mpv_property_changed = mpv_property_changed_handler;
	mpv_class->initialize = initialize;
	mpv_class->load_file = load_file;
	mpv_class->reset = reset;
	obj_class->finalize = finalize;
}

static void gmpv_player_init(GmpvPlayer *player)
{
	player->playlist = g_ptr_array_new_full(	1,
							(GDestroyNotify)
							gmpv_playlist_entry_free );
	player->metadata = g_ptr_array_new_full(	1,
							(GDestroyNotify)
							gmpv_metadata_entry_free );
	player->track_list = g_ptr_array_new_full(	1,
							(GDestroyNotify)
							gmpv_track_free );
	player->init_vo_config = TRUE;
	player->tmp_input_config = NULL;
}

GmpvPlayer *gmpv_player_new(gint64 wid)
{
	return GMPV_PLAYER(g_object_new(	gmpv_player_get_type(),
						"wid", wid,
						NULL ));
}

GPtrArray *gmpv_player_get_playlist(GmpvPlayer *player)
{
	return player->playlist;
}

GPtrArray *gmpv_player_get_metadata(GmpvPlayer *player)
{
	return player->metadata;
}

GPtrArray *gmpv_player_get_track_list(GmpvPlayer *player)
{
	return player->track_list;
}

