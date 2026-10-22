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
#include "look_item.h"
#include "mod_schema.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

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

// A character of a mod's look: characters/<name>/ of its workshop item (checked against the item's
// SHA-256), or of a folder (tests, a mod's client project): baked there from a CbCharacter scene of
// the mod's client project, as a character item's is.
std::shared_ptr<const CharacterAsset> LoadModCharacterItem( const std::string& zipPath, const ModItem& item, const std::string& name,
														   std::string& error, std::string& warnings );
std::shared_ptr<const CharacterAsset> LoadModCharacterFolder( const std::string& dir, const std::string& name, std::string& error,
															 std::string& warnings );

// A character that ships with the game (godot/characters/<name>/, which every client has in its base
// pack): the build copies its baked files next to cb_server, into characters/<name>/.
std::shared_ptr<const CharacterAsset> LoadCharacterFolder( const std::string& dir, const std::string& name, std::string& error,
														   std::string& warnings );

// A mod's animation pack: anim/<pack>/ of its workshop item (checked against the item's SHA-256), or of
// a folder (tests, a mod's client project).
std::shared_ptr<const anim::AnimSet> LoadAnimPackItem( const std::string& zipPath, const ModItem& item, const std::string& pack,
													   std::string& error, std::string& warnings );
std::shared_ptr<const anim::AnimSet> LoadAnimPackFolder( const std::string& dir, const std::string& pack, std::string& error,
														 std::string& warnings );

// An item kind's body when it lies in the world, baked from its scene's CbItem into
// items/<kind>.cfg of the mod's item:
//     shape box            or sphere
//     half 0.035 0.035 0.41    half extents in metres (a sphere: its radius, three times)
//     center 0 0 -0.31     the shape's centre in the grip's frame
//     mass 1.1             kg
// `properties` also gets the file's "property <name> <number>" lines (what the item's CbItem
// says about it: "pickup.hold_seconds 0.5").
using ItemProperties = std::map<std::string, float>;
bool ParseItemShape( const std::string& text, ItemShape& out, std::string& error, ItemProperties* properties = nullptr );
// From a workshop item (checked against its SHA-256), or from a folder (tests, a mod's client
// project). False when the item has no such file, or it does not parse (then `error` says why).
bool LoadItemShapeItem( const std::string& zipPath, const ModItem& item, const std::string& kind, ItemShape& out, std::string& error,
						ItemProperties* properties = nullptr );
bool LoadItemShapeFolder( const std::string& dir, const std::string& kind, ItemShape& out, std::string& error,
						  ItemProperties* properties = nullptr );

// A mod's motion set as text: motions/<set>.cfg of its workshop item (checked against its SHA-256),
// or of a folder (tests, a mod's client project). False when it is not there.
bool LoadMotionsItem( const std::string& zipPath, const ModItem& item, const std::string& set, std::string& text, std::string& error );
bool LoadMotionsFolder( const std::string& dir, const std::string& set, std::string& text, std::string& error );

// Every item of a mod's look: each items/<kind>.cfg of its workshop item (checked against its
// SHA-256), or of a folder (tests, a mod's client project), with its properties and its "pack" and
// "holster" lines. In the order of their names. False when the item cannot be read.
bool ListLookItemsItem( const std::string& zipPath, const ModItem& item, std::vector<LookItem>& out, std::string& error );
bool ListLookItemsFolder( const std::string& dir, std::vector<LookItem>& out );

// What of a workshop item the server reads, by file: items/*.cfg without the lines only the game
// reads (the scene, the name, the first-person view), and everything under anim/, motions/ and
// characters/ that is baked (.cfg, .ozz). Two items of a mod with the same parts differ in their
// look alone: one can take the other's place while the server runs. False (and `error`) when the
// file is not the item the manifest names.
bool ItemServerParts( const std::string& zipPath, const ModItem& item, std::map<std::string, std::string>& parts, std::string& error );

// Where a player's workshop keeps items (the game's user:// folder), for the default --workshop.
std::string DefaultWorkshopDir();

} // namespace cb
