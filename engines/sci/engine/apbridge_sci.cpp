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

#include "sci/engine/apbridge_sci.h"

#include "sci/sci.h"
#include "sci/engine/kernel.h"
#include "sci/engine/object.h"
#include "sci/engine/seg_manager.h"
#include "sci/engine/script.h"
#include "sci/engine/selector.h"
#include "sci/engine/state.h"
#include "sci/engine/vm.h"

namespace Sci {

static APBridge::Bridge *activeBridge() {
	APBridge::Bridge *b = APBridge::Bridge::instance();
	return b;
}

static Common::String objName(EngineState *s, reg_t obj) {
	const char *n = s->_segMan->getObjectName(obj);
	return n ? Common::String(n) : Common::String::format("%04x:%04x", PRINT_REG(obj));
}

static Common::String className(EngineState *s, reg_t obj) {
	const Object *o = s->_segMan->getObject(obj);
	if (!o)
		return "";
	reg_t sup = o->isClass() ? o->getPos() : o->getSuperClassSelector();
	const char *n = s->_segMan->getObjectName(sup);
	return n ? Common::String(n) : Common::String();
}

static void pushArgs(APBridge::Args &out, uint16 argc, const reg_t *argp) {
	// argp[0] is argc itself
	for (uint16 i = 1; i <= argc; i++)
		out.push_back((int32)(int16)argp[i].getOffset());
}

bool apBridgeCall(EngineState *s, uint16 script, uint16 pubfunct, uint16 argc, const reg_t *argp, reg_t &acc) {
	APBridge::Bridge *b = activeBridge();
	if (!b || !b->wants("call"))
		return false;
	APBridge::Keys keys;
	keys.push_back(Common::String::format("%d", script));
	keys.push_back(Common::String::format("%d", pubfunct));
	APBridge::Args args;
	pushArgs(args, argc, argp);
	APBridge::Verdict v = b->notify("call", keys, args);
	if (v.veto)
		acc = make_reg(0, (uint16)v.value);
	return v.veto;
}

bool apBridgeSend(EngineState *s, reg_t obj, int selector, bool isMethod, uint16 argc, const reg_t *argp, reg_t &acc) {
	APBridge::Bridge *b = activeBridge();
	// Property reads made by message ("(obj prop:)") are reported as getprop.
	// Reads done with the VM's own property opcodes (inside the object's
	// methods) never come through here, so a getprop override changes what
	// other scripts see without changing the object's own behaviour.
	const char *kind = isMethod ? "send" : (argc ? "setprop" : "getprop");
	if (!b || !b->wants(kind))
		return false;
	APBridge::Keys keys;
	keys.push_back(objName(s, obj));
	keys.push_back(g_sci->getKernel()->getSelectorName(selector));
	keys.push_back(className(s, obj));
	APBridge::Args args;
	pushArgs(args, argc, argp);
	APBridge::Verdict v = b->notify(kind, keys, args);
	if (v.veto)
		acc = make_reg(0, (uint16)v.value);
	return v.veto;
}

void apBridgeKernel(EngineState *s, const char *name, int argc, const reg_t *argv) {
	APBridge::Bridge *b = activeBridge();
	if (!b || !b->wants("kernel"))
		return;
	APBridge::Keys keys;
	keys.push_back(name);
	APBridge::Args args;
	for (int i = 0; i < argc; i++)
		args.push_back((int32)(int16)argv[i].getOffset());
	b->notify("kernel", keys, args);
}

// ---------------------------------------------------------------------------

void SciBridgeAdapter::getContext(Common::HashMap<Common::String, int32> &ctx) {
	if (_state && _state->variables[VAR_GLOBAL])
		ctx["room"] = (int16)_state->variables[VAR_GLOBAL][kGlobalVarCurrentRoomNo].getOffset();
}

void SciBridgeAdapter::tick(int argc, reg_t *argv) {
	APBridge::Bridge *b = activeBridge();
	if (!b)
		return;
	_kArgc = argc;
	_kArgv = argv;
	b->tick();
	_kArgc = 0;
	_kArgv = nullptr;
}

bool SciBridgeAdapter::globalIndex(int32 n) const {
	return n >= 0 && n < (int32)_state->variablesMax[VAR_GLOBAL];
}

reg_t SciBridgeAdapter::resolveObject(const Common::String &name) {
	if (name.hasPrefix("global:")) {
		int32 n = atol(name.c_str() + 7);
		if (globalIndex(n))
			return _state->variables[VAR_GLOBAL][n];
		return NULL_REG;
	}
	// "name@script" picks the object defined in that script, for names used by
	// several scripts (e.g. a room's royalRing prop vs. the inventory item).
	Common::String base = name;
	int wantScript = -1;
	size_t at = name.findLastOf('@');
	if (at != Common::String::npos) {
		base = Common::String(name.c_str(), at);
		wantScript = atol(name.c_str() + at + 1);
	}
	Common::Array<reg_t> found = _state->_segMan->findObjectsByName(base);
	reg_t match = NULL_REG;
	int count = 0;
	for (uint i = 0; i < found.size(); i++) {
		if (wantScript >= 0) {
			const SegmentObj *seg = _state->_segMan->getSegmentObj(found[i].getSegment());
			if (!seg || seg->getType() != SEG_TYPE_SCRIPT ||
			    ((const Script *)seg)->getScriptNumber() != wantScript)
				continue;
		}
		match = found[i];
		count++;
	}
	return count == 1 ? match : NULL_REG;
}

bool SciBridgeAdapter::performAction(const Common::String &op, const APBridge::Keys &keys, const APBridge::Args &args,
                                     int32 &result, Common::String &error) {
	if (!_state || !_state->variables[VAR_GLOBAL]) {
		error = "game not initialized";
		return false;
	}
	reg_t *globals = _state->variables[VAR_GLOBAL];

	if (op == "getGlobal" || op == "setGlobal") {
		if (args.empty() || !globalIndex(args[0])) {
			error = "bad global";
			return false;
		}
		if (op == "setGlobal") {
			if (args.size() < 2) {
				error = "missing value";
				return false;
			}
			globals[args[0]] = make_reg(0, (uint16)args[1]);
		}
		result = (int16)globals[args[0]].getOffset();
		return true;
	}

	if (op == "getBit" || op == "setBit") {
		if (args.size() < 2 || args[1] < 0) {
			error = "need base and bit";
			return false;
		}
		int32 idx = args[0] + args[1] / 16;
		if (!globalIndex(args[0]) || !globalIndex(idx)) {
			error = "bit out of range";
			return false;
		}
		uint16 mask = 0x8000 >> (args[1] % 16);
		uint16 word = globals[idx].getOffset();
		result = (word & mask) ? 1 : 0;
		if (op == "setBit") {
			bool on = args.size() < 3 || args[2] != 0;
			word = on ? (word | mask) : (word & ~mask);
			globals[idx] = make_reg(0, word);
		}
		return true;
	}

	if (op == "getProp" || op == "setProp" || op == "send") {
		if (keys.size() < 2) {
			error = "need [object, selector]";
			return false;
		}
		reg_t obj = resolveObject(keys[0]);
		if (!_state->_segMan->isObject(obj)) {
			error = "no such object: " + keys[0];
			return false;
		}
		int sel = g_sci->getKernel()->findSelector(keys[1].c_str());
		if (sel < 0) {
			error = "no such selector: " + keys[1];
			return false;
		}
		if (op == "getProp") {
			result = (int16)readSelectorValue(_state->_segMan, obj, sel);
			return true;
		}
		if (op == "setProp") {
			if (args.empty()) {
				error = "missing value";
				return false;
			}
			writeSelectorValue(_state->_segMan, obj, sel, (uint16)args[0]);
			result = args[0];
			return true;
		}
		// send: invoke the method from kernel context
		if (!_kArgv) {
			error = "send is only possible from the game loop";
			return false;
		}
		Common::Array<reg_t> argv;
		for (uint i = 0; i < args.size(); i++)
			argv.push_back(make_reg(0, (uint16)args[i]));
		reg_t savedAcc = _state->r_acc;
		invokeSelector(_state, obj, sel, _kArgc, _kArgv, argv.size(), argv.empty() ? nullptr : &argv[0]);
		result = (int16)_state->r_acc.getOffset();
		_state->r_acc = savedAcc;
		return true;
	}

	if (op == "room") {
		result = (int16)globals[kGlobalVarCurrentRoomNo].getOffset();
		return true;
	}

	error = "unknown op " + op;
	return false;
}

} // End of namespace Sci
