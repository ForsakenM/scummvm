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

#define FORBIDDEN_SYMBOL_EXCEPTION_getenv

#include "engines/apbridge.h"

#include "common/config-manager.h"
#include "common/debug.h"
#include "common/formats/json.h"
#include "common/system.h"
#include "common/textconsole.h"
#include "common/ustr.h"

#ifdef USE_LIBCURL
#include "backends/networking/basic/socket.h"
#endif

namespace APBridge {

static const int kProtocolVersion = 1;
static const uint32 kReconnectDelay = 2000;
static const uint kMaxQueued = 4096;

static Bridge *s_bridge = nullptr;

Common::String Bridge::configuredEndpoint() {
	// Per-game (or global) settings from the Archipelago options tab
	if (ConfMan.hasKey("ap_bridge_enabled") && ConfMan.getBool("ap_bridge_enabled")) {
		Common::String address = ConfMan.hasKey("ap_bridge_address") ? ConfMan.get("ap_bridge_address") : "";
		address.trim();
		return address.empty() ? Common::String(kDefaultAddress) : address;
	}
	// Older/advanced ways to turn it on: a raw "ap_bridge" key or an environment variable
	if (ConfMan.hasKey("ap_bridge") && !ConfMan.get("ap_bridge").empty())
		return ConfMan.get("ap_bridge");
	const char *env = getenv("SCUMMVM_AP_BRIDGE");
	return env ? Common::String(env) : Common::String();
}

Bridge *Bridge::instance() {
	return s_bridge;
}

Bridge *Bridge::startGame() {
	Common::String endpoint = configuredEndpoint();
	if (endpoint.empty()) {
		delete s_bridge;
		s_bridge = nullptr;
		return nullptr;
	}
#ifdef USE_LIBCURL
	if (s_bridge && s_bridge->_endpoint != endpoint) {
		delete s_bridge;
		s_bridge = nullptr;
	}
	if (!s_bridge)
		s_bridge = new Bridge(endpoint);
#else
	warning("APBridge: requested (%s) but this build has no libcurl networking", endpoint.c_str());
#endif
	return s_bridge;
}

Bridge::Bridge(const Common::String &endpoint)
	: _endpoint(endpoint), _socket(nullptr), _connected(false), _nextConnectTime(0),
	  _adapter(nullptr), _inAction(false) {
}

Bridge::~Bridge() {
	disconnect("shutdown");
}

void Bridge::attach(Adapter *adapter, const Common::String &gameId, const Common::String &extra) {
	_adapter = adapter;
	_gameId = gameId;
	_extra = extra;
	_rules.clear();
	rebuildKindIndex();
	if (_connected)
		sendHello();
	else
		_nextConnectTime = 0;
}

void Bridge::detach(Adapter *adapter) {
	if (_adapter != adapter)
		return;
	_adapter = nullptr;
	_rules.clear();
	rebuildKindIndex();
	if (_connected)
		send("{\"type\":\"bye\"}");
}

// ---------------------------------------------------------------------------
// Connection handling

void Bridge::connect() {
#ifdef USE_LIBCURL
	Common::String url = _endpoint.contains("://") ? _endpoint : "http://" + _endpoint;
	_socket = Networking::Socket::connect(url);
	if (!_socket) {
		_nextConnectTime = g_system->getMillis() + kReconnectDelay;
		return;
	}
	_connected = true;
	_rxBuffer.clear();
	debug(1, "APBridge: connected to %s", _endpoint.c_str());
	sendHello();
	for (uint i = 0; i < _outQueue.size(); i++)
		send(_outQueue[i]);
	_outQueue.clear();
#endif
}

void Bridge::disconnect(const char *reason) {
#ifdef USE_LIBCURL
	if (_socket) {
		debug(1, "APBridge: disconnected (%s)", reason);
		delete _socket;
		_socket = nullptr;
	}
#endif
	_connected = false;
	_tracedKinds.clear();
	_nextConnectTime = g_system->getMillis() + kReconnectDelay;
}

void Bridge::send(const Common::String &line) {
#ifdef USE_LIBCURL
	if (!_connected || !_socket) {
		if (_outQueue.size() < kMaxQueued)
			_outQueue.push_back(line);
		return;
	}
	Common::String data = line + "\n";
	if (_socket->send(data.c_str(), data.size()) != data.size())
		disconnect("send failed");
#endif
}

void Bridge::sendHello() {
	Common::JSONObject o;
	o["type"] = new Common::JSONValue("hello");
	o["protocol"] = new Common::JSONValue((long long int)kProtocolVersion);
	o["engine"] = new Common::JSONValue(_adapter ? _adapter->engineId() : "");
	o["gameid"] = new Common::JSONValue(_gameId);
	o["extra"] = new Common::JSONValue(_extra);
	o["target"] = new Common::JSONValue(ConfMan.getActiveDomainName());
	Common::JSONValue v(o);
	send(v.stringify());
}

void Bridge::tick() {
#ifdef USE_LIBCURL
	if (!_connected) {
		if (_adapter && g_system->getMillis() >= _nextConnectTime)
			connect();
		return;
	}
	// Drain everything that is available right now.
	char buf[1024];
	for (int guard = 0; guard < 64 && _socket && _socket->ready() > 0; guard++) {
		size_t n = _socket->recv(buf, sizeof(buf));
		if (n == 0) {
			disconnect("closed by peer");
			return;
		}
		_rxBuffer += Common::String(buf, n);
	}
	size_t nl;
	while ((nl = _rxBuffer.findFirstOf('\n')) != Common::String::npos) {
		Common::String line(_rxBuffer.c_str(), nl);
		_rxBuffer = Common::String(_rxBuffer.c_str() + nl + 1);
		line.trim();
		if (!line.empty())
			handleLine(line);
		if (!_connected)
			return;
	}
#endif
}

// ---------------------------------------------------------------------------
// Rules and events

void Bridge::rebuildKindIndex() {
	_watchedKinds.clear();
	for (uint i = 0; i < _rules.size(); i++)
		_watchedKinds[_rules[i].kind] = true;
}

bool Bridge::matches(const Rule &rule, const char *kind, const Keys &keys, const Args &args,
                     const Common::HashMap<Common::String, int32> &ctx) const {
	if (rule.kind != kind)
		return false;
	if (rule.keys.size() > keys.size())
		return false;
	for (uint i = 0; i < rule.keys.size(); i++)
		if (rule.keys[i] != "*" && !rule.keys[i].equalsIgnoreCase(keys[i]))
			return false;
	if (rule.args.size() > args.size())
		return false;
	for (uint i = 0; i < rule.args.size(); i++)
		if (rule.args[i] != "*" && (int32)atol(rule.args[i].c_str()) != args[i])
			return false;
	for (Common::HashMap<Common::String, int32>::const_iterator it = rule.ctx.begin(); it != rule.ctx.end(); ++it) {
		if (!ctx.contains(it->_key) || ctx[it->_key] != it->_value)
			return false;
	}
	return true;
}

Common::String Bridge::eventJson(const char *type, const char *kind, const Keys &keys, const Args &args,
                                 const Common::HashMap<Common::String, int32> &ctx, int32 ruleId) {
	Common::JSONObject o;
	o["type"] = new Common::JSONValue(type);
	o["kind"] = new Common::JSONValue(kind);
	if (ruleId >= 0)
		o["rule"] = new Common::JSONValue((long long int)ruleId);
	Common::JSONArray k, a;
	for (uint i = 0; i < keys.size(); i++)
		k.push_back(new Common::JSONValue(keys[i]));
	for (uint i = 0; i < args.size(); i++)
		a.push_back(new Common::JSONValue((long long int)args[i]));
	o["keys"] = new Common::JSONValue(k);
	o["args"] = new Common::JSONValue(a);
	Common::JSONObject c;
	for (Common::HashMap<Common::String, int32>::const_iterator it = ctx.begin(); it != ctx.end(); ++it)
		c[it->_key] = new Common::JSONValue((long long int)it->_value);
	o["ctx"] = new Common::JSONValue(c);
	o["action"] = new Common::JSONValue(_inAction);
	Common::JSONValue v(o);
	return v.stringify();
}

Verdict Bridge::notify(const char *kind, const Keys &keys, const Args &args) {
	Verdict verdict;
	Common::HashMap<Common::String, int32> ctx;
	if (_adapter)
		_adapter->getContext(ctx);

	if (_connected && _tracedKinds.contains(kind))
		send(eventJson("trace", kind, keys, args, ctx, -1));

	for (uint i = 0; i < _rules.size(); i++) {
		const Rule &rule = _rules[i];
		if (!matches(rule, kind, keys, args, ctx))
			continue;
		// Events caused by the client's own actions are reported but never vetoed,
		// so that "give the item" actions are not swallowed by "suppress pickup" rules.
		if (_inAction)
			continue;
		send(eventJson("match", kind, keys, args, ctx, rule.id));
		if (rule.veto) {
			verdict.veto = true;
			verdict.value = rule.value;
		}
		if (rule.once) {
			_rules.remove_at(i);
			rebuildKindIndex();
			i--;
		}
		if (verdict.veto)
			break;
	}
	return verdict;
}

// ---------------------------------------------------------------------------
// Client commands

static Common::String jsonToKey(const Common::JSONValue *v) {
	if (v->isString())
		return v->asString();
	if (v->isIntegerNumber())
		return Common::String::format("%lld", v->asIntegerNumber());
	if (v->isNumber())
		return Common::String::format("%d", (int)v->asNumber());
	return "*";
}

static int32 jsonToInt(const Common::JSONValue *v, int32 def = 0) {
	if (!v)
		return def;
	if (v->isIntegerNumber())
		return (int32)v->asIntegerNumber();
	if (v->isNumber())
		return (int32)v->asNumber();
	if (v->isBool())
		return v->asBool() ? 1 : 0;
	if (v->isString())
		return (int32)atol(v->asString().c_str());
	return def;
}

void Bridge::handleLine(const Common::String &line) {
	Common::JSONValue *root = Common::JSON::parse(line.c_str());
	if (!root || !root->isObject()) {
		warning("APBridge: bad message: %s", line.c_str());
		delete root;
		return;
	}
	const Common::JSONObject &o = root->asObject();
	Common::String cmd = o.contains("cmd") && o["cmd"]->isString() ? o["cmd"]->asString() : "";
	int32 req = o.contains("req") ? jsonToInt(o["req"], -1) : -1;

	Common::JSONObject reply;
	reply["type"] = new Common::JSONValue("result");
	reply["cmd"] = new Common::JSONValue(cmd);
	if (req >= 0)
		reply["req"] = new Common::JSONValue((long long int)req);
	bool ok = true;
	Common::String error;

	if (cmd == "watch") {
		Rule r;
		r.id = jsonToInt(o.contains("rule") ? o["rule"] : nullptr, -1);
		r.kind = o.contains("kind") ? jsonToKey(o["kind"]) : "";
		if (o.contains("keys") && o["keys"]->isArray())
			for (uint i = 0; i < o["keys"]->asArray().size(); i++)
				r.keys.push_back(jsonToKey(o["keys"]->asArray()[i]));
		if (o.contains("args") && o["args"]->isArray())
			for (uint i = 0; i < o["args"]->asArray().size(); i++)
				r.args.push_back(jsonToKey(o["args"]->asArray()[i]));
		if (o.contains("ctx") && o["ctx"]->isObject()) {
			const Common::JSONObject &c = o["ctx"]->asObject();
			for (Common::JSONObject::const_iterator it = c.begin(); it != c.end(); ++it)
				r.ctx[it->_key] = jsonToInt(it->_value);
		}
		r.veto = o.contains("veto") && o["veto"]->isBool() && o["veto"]->asBool();
		r.value = jsonToInt(o.contains("value") ? o["value"] : nullptr);
		r.once = o.contains("once") && o["once"]->isBool() && o["once"]->asBool();
		if (r.kind.empty()) {
			ok = false;
			error = "watch needs a kind";
		} else {
			for (uint i = 0; i < _rules.size(); i++)
				if (_rules[i].id == r.id && r.id >= 0)
					_rules.remove_at(i--);
			_rules.push_back(r);
			rebuildKindIndex();
		}
	} else if (cmd == "unwatch") {
		int32 id = jsonToInt(o.contains("rule") ? o["rule"] : nullptr, -1);
		for (uint i = 0; i < _rules.size(); i++)
			if (_rules[i].id == id)
				_rules.remove_at(i--);
		rebuildKindIndex();
	} else if (cmd == "clear") {
		_rules.clear();
		rebuildKindIndex();
	} else if (cmd == "trace") {
		Common::String kind = o.contains("kind") ? jsonToKey(o["kind"]) : "";
		bool on = !o.contains("on") || jsonToInt(o["on"], 1) != 0;
		if (on)
			_tracedKinds[kind] = true;
		else
			_tracedKinds.erase(kind);
	} else if (cmd == "say") {
		Common::String text = o.contains("text") && o["text"]->isString() ? o["text"]->asString() : "";
		g_system->displayMessageOnOSD(Common::U32String(text, Common::kUtf8));
	} else if (cmd == "do") {
		if (!_adapter) {
			ok = false;
			error = "no game running";
		} else {
			Common::String op = o.contains("op") ? jsonToKey(o["op"]) : "";
			Keys keys;
			Args args;
			if (o.contains("keys") && o["keys"]->isArray())
				for (uint i = 0; i < o["keys"]->asArray().size(); i++)
					keys.push_back(jsonToKey(o["keys"]->asArray()[i]));
			if (o.contains("args") && o["args"]->isArray())
				for (uint i = 0; i < o["args"]->asArray().size(); i++)
					args.push_back(jsonToInt(o["args"]->asArray()[i]));
			int32 result = 0;
			// "asGame": run the action as if the player did it, so rules can veto
			// what it causes (used by test harnesses); otherwise the client's own
			// actions are never vetoed.
			bool asGame = o.contains("asGame") && o["asGame"]->isBool() && o["asGame"]->asBool();
			_inAction = !asGame;
			ok = _adapter->performAction(op, keys, args, result, error);
			_inAction = false;
			reply["value"] = new Common::JSONValue((long long int)result);
		}
	} else if (cmd == "ping") {
		// just reply
	} else {
		ok = false;
		error = "unknown cmd " + cmd;
	}

	reply["ok"] = new Common::JSONValue(ok);
	if (!ok)
		reply["error"] = new Common::JSONValue(error);
	Common::JSONValue v(reply);
	send(v.stringify());
	delete root;
}

} // End of namespace APBridge
