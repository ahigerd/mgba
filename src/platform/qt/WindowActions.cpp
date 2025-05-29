/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "WindowActions.h"
#include "Window.h"
#include "Window_p.h"

#include <QMenuBar>

#include "AboutScreen.h"
#include "BattleChipView.h"
#include "CoreController.h"
#include "Display.h"
#include "ForwarderView.h"
#include "IOViewer.h"
#include "MapView.h"
#include "MemoryAccessLogView.h"
#include "MemorySearch.h"
#include "MemoryView.h"
#include "ObjView.h"
#include "PaletteView.h"
#include "PlacementControl.h"
#include "ReportView.h"
#include "ROMInfo.h"
#include "SaveConverter.h"
#include "ShortcutController.h"
#include "TileView.h"

#include <mgba/core/cheats.h>
#include <mgba/core/core.h>
#include <mgba/internal/gba/input.h>
#ifdef M_CORE_GB
#include <mgba/internal/gb/gb.h>
#include <mgba/internal/gb/video.h>
#endif
#ifdef M_CORE_GBA
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/gba.h>
#endif

using namespace QGBA;

WindowActions::WindowActions(QObject* parent)
	: QObject(parent)
	, m_menuBar(nullptr)
	, m_controller(this)
	, m_shortcutController(new ShortcutController(this))
{
	m_shortcutController->setActionMapper(&m_actions);
	installEventFilter(m_shortcutController);
}

ActionMapper* WindowActions::actionMapper() {
	return &m_actions;
}

ShortcutController* WindowActions::shortcutController() const {
	return m_shortcutController;
}

void WindowActions::setCoreController(CoreProvider* provider) {
	m_controller.setCoreProvider(provider);
}

void WindowActions::setConfigController(ConfigController* controller) {
	m_shortcutController->setConfigController(controller);
}

std::shared_ptr<Action> WindowActions::addGameAction(const QString& visibleName, const QString& name, Action::Function function, const QString& menu, const QKeySequence& shortcut) {
	auto action = m_actions.addAction(visibleName, name, [this, function = std::move(function)]() {
		if (m_controller) {
			function();
		}
	}, menu, shortcut);
	m_gameActions.append(action);
	return action;
}

template<typename T, typename V>
std::shared_ptr<Action> WindowActions::addGameAction(const QString& visibleName, const QString& name, T* obj, V (T::*method)(), const QString& menu, const QKeySequence& shortcut) {
	return addGameAction(visibleName, name, [obj, method]() {
		(obj->*method)();
	}, menu, shortcut);
}

template<typename V>
std::shared_ptr<Action> WindowActions::addGameAction(const QString& visibleName, const QString& name, V (CoreController::*method)(), const QString& menu, const QKeySequence& shortcut) {
	return addGameAction(visibleName, name, [this, method]() {
		(m_controller.get()->*method)();
	}, menu, shortcut);
}

std::shared_ptr<Action> WindowActions::addGameAction(const QString& visibleName, const QString& name, Action::BooleanFunction function, const QString& menu, const QKeySequence& shortcut) {
	auto action = m_actions.addBooleanAction(visibleName, name, [this, function = std::move(function)](bool value) {
		if (m_controller) {
			function(value);
		}
	}, menu, shortcut);
	m_gameActions.append(action);
	return action;
}

void WindowActions::setWindow(Window* window) {
	m_window = window;
	m_popups = window->popups();
	m_menuBar = window->menuBar();

	m_menuBar->clear();
	setupFileMenu();
	setupEmuMenu();
	setupAVMenu();
	setupToolsMenu();
	setupHiddenActions();

	for (auto& action : m_gameActions) {
		action->setEnabled(false);
	}

	m_shortcutController->rebuildItems();
	m_actions.rebuildMenu(m_menuBar, m_window, *m_shortcutController);
}

QGBA::Display* WindowActions::display() {
	return m_window->m_display.get();
}

LoadSaveState* WindowActions::stateWindow() {
	return m_window->m_stateWindow;
}

