#pragma once

// An item as its scene says it: what a mod's look has in items/<kind>.cfg, baked from a scene whose
// root is a CbItem. A mod that takes its items from its look (Declarations::ItemsOfLook) declares
// nothing about them in code: a new item is a new scene.

#include <map>
#include <string>

namespace cb
{

struct LookItem
{
	std::string kind;						 // "gun.pistol": the file's name
	std::map<std::string, float> properties; // the CbItem's properties ("gun.damage" = 25)
	std::string pack;						 // the animation pack it brings while it is held ("gun.pistol_hold"), or none
	std::string holster;					 // the socket it hangs in while it is put away ("Hip"), or none
};

} // namespace cb
