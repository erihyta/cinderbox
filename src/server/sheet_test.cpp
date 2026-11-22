#include "sheet_test.h"

#include "character_item.h"
#include "detmath.h"
#include "game_server.h"
#include "mod_api.h"
#include "registry.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

// The words of a .cbtest file (a test's lines are indented under it; # starts a comment):
//
//   test "NAME"                       a test: the lines under it, a fresh server each
//   mods NAME, NAME                   mods to run beside the defaults (brawl, a switched-off one); the
//                                     file's own mod always runs
//   option NAME VALUE                 a mod option (cb_server --mod-option)
//   item KIND in hand                 the tester holds one (given, if it has none, and its slot chosen)
//   set FIELD [of item|of world] = N  a field when the script starts: the tester's, its item's, the game's
//   other ahead D [right R]           a second player D metres ahead (R to the right), facing the tester
//   crate ahead D [right R] [up U] [size S]   a loose box (S metres a side: 0.5)
//   look PITCH                        the tester looks PITCH degrees up (down: negative)
//   press ACTION at T                 down for one tick: use (the use button), jump, sprint, forward,
//                                     back, left, right, a mod's action by its name (use2, reload), or an
//                                     item kind the tester carries (its slot's key: brawl.slam)
//   hold ACTION from T to T2          down from T until T2
//   expect event EVENT at T [within S]          said between T and T + S (0.2)
//   expect no event EVENT [from T to T2]        never said then (the whole test without a span)
//   expect NAME [of item|of other|of world] OP N at T    after the tick at T: a field, a status (1
//                                     or 0), or distance (from the tester to the other, metres) and
//                                     speed (m/s); OP one of == != < <= > >=
//
// The tester stands where it joined, facing ahead (+Z), its camera level unless it looks; the run
// ends half a second after the last time a line names.