void WindowActions::setupFileMenu() {
	m_actions.addMenu(tr("&File"), "file");

	m_actions.addAction(tr("Load &ROM..."), "loadROM", m_window, &Window::selectROM, "file", QKeySequence::Open);

#ifdef USE_SQLITE3
	m_actions.addAction(tr("Load ROM in archive..."), "loadROMInArchive", m_window, &Window::selectROMInArchive, "file");
	m_actions.addAction(tr("Add folder to library..."), "addDirToLibrary", m_window, &Window::addDirToLibrary, "file");
#endif

	setupFileSavesMenu();

	m_actions.addAction(tr("Load &patch..."), "loadPatch", m_window, &Window::selectPatch, "file");

#ifdef M_CORE_GBA
	m_actions.addAction(tr("Boot BIOS"), "bootBIOS", m_window, &Window::bootBIOS, "file");
#endif

#ifdef M_CORE_GBA
	auto scanCard = addGameAction(tr("Scan e-Reader dotcodes..."), "scanCard", m_window, &Window::scanCard, "file");
	m_platformActions.insert(mPLATFORM_GBA, scanCard);
#endif

	addGameAction(tr("ROM &info..."), "romInfo", PopupManager<ROMInfo>().withController(m_controller), "file");

	m_actions.addMenu(tr("Recent"), "mru", "file");
	m_actions.addSeparator("file");

	auto loadState = addGameAction(tr("&Load state"), "loadState", [this]() {
		m_window->openStateWindow(LoadSave::LOAD);
	}, "file", QKeySequence("F10"));
	m_nonMpActions.append(loadState);

	auto loadStateFile = addGameAction(tr("Load state file..."), "loadStateFile", [this]() {
		m_window->selectState(true);
	}, "file");
	m_nonMpActions.append(loadStateFile);

	auto saveState = addGameAction(tr("&Save state"), "saveState", [this]() {
		m_window->openStateWindow(LoadSave::SAVE);
	}, "file", QKeySequence("Shift+F10"));
	m_nonMpActions.append(saveState);

	auto saveStateFile = addGameAction(tr("Save state file..."), "saveStateFile", [this]() {
		m_window->selectState(false);
	}, "file");
	m_nonMpActions.append(saveStateFile);

	setupFileQuickMenu(true);
	setupFileQuickMenu(false);

	m_actions.addSeparator("file");
	m_multiWindow = m_actions.addAction(tr("New multiplayer window"), "multiWindow", GBAApp::app(), &GBAApp::newWindow, "file");

#ifdef M_CORE_GBA
	auto dolphin = m_actions.addAction(tr("Connect to Dolphin..."), "connectDolphin", m_popups->dolphinView, "file");
	m_platformActions.insert(mPLATFORM_GBA, dolphin);
#endif

	m_actions.addSeparator("file");

	m_actions.addAction(tr("Report bug..."), "bugReport", PopupManager<ReportView>(), "file");

#ifndef Q_OS_MAC
	m_actions.addSeparator("file");
#endif

	m_actions.addAction(tr("About..."), "about", PopupManager<AboutScreen>(), "file")->setRole(Action::Role::ABOUT);
	m_actions.addAction(tr("E&xit"), "quit", &QApplication::quit, "file", QKeySequence::Quit)->setRole(Action::Role::QUIT);
}

