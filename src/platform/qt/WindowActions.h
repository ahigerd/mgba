/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QList>
#include <QMap>
#include <QObject>

#include <memory>

#include <mgba/core/core.h>

#include "ActionMapper.h"
#include "CoreConsumer.h"

namespace QGBA {

class ConfigController;
class Display;
class LoadSaveState;
class ShortcutController;
class Window;
class WindowPopups;

class WindowActions : public QObject {
Q_OBJECT
public:
	WindowActions(QObject* parent = nullptr);

	ActionMapper* actionMapper();
	ShortcutController* shortcutController() const;

	void setCoreController(CoreProvider*);
	void setConfigController(ConfigController*);

	void setWindow(Window*);

	void updateMultiplayerStatus(bool);
	void setActivePlatform(mPlatform);
	void setNonMultiplayerActionsEnabled(bool);
	void setScaleFactor(int factor);
	void updateLayers();
	void updateMRU(const QStringList& mru);

private:
	void setupFileMenu();
	void setupFileSavesMenu();
	void setupFileQuickMenu(bool isLoad);
	void setupEmuMenu();
	void setupAVMenu();
	void setupToolsMenu();
	void setupHiddenActions();

	QGBA::Display* display();
	LoadSaveState* stateWindow();

	std::shared_ptr<Action> addGameAction(const QString& visibleName, const QString& name, Action::Function action, const QString& menu = {}, const QKeySequence& = {});
	template<typename T, typename V> std::shared_ptr<Action> addGameAction(const QString& visibleName, const QString& name, T* obj, V (T::*action)(), const QString& menu = {}, const QKeySequence& = {});
	template<typename V> std::shared_ptr<Action> addGameAction(const QString& visibleName, const QString& name, V (CoreController::*action)(), const QString& menu = {}, const QKeySequence& = {});
	std::shared_ptr<Action> addGameAction(const QString& visibleName, const QString& name, Action::BooleanFunction action, const QString& menu = {}, const QKeySequence& = {});

	ActionMapper m_actions;
	QList<std::shared_ptr<Action>> m_gameActions;
	QList<std::shared_ptr<Action>> m_nonMpActions;
	QMultiMap<mPlatform, std::shared_ptr<Action>> m_platformActions;
	std::shared_ptr<Action> m_multiWindow;
	QMap<int, std::shared_ptr<Action>> m_frameSizes;

	Window* m_window;
	WindowPopups* m_popups;
	QMenuBar* m_menuBar;
	CorePointer<WindowActions> m_controller;
	ShortcutController* m_shortcutController;
};

}
