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

#ifndef GUI_ARCHIPELAGO_OPTIONS_H
#define GUI_ARCHIPELAGO_OPTIONS_H

#include "gui/widget.h"

namespace GUI {
class EditTextWidget;
class StaticTextWidget;
}

namespace APBridge {

/**
 * The "Archipelago" tab of a game's options: turns the bridge on for that
 * game and sets the address of the Archipelago client it connects to.
 * Stored as ap_bridge_enabled / ap_bridge_address in the game's domain.
 */
class OptionsWidget : public GUI::OptionsContainerWidget {
public:
	OptionsWidget(GuiObject *boss, const Common::String &name, const Common::String &domain);

	void load() override;
	bool save() override;
	bool hasKeys() override;
	void setEnabled(bool e) override;

private:
	void defineLayout(GUI::ThemeEval &layouts, const Common::String &layoutName,
	                  const Common::String &overlayedLayout) const override;
	void handleCommand(GUI::CommandSender *sender, uint32 cmd, uint32 data) override;
	void updateState();

	GUI::CheckboxWidget *_enabledCheckbox;
	GUI::StaticTextWidget *_addressLabel;
	GUI::EditTextWidget *_addressField;
	GUI::StaticTextWidget *_hint;
};

} // End of namespace APBridge

#endif