void WindowActions::setupFileSavesMenu() {
	m_actions.addMenu(tr("Save games"), "saves", "file");
	addGameAction(tr("Load alternate save game..."), "loadAlternateSave", [this]() {
		m_window->selectSave(false);
	}, "saves");
	addGameAction(tr("Load temporary save game..."), "loadTemporarySave", [this]() {
		m_window->selectSave(true);
	}, "saves");

	m_actions.addSeparator("saves");

	m_actions.addAction(tr("Convert save game..."), "convertSave", PopupManager<SaveConverter>(), "saves");

#ifdef M_CORE_GBA
	auto importShark = addGameAction(tr("Import GameShark Save..."), "importShark", m_window, &Window::importSharkport, "saves");
	m_platformActions.insert(mPLATFORM_GBA, importShark);

	auto exportShark = addGameAction(tr("Export GameShark Save..."), "exportShark", m_window, &Window::exportSharkport, "saves");
	m_platformActions.insert(mPLATFORM_GBA, exportShark);
#endif

	m_actions.addSeparator("saves");
	std::shared_ptr<Action> savePlayerAction;
	ConfigOption* savePlayer = m_window->config()->addOption("savePlayerId");
	savePlayerAction = savePlayer->addValue(tr("Automatically determine"), 0, &m_actions, "saves");
	m_nonMpActions.append(savePlayerAction);

	for (int i = 1; i < 5; ++i) {
		savePlayerAction = savePlayer->addValue(tr("Use player %0 save game").arg(i), i, &m_actions, "saves");
		m_nonMpActions.append(savePlayerAction);
	}
	savePlayer->connect([this](const QVariant& value) {
		if (m_controller) {
			m_controller->changePlayer(value.toInt());
		}
	}, this);
	m_window->config()->updateOption("savePlayerId");
}

struct QuickMenu {
	QString section;
	QString sectionLabel;
	QString recentLabel;
	QString undo;
	QString undoLabel;

	using BackupFn = void (CoreController::*)();
	BackupFn loadSaveBackup;

	using SlotFn = void (CoreController::*)(int);
	SlotFn loadSaveSlot;

	QKeySequence shortcut;
	QString slotShortcut;
};

void WindowActions::setupFileQuickMenu(bool isLoad) {
	static QuickMenu loadMenu = {
		.section = "quickLoad",
		.sectionLabel = tr("Quick load"),
		.recentLabel = tr("Load recent"),
		.undo = "undoLoadState",
		.undoLabel = tr("Undo load state"),
		.loadSaveBackup = &CoreController::loadBackupState,
		.loadSaveSlot = &CoreController::loadState,
		.shortcut = QKeySequence("F11"),
		.slotShortcut = "F%1",
	};
	static QuickMenu saveMenu = {
		.section = "quickSave",
		.sectionLabel = tr("Quick save"),
		.recentLabel = tr("Save recent"),
		.undo = "undoSaveState",
		.undoLabel = tr("Undo save state"),
		.loadSaveBackup = &CoreController::saveBackupState,
		.loadSaveSlot = &CoreController::saveState,
		.shortcut = QKeySequence("Shift+F11"),
		.slotShortcut = "Shift+F%1",
	};
	QuickMenu m = isLoad ? loadMenu : saveMenu;
	m_actions.addMenu(m.sectionLabel, m.section, "file");

	auto quickAction = addGameAction(m.recentLabel, m.section, [this, m]{ (m_controller.get()->*(m.loadSaveSlot))(0); }, m.section);
	m_nonMpActions.append(quickAction);

	m_actions.addSeparator(m.section);

	auto undoState = addGameAction(m.undoLabel, m.undo, m.loadSaveBackup, m.section, m.shortcut);
	m_nonMpActions.append(undoState);

	m_actions.addSeparator(m.section);

	for (int i = 1; i < 10; ++i) {
		auto slotAction = addGameAction(tr("State &%1").arg(i),  QString("%1.%2").arg(m.section).arg(i), [this, &m, i]() {
			(m_controller.get()->*(m.loadSaveSlot))(i);
		}, m.section, QKeySequence(m.slotShortcut.arg(i)));
		m_nonMpActions.append(slotAction);
	}
}

