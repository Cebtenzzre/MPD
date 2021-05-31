/*
 * Copyright 2003-2021 The Music Player Daemon Project
 * http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "Walk.hxx"
#include "UpdateDomain.hxx"
#include "db/DatabaseLock.hxx"
#include "db/PlaylistVector.hxx"
#include "db/plugins/simple/Directory.hxx"
#include "fs/Traits.hxx"
#include "lib/fmt/ExceptionFormatter.hxx"
#include "song/DetachedSong.hxx"
#include "input/InputStream.hxx"
#include "playlist/PlaylistPlugin.hxx"
#include "playlist/PlaylistRegistry.hxx"
#include "playlist/PlaylistStream.hxx"
#include "playlist/SongEnumerator.hxx"
#include "storage/FileInfo.hxx"
#include "storage/StorageInterface.hxx"
#include "fs/Traits.hxx"
#include "util/StringFormat.hxx"
#include "Log.hxx"

#include <cstring>
#include <map>
#include <sys/stat.h>

inline void
UpdateWalk::UpdatePlaylistFile(Directory &parent, Directory *directory,
			       SongEnumerator &contents) noexcept
{
	unsigned track = 0;

	std::map<std::string, Song *> song_map;
	for (auto &song: parent.songs)
		song_map[song.filename] = &song;

	while (true) {
		auto song = contents.NextSong();
		if (!song)
			break;
		if (strcmp(song->GetURI(), "mpd://bail") == 0) {
			// Unsupported playlist
			if (directory != nullptr)
				editor.LockDeleteDirectory(directory);
			break;
		}

		const auto target_filename = std::string(song->GetURI());
		std::unique_ptr<Song> db_song;
		if (directory != nullptr) {
			db_song = std::make_unique<Song>(std::move(*song),
							 *directory);
			const bool is_absolute =
				PathTraitsUTF8::IsAbsoluteOrHasScheme(target_filename.c_str());
			db_song->target = is_absolute
				? target_filename
				/* prepend "../" to relative paths to go from
				   the virtual directory (DEVICE_PLAYLIST) to
				   the containing directory */
				: "../" + target_filename;
			db_song->filename = StringFormat<64>("track%04u",
							     ++track);
		}

		struct stat st;
		const auto path = storage.MapUTF8(parent.GetPath()) + "/" + target_filename;
		if (stat(path.c_str(), &st) < 0) {
			// Song does not exist
			FmtError(update_domain, "File not found: '{}'", path);
			if (directory != nullptr)
				editor.LockDeleteDirectory(directory);
			break;
		}

		{
			const ScopeDatabaseLock protect;
			if (directory != nullptr)
				directory->AddSong(std::move(db_song));
			const auto match = song_map.find(target_filename);
			if (match != song_map.end()) {
				parent.RemoveSong(match->second); // Playlist overrides the target
				song_map.erase(match);
			}
		}
	}
}

inline void
UpdateWalk::UpdatePlaylistFile(Directory &parent, std::string_view name,
			       const StorageFileInfo &info,
			       const PlaylistPlugin &plugin) noexcept
{
	assert(plugin.open_stream);

	Directory *directory =
		LockMakeVirtualDirectoryIfModified(parent, name, info,
						   DEVICE_PLAYLIST);

	const auto path_utf8 = parent.IsRoot()
		? std::string(name)
		: PathTraitsUTF8::Build(parent.GetPath(), name);
	const auto uri_utf8 = storage.MapUTF8(path_utf8);

	FmtDebug(update_domain, "scanning playlist '{}'", uri_utf8);

	try {
		Mutex mutex;
		auto e = plugin.open_stream(InputStream::OpenReady(uri_utf8.c_str(),
								   mutex));
		if (!e) {
			/* unsupported URI? roll back.. */
			if (directory != nullptr)
				editor.LockDeleteDirectory(directory);
			return;
		}

		UpdatePlaylistFile(parent, directory, *e);

		if (directory->IsEmpty())
			editor.LockDeleteDirectory(directory);
	} catch (...) {
		FmtError(update_domain,
			 "Failed to scan playlist '{}': {}",
			 uri_utf8, std::current_exception());
		if (directory != nullptr)
			editor.LockDeleteDirectory(directory);
	}
}

bool
UpdateWalk::UpdatePlaylistFile(Directory &directory,
			       std::string_view name, std::string_view suffix,
			       const StorageFileInfo &info) noexcept
{
	const auto *const plugin = FindPlaylistPluginBySuffix(suffix);
	if (plugin == nullptr)
		return false;

	if (GetPlaylistPluginAsFolder(*plugin)) {
		UpdatePlaylistFile(directory, name, info, *plugin);
	} else {
		PlaylistInfo pi(name, info.mtime);

		const ScopeDatabaseLock protect;
		if (directory.playlists.UpdateOrInsert(std::move(pi)))
			modified = true;
	}

	return true;
}

void
UpdateWalk::PurgeDanglingFromPlaylists(Directory &directory) noexcept
{
	/* recurse */
	for (Directory &child : directory.children)
		PurgeDanglingFromPlaylists(child);

	if (!directory.IsPlaylist())
		/* this check is only for virtual directories
		   representing a playlist file */
		return;

	directory.ForEachSongSafe([&](Song &song){
		if (!song.target.empty() &&
		    !PathTraitsUTF8::IsAbsoluteOrHasScheme(song.target.c_str())) {
			Song *target = directory.LookupTargetSong(song.target.c_str());
			if (target == nullptr) {
				/* the target does not exist: remove
				   the virtual song */
				editor.DeleteSong(directory, &song);
				modified = true;
			} else {
				/* the target exists: mark it (for
				   option "hide_playlist_targets") */
				target->in_playlist = true;
			}
		}
	});
}
