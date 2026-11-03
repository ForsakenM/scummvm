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

#include "gui/archipelago-options.h"
#include "engines/apbridge.h"

#include "common/config-manager.h"
#include "common/translation.h"

#include "gui/ThemeEval.h"
#include "gui/widgets/edittext.h"

namespace APBridge {

enum {
	kEnabledChangedCmd = 'apEn'
};

OptionsWidget::OptionsWidget(GuiObject *boss, const Common::String &name, const Common::String &domain) :
	OptionsContainerWidget(boss, name, "ArchipelagoOptions", domain) {

	_enabledCheckbox = new GUI::CheckboxWidget(widgetsBoss(), _dialogLayout + ".enabled",
		_("Connect to an Archipelago client"),
		_("Lets an Archipelago multiworld client randomize this game. Start the client for this game, then the game."),
		kEnabledChangedCmd);

	_addressLabel = new GUI::StaticTextWidget(widgetsBoss(), _dialogLayout + ".address_desc", _("Client address:"),
		_("Where the Archipelago client listens. Leave the default unless the client says otherwise."));
	_addressLabel->setAlign(Graphics::kTextAlignEnd);
	_addressField = new GUI::EditTextWidget(widgetsBoss(), _dialogLayout + ".address", Common::U32String(Bridge::kDefaultAddress),
		_("Where the Archipelago client listens. Leave the default unless the client says otherwise."));

	_hint = new GUI::StaticTextWidget(widgetsBoss(), _dialogLayout + ".hint",
		_("The game connects to the client by itself while it runs."));
}

void OptionsWidget::defineLayout(GUI::ThemeEval &layouts, const Common::String &layoutName,
                                 const Common::String &overlayedLayout) const {
	layouts.addDialog(layoutName, overlayedLayout);
	layouts.addLayout(GUI::ThemeLayout::kLayoutVertical).addPadding(16, 16, 16, 16);

	layouts.addWidget("enabled", "Checkbox");

	layouts.addLayout(GUI::ThemeLayout::kLayoutHorizontal).addPadding(0, 0, 0, 0);
	layouts.addWidget("address_desc", "OptionsLabel");
	layouts.addWidget("address", "EditTextWidget");
	layouts.closeLayout();

	layouts.addWidget("hint", "Checkbox"); // a full-width, one-line text row

	layouts.closeLayout().closeDialog();
}

void OptionsWidget::load() {
	bool enabled = ConfMan.hasKey("ap_bridge_enabled", _domain) && ConfMan.getBool("ap_bridge_enabled", _domain);
	Common::String address = ConfMan.hasKey("ap_bridge_address", _domain) ? ConfMan.get("ap_bridge_address", _domain) : "";
	if (address.empty())
		address = Bridge::kDefaultAddress;
	_enabledCheckbox->setState(enabled);
	_addressField->setEditString(Common::U32String(address));
	updateState();
}

bool OptionsWidget::save() {
	bool enabled = _enabledCheckbox->getState();
	Common::String address = _addressField->getEditString().encode();
	address.trim();
	ConfMan.setBool("ap_bridge_enabled", enabled, _domain);
	if (address.empty() || address == Bridge::kDefaultAddress)
		ConfMan.removeKey("ap_bridge_address", _domain);
	else
		ConfMan.set("ap_bridge_address", address, _domain);
	return true;
}

bool OptionsWidget::hasKeys() {
	return ConfMan.hasKey("ap_bridge_enabled", _domain) || ConfMan.hasKey("ap_bridge_address", _domain);
}

void OptionsWidget::setEnabled(bool e) {
	_enabledCheckbox->setEnabled(e);
	updateState();
}

void OptionsWidget::updateState() {
	bool on = _enabledCheckbox->isEnabled() && _enabledCheckbox->getState();
	_addressLabel->setEnabled(on);
	_addressField->setEnabled(on);
	_hint->setEnabled(on);
}

void OptionsWidget::handleCommand(GUI::CommandSender *sender, uint32 cmd, uint32 data) {
	if (cmd == kEnabledChangedCmd) {
		updateState();
		return;
	}
	OptionsContainerWidget::handleCommand(sender, cmd, data);
}

} // End of namespace APBridge