void WindowActions::setupEmuMenu() {
	ConfigController* config = m_window->config();

	m_actions.addMenu(tr("&Emulation"), "emu");
	addGameAction(tr("&Reset"), "reset", &CoreController::reset, "emu", QKeySequence("Ctrl+R"));
	addGameAction(tr("Sh&utdown"), "shutdown", &CoreController::stop, "emu");
	m_actions.addSeparator("emu");

	addGameAction(tr("Replace ROM..."), "replaceROM", m_window, &Window::replaceROM, "emu");
	addGameAction(tr("Yank game pak"), "yank", &CoreController::yankPak, "emu");
	m_actions.addSeparator("emu");

	auto pause = m_actions.addBooleanAction(tr("&Pause"), "pause", [this](bool paused) {
		if (m_controller) {
			m_controller->setPaused(paused);
		} else {
			m_window->m_pendingPause = paused;
		}
	}, "emu", QKeySequence("Ctrl+P"));
	connect(m_window, &Window::paused, pause.get(), &Action::setActive);

	addGameAction(tr("&Next frame"), "frameAdvance", &CoreController::frameAdvance, "emu", QKeySequence("Ctrl+N"));

	m_actions.addSeparator("emu");

	m_actions.addHeldAction(tr("Fast forward (held)"), "holdFastForward", [this](bool held) {
		if (m_controller) {
			m_controller->setFastForward(held);
		}
	}, "emu", QKeySequence(Qt::Key_Tab));

	addGameAction(tr("&Fast forward"), "fastForward", [this](bool value) {
		m_controller->forceFastForward(value);
	}, "emu", QKeySequence("Shift+Tab"));

	m_actions.addMenu(tr("Fast forward speed"), "fastForwardSpeed", "emu");
	ConfigOption* ffspeed = config->addOption("fastForwardRatio");
	ffspeed->connect([this](const QVariant&) {
		m_window->reloadConfig();
	}, this);
	ffspeed->addValue(tr("Unbounded"), -1.0f, &m_actions, "fastForwardSpeed");
	ffspeed->setValue(QVariant(-1.0f));
	m_actions.addSeparator("fastForwardSpeed");
	for (int i = 2; i < 11; ++i) {
		ffspeed->addValue(tr("%0x").arg(i), i, &m_actions, "fastForwardSpeed");
	}
	config->updateOption("fastForwardRatio");

	addGameAction(tr("Increase fast forward speed"), "fastForwardUp", [config] {
		float newRatio = config->getOption("fastForwardRatio", 1.0f).toFloat() + 1.0f;
		if (newRatio >= 3.0f) {
			config->setOption("fastForwardRatio", QVariant(newRatio));
		}
	}, "emu");

	addGameAction(tr("Decrease fast forward speed"), "fastForwardDown", [config] {
		float newRatio = config->getOption("fastForwardRatio").toFloat() - 1.0f;
		if (newRatio >= 2.0f) {
			config->setOption("fastForwardRatio", QVariant(newRatio));
		}
	}, "emu");

	auto rewindHeld = m_actions.addHeldAction(tr("Rewind (held)"), "holdRewind", [this](bool held) {
		// Prevent rewinding while the load/save state window is active
		if (held && stateWindow() != nullptr) {
			return;
		}

		if (m_controller) {
			m_controller->setRewinding(held);
		}
	}, "emu", QKeySequence("`"));
	m_nonMpActions.append(rewindHeld);

	auto rewind = addGameAction(tr("Re&wind"), "rewind", [this]() {
		m_controller->rewind();
	}, "emu", QKeySequence("~"));
	m_nonMpActions.append(rewind);

	auto frameRewind = addGameAction(tr("Step backwards"), "frameRewind", [this] () {
		m_controller->rewind(1);
	}, "emu", QKeySequence("Ctrl+B"));
	m_nonMpActions.append(frameRewind);

	m_actions.addSeparator("emu");

	InputController* input = m_window->inputController();
	m_actions.addMenu(tr("Solar sensor"), "solar", "emu");
	m_actions.addAction(tr("Increase solar level"), "increaseLuminanceLevel", input, &InputController::increaseLuminanceLevel, "solar");
	m_actions.addAction(tr("Decrease solar level"), "decreaseLuminanceLevel", input, &InputController::decreaseLuminanceLevel, "solar");
	m_actions.addAction(tr("Brightest solar level"), "maxLuminanceLevel", [input]() {
		input->setLuminanceLevel(10);
	}, "solar");
	m_actions.addAction(tr("Darkest solar level"), "minLuminanceLevel", [input]() {
		input->setLuminanceLevel(0);
	}, "solar");

	m_actions.addSeparator("solar");
	for (int i = 0; i <= 10; ++i) {
		m_actions.addAction(tr("Brightness %1").arg(QString::number(i)), QString("luminanceLevel.%1").arg(QString::number(i)), [input, i]() {
			input->setLuminanceLevel(i);
		}, "solar");
	}

#ifdef M_CORE_GB
	m_actions.addAction(tr("Load camera image..."), "loadCamImage", m_window, &Window::loadCamImage, "emu");

	auto gbPrint = addGameAction(tr("Game Boy Printer..."), "gbPrint", m_popups->printerView, "emu");
	m_platformActions.insert(mPLATFORM_GB, gbPrint);
#endif

#ifdef M_CORE_GBA
	auto bcGate = addGameAction(tr("BattleChip Gate..."), "bcGate", PopupManager<BattleChipView>().withController(m_controller), "emu");
	m_platformActions.insert(mPLATFORM_GBA, bcGate);
#endif
}