namespace cb
{

namespace
{

constexpr uint32_t kTickRate = 60;

std::string Trim( const std::string& s )
{
	const size_t a = s.find_first_not_of( " \t\r" );
	const size_t b = s.find_last_not_of( " \t\r" );
	return a == std::string::npos ? std::string() : s.substr( a, b - a + 1 );
}

std::vector<std::string> Words( const std::string& s )
{
	std::vector<std::string> out;
	std::istringstream in( s );
	std::string w;
	while ( in >> w )
	{
		out.push_back( w );
	}
	return out;
}

bool Number( const std::string& word, float& out )
{
	char* end = nullptr;
	const double v = std::strtod( word.c_str(), &end );
	if ( end == word.c_str() || *end != '\0' || std::isfinite( v ) == false )
	{
		return false;
	}
	out = float( v );
	return true;
}

uint32_t TicksOf( float seconds )
{
	return uint32_t( std::lround( std::max( 0.0f, seconds ) * float( kTickRate ) ) );
}

struct Placed
{
	float ahead = 0.0f, right = 0.0f, up = 0.0f, size = 0.5f;
};

struct Input
{
	std::string action;
	uint32_t from = 0, to = 0; // ticks from the start: down while from <= tick < to
};

struct Expect
{
	enum class Kind
	{
		Event,
		NoEvent,
		Value,
	};
	Kind kind = Kind::Value;
	int line = 0;
	std::string name;
	std::string of = "self"; // self, item, other, world
	std::string op;
	float value = 0.0f;
	uint32_t at = 0, until = 0; // ticks from the start
	bool always = false;		// a no event without a span
};

struct Setting
{
	int line = 0;
	std::string field, of = "self";
	float value = 0.0f;
};

struct Test
{
	std::string name;
	int line = 0;
	std::vector<std::string> mods;
	std::map<std::string, std::string> options;
	std::string item;
	int itemLine = 0;
	std::vector<Setting> sets;
	bool other = false;
	Placed otherAt;
	std::vector<Placed> crates;
	float pitch = 0.0f;
	std::vector<Input> inputs;
	std::vector<Expect> expects;
	uint32_t last = 0; // the last tick a line names
};

// "of item", "of other", "of world" from word k on (none: self).
size_t OfWord( const std::vector<std::string>& w, size_t k, std::string& of )
{
	if ( k + 1 < w.size() && w[k] == "of" && ( w[k + 1] == "item" || w[k + 1] == "other" || w[k + 1] == "world" ) )
	{
		of = w[k + 1];
		return k + 2;
	}
	return k;
}

bool ParsePlaced( const std::vector<std::string>& w, size_t k, Placed& out )
{
	for ( ; k < w.size(); k += 2 )
	{
		float v = 0.0f;
		if ( k + 1 >= w.size() || Number( w[k + 1], v ) == false )
		{
			return false;
		}
		if ( w[k] == "ahead" )
		{
			out.ahead = v;
		}
		else if ( w[k] == "right" )
		{
			out.right = v;
		}
		else if ( w[k] == "up" )
		{
			out.up = v;
		}
		else if ( w[k] == "size" && v > 0.05f )
		{
			out.size = v;
		}
		else
		{
			return false;
		}
	}
	return true;
}

bool ParseTests( const std::string& path, const std::string& text, std::vector<Test>& tests, std::string& error )
{
	std::istringstream lines( text );
	std::string line;
	int number = 0;
	auto fail = [&]( const std::string& why ) {
		error = path + ":" + std::to_string( number ) + ": " + why;
		return false;
	};
	while ( std::getline( lines, line ) )
	{
		number += 1;
		const size_t hash = line.find( '#' );
		const std::string code = Trim( hash == std::string::npos ? line : line.substr( 0, hash ) );
		if ( code.empty() )
		{
			continue;
		}
		const bool indented = line[0] == '\t' || line[0] == ' ';
		if ( indented == false )
		{
			const size_t open = code.find( '"' ), close = code.rfind( '"' );
			if ( code.rfind( "test ", 0 ) != 0 || open == std::string::npos || close <= open )
			{
				return fail( "test \"NAME\", then its lines under it" );
			}
			Test t;
			t.name = code.substr( open + 1, close - open - 1 );
			t.line = number;
			tests.push_back( t );
			continue;
		}
		if ( tests.empty() )
		{
			return fail( "a line before the first test" );
		}
		Test& t = tests.back();
		std::vector<std::string> w = Words( code );
		const std::string& verb = w[0];
		float a = 0.0f, b = 0.0f;
		if ( verb == "mods" )
		{
			std::string rest = code.substr( 4 );
			std::replace( rest.begin(), rest.end(), ',', ' ' );
			for ( const std::string& mod : Words( rest ) )
			{
				t.mods.push_back( mod );
			}
		}
		else if ( verb == "option" && w.size() == 3 )
		{
			t.options[w[1]] = w[2];
		}
		else if ( verb == "item" && w.size() == 4 && w[2] == "in" && w[3] == "hand" )
		{
			t.item = w[1];
			t.itemLine = number;
		}
		else if ( verb == "set" && w.size() >= 4 )
		{
			Setting s;
			s.line = number;
			s.field = w[1];
			size_t k = OfWord( w, 2, s.of );
			if ( k + 2 != w.size() || w[k] != "=" || Number( w[k + 1], s.value ) == false || s.of == "other" )
			{
				return fail( "set FIELD [of item|of world] = NUMBER" );
			}
			t.sets.push_back( s );
		}
		else if ( verb == "other" )
		{
			t.other = true;
			if ( ParsePlaced( w, 1, t.otherAt ) == false )
			{
				return fail( "other ahead D [right R]" );
			}
		}
		else if ( verb == "crate" )
		{
			Placed p;
			if ( ParsePlaced( w, 1, p ) == false )
			{
				return fail( "crate ahead D [right R] [up U] [size S]" );
			}
			t.crates.push_back( p );
		}
		else if ( verb == "look" && w.size() == 2 && Number( w[1], a ) && std::fabs( a ) <= 89.0f )
		{
			t.pitch = a;
		}
		else if ( verb == "press" && w.size() == 4 && w[2] == "at" && Number( w[3], a ) )
		{
			t.inputs.push_back( { w[1], TicksOf( a ), TicksOf( a ) + 1 } );
			t.last = std::max( t.last, TicksOf( a ) );
		}
		else if ( verb == "hold" && w.size() == 6 && w[2] == "from" && w[4] == "to" && Number( w[3], a ) && Number( w[5], b ) && b > a )
		{
			t.inputs.push_back( { w[1], TicksOf( a ), TicksOf( b ) } );
			t.last = std::max( t.last, TicksOf( b ) );
		}
		else if ( verb == "expect" && w.size() >= 3 && ( w[1] == "event" || ( w[1] == "no" && w[2] == "event" ) ) )
		{
			Expect e;
			e.line = number;
			const bool no = w[1] == "no";
			e.kind = no ? Expect::Kind::NoEvent : Expect::Kind::Event;
			size_t k = no ? 3 : 2;
			if ( k >= w.size() )
			{
				return fail( "expect [no] event EVENT ..." );
			}
			e.name = w[k++];
			if ( no )
			{
				e.always = k == w.size();
				if ( e.always == false && ( k + 4 != w.size() || w[k] != "from" || w[k + 2] != "to" || Number( w[k + 1], a ) == false ||
											Number( w[k + 3], b ) == false || b < a ) )
				{
					return fail( "expect no event EVENT [from T to T2]" );
				}
				e.at = e.always ? 0 : TicksOf( a );
				e.until = e.always ? 0 : TicksOf( b );
			}
			else
			{
				float within = 0.2f;
				if ( k + 2 > w.size() || w[k] != "at" || Number( w[k + 1], a ) == false ||
					 ( k + 2 != w.size() && ( k + 4 != w.size() || w[k + 2] != "within" || Number( w[k + 3], within ) == false ) ) )
				{
					return fail( "expect event EVENT at T [within S]" );
				}
				e.at = TicksOf( a );
				e.until = e.at + std::max<uint32_t>( 1, TicksOf( within ) );
			}
			t.last = std::max( t.last, e.until );
			t.expects.push_back( e );
		}
		else if ( verb == "expect" && w.size() >= 6 )
		{
			Expect e;
			e.line = number;
			e.name = w[1];
			size_t k = OfWord( w, 2, e.of );
			static const std::set<std::string> ops = { "==", "!=", "<", "<=", ">", ">=" };
			if ( k + 4 != w.size() || ops.count( w[k] ) == 0 || Number( w[k + 1], e.value ) == false || w[k + 2] != "at" ||
				 Number( w[k + 3], a ) == false )
			{
				return fail( "expect NAME [of item|of other|of world] OP NUMBER at T" );
			}
			e.op = w[k];
			e.at = TicksOf( a );
			t.last = std::max( t.last, e.at );
			t.expects.push_back( e );
		}
		else
		{
			return fail( "\"" + code + "\": not a line of a test (mods, option, item, set, other, crate, look, press, hold, expect)" );
		}
	}
	if ( tests.empty() )
	{
		number = 1;
		return fail( "no test in it (test \"NAME\")" );
	}
	return true;
}

std::string Num( float v )
{
	char text[32];
	std::snprintf( text, sizeof( text ), "%g", double( v ) );
	return text;
}

// What the script does, run as a mod (first, before the others): the bots, their places, the item,
// the fields; then the tester's keys, tick by tick.
struct Run
{
	const Test* test = nullptr;
	int tester = -1, other = -1;
	int phase = 0; // 0 joining, 1 placing, 2 choosing the item, 3 settling, 4 the script, 5 done
	uint32_t phaseTick = 0;
	uint32_t start = 0; // the script's first tick
	uint32_t item = 0;
	uint8_t intentSeq = 0;
	std::vector<std::string> failures;
};

class ScriptMod final : public mods::ServerMod
{
public:
	explicit ScriptMod( Run& run )
		: m_run( run )
	{
	}
	const char* Name() const override
	{
		return "sheettest";
	}
	void Declare( mods::Declarations& declare ) override
	{
		m_hand = declare.Socket( "RightHand" );
	}
	void Tick( mods::Context& ctx ) override
	{
		Run& r = m_run;
		const Test& t = *r.test;
		const uint32_t now = ctx.Tick();
		auto next = [&]( int phase ) {
			r.phase = phase;
			r.phaseTick = now;
		};
		switch ( r.phase )
		{
			case 0:
				if ( r.tester < 0 )
				{
					r.tester = ctx.AddBot( "tester" );
					r.other = t.other ? ctx.AddBot( "other" ) : -1;
					r.phaseTick = now;
				}
				if ( ctx.InWorld( PlayerSlot( r.tester ) ) && ( r.other < 0 || ctx.InWorld( PlayerSlot( r.other ) ) ) )
				{
					next( 1 );
				}
				else if ( now - r.phaseTick > 10 * kTickRate )
				{
					r.failures.push_back( "the bots did not join" );
					next( 5 );
				}
				break;
			case 1:
			{
				// Where the tester joined, facing ahead (+Z); the rest around it.
				const Transform* at = ctx.EntityTransform( ctx.PlayerNetId( PlayerSlot( r.tester ) ) );
				m_base = at != nullptr ? at->position : b3Vec3{};
				ctx.RespawnAt( SlotTarget( PlayerSlot( r.tester ) ), m_base, 0.0f );
				if ( r.other >= 0 )
				{
					ctx.RespawnAt( SlotTarget( PlayerSlot( r.other ) ), Around( t.otherAt ), detmath::kTwoPi * 0.5f );
				}
				for ( const Placed& crate : t.crates )
				{
					const float half = 0.5f * crate.size;
					b3Vec3 p = Around( crate );
					p.y += half - 0.9f; // (from the feet: the base is the capsule's middle)
					ctx.SpawnProp( ShapeKind::Box, { half, half, half }, p, 0.0f, {}, 0, 0 );
				}
				if ( t.item.empty() == false )
				{
					m_kind = ctx.Schema().FindItemKind( t.item );
					if ( m_kind < 0 )
					{
						r.failures.push_back( std::to_string( t.itemLine ) + ": no mod declares the item " + t.item );
						next( 5 );
						break;
					}
					if ( Carried( ctx ) == nullptr )
					{
						ctx.GiveItem( SlotTarget( PlayerSlot( r.tester ) ), mods::ItemKindHandle{ m_kind } );
					}
				}
				next( 2 );
				break;
			}
			case 2:
			{
				if ( t.item.empty() )
				{
					next( 3 );
					break;
				}
				const uint32_t held = ctx.HeldItem( PlayerSlot( r.tester ), m_hand );
				if ( held != 0 && ctx.ItemKindOf( held ).index == m_kind )
				{
					r.item = held;
					next( 3 );
					break;
				}
				const mods::CarriedItem* carried = Carried( ctx );
				if ( carried != nullptr && ( now - r.phaseTick ) % 10 == 1 )
				{
					PlayerInput in = Keys( 0 );
					in.intent = uint8_t( SlotIntent::Select );
					in.intentA = uint8_t( carried->slot >= 0 ? carried->slot : 0 );
					in.intentSeq = ++r.intentSeq;
					ctx.BotInput( PlayerSlot( r.tester ), in );
					m_lastIntent = in;
				}
				if ( now - r.phaseTick > 3 * kTickRate )
				{
					r.failures.push_back( std::to_string( t.itemLine ) + ": the tester could not take " + t.item + " in hand" );
					next( 5 );
				}
				break;
			}
			case 3:
				if ( now == r.phaseTick + 1 )
				{
					Carried( ctx ); // (what it carries, in which slots: an item kind's key)
					for ( const Setting& s : t.sets )
					{
						const BoardField* field = ctx.Schema().FindField( s.field );
						if ( field == nullptr )
						{
							r.failures.push_back( std::to_string( s.line ) + ": no mod declares the field " + s.field );
							continue;
						}
						const mods::FieldHandle handle{ int( field->slot ), field->scope, field->type };
						const int32_t value = field->type == BoardType::Float ? BoardFromFloat( s.value ) : int32_t( std::lround( s.value ) );
						const uint32_t target = s.of == "item" ? r.item : s.of == "world" ? 0 : ctx.PlayerNetId( PlayerSlot( r.tester ) );
						ctx.Set( target, handle, value );
					}
				}
				if ( now - r.phaseTick >= kTickRate / 2 )
				{
					// (Its first tick: the next. What a bot presses holds from the tick after it is said.)
					r.start = now + 1;
					next( 4 );
				}
				ctx.BotInput( PlayerSlot( r.tester ), Keys( 0 ) );
				break;
			case 4:
			{
				ctx.BotInput( PlayerSlot( r.tester ), Keys( now + 1 - r.start ) );
				if ( now + 1 - r.start > t.last + kTickRate / 2 )
				{
					next( 5 );
				}
				break;
			}
			default:
				break;
		}
	}

private:
	b3Vec3 Around( const Placed& p ) const
	{
		return { m_base.x + p.right, m_base.y + p.up, m_base.z + p.ahead };
	}

