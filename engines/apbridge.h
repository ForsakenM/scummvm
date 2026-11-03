/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef ENGINES_APBRIDGE_H
#define ENGINES_APBRIDGE_H

/*
 * Archipelago bridge: a game-agnostic event/action channel between a running
 * engine and an external randomizer client (e.g. an Archipelago client).
 *
 * The bridge knows nothing about any particular game. Engines report
 * low-level VM events ("hooks") such as procedure calls, object messages,
 * variable writes or inventory ownership changes, each identified by a list
 * of string keys plus integer arguments. The client registers rules that
 * match those events; a matching rule is reported back and may veto the
 * event (e.g. suppress a vanilla item pickup). The client can also ask the
 * engine to perform primitive actions (set a variable, give an item, ...),
 * which each engine implements in its Adapter.
 *
 * Enable it by setting the config key "ap_bridge" (or the environment
 * variable SCUMMVM_AP_BRIDGE) to "host:port". The engine connects out to
 * the client, which listens on that port. Protocol: newline-delimited JSON,
 * see doc/archipelago-bridge.md.
 */

#include "common/array.h"
#include "common/str.h"
#include "common/hashmap.h"
#include "common/hash-str.h"

namespace Networking {
class Socket;
}

namespace APBridge {

typedef Common::Array<Common::String> Keys;
typedef Common::Array<int32> Args;

/** Result of reporting an event to the bridge. */
struct Verdict {
	bool veto;      ///< true: the engine must skip the event
	int32 value;    ///< replacement result for a vetoed call/send (accumulator)
	Verdict() : veto(false), value(0) {}
};

/** Implemented once per engine. Performs primitive actions for the client. */
class Adapter {
public:
	virtual ~Adapter() {}
	/** Short engine identifier, e.g. "sci" or "scumm". */
	virtual const char *engineId() const = 0;
	/** Context integers attached to every event (e.g. current room). */
	virtual void getContext(Common::HashMap<Common::String, int32> &ctx) {}
	/**
	 * Perform an action requested by the client. Return true on success and
	 * fill @p result; on failure fill @p error. Hooks fired while an action
	 * runs are traced but never vetoed.
	 */
	virtual bool performAction(const Common::String &op, const Keys &keys, const Args &args,
	                           int32 &result, Common::String &error) = 0;
};

class Bridge {
public:
	/** Returns the bridge if enabled in the configuration, else nullptr. */
	static Bridge *instance();

	/** Called by an engine once it is running. @p gameId is the detected game id. */
	void attach(Adapter *adapter, const Common::String &gameId, const Common::String &extra);
	/** Called by an engine before it is destroyed. */
	void detach(Adapter *adapter);

	/** Cheap check engines use before building an event. */
	bool wants(const char *kind) const {
		// Rules stay active while the client is disconnected so that vetoes keep
		// working; matches are queued and delivered on reconnect.
		return _watchedKinds.contains(kind) || (_connected && _tracedKinds.contains(kind));
	}

	/** Report an event. May only veto when no client action is executing. */
	Verdict notify(const char *kind, const Keys &keys, const Args &args);

	/** Call once per frame from a point where performing actions is safe. */
	void tick();

	/** True while the bridge executes a client action (hooks must not veto). */
	bool inAction() const { return _inAction; }

private:
	Bridge(const Common::String &endpoint);
	~Bridge();

	struct Rule {
		int32 id;
		Common::String kind;
		Keys keys;           ///< "*" matches anything
		Common::Array<Common::String> args; ///< prefix match; "*" wildcard
		Common::HashMap<Common::String, int32> ctx; ///< required context values
		bool veto;
		int32 value;
		bool once;
	};

	bool matches(const Rule &rule, const char *kind, const Keys &keys, const Args &args,
	             const Common::HashMap<Common::String, int32> &ctx) const;
	void connect();
	void disconnect(const char *reason);
	void send(const Common::String &line);
	void handleLine(const Common::String &line);
	void sendHello();
	void rebuildKindIndex();
	Common::String eventJson(const char *type, const char *kind, const Keys &keys, const Args &args,
	                         const Common::HashMap<Common::String, int32> &ctx, int32 ruleId);

	Common::String _endpoint;
	Networking::Socket *_socket;
	bool _connected;
	uint32 _nextConnectTime;
	Common::String _rxBuffer;
	Adapter *_adapter;
	Common::String _gameId, _extra;
	bool _inAction;
	Common::Array<Rule> _rules;
	Common::HashMap<Common::String, bool> _watchedKinds;
	Common::HashMap<Common::String, bool> _tracedKinds;
	Common::Array<Common::String> _outQueue; ///< events raised before connect
};

} // End of namespace APBridge

#endif