void WindowActions::setupAVMenu() {
	ConfigController* config = m_window->config();

	m_actions.addMenu(tr("Audio/&Video"), "av");
	m_actions.addMenu(tr("Frame size"), "frame", "av");
	for (int i = 1; i <= 8; ++i) {
		auto setSize = m_actions.addAction(tr("%1×").arg(QString::number(i)), QString("frame.%1x").arg(QString::number(i)), [this, i]() {
			m_window->setScaleMultiplier(i);
			m_frameSizes[i]->setActive(true);
		}, "frame");
		setSize->setExclusive(true);
		if (m_window->scaleMultiplier() == i) {
			setSize->setActive(true);
		}
		m_frameSizes[i] = setSize;
	}
	QKeySequence fullscreenKeys;
#ifdef Q_OS_WIN
	fullscreenKeys = QKeySequence("Alt+Return");
#else
	fullscreenKeys = QKeySequence("Ctrl+F");
#endif
	m_actions.addSeparator("frame");
	m_actions.addAction(tr("Toggle fullscreen"), "fullscreen", m_window, &Window::toggleFullScreen, "frame", fullscreenKeys);

	ConfigOption* lockFrameSize = config->addOption("lockFrameSize");
	lockFrameSize->addBoolean(tr("&Lock frame size"), &m_actions, "frame");
	lockFrameSize->connect([this](const QVariant& value) {
		if (display()) {
			if (value.toBool()) {
				display()->setMaximumSize(display()->size());
			} else {
				display()->setMaximumSize({});
			}
		}
	}, this);
	config->updateOption("lockFrameSize");

	ConfigOption* lockAspectRatio = config->addOption("lockAspectRatio");
	lockAspectRatio->addBoolean(tr("Lock aspect ratio"), &m_actions, "av");
	lockAspectRatio->connect([this](const QVariant& value) {
		if (display()) {
			display()->lockAspectRatio(value.toBool());
		}
		if (stateWindow()) {
			stateWindow()->setLockAspectRatio(value.toBool());
		}
	}, this);
	config->updateOption("lockAspectRatio");

	ConfigOption* lockIntegerScaling = config->addOption("lockIntegerScaling");
	lockIntegerScaling->addBoolean(tr("Force integer scaling"), &m_actions, "av");
	lockIntegerScaling->connect([this](const QVariant& value) {
		if (display()) {
			display()->lockIntegerScaling(value.toBool());
		}
		if (stateWindow()) {
			stateWindow()->setLockIntegerScaling(value.toBool());
		}
	}, this);
	config->updateOption("lockIntegerScaling");

	ConfigOption* interframeBlending = config->addOption("interframeBlending");
	interframeBlending->addBoolean(tr("Interframe blending"), &m_actions, "av");
	interframeBlending->connect([this](const QVariant& value) {
		if (display()) {
			display()->interframeBlending(value.toBool());
		}
	}, this);
	config->updateOption("interframeBlending");

	ConfigOption* resampleVideo = config->addOption("resampleVideo");
	resampleVideo->addBoolean(tr("Bilinear filtering"), &m_actions, "av");
	resampleVideo->connect([this](const QVariant& value) {
		if (display()) {
			display()->filter(value.toBool());
		}
	}, this);
	config->updateOption("resampleVideo");

	m_actions.addMenu(tr("Frame&skip"),"skip", "av");
	ConfigOption* skip = config->addOption("frameskip");
	skip->connect([this](const QVariant&) {
		m_window->reloadConfig();
	}, this);
	for (int i = 0; i <= 10; ++i) {
		skip->addValue(QString::number(i), i, &m_actions, "skip");
	}
	config->updateOption("frameskip");

	m_actions.addSeparator("av");

	ConfigOption* mute = config->addOption("mute");
	auto muteAction = mute->addBoolean(tr("Mute"), &m_actions, "av");
	muteAction->setActive(config->getOption("mute").toInt());
	mute->connect([this](const QVariant& value) {
		m_window->setFastForwardMute(static_cast<bool>(value.toInt()));
	}, this);

	m_actions.addMenu(tr("FPS target"),"target", "av");
	ConfigOption* fpsTargetOption = config->addOption("fpsTarget");
	QMap<double, std::shared_ptr<Action>> fpsTargets;
	for (int fps : {15, 30, 45, 60, 90, 120, 240}) {
		fpsTargets[fps] = fpsTargetOption->addValue(QString::number(fps), fps, &m_actions, "target");
	}
	m_actions.addSeparator("target");
	double nativeGB = double(GBA_ARM7TDMI_FREQUENCY) / double(VIDEO_TOTAL_LENGTH);
	fpsTargets[nativeGB] = fpsTargetOption->addValue(tr("Native (59.7275)"), nativeGB, &m_actions, "target");

	fpsTargetOption->connect([this, fpsTargets = std::move(fpsTargets)](const QVariant& value) {
		m_window->reloadConfig();
		for (auto iter = fpsTargets.begin(); iter != fpsTargets.end(); ++iter) {
			bool enableSignals = iter.value()->blockSignals(true);
			iter.value()->setActive(abs(iter.key() - value.toDouble()) < 0.001);
			iter.value()->blockSignals(enableSignals);
		}
	}, this);
	config->updateOption("fpsTarget");

	m_actions.addSeparator("av");

#ifdef USE_PNG
	addGameAction(tr("Take &screenshot"), "screenshot", [this]() {
		m_controller->screenshot();
	}, "av", tr("F12"));
#endif

#ifdef USE_FFMPEG
	addGameAction(tr("Record A/V..."), "recordOutput", m_popups->videoView, "av");
	addGameAction(tr("Record GIF/WebP/APNG..."), "recordGIF", m_popups->gifView, "av");
#endif

	m_actions.addSeparator("av");
	m_actions.addMenu(tr("Video layers"), "videoLayers", "av");
	m_actions.addMenu(tr("Audio channels"), "audioChannels", "av");

	addGameAction(tr("Adjust layer placement..."), "placementControl", PopupManager<PlacementControl>().withController(m_controller), "av");
}

