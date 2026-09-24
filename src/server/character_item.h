#pragma once

// The character a server's players play as, read from its workshop item.
//
// A character item is a Godot resource pack (zip) like any other item. Besides the character's scene
// and meshes it carries, under characters/<name>/, the files baked from it in the editor: anim.cfg,
// skeleton.ozz and the clips, and hitboxes.cfg. Clients mount the pack; the server opens the very
// same file (its SHA-256 must match the item manifest) and reads only those baked files. Nothing is
// imported or converted at runtime.

#include "anim_set.h"
#include "hitboxes.h"
#include "mod_schema.h"

#include <memory>
#include <string>

namespace cb
{

struct CharacterAsset
{
	std::string name; // empty: the built-in placeholder rig
	std::shared_ptr<const anim::AnimSet> animations;
	anim::HitboxSet hitboxes; // bound to `animations`' skeleton
};

// The placeholder rig and its default zones.
std::shared_ptr<const CharacterAsset> BuiltInCharacter();

// Reads characters/<item.mod>/ from `zipPath`, after checking the file's SHA-256 against the item.
// Returns null and sets `error` on failure; non-fatal problems go to `warnings`.
std::shared_ptr<const CharacterAsset> LoadCharacterItem( const std::string& zipPath, const ModItem& item, std::string& error,
													std::string& warnings );

// Where a player's workshop keeps items (the game's user:// folder), for the default --workshop.
std::string DefaultWorkshopDir();

} // namespace cb