	const mods::CarriedItem* Carried( mods::Context& ctx )
	{
		m_carried = ctx.CarriedItems( PlayerSlot( m_run.tester ) );
		for ( const mods::CarriedItem& c : m_carried )
		{
			if ( c.kind.index == m_kind )
			{
				return &c;
			}
		}
		return nullptr;
	}

	// The slot an item kind the tester carries is in (-1: none, or not an item kind).
	int SlotOfKind( const std::string& kind ) const
	{
		const int index = m_schema != nullptr ? m_schema->FindItemKind( kind ) : -1;
		for ( const mods::CarriedItem& c : m_carried )
		{
			if ( index >= 0 && c.kind.index == index && c.slot >= 0 && c.slot < 16 )
			{
				return c.slot;
			}
		}
		return -1;
	}

	// The tester's keys on the script's tick k: level, ahead, what its lines hold down.
	PlayerInput Keys( uint32_t k ) const
	{
		PlayerInput in = m_lastIntent; // (the slot's intent stays said, as a player's does)
		in.buttons = 0;
		in.actions = 0;
		in.slotKeys = 0;
		in.moveForward = 0;
		in.moveRight = 0;
		in.cameraYaw = 0;
		in.cameraPitch = int16_t( std::lround( m_run.test->pitch / 360.0f * 65536.0f ) );
		for ( const Input& i : m_run.test->inputs )
		{
			if ( m_run.phase != 4 || k < i.from || k >= i.to )
			{
				continue;
			}
			if ( i.action == "use" )
			{
				in.buttons |= BtnUse;
			}
			else if ( i.action == "jump" )
			{
				in.buttons |= BtnJump;
			}
			else if ( i.action == "sprint" )
			{
				in.buttons |= BtnSprint;
			}
			else if ( i.action == "forward" || i.action == "back" )
			{
				in.moveForward = int8_t( i.action == "forward" ? 127 : -127 );
			}
			else if ( i.action == "left" || i.action == "right" )
			{
				in.moveRight = int8_t( i.action == "right" ? 127 : -127 );
			}
			else if ( const int slot = SlotOfKind( i.action ); slot >= 0 )
			{
				in.slotKeys = uint16_t( in.slotKeys | ( 1u << slot ) ); // (an item kind: its slot's key)
			}
			else
			{
				in.actions |= m_schema != nullptr ? m_schema->ActionMask( i.action ) : 0;
			}
		}
		return in;
	}

public:
	const ModSchema* m_schema = nullptr;

private:
	Run& m_run;
	mods::SocketHandle m_hand;
	int m_kind = -1;
	b3Vec3 m_base = {};
	std::vector<mods::CarriedItem> m_carried;
	PlayerInput m_lastIntent;
};

bool Compare( float a, const std::string& op, float b )
{
	const float eps = 1.0e-4f;
	return op == "==" ? std::fabs( a - b ) <= eps
		   : op == "!=" ? std::fabs( a - b ) > eps
		   : op == "<"	? a < b
		   : op == "<=" ? a <= b + eps
		   : op == ">"	? a > b
						: a >= b - eps;
}

SheetTestResult RunOne( const std::string& path, const std::string& root, const std::string& ownMod, const Test& t )
{
	SheetTestResult result;
	result.name = t.name;
	Run run;
	run.test = &t;
	GameServer server;
	auto script = std::make_unique<ScriptMod>( run );
	ScriptMod* scriptMod = script.get();
	server.AddMod( std::move( script ) );
	// The mods cb_server runs by default (the brawler's kit only when named: its skills take the
	// hotbar's first slots), the test's own and those it names.
	std::set<std::string> wanted( t.mods.begin(), t.mods.end() );
	wanted.insert( ownMod );
	for ( const mods::ModInfo& info : mods::CompiledMods() )
	{
		const std::string name = info.name;
		if ( ( info.onByDefault && name != "brawl" && name != "bots" ) || wanted.count( name ) != 0 )
		{
			server.AddMod( info.create() );
			wanted.erase( name );
		}
	}
	for ( const std::string& missing : wanted )
	{
		result.failures.push_back( path + ":" + std::to_string( t.line ) + ": no mod named " + missing );
	}
	const std::string mods = root + "/server_mods/";
	ServerOptions options;
	options.port = 0;
	options.verbose = false;
	options.config.physicsArenaMB = 64;
	options.modOptions = t.options;
	options.modOptions.emplace( "chest.count", "0" );
	options.lookItems = [mods]( const std::string& mod ) {
		std::vector<LookItem> items;
		ListLookItemsFolder( mods + mod + "/client", items );
		return items;
	};
	options.lookSheets = [mods]( const std::string& mod ) {
		Sheets sheets;
		ListSheetsFolder( mods + mod + "/client", sheets );
		return sheets;
	};
	options.loadItemShape = [mods]( const std::string& mod, const std::string& kind, ItemShape& shape, std::string& error,
									std::map<std::string, float>& properties ) {
		return LoadItemShapeFolder( mods + mod + "/client", kind, shape, error, &properties );
	};
	options.loadMotions = [mods]( const std::string& mod, const std::string& set, std::string& text, std::string& error ) {
		return LoadMotionsFolder( mods + mod + "/client", set, text, error );
	};
	if ( result.failures.empty() == false || server.Start( options ) == false )
	{
		result.failures.push_back( path + ":" + std::to_string( t.line ) + ": the server did not start" );
		return result;
	}
	scriptMod->m_schema = &server.Schema();
	const ModSchema& schema = server.Schema();

	// What happened: every event said, and the values the expects ask for, on their ticks.
	struct Said
	{
		uint32_t tick;
		uint16_t type;
	};
	std::vector<Said> said;
	uint32_t counted = server.Sim().Globals().modEventCount;
	std::map<size_t, float> values; // by expect
	std::map<size_t, bool> known;
	server.SetTickObserver( [&]() {
		Simulation& sim = server.Sim();
		const SimGlobals& g = sim.Globals();
		const uint32_t kept = std::min<uint32_t>( g.modEventCount - counted, kModEventHistory );
		for ( uint32_t i = g.modEventCount - kept; i < g.modEventCount; ++i )
		{
			const ModEventRecord& e = g.modEvents[i % kModEventHistory];
			said.push_back( { e.tick, e.type } );
		}
		counted = g.modEventCount;
		if ( run.phase != 4 && run.phase != 5 )
		{
			return;
		}
		const uint32_t ran = sim.Tick() - 1; // the tick just run
		for ( size_t k = 0; k < t.expects.size(); ++k )
		{
			const Expect& e = t.expects[k];
			if ( e.kind != Expect::Kind::Value || ran != run.start + e.at )
			{
				continue;
			}
			const uint32_t tester = sim.PlayerNetId( PlayerSlot( run.tester ) );
			const uint32_t other = run.other >= 0 ? sim.PlayerNetId( PlayerSlot( run.other ) ) : 0;
			const uint32_t who = e.of == "item" ? run.item : e.of == "other" ? other : tester;
			const flecs::entity entity = sim.FindEntity( who );
			float v = 0.0f;
			bool found = true;
			if ( e.name == "distance" )
			{
				const Transform* a = sim.EntityTransform( tester );
				const Transform* b = sim.EntityTransform( other );
				found = a != nullptr && b != nullptr;
				v = found ? b3Distance( a->position, b->position ) : 0.0f;
			}
			else if ( e.name == "speed" )
			{
				const Character* c = entity.is_valid() ? entity.try_get<Character>() : nullptr;
				found = c != nullptr;
				v = found ? b3Length( c->velocity ) : 0.0f;
			}
			else if ( const int status = schema.FindStatus( e.name ); status >= 0 )
			{
				found = entity.is_valid() && sim.HasMotionState( entity );
				v = found && HasMotionStatus( sim.GetMotionState( entity ), status, sim.Tick() ) ? 1.0f : 0.0f;
			}
			else if ( const BoardField* field = schema.FindField( e.name ) )
			{
				int32_t raw = 0;
				if ( field->scope == BoardScope::Global || e.of == "world" )
				{
					raw = sim.GlobalBoardValue( int( field->slot ) );
				}
				else if ( field->scope == BoardScope::Private )
				{
					raw = run.tester >= 0 && e.of == "self" ? server.Privates( PlayerSlot( run.tester ) ).values[field->slot] : 0;
				}
				else
				{
					found = entity.is_valid() && sim.HasBoard( entity );
					raw = found ? sim.GetBoard( entity ).values[field->slot] : 0;
				}
				v = field->type == BoardType::Float ? BoardToFloat( raw ) : float( raw );
			}
			else
			{
				found = false;
			}
			values[k] = v;
			known[k] = found;
		}
	} );
	double now = 0.0;
	for ( int guard = 0; run.phase != 5 && guard < 60 * 120; ++guard )
	{
		now += 1.0 / double( kTickRate );
		server.Update( now );
	}

	// Judged.
	for ( const std::string& f : run.failures )
	{
		result.failures.push_back( path + ":" + f );
	}
	auto at = [&]( const Expect& e ) { return path + ":" + std::to_string( e.line ) + ": "; };
	for ( size_t k = 0; k < t.expects.size() && run.start != 0; ++k )
	{
		const Expect& e = t.expects[k];
		if ( e.kind == Expect::Kind::Value )
		{
			if ( known.count( k ) == 0 || known[k] == false )
			{
				result.failures.push_back( at( e ) + e.name + ": nothing of that name" + ( e.of != "self" ? " of the " + e.of : std::string() ) );
			}
			else if ( Compare( values[k], e.op, e.value ) == false )
			{
				result.failures.push_back( at( e ) + "expected " + e.name + ( e.of != "self" ? " of " + e.of : std::string() ) + " " + e.op + " " +
										   Num( e.value ) + " at " + Num( float( e.at ) / kTickRate ) + " s: it is " + Num( values[k] ) );
			}
			continue;
		}
		const int type = schema.FindEvent( e.name );
		if ( type < 0 )
		{
			result.failures.push_back( at( e ) + "no mod declares the event " + e.name );
			continue;
		}
		std::vector<float> times;
		for ( const Said& s : said )
		{
			if ( s.type == type && s.tick >= run.start )
			{
				times.push_back( float( s.tick - run.start ) / kTickRate );
			}
		}
		const float from = float( e.at ) / kTickRate, to = float( e.until ) / kTickRate;
		// (A tick's slack before: what a press starts may be said on the tick it is read.)
		const bool inSpan = std::any_of( times.begin(), times.end(), [&]( float s ) {
			return e.kind == Expect::Kind::NoEvent && e.always ? true : s >= from - 1.0f / kTickRate && s <= to;
		} );
		if ( e.kind == Expect::Kind::Event && inSpan == false )
		{
			std::string when;
			for ( size_t i = 0; i < times.size() && i < 6; ++i )
			{
				when += ( i > 0 ? ", " : "" ) + Num( times[i] );
			}
			result.failures.push_back( at( e ) + e.name + " was not said from " + Num( from ) + " to " + Num( to ) + " s" +
									   ( times.empty() ? std::string( " (never)" ) : " (at " + when + " s)" ) );
		}
		if ( e.kind == Expect::Kind::NoEvent && inSpan )
		{
			result.failures.push_back( at( e ) + e.name + " was said" + ( e.always ? std::string() : " from " + Num( from ) + " to " + Num( to ) + " s" ) );
		}
	}
	result.passed = result.failures.empty();
	return result;
}

} // namespace

std::string SheetTestRoot( const std::string& path )
{
	namespace fs = std::filesystem;
	std::error_code ec;
	fs::path at = fs::absolute( fs::path( path ), ec ).parent_path();
	while ( at.has_parent_path() && at != at.parent_path() )
	{
		if ( fs::is_directory( at / "server_mods", ec ) )
		{
			return at.generic_string();
		}
		at = at.parent_path();
	}
	return std::string();
}

bool RunSheetTests( const std::string& path, const std::string& root, std::vector<SheetTestResult>& results, std::string& error )
{
	std::ifstream in( path, std::ios::binary );
	if ( in.good() == false )
	{
		error = path + ": cannot be read";
		return false;
	}
	std::stringstream text;
	text << in.rdbuf();
	std::vector<Test> tests;
	if ( ParseTests( path, text.str(), tests, error ) == false )
	{
		return false;
	}
	// Its mod: the folder under server_mods/ it is in.
	namespace fs = std::filesystem;
	std::error_code ec;
	const std::string full = fs::absolute( fs::path( path ), ec ).generic_string();
	const std::string marker = "/server_mods/";
	const size_t at = full.find( marker );
	if ( at == std::string::npos )
	{
		error = path + ": a test is in a mod's folder (server_mods/<mod>/client/...)";
		return false;
	}
	const std::string rest = full.substr( at + marker.size() );
	const std::string mod = rest.substr( 0, rest.find( '/' ) );
	for ( const Test& t : tests )
	{
		results.push_back( RunOne( path, root, mod, t ) );
	}
	return true;
}

} // namespace cb