void WindowActions::setupToolsMenu() {
	m_actions.addMenu(tr("&Tools"), "tools");
	m_actions.addAction(tr("View &logs..."), "viewLogs", m_popups->logView, "tools");
	m_actions.addAction(tr("Game &overrides..."), "overrideWindow", m_popups->overrideView, "tools");
	m_actions.addAction(tr("Game Pak sensors..."), "sensorWindow", m_popups->sensorView, "tools");

	addGameAction(tr("&Cheats..."), "cheatsWindow", m_popups->cheatsView, "tools");
#ifdef ENABLE_SCRIPTING
	m_actions.addAction(tr("Scripting..."), "scripting", m_window, &Window::scriptingOpen, "tools");
#endif

	m_actions.addAction(tr("Create forwarder..."), "createForwarder", PopupManager<ForwarderView>(), "tools");

	m_actions.addSeparator("tools");
	m_actions.addAction(tr("Settings..."), "settings", m_window, &Window::openSettingsWindow, "tools")->setRole(Action::Role::SETTINGS);
	m_actions.addAction(tr("Make portable"), "makePortable", m_window, &Window::tryMakePortable, "tools");

	m_actions.addSeparator("tools");
#ifdef ENABLE_DEBUGGERS
	m_actions.addAction(tr("Open debugger console..."), "debuggerWindow", m_window, &Window::consoleOpen, "tools");
#ifdef ENABLE_GDB_STUB
	auto gdbWindow = addGameAction(tr("Start &GDB server..."), "gdbWindow", m_window, &Window::gdbOpen, "tools");
	m_platformActions.insert(mPLATFORM_GBA, gdbWindow);
#endif
#endif
#if defined(ENABLE_DEBUGGERS) || defined(ENABLE_SCRIPTING)
	m_actions.addSeparator("tools");
#endif

	m_actions.addMenu(tr("Game state views"), "stateViews", "tools");
	addGameAction(tr("View &palette..."), "paletteWindow", PopupManager<PaletteView>().withController(m_controller), "stateViews");
	addGameAction(tr("View &sprites..."), "spriteWindow", PopupManager<ObjView>().withController(m_controller), "stateViews");
	addGameAction(tr("View &tiles..."), "tileWindow", PopupManager<TileView>().withController(m_controller), "stateViews");
	addGameAction(tr("View &map..."), "mapWindow", PopupManager<MapView>().withController(m_controller), "stateViews");
	addGameAction(tr("&Frame inspector..."), "frameWindow", m_popups->frameView, "stateViews");
	addGameAction(tr("View memory..."), "memoryView", PopupManager<MemoryView>().withController(m_controller), "stateViews");
	addGameAction(tr("Search memory..."), "memorySearch", PopupManager<MemorySearch>().withController(m_controller), "stateViews");
	addGameAction(tr("View &I/O registers..."), "ioViewer", PopupManager<IOViewer>().withController(m_controller), "stateViews");

#ifdef ENABLE_DEBUGGERS
	addGameAction(tr("Log memory &accesses..."), "memoryAccessView", [this]() {
		std::weak_ptr<MemoryAccessLogController> controller = m_controller->memoryAccessLogController();
		MemoryAccessLogView* view = new MemoryAccessLogView(controller);
		connect(m_controller.get(), &CoreController::stopping, view, &QWidget::close);
		m_window->openView(view);
	}, "tools");
#endif

#if defined(USE_FFMPEG) && defined(M_CORE_GBA)
	m_actions.addSeparator("tools");
	m_actions.addAction(tr("Convert e-Reader card image to raw..."), "parseCard", m_window, &Window::parseCard, "tools");
#endif

	m_actions.addSeparator("tools");
	addGameAction(tr("Record debug video log..."), "recordVL", m_window, &Window::startVideoLog, "tools");
	addGameAction(tr("Stop debug video log"), "stopVL", [this]() {
		m_controller->endVideoLog();
	}, "tools");
}

