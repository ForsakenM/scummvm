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

#include "scumm/apbridge_scumm.h"
#include "scumm/scumm.h"
#include "scumm/object.h"

namespace Scumm {

static APBridge::Bridge *bridge() {
	return APBridge::Bridge::instance();
}

static Common::String num(int n) {
	return Common::String::format("%d", n);
}

void ScummBridgeAdapter::getContext(Common::HashMap<Common::String, int32> &ctx) {
	ctx["room"] = _vm->_currentRoom;
	if (_vm->VAR_EGO != 0xFF)
		ctx["ego"] = _vm->VAR(_vm->VAR_EGO);
}

void ScummBridgeAdapter::tick() {
	if (APBridge::Bridge *b = bridge())
		b->tick();
}

bool ScummBridgeAdapter::onPutOwner(int obj, int owner) {
	APBridge::Bridge *b = bridge();
	if (!b || !b->wants("owner"))
		return false;
	APBridge::Keys keys;
	keys.push_back(num(obj));
	APBridge::Args args;
	args.push_back(owner);
	args.push_back(_vm->_objectOwnerTable[obj]);
	args.push_back((_vm->VAR_EGO != 0xFF && owner == _vm->VAR(_vm->VAR_EGO)) ? 1 : 0);
	return b->notify("owner", keys, args).veto;
}

bool ScummBridgeAdapter::onWriteVar(unsigned int var, int value) {
	APBridge::Bridge *b = bridge();
	if (!b)
		return false;
	const char *kind;
	unsigned int index;
	if (!(var & 0xF000)) {
		kind = "var";
		index = var;
	} else if (var & 0x8000) {
		kind = "bitvar";
		index = var & 0x7FFF;
	} else {
		return false; // local/script variables are not reported
	}
	if (!b->wants(kind))
		return false;
	APBridge::Keys keys;
	keys.push_back(num(index));
	APBridge::Args args;
	args.push_back(value);
	args.push_back(_vm->readVar(var));
	return b->notify(kind, keys, args).veto;
}

void ScummBridgeAdapter::onRoom(int room) {
	APBridge::Bridge *b = bridge();
	if (!b || !b->wants("room"))
		return;
	APBridge::Keys keys;
	keys.push_back(num(room));
	b->notify("room", keys, APBridge::Args());
}

bool ScummBridgeAdapter::onRunScript(int script) {
	APBridge::Bridge *b = bridge();
	if (!b || !b->wants("script"))
		return false;
	APBridge::Keys keys;
	keys.push_back(num(script));
	return b->notify("script", keys, APBridge::Args()).veto;
}

bool ScummBridgeAdapter::performAction(const Common::String &op, const APBridge::Keys &keys, const APBridge::Args &args,
                                       int32 &result, Common::String &error) {
	int ego = (_vm->VAR_EGO != 0xFF) ? _vm->VAR(_vm->VAR_EGO) : 0;

	if (op == "room") {
		result = _vm->_currentRoom;
		return true;
	}
	if (args.empty()) {
		error = "missing arguments";
		return false;
	}

	if (op == "getVar" || op == "setVar" || op == "getBit" || op == "setBit") {
		bool bit = op.hasSuffix("Bit");
		uint var = bit ? (0x8000 | (uint)args[0]) : (uint)args[0];
		if (!bit && args[0] >= _vm->_numVariables) {
			error = "variable out of range";
			return false;
		}
		if (bit && args[0] >= _vm->_numBitVariables) {
			error = "bit variable out of range";
			return false;
		}
		if (op.hasPrefix("set")) {
			if (args.size() < 2) {
				error = "missing value";
				return false;
			}
			_vm->writeVar(var, bit ? (args[1] ? 1 : 0) : args[1]);
		}
		result = _vm->readVar(var);
		return true;
	}

	if (op == "runScript") {
		int scriptArgs[NUM_SCRIPT_LOCAL];
		memset(scriptArgs, 0, sizeof(scriptArgs));
		for (uint i = 1; i < args.size() && i - 1 < NUM_SCRIPT_LOCAL; i++)
			scriptArgs[i - 1] = args[i];
		_vm->runScript(args[0], false, false, scriptArgs);
		result = 1;
		return true;
	}

	int obj = args[0];
	if (obj <= 0 || obj >= _vm->_numGlobalObjects) {
		error = "object out of range";
		return false;
	}

	if (op == "getOwner") {
		result = _vm->getOwner(obj);
		return true;
	}
	if (op == "setOwner") {
		int owner = args.size() > 1 ? args[1] : -1;
		if (owner < 0)
			owner = ego;
		_vm->setOwnerOf(obj, owner);
		result = owner;
		return true;
	}
	if (op == "give") {
		if (_vm->whereIsObject(obj) != WIO_INVENTORY) {
			if (args.size() < 2) {
				error = "give needs [object, homeRoom]";
				return false;
			}
			_vm->addObjectToInventory(obj, args[1]);
		}
		_vm->putOwner(obj, ego);
		_vm->runInventoryScript(1);
		result = ego;
		return true;
	}
	if (op == "take") {
		_vm->setOwnerOf(obj, 0);
		result = 0;
		return true;
	}
	error = "unknown op " + op;
	return false;
}

} // End of namespace Scumm