void WindowActions::setupHiddenActions() {
	m_actions.addHiddenAction(tr("Exit fullscreen"), "exitFullScreen", m_window, &Window::exitFullScreen, "frame", QKeySequence("Esc"));

	m_actions.addHeldAction(tr("GameShark Button (held)"), "holdGSButton", [this](bool held) {
		if (m_controller) {
			mCheatPressButton(m_controller->cheatDevice(), held);
		}
	}, "tools");

	m_actions.addHiddenMenu(tr("Autofire"), "autofire");
	m_actions.addHeldAction(tr("Autofire A"), "autofireA", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_A, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire B"), "autofireB", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_B, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire L"), "autofireL", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_L, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire R"), "autofireR", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_R, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire Start"), "autofireStart", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_START, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire Select"), "autofireSelect", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_SELECT, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire Up"), "autofireUp", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_UP, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire Right"), "autofireRight", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_RIGHT, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire Down"), "autofireDown", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_DOWN, held);
		}
	}, "autofire");
	m_actions.addHeldAction(tr("Autofire Left"), "autofireLeft", [this](bool held) {
		if (m_controller) {
			m_controller->setAutofire(GBA_KEY_LEFT, held);
		}
	}, "autofire");
}

void WindowActions::updateMRU(const QStringList& mru) {
	m_actions.clearMenu("mru");

	int i = 0;
	for (const QString& file : mru) {
		QString displayName(QDir::toNativeSeparators(file).replace("&", "&&"));
		m_actions.addAction(displayName, QString("mru.%1").arg(QString::number(i)), [this, file]() {
			m_window->loadROM(file);
		}, "mru", QString("Ctrl+%1").arg(i));
		++i;
	}
	m_actions.addSeparator("mru");
	m_actions.addAction(tr("Clear"), "resetMru", m_window, &Window::clearMRU, "mru");

	m_actions.rebuildMenu(m_menuBar, m_window, *m_shortcutController);
}

void WindowActions::updateMultiplayerStatus(bool canOpenAnother) {
	m_multiWindow->setEnabled(canOpenAnother);
}

void WindowActions::setActivePlatform(mPlatform platform) {
	bool stopping = platform == mPLATFORM_NONE;

	for (auto& action : m_gameActions) {
		action->setEnabled(!stopping);
	}
	for (auto action = m_platformActions.begin(); action != m_platformActions.end(); ++action) {
		action.value()->setEnabled(m_controller->platform() == action.key());
	}

	if (stopping) {
		m_actions.clearMenu("videoLayers");
		m_actions.clearMenu("audioChannels");
	}
}

void WindowActions::setNonMultiplayerActionsEnabled(bool on) {
	for (auto& action : m_nonMpActions) {
		action->setEnabled(on);
	}
}

void WindowActions::setScaleFactor(int factor) {
	for (QMap<int, std::shared_ptr<Action>>::iterator iter = m_frameSizes.begin(); iter != m_frameSizes.end(); ++iter) {
		iter.value()->setActive(iter.key() == factor);
	}
}

void WindowActions::updateLayers() {
	CoreController::Interrupter interrupter(m_controller);
	mCore* core = m_controller->thread()->core;
	m_actions.clearMenu("videoLayers");
	m_actions.clearMenu("audioChannels");
	const mCoreChannelInfo* videoLayers;
	const mCoreChannelInfo* audioChannels;
	size_t nVideo = core->listVideoLayers(core, &videoLayers);
	size_t nAudio = core->listAudioChannels(core, &audioChannels);

	if (nVideo) {
		for (size_t i = 0; i < nVideo; ++i) {
			auto action = m_actions.addBooleanAction(videoLayers[i].visibleName, QString("videoLayer.%1").arg(videoLayers[i].internalName), [this, videoLayers, i](bool enable) {
				m_controller->thread()->core->enableVideoLayer(m_controller->thread()->core, videoLayers[i].id, enable);
			}, "videoLayers");
			action->setActive(true);
		}
	}
	if (nAudio) {
		for (size_t i = 0; i < nAudio; ++i) {
			auto action = m_actions.addBooleanAction(audioChannels[i].visibleName, QString("audioChannel.%1").arg(audioChannels[i].internalName), [this, audioChannels, i](bool enable) {
				m_controller->thread()->core->enableAudioChannel(m_controller->thread()->core, audioChannels[i].id, enable);
			}, "audioChannels");
			action->setActive(true);
		}
	}
	interrupter.resume();

	m_actions.rebuildMenu(m_menuBar, m_window, *m_shortcutController);
}
