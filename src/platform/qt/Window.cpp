/* Copyright (c) 2013-2017 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#include "Window.h"
#include "Window_p.h"

#include <QKeyEvent>
#include <QKeySequence>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QScreen>
#include <QWindow>

#ifdef Q_OS_WIN
#include <dwmapi.h>
#endif

#ifdef USE_SQLITE3
#include "ArchiveInspector.h"
#include "library/LibraryController.h"
#endif

#include "AudioProcessor.h"
#include "ConfigController.h"
#include "CoreController.h"
#include "DebuggerConsoleController.h"
#include "Display.h"
#include "CoreController.h"
#include "GBAApp.h"
#include "GDBController.h"
#ifdef BUILD_SDL
#include "input/SDLInputDriver.h"
#endif
#include "LoadSaveState.h"
#include "MultiplayerController.h"
#ifdef ENABLE_SCRIPTING
#include "scripting/ScriptingView.h"
#endif
#include "ShaderSelector.h"
#include "ShortcutController.h"
#include "utils.h"
#include "VideoProxy.h"
#include "WindowActions.h"

#ifdef USE_DISCORD_RPC
#include "DiscordCoordinator.h"
#endif

#include <mgba/core/version.h>
#ifdef M_CORE_GB
#include <mgba/internal/gb/video.h>
#endif
#include <mgba/feature/commandline.h>
#include <mgba-util/vfs.h>

#include <mgba-util/convolve.h>

using namespace QGBA;

Window::Window(CoreManager* manager, ConfigController* config, int playerId, QWidget* parent)
	: QMainWindow(parent)
	, m_manager(manager)
	, m_actions(new WindowActions(this))
	, m_screenWidget(new WindowBackground())
	, m_config(config)
	, m_inputController(this)
	, m_playerId(playerId)
	, m_popups(new WindowPopups)
{
	setFocusPolicy(Qt::StrongFocus);
	setAcceptDrops(true);
	setAttribute(Qt::WA_DeleteOnClose);
	updateTitle();

	m_logo.setDevicePixelRatio(m_screenWidget->devicePixelRatio());
	m_logo = m_logo; // Free memory left over in old pixmap

#if defined(M_CORE_GBA)
	float i = 2;
#elif defined(M_CORE_GB)
	float i = 3;
#endif
	QVariant multiplier = m_config->getOption("scaleMultiplier");
	if (!multiplier.isNull()) {
		m_savedScale = multiplier.toInt();
		i = m_savedScale;
	}
#ifdef USE_SQLITE3
	m_libraryView = new LibraryController(nullptr, ConfigController::configDir() + "/library.sqlite3", m_config);
	ConfigOption* showLibrary = m_config->addOption("showLibrary");
	showLibrary->connect([this](const QVariant& value) {
		if (!m_controller) {
			if (value.toBool()) {
				attachWidget(m_libraryView);
			} else {
				attachWidget(m_screenWidget);
			}
		}
	}, this);
	m_config->updateOption("showLibrary");

	ConfigOption* showFilenameInLibrary = m_config->addOption("showFilenameInLibrary");
	showFilenameInLibrary->connect([this](const QVariant& value) {
			m_libraryView->setShowFilename(value.toBool());
	}, this);
	m_config->updateOption("showFilenameInLibrary");
	ConfigOption* libraryStyle = m_config->addOption("libraryStyle");
	libraryStyle->connect([this](const QVariant& value) {
		m_libraryView->setViewStyle(static_cast<LibraryStyle>(value.toInt()));
	}, this);
	m_config->updateOption("libraryStyle");

	connect(m_libraryView, &LibraryController::startGame, [this]() {
		VFile* output = m_libraryView->selectedVFile();
		if (output) {
			QPair<QString, QString> path = m_libraryView->selectedPath();
			setController(m_manager->loadGame(output, path.second, path.first), path.first + "/" + path.second);
		}
	});
#endif
#if defined(M_CORE_GBA)
	QSize minimumSize = QSize(GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);
#elif defined(M_CORE_GB)
	QSize minimumSize = QSize(GB_VIDEO_HORIZONTAL_PIXELS, GB_VIDEO_VERTICAL_PIXELS);
#endif
	setMinimumSize(minimumSize);
	if (i > 0) {
		m_initialSize = minimumSize * i;
	} else {
		m_initialSize = minimumSize * 2;
	}
	setLogo();

	connect(&m_fpsTimer, &QTimer::timeout, this, &Window::showFPS);
	connect(&m_focusCheck, &QTimer::timeout, this, &Window::focusCheck);
	connect(&m_inputController, &InputController::profileLoaded, m_actions->shortcutController(), &ShortcutController::loadProfile);

	m_log.setLevels(mLOG_WARN | mLOG_ERROR | mLOG_FATAL);
	m_log.load(m_config);
	m_fpsTimer.setInterval(FPS_TIMER_INTERVAL);
	m_focusCheck.setInterval(200);
	m_mustRestart.setInterval(MUST_RESTART_TIMEOUT);
	m_mustRestart.setSingleShot(true);
	m_mustReset.setInterval(MUST_RESTART_TIMEOUT);
	m_mustReset.setSingleShot(true);

#ifdef BUILD_SDL
	m_inputController.addInputDriver(std::make_shared<SDLInputDriver>(&m_inputController));
#if SDL_VERSION_ATLEAST(2, 0, 0)
	m_inputController.setGamepadDriver(SDL_BINDING_CONTROLLER);
	m_inputController.setSensorDriver(SDL_BINDING_CONTROLLER);
#else
	m_inputController.setGamepadDriver(SDL_BINDING_BUTTON);
	m_inputController.setSensorDriver(SDL_BINDING_BUTTON);
#endif
#endif

	m_actions->setCoreController(&m_controller);
	m_actions->setConfigController(m_config);
	m_actions->setWindow(this);

	setupPopups();
	setupOptions();
}

Window::~Window() {
#ifdef USE_SQLITE3
	delete m_libraryView;
#endif
}

void Window::argumentsPassed() {
	const mArguments* args = m_config->args();

	if (args->patch) {
		m_pendingPatch = args->patch;
	}

	if (args->savestate) {
		m_pendingState = args->savestate;
	}

#ifdef ENABLE_GDB_STUB
	if (args->debugGdb) {
		if (!m_gdbController) {
			m_gdbController = new GDBController(&m_controller, this);
		}
		m_gdbController->attach();
		m_gdbController->listen();
	}
#endif

#ifdef ENABLE_DEBUGGERS
	if (args->debugCli) {
		consoleOpen();
	}
#endif

	if (m_config->graphicsOpts()->multiplier > 0) {
		m_savedScale = m_config->graphicsOpts()->multiplier;

#if defined(M_CORE_GBA)
		QSize size(GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);
#elif defined(M_CORE_GB)
		QSize size(GB_VIDEO_HORIZONTAL_PIXELS, GB_VIDEO_VERTICAL_PIXELS);
#endif
		m_initialSize = size * m_savedScale;
	}

	if (args->fname) {
		setController(m_manager->loadGame(args->fname), args->fname);
	}

	if (m_config->graphicsOpts()->fullscreen) {
		enterFullScreen();
	}
}

int Window::scaleMultiplier() const {
	return m_savedScale;
}

void Window::setScaleMultiplier(int factor) {
	bool lockFrameSize = m_config->getOption("lockFrameSize").toInt();
	if (!lockFrameSize) {
		showNormal();
	}
#if defined(M_CORE_GBA)
	QSize minimumSize = QSize(GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);
#elif defined(M_CORE_GB)
	QSize minimumSize = QSize(GB_VIDEO_HORIZONTAL_PIXELS, GB_VIDEO_VERTICAL_PIXELS);
#endif
	QSize size;
	if (m_display) {
		size = m_display->contentSize();
	}
	if (size.isNull()) {
		size = minimumSize;
	}
	size *= factor;
	m_savedScale = factor;
	m_config->setOption("scaleMultiplier", factor); // TODO: Port to other
	m_savedSize = size;
	resizeFrame(size);
	if (lockFrameSize) {
		m_display->setMaximumSize(size);
	}
}

void Window::resizeFrame(const QSize& size) {
	QSize newSize(size);
	if (!m_config->getOption("lockFrameSize").toInt()) {
		m_savedSize = size;
	}
	if (windowHandle()) {
		QRect geom = windowHandle()->screen()->availableGeometry();
		if (newSize.width() > geom.width()) {
			newSize.setWidth(geom.width());
		}
		if (newSize.height() > geom.height()) {
			newSize.setHeight(geom.height());
		}
	}
	newSize += this->size();
	newSize -= centralWidget()->size();
	if (!isFullScreen()) {
		resize(newSize);
	}
}

void Window::updateMultiplayerStatus(bool canOpenAnother) {
	m_actions->updateMultiplayerStatus(canOpenAnother);
	multiplayerChanged();
}

void Window::updateMultiplayerActive(bool active) {
	m_multiActive = active;
	updateMute();
}

void Window::setConfig(ConfigController* config) {
	m_config = config;
}

void Window::loadConfig() {
	const mCoreOptions* opts = m_config->options();
	reloadConfig();

	if (opts->width && opts->height) {
		m_initialSize = QSize(opts->width, opts->height);
	}

	if (opts->fullscreen) {
		enterFullScreen();
	}

	m_mruFiles = m_config->getMRU();
	updateMRU();

	m_inputController.setConfiguration(m_config);

	if (!m_config->getList("autorunSettings").isEmpty()) {
		ensureScripting();
	}
}

void Window::reloadConfig() {
	const mCoreOptions* opts = m_config->options();

	m_log.setLevels(opts->logLevel);

	if (m_controller) {
		m_controller->loadConfig(m_config);
		if (m_audioProcessor) {
			m_audioProcessor->configure(m_config);
		}
		updateMute();
		m_display->resizeContext();
	}

	GBAApp::app()->setScreensaverSuspendable(opts->suspendScreensaver);
}

void Window::saveConfig() {
	m_inputController.saveConfiguration();
	m_config->write();
}

QString Window::getFiltersArchive() const {
	QStringList filters;

	QStringList formats{
#if defined(USE_LIBZIP) || defined(USE_MINIZIP)
		"*.zip",
#endif
#ifdef USE_LZMA
		"*.7z",
#endif
	};
	filters.append(tr("Archives (%1)").arg(formats.join(QChar(' '))));
	return filters.join(";;");
}

void Window::loadROM(const QString& filename) {
	if (!filename.isEmpty()) {
		setController(m_manager->loadGame(filename), filename);
	}
}

void Window::selectROM() {
	QString filename = GBAApp::app()->getOpenFileName(this, tr("Select ROM"), romFilters(true));
	loadROM(filename);
}

void Window::bootBIOS() {
	QString bios(m_config->getOption("gba.bios"));
	if (bios.isEmpty()) {
		bios = m_config->getOption("bios");
	}
	setController(m_manager->loadBIOS(mPLATFORM_GBA, bios), QString());
}

#ifdef USE_SQLITE3
void Window::selectROMInArchive() {
	QString filename = GBAApp::app()->getOpenFileName(this, tr("Select ROM"), getFiltersArchive());
	if (filename.isEmpty()) {
		return;
	}
	ArchiveInspector* archiveInspector = new ArchiveInspector(filename);
	connect(archiveInspector, &QDialog::accepted, [this,  archiveInspector]() {
		VFile* output = archiveInspector->selectedVFile();
		QPair<QString, QString> path = archiveInspector->selectedPath();
		if (output) {
			setController(m_manager->loadGame(output, path.second, path.first), path.first + "/" + path.second);
		}
		archiveInspector->close();
	});
	archiveInspector->setAttribute(Qt::WA_DeleteOnClose);
	archiveInspector->show();
}

void Window::addDirToLibrary() {
	QString filename = GBAApp::app()->getOpenDirectoryName(this, tr("Select folder"));
	if (filename.isEmpty()) {
		return;
	}
	m_libraryView->addDirectory(filename);
}
#endif

void Window::replaceROM() {
	QString filename = GBAApp::app()->getOpenFileName(this, tr("Select ROM"), romFilters());
	if (!filename.isEmpty()) {
		m_controller->replaceGame(filename);
	}
}

void Window::selectSave(bool temporary) {
	QStringList formats{"*.sav"};
	QString filter = tr("Save games (%1)").arg(formats.join(QChar(' ')));
	QString filename = GBAApp::app()->getOpenFileName(this, tr("Select save game"), filter);
	if (!filename.isEmpty()) {
		m_controller->loadSave(filename, temporary);
	}
}

void Window::selectState(bool load) {
	QStringList formats{"*.ss0", "*.ss1", "*.ss2", "*.ss3", "*.ss4", "*.ss5", "*.ss6", "*.ss7", "*.ss8", "*.ss9"};
	QString filter = tr("mGBA save state files (%1)").arg(formats.join(QChar(' ')));
	if (load) {
		QString filename = GBAApp::app()->getOpenFileName(this, tr("Select save state"), filter);
		if (!filename.isEmpty()) {
			m_controller->loadState(filename);
		}
	} else {
		QString filename = GBAApp::app()->getSaveFileName(this, tr("Select save state"), filter);
		if (!filename.isEmpty()) {
			m_controller->saveState(filename);
		}
	}
}

void Window::multiplayerChanged() {
	if (!m_controller) {
		return;
	}
	int attached = 1;
	MultiplayerController* multiplayer = m_controller->multiplayerController();
	if (multiplayer) {
		attached = multiplayer->attached();
		m_playerId = multiplayer->playerId(m_controller.get());
	}
	m_actions->setNonMultiplayerActionsEnabled(attached < 2);
}

void Window::selectPatch() {
	QString filename = GBAApp::app()->getOpenFileName(this, tr("Select patch"), tr("Patches (*.ips *.ups *.bps)"));
	if (!filename.isEmpty()) {
		if (m_controller) {
			m_controller->loadPatch(filename);
		} else {
			m_pendingPatch = filename;
		}
	}
}

void Window::scanCard() {
	QStringList filenames = GBAApp::app()->getOpenFileNames(this, tr("Select e-Reader dotcode"), tr("e-Reader card (*.raw *.bin *.bmp)"));
	for (QString& filename : filenames) {
		m_controller->scanCard(filename);
	}
}

void Window::parseCard() {
#ifdef USE_FFMPEG
	QStringList filenames = GBAApp::app()->getOpenFileNames(this, tr("Select e-Reader card images"), tr("Image file (*.png *.jpg *.jpeg)"));
	QMessageBox* dialog = new QMessageBox(QMessageBox::Information, tr("Conversion finished"),
	                                      QString("oh"), QMessageBox::Ok);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	auto status = std::make_shared<QPair<int, int>>(0, filenames.size());
	GBAApp::app()->submitWorkerJob([filenames, status]() {
		int success = 0;
		for (QString filename : filenames) {
			if (filename.isEmpty()) {
				continue;
			}
			QImage image(filename);
			if (image.isNull()) {
				continue;
			}
			EReaderScan* scan;
			switch (image.depth()) {
			case 8:
				scan = EReaderScanLoadImage8(image.constBits(), image.width(), image.height(), image.bytesPerLine());
				break;
			case 24:
				scan = EReaderScanLoadImage(image.constBits(), image.width(), image.height(), image.bytesPerLine());
				break;
			case 32:
				scan = EReaderScanLoadImageA(image.constBits(), image.width(), image.height(), image.bytesPerLine());
				break;
			default:
				continue;
			}
			QFileInfo ofile(filename);
			if (EReaderScanCard(scan)) {
				QString ofilename = ofile.path() + QDir::separator() + ofile.baseName() + ".raw";
				EReaderScanSaveRaw(scan, ofilename.toUtf8().constData(), false);
				++success;
			}
			EReaderScanDestroy(scan);
		}
		status->first = success;
	}, [dialog, status]() {
		if (status->second == 0) {
			return;
		}
		dialog->setText(tr("%1 of %2 e-Reader cards converted successfully.").arg(status->first).arg(status->second));
		dialog->show();
	});
#endif
}

void Window::openView(QWidget* widget) {
	// Attempt to disconnect first to prevent multiple connections
	disconnect(this, &Window::shutdown, widget, &QWidget::close);
	connect(this, &Window::shutdown, widget, &QWidget::close);
	widget->setAttribute(Qt::WA_DeleteOnClose);
	widget->show();
	widget->activateWindow();
	widget->raise();
}

void Window::loadCamImage() {
	QString filename = GBAApp::app()->getOpenFileName(this, tr("Select image"), tr("Image file (*.png *.gif *.jpg *.jpeg);;All files (*)"));
	if (!filename.isEmpty()) {
		m_inputController.loadCamImage(filename);
	}
}

void Window::importSharkport() {
	QString filename = GBAApp::app()->getOpenFileName(this, tr("Select save"), tr("GameShark saves (*.gsv *.sps *.xps)"));
	if (!filename.isEmpty()) {
		m_controller->importSharkport(filename);
	}
}

void Window::exportSharkport() {
	QString filename = GBAApp::app()->getSaveFileName(this, tr("Select save"), tr("GameShark saves (*.sps *.xps)"));
	if (!filename.isEmpty()) {
		m_controller->exportSharkport(filename);
	}
}

void Window::openSettingsWindow() {
	openSettingsWindow(SettingsView::Page::AV);
}

void Window::openSettingsWindow(SettingsView::Page page) {
	if (!m_popups->settingsView) {
		SettingsView* settingsWindow = new SettingsView(m_config, &m_inputController, m_actions->shortcutController(), &m_log);
#if defined(BUILD_GL) || defined(BUILD_GLES2)
		if (m_display->supportsShaders()) {
			settingsWindow->setShaderSelector(m_shaderView.get());
		}
#endif
		connect(settingsWindow, &SettingsView::displayDriverChanged, this, &Window::reloadDisplayDriver);
		connect(settingsWindow, &SettingsView::audioDriverChanged, this, &Window::reloadAudioDriver);
		connect(settingsWindow, &SettingsView::cameraDriverChanged, this, &Window::mustReset);
		connect(settingsWindow, &SettingsView::cameraChanged, &m_inputController, &InputController::setCamera);
		connect(settingsWindow, &SettingsView::videoRendererChanged, this, &Window::changeRenderer);
		connect(settingsWindow, &SettingsView::languageChanged, this, &Window::mustRestart);
		connect(settingsWindow, &SettingsView::pathsChanged, this, &Window::reloadConfig);
#ifdef USE_SQLITE3
		connect(settingsWindow, &SettingsView::libraryCleared, m_libraryView, &LibraryController::clear);
#endif
#ifdef ENABLE_SCRIPTING
		connect(settingsWindow, &SettingsView::openAutorunScripts, this, [this]() {
			ensureScripting();
			m_scripting->openAutorunEdit();
		});
#endif
		connect(this, &Window::shaderSelectorAdded, settingsWindow, &SettingsView::setShaderSelector);
		m_popups->settingsView = settingsWindow;
	}
	openView(m_popups->settingsView);
	m_popups->settingsView->selectPage(page);
}

void Window::startVideoLog() {
	QString filename = GBAApp::app()->getSaveFileName(this, tr("Select video log"), tr("Video logs (*.mvl)"));
	if (!filename.isEmpty()) {
		m_controller->startVideoLog(filename);
	}
}

#ifdef ENABLE_GDB_STUB
void Window::gdbOpen() {
	if (!m_gdbController) {
		m_gdbController = new GDBController(&m_controller, this);
		m_popups->gdbWindow.withController(m_controller).constructWith(m_gdbController);
	}
	m_popups->gdbWindow();
}
#endif

#ifdef ENABLE_DEBUGGERS
void Window::consoleOpen() {
	if (!m_console) {
		m_console = new DebuggerConsoleController(&m_controller, this);
		m_popups->console.withController(m_controller).constructWith(m_console);
	}
	m_popups->console();
}
#endif

#ifdef ENABLE_SCRIPTING
void Window::scriptingOpen() {
	ensureScripting();
	ScriptingView* view = new ScriptingView(m_scripting.get(), m_config);
	openView(view);
}
#endif

void Window::setFastForwardMute(bool mute) {
	m_config->setOption("fastForwardMute", mute);
	reloadConfig();
}

void Window::keyPressEvent(QKeyEvent* event) {
	if (event->isAutoRepeat()) {
		QWidget::keyPressEvent(event);
		return;
	}
	int key = m_inputController.mapKeyboard(event->key());
	if (key == -1) {
		QWidget::keyPressEvent(event);
		return;
	}
	if (m_controller) {
		m_controller->addKey(key);
	}
	event->accept();
}

void Window::keyReleaseEvent(QKeyEvent* event) {
	if (event->isAutoRepeat()) {
		QWidget::keyReleaseEvent(event);
		return;
	}
	int key = m_inputController.mapKeyboard(event->key());
	if (key == -1) {
		QWidget::keyPressEvent(event);
		return;
	}
	if (m_controller) {
		m_controller->clearKey(key);
	}
	event->accept();
}

void Window::resizeEvent(QResizeEvent*) {
	QSize newSize = centralWidget()->size();
	if (!isFullScreen()) {
		m_config->setOption("height", newSize.height());
		m_config->setOption("width", newSize.width());
	}

	int factor = 0;
	QSize size(GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);
	if (m_controller) {
		size = m_controller->screenDimensions();
	}
	if (newSize.width() % size.width() == 0 && newSize.height() % size.height() == 0 &&
	    newSize.width() / size.width() == newSize.height() / size.height()) {
		factor = newSize.width() / size.width();
	}
	m_savedScale = factor;
	m_actions->setScaleFactor(factor);

	m_config->setOption("fullscreen", isFullScreen());
}

void Window::showEvent(QShowEvent* event) {
	if (m_wasOpened) {
		if (event->spontaneous() && m_controller) {
			focusCheck();
			if (m_config->getOption("pauseOnMinimize").toInt() && m_autoresume) {
				m_controller->setPaused(false);
				m_autoresume = false;
			}

			if (m_config->getOption("muteOnMinimize").toInt()) {
				m_inactiveMute = false;
				updateMute();
			}
		}
		return;
	}
	m_wasOpened = true;
#ifdef Q_OS_WIN
	HWND hwnd = reinterpret_cast<HWND>(winId());
	DWM_WINDOW_CORNER_PREFERENCE cornerPref = DWMWCP_DONOTROUND;
	DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref, sizeof(cornerPref));
#endif
	if (m_initialSize.isValid()) {
		resizeFrame(m_initialSize);
	}
	QVariant windowPos = m_config->getQtOption("windowPos", m_playerId > 0 ? QString("player%0").arg(m_playerId) : QString());
	bool maximized = m_config->getQtOption("maximized").toBool();
	QRect geom = windowHandle()->screen()->availableGeometry();
	if (!windowPos.isNull() && geom.contains(windowPos.toPoint())) {
		move(windowPos.toPoint());
	} else {
		QRect rect = frameGeometry();
		rect.moveCenter(geom.center());
		move(rect.topLeft());
	}
	if (maximized) {
		showMaximized();
	}
	if (m_fullscreenOnStart) {
		enterFullScreen();
		m_fullscreenOnStart = false;
	}
	reloadDisplayDriver();
	setFocus();
}

void Window::hideEvent(QHideEvent* event) {
	if (!event->spontaneous()) {
		return;
	}
	if (!m_controller) {
		return;
	}

	if (m_config->getOption("pauseOnMinimize").toInt() && !m_controller->isPaused()) {
		m_autoresume = true;
		m_controller->setPaused(true);
	}
	if (m_config->getOption("muteOnMinimize").toInt()) {
		m_inactiveMute = true;
		updateMute();
	}
}

void Window::closeEvent(QCloseEvent* event) {
	emit shutdown();
	m_config->setQtOption("windowPos", pos(), m_playerId > 0 ? QString("player%0").arg(m_playerId) : QString());
	m_config->setQtOption("maximized", isMaximized());

	if (m_savedScale > 0) {
		m_config->setOption("height", GBA_VIDEO_VERTICAL_PIXELS * m_savedScale);
		m_config->setOption("width", GBA_VIDEO_HORIZONTAL_PIXELS * m_savedScale);
	}
	saveConfig();
	if (m_controller) {
		event->ignore();
		m_pendingClose = true;
	} else {
		m_display.reset();
	}
}

void Window::focusInEvent(QFocusEvent*) {
	for (Window* window : GBAApp::app()->windows()) {
		if (window != this) {
			window->updateMultiplayerActive(false);
		} else {
			updateMultiplayerActive(true);
		}
	}
	if (m_display) {
		m_display->forceDraw();
	}
}

void Window::focusOutEvent(QFocusEvent*) {
}

void Window::dragEnterEvent(QDragEnterEvent* event) {
	if (event->mimeData()->hasFormat("text/uri-list")) {
		event->acceptProposedAction();
	}
}

void Window::dropEvent(QDropEvent* event) {
	QString uris = event->mimeData()->data("text/uri-list");
	uris = uris.trimmed();
	if (uris.contains("\n")) {
		// Only one file please
		return;
	}
	QUrl url(uris);
	if (!url.isLocalFile()) {
		// No remote loading
		return;
	}
	event->accept();
	loadROM(url.toLocalFile());
}

void Window::enterFullScreen() {
	if (!isVisible()) {
		m_fullscreenOnStart = true;
		return;
	}
	if (isFullScreen()) {
		return;
	}
	showFullScreen();
#ifndef Q_OS_MAC
	if (m_controller && !m_controller->isPaused()) {
		menuBar()->hide();
	}
#endif
}

void Window::exitFullScreen() {
	if (!isFullScreen()) {
		return;
	}
	centralWidget()->unsetCursor();
	menuBar()->show();
	showNormal();
}

void Window::toggleFullScreen() {
	if (isFullScreen()) {
		exitFullScreen();
	} else {
		enterFullScreen();
	}
}

void Window::gameStarted() {
	m_actions->setActivePlatform(m_controller->platform());
	QSize size = m_controller->screenDimensions();
	m_config->updateOption("lockIntegerScaling");
	m_config->updateOption("lockAspectRatio");
	m_config->updateOption("interframeBlending");
	m_config->updateOption("resampleVideo");
	if (m_savedScale > 0) {
		resizeFrame(size * m_savedScale);
	}
	attachWidget(m_display.get());
	setFocus();

#ifndef Q_OS_MAC
	if (isFullScreen()) {
		menuBar()->hide();
	}
#endif

	reloadAudioDriver();
	multiplayerChanged();
	updateTitle();

	m_hitUnimplementedBiosCall = false;
	if (m_config->getOption("showFps", "1").toInt()) {
		m_fpsTimer.start();
		m_frameTimer.start();
	}
	m_focusCheck.start();
	if (m_display->underMouse()) {
		centralWidget()->setCursor(Qt::BlankCursor);
	}

	m_actions->updateLayers();

#ifdef M_CORE_GBA
	if (m_controller->platform() == mPLATFORM_GBA) {
		QVariant eCardList = m_config->takeArgvOption(QString("ecard"));
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
		if (eCardList.canConvert(QMetaType::QStringList)) {
#else
		if (QMetaType::canConvert(eCardList.metaType(), QMetaType(QMetaType::QStringList))) {
#endif
			m_controller->scanCards(eCardList.toStringList());
		}
	}
#endif

#ifdef USE_DISCORD_RPC
	DiscordCoordinator::gameStarted(m_controller);
#endif
}

void Window::gameStopped() {
	m_actions->setActivePlatform(mPLATFORM_NONE);
	setWindowFilePath(QString());

	m_fpsTimer.stop();
	m_focusCheck.stop();

	if (m_audioProcessor) {
		m_audioProcessor->stop();
		m_audioProcessor.reset();
	}
	m_display->stopDrawing();
	setLogo();
	if (m_display) {
#ifdef M_CORE_GB
		m_display->setMinimumSize(GB_VIDEO_HORIZONTAL_PIXELS, GB_VIDEO_VERTICAL_PIXELS);
#elif defined(M_CORE_GBA)
		m_display->setMinimumSize(GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);
#endif
	}

	std::shared_ptr<CoreController> controller;
	m_controller.swap(controller);
	QTimer::singleShot(0, this, [controller]() {
		// Destroy the controller after everything else has cleaned up
		Q_UNUSED(controller);
	});
	detachWidget();
	updateTitle();

	if (m_pendingClose) {
#ifdef ENABLE_SCRIPTING
		std::shared_ptr<VideoProxy> proxy = m_display->videoProxy();
		if (m_scripting && proxy) {
			m_scripting->setVideoBackend(nullptr);
		}
#endif
		m_display.reset();
		close();
	}
#ifndef Q_OS_MAC
	menuBar()->show();
#endif

#ifdef USE_DISCORD_RPC
	DiscordCoordinator::gameStopped();
#endif

	emit paused(false);
}

void Window::gameCrashed(const QString& errorMessage) {
	QMessageBox* crash = new QMessageBox(QMessageBox::Critical, tr("Crash"),
	                                     tr("The game has crashed with the following error:\n\n%1").arg(errorMessage),
	                                     QMessageBox::Ok, this, Qt::Sheet);
	crash->setAttribute(Qt::WA_DeleteOnClose);
	crash->show();
}

void Window::gameFailed() {
	QMessageBox* fail = new QMessageBox(QMessageBox::Warning, tr("Couldn't Start"),
	                                    tr("Could not start game."),
	                                    QMessageBox::Ok, this, Qt::Sheet);
	fail->setAttribute(Qt::WA_DeleteOnClose);
	fail->show();
}

void Window::unimplementedBiosCall(int) {
	// TODO: Mention which call?
	if (m_hitUnimplementedBiosCall) {
		return;
	}
	m_hitUnimplementedBiosCall = true;

	QMessageBox* fail = new QMessageBox(
	    QMessageBox::Warning, tr("Unimplemented BIOS call"),
	    tr("This game uses a BIOS call that is not implemented. Please use the official BIOS for best experience."),
	    QMessageBox::Ok, this, Qt::Sheet);
	fail->setAttribute(Qt::WA_DeleteOnClose);
	fail->show();
}

void Window::reloadDisplayDriver() {
	if (m_controller) {
		m_display->stopDrawing();
		detachWidget();
	}
#ifdef ENABLE_SCRIPTING
	if (m_scripting) {
		m_scripting->setVideoBackend(nullptr);
	}
#endif
	std::shared_ptr<VideoProxy> proxy;
	if (m_display) {
		proxy = m_display->videoProxy();
	}
	m_display = std::unique_ptr<QGBA::Display>(Display::create(this));
	if (!m_display) {
		qCritical() << tr("Failed to create an appropriate display device, falling back to software display. "
		                     "Games may run slowly, especially with larger windows.");
		Display::setDriver(Display::Driver::QT);
		m_display = std::unique_ptr<Display>(Display::create(this));
	}
#if defined(BUILD_GL) || defined(BUILD_GLES2)
	m_shaderView.reset();
	if (m_display->supportsShaders()) {
		m_shaderView = std::make_unique<ShaderSelector>(m_display.get(), m_config);
		emit shaderSelectorAdded(m_shaderView.get());
	} else {
		emit shaderSelectorAdded(nullptr);
	}
#endif

	connect(m_display.get(), &QGBA::Display::hideCursor, [this]() {
		if (centralWidget() == m_display.get()) {
			centralWidget()->setCursor(Qt::BlankCursor);
		}
	});
	connect(m_display.get(), &QGBA::Display::showCursor, [this]() {
		centralWidget()->unsetCursor();
	});

	m_display->configure(m_config);
#if defined(BUILD_GL) || defined(BUILD_GLES2)
	if (m_shaderView) {
		m_shaderView->refreshShaders();
	}
#endif

	if (m_controller) {
		attachDisplay();

		attachWidget(m_display.get());
	}
#ifdef M_CORE_GB
	m_display->setMinimumSize(GB_VIDEO_HORIZONTAL_PIXELS, GB_VIDEO_VERTICAL_PIXELS);
#elif defined(M_CORE_GBA)
	m_display->setMinimumSize(GBA_VIDEO_HORIZONTAL_PIXELS, GBA_VIDEO_VERTICAL_PIXELS);
#endif

	QString backgroundImage = m_config->getOption("backgroundImage");
	if (backgroundImage.isEmpty()) {
		m_display->setBackgroundImage(QImage{});
	} else {
		m_display->setBackgroundImage(QImage{backgroundImage});
	}

	if (!proxy) {
		proxy = std::make_shared<VideoProxy>();
	}
	m_display->setVideoProxy(std::move(proxy));
#ifdef ENABLE_SCRIPTING
	if (m_scripting) {
		m_scripting->setVideoBackend(m_display->videoBackend());
	}
#endif
}

void Window::reloadAudioDriver() {
	if (!m_controller) {
		return;
	}
	if (m_audioProcessor) {
		m_audioProcessor->stop();
		m_audioProcessor.reset();
	}

	m_audioProcessor = std::unique_ptr<AudioProcessor>(AudioProcessor::create());
	m_audioProcessor->setInput(m_controller);
	m_audioProcessor->configure(m_config);
	if (!m_audioProcessor->start()) {
		qWarning() << "Failed to start audio processor";
	}
}

void Window::changeRenderer() {
	if (!m_controller) {
		return;
	}

	CoreController::Interrupter interrupter(m_controller);
	if (m_config->getOption("hwaccelVideo").toInt() && m_display->supportsShaders() && m_controller->supportsFeature(CoreController::Feature::OPENGL)) {
		m_display->videoProxy()->attach(m_controller.get());

		int fb = m_display->framebufferHandle();
		if (fb >= 0) {
			m_controller->setFramebufferHandle(fb);
			m_config->updateOption("videoScale");
		}
	} else {
		m_display->videoProxy()->detach(m_controller.get());
		m_controller->setFramebufferHandle(-1);
	}
}

void Window::tryMakePortable() {
	QMessageBox* confirm = new QMessageBox(QMessageBox::Question, tr("Really make portable?"),
	                                       tr("This will make the emulator load its configuration from the same directory as the executable. Do you want to continue?"),
	                                       QMessageBox::Yes | QMessageBox::Cancel, this, Qt::Sheet);
	confirm->setAttribute(Qt::WA_DeleteOnClose);
	connect(confirm->button(QMessageBox::Yes), &QAbstractButton::clicked, m_config, &ConfigController::makePortable);
	confirm->show();
}

void Window::mustRestart() {
	if (m_mustRestart.isActive()) {
		return;
	}
	m_mustRestart.start();
	QMessageBox* dialog = new QMessageBox(QMessageBox::Warning, tr("Restart needed"),
	                                      tr("Some changes will not take effect until the emulator is restarted."),
	                                      QMessageBox::Ok, this, Qt::Sheet);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->show();
}

void Window::mustReset() {
	if (m_mustReset.isActive() || !m_controller) {
		return;
	}
	m_mustReset.start();
	QMessageBox* dialog = new QMessageBox(QMessageBox::Warning, tr("Reset needed"),
	                                      tr("Some changes will not take effect until the game is reset."),
	                                      QMessageBox::Ok, this, Qt::Sheet);
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->show();
}

void Window::recordFrame() {
	m_frameList.append(m_frameTimer.nsecsElapsed());
	m_frameTimer.restart();
}

void Window::showFPS() {
	qint64 total = 0;
	for (qint64 t : m_frameList) {
		total += t;
	}
	if (!total) {
		updateTitle();
		return;
	}
	double fps = (m_frameList.size() * 1e10) / total;
	m_frameList.clear();
	fps = round(fps) / 10.f;
	updateTitle(fps);
}

void Window::updateTitle(float fps) {
	QString title;
	if (m_config->getOption("dynamicTitle", 1).toInt() && m_controller) {
		QString filePath = windowFilePath();
		if (m_config->getOption("showFilename").toInt() && !filePath.isNull()) {
			QFileInfo fileInfo(filePath);
			title = fileInfo.fileName();
		} else {
			title = m_controller->title();
		}

		MultiplayerController* multiplayer = m_controller->multiplayerController();
		if (multiplayer && multiplayer->attached() > 1) {
			title += tr(" -  Player %1 of %2").arg(m_playerId + 1).arg(multiplayer->attached());
			m_actions->setNonMultiplayerActionsEnabled(false);
		} else {
			m_actions->setNonMultiplayerActionsEnabled(true);
		}
	}
	if (title.isNull()) {
		setWindowTitle(tr("%1 - %2").arg(projectName).arg(projectVersion));
	} else if (fps < 0) {
		setWindowTitle(tr("%1 - %2 - %3").arg(projectName).arg(title).arg(projectVersion));
	} else {
		setWindowTitle(tr("%1 - %2 (%3 fps) - %4").arg(projectName).arg(title).arg(fps).arg(projectVersion));
	}
}

void Window::openStateWindow(LoadSave ls) {
	if (m_stateWindow) {
		return;
	}
	MultiplayerController* multiplayer = m_controller->multiplayerController();
	if (multiplayer && multiplayer->attached() > 1) {
		return;
	}
	bool wasPaused = m_controller->isPaused();
	m_stateWindow = new LoadSaveState(m_controller);
	connect(this, &Window::shutdown, m_stateWindow, &QWidget::close);
	connect(m_stateWindow, &LoadSaveState::closed, [this]() {
		attachWidget(m_display.get());
		m_stateWindow = nullptr;
		QMetaObject::invokeMethod(this, "setFocus", Qt::QueuedConnection);
	});
	if (!wasPaused) {
		m_controller->setPaused(true);
		connect(m_stateWindow, &LoadSaveState::closed, [this]() {
			if (m_controller) {
				m_controller->setPaused(false);
			}
		});
	}
	m_stateWindow->setAttribute(Qt::WA_DeleteOnClose);
	m_stateWindow->setMode(ls);

	m_stateWindow->setDimensions(m_controller->screenDimensions());
	m_config->updateOption("lockAspectRatio");
	m_config->updateOption("lockIntegerScaling");

	QImage still(m_controller->getPixels());
	if (still.format() != QImage::Format_RGB888) {
		still = still.convertToFormat(QImage::Format_RGB888);
	}
	if (still.height() > 512 || still.width() > 512) {
		still = still.scaled(384, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB888);
	}
	QImage output(still.size(), QImage::Format_RGB888);
	size_t dims[] = {7, 7};
	struct ConvolutionKernel kern;
	ConvolutionKernelCreate(&kern, 2, dims);
	ConvolutionKernelFillRadial(&kern, true);
	Convolve2DClampChannels8(still.constBits(), output.bits(), still.width(), still.height(), still.bytesPerLine(), 3, &kern);
	ConvolutionKernelDestroy(&kern);

	QPixmap pixmap;
	pixmap.convertFromImage(output);
	m_stateWindow->setBackground(pixmap);

#ifndef Q_OS_MAC
	menuBar()->show();
#endif
	attachWidget(m_stateWindow);
}

void Window::setupPopups() {
	m_popups->logView.constructWith(&m_log, this).setKeepAlive(false);
	m_popups->overrideView.withController(m_controller).constructWith(m_config, this);
	// Why is SensorView keepalive?
	m_popups->sensorView.withController(m_controller).constructWith(&m_inputController, this).setKeepAlive(true);
	m_popups->dolphinView.constructWith(this).setKeepAlive(true);
	m_popups->videoView.withController(m_controller).setKeepAlive(true);
	m_popups->gifView.withController(m_controller).setKeepAlive(true);

#ifdef M_CORE_GB
	m_popups->printerView.withController(m_controller).constructWithCallback([this]() -> PrinterView* {
		m_controller->attachPrinter();
		return new PrinterView(m_controller);
	});
#endif
}

void Window::setupOptions() {
	ConfigOption* videoSync = m_config->addOption("videoSync");
	videoSync->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* audioSync = m_config->addOption("audioSync");
	audioSync->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* skipBios = m_config->addOption("skipBios");
	skipBios->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* useBios = m_config->addOption("useBios");
	useBios->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* buffers = m_config->addOption("audioBuffers");
	buffers->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* sampleRate = m_config->addOption("sampleRate");
	sampleRate->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* volume = m_config->addOption("volume");
	volume->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* volumeFf = m_config->addOption("fastForwardVolume");
	volumeFf->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* muteFf = m_config->addOption("fastForwardMute");
	muteFf->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* rewindEnable = m_config->addOption("rewindEnable");
	rewindEnable->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* rewindBufferCapacity = m_config->addOption("rewindBufferCapacity");
	rewindBufferCapacity->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* rewindBufferInterval = m_config->addOption("rewindBufferInterval");
	rewindBufferInterval->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* allowOpposingDirections = m_config->addOption("allowOpposingDirections");
	allowOpposingDirections->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* saveStateExtdata = m_config->addOption("saveStateExtdata");
	saveStateExtdata->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* loadStateExtdata = m_config->addOption("loadStateExtdata");
	loadStateExtdata->connect([this](const QVariant&) {
		reloadConfig();
	}, this);

	ConfigOption* preload = m_config->addOption("preload");
	preload->connect([this](const QVariant& value) {
		m_manager->setPreload(value.toBool());
	}, this);
	m_config->updateOption("preload");

	ConfigOption* showFps = m_config->addOption("showFps");
	showFps->connect([this](const QVariant& value) {
		if (!value.toInt()) {
			m_fpsTimer.stop();
			updateTitle();
		} else if (m_controller) {
			m_fpsTimer.start();
			m_frameTimer.start();
		}
	}, this);

	ConfigOption* showOSD = m_config->addOption("showOSD");
	showOSD->connect([this](const QVariant& value) {
		if (m_display && !value.isNull()) {
			m_display->showOSDMessages(value.toBool());
		}
	}, this);

	ConfigOption* showFrameCounter = m_config->addOption("showFrameCounter");
	showFrameCounter->connect([this](const QVariant& value) {
		if (m_display) {
			m_display->showFrameCounter(value.toBool());
		}
	}, this);

	ConfigOption* showResetInfo = m_config->addOption("showResetInfo");
	showResetInfo->connect([this](const QVariant& value) {
		if (m_controller) {
			m_controller->showResetInfo(value.toBool());
		}
	}, this);

	ConfigOption* videoScale = m_config->addOption("videoScale");
	videoScale->connect([this](const QVariant& value) {
		if (m_display) {
			m_display->setVideoScale(value.toInt());
#ifdef ENABLE_SCRIPTING
			if (m_controller && m_scripting) {
				m_scripting->updateVideoScale();
			}
#endif
		}
	}, this);

	ConfigOption* dynamicTitle = m_config->addOption("dynamicTitle");
	dynamicTitle->connect([this](const QVariant&) {
		updateTitle();
	}, this);

	ConfigOption* backgroundImage = m_config->addOption("backgroundImage");
	backgroundImage->connect([this](const QVariant& value) {
		if (m_display) {
			QString backgroundImage = value.toString();
			if (backgroundImage.isEmpty()) {
				m_display->setBackgroundImage(QImage{});
			} else {
				m_display->setBackgroundImage(QImage{backgroundImage});
			}
		}
	}, this);
	m_config->updateOption("backgroundImage");
}

void Window::attachWidget(QWidget* widget) {
	// Fix https://mgba.io/i/2885 -- seems like a Qt bug
	if (m_display && widget != m_display.get()) {
		m_display->hide();
	}
	takeCentralWidget();
	setCentralWidget(widget);
	if (m_display && widget == m_display.get()) {
		m_display->show();
	}
}

void Window::detachWidget() {
	m_config->updateOption("showLibrary");
}

void Window::appendMRU(const QString& fname) {
	int index = m_mruFiles.indexOf(fname);
	if (index >= 0) {
		m_mruFiles.removeAt(index);
	}
	m_mruFiles.prepend(fname);
	while (m_mruFiles.size() > ConfigController::MRU_LIST_SIZE) {
		m_mruFiles.removeLast();
	}
	updateMRU();
}

void Window::clearMRU() {
	m_mruFiles.clear();
	updateMRU();
}

void Window::updateMRU() {
	m_actions->updateMRU(m_mruFiles);
	m_config->setMRU(m_mruFiles);
	m_config->write();
}

void Window::ensureScripting() {
#ifdef ENABLE_SCRIPTING
	if (m_scripting) {
		return;
	}
	m_scripting = std::make_unique<ScriptingController>(m_config);
	m_scripting->setInputController(&m_inputController);
	m_actions->shortcutController()->setScriptingController(m_scripting.get());
	if (m_controller) {
		m_scripting->setController(m_controller);
		m_display->installEventFilter(m_scripting.get());
	}

	if (m_display) {
		m_scripting->setVideoBackend(m_display->videoBackend());
	}

	connect(m_scripting.get(), &ScriptingController::autorunScriptsOpened, this, &Window::openView);
#endif
}

void Window::focusCheck() {
	if (!m_controller) {
		return;
	}
	if (m_config->getOption("pauseOnFocusLost").toInt()) {
		if (QGuiApplication::focusWindow() && m_autoresume) {
			m_controller->setPaused(false);
			m_autoresume = false;
		} else if (!QGuiApplication::focusWindow() && !m_controller->isPaused()) {
			m_autoresume = true;
			m_controller->setPaused(true);
		}
	}
	if (m_config->getOption("muteOnFocusLost").toInt()) {
		if (QGuiApplication::focusWindow()) {
			m_inactiveMute = false;
		} else {
			m_inactiveMute = true;
		}
		updateMute();
	}
}

void Window::updateFrame() {
	if (!m_controller) {
		return;
	}
	QPixmap pixmap;
	pixmap.convertFromImage(m_controller->getPixels());
	m_screenWidget->setPixmap(pixmap);
}

void Window::setController(CoreController* controller, const QString& fname) {
	if (!controller) {
		return;
	}
	if (m_pendingClose) {
		return;
	}

	if (m_controller) {
		m_controller->stop();
		QTimer::singleShot(0, this, [this, controller, fname]() {
			setController(controller, fname);
		});
		return;
	}

	if (!fname.isEmpty()) {
		setWindowFilePath(fname);
		appendMRU(fname);
	}

	if (!m_display) {
		reloadDisplayDriver();
	}

	m_controller.setController(controller);
	m_controller->setInputController(&m_inputController);
	m_controller->setLogger(&m_log);

	connect(this, &Window::shutdown, [this]() {
		if (!m_controller) {
			return;
		}
		m_controller->stop();
		disconnect(m_controller.get(), &CoreController::started, this, &Window::gameStarted);
	});

	connect(m_controller.get(), &CoreController::started, this, &Window::gameStarted);
	connect(m_controller.get(), &CoreController::started, GBAApp::app(), &GBAApp::suspendScreensaver);
	connect(m_controller.get(), &CoreController::stopping, this, &Window::gameStopped);
	connect(m_controller.get(), &CoreController::stopping, GBAApp::app(), &GBAApp::resumeScreensaver);
	connect(m_controller.get(), &CoreController::paused, this, &Window::updateFrame);

#ifndef Q_OS_MAC
	connect(m_controller.get(), &CoreController::paused, menuBar(), &QWidget::show);
	connect(m_controller.get(), &CoreController::unpaused, [this]() {
		if(isFullScreen()) {
			menuBar()->hide();
		}
	});
#endif

	connect(m_controller.get(), &CoreController::paused, GBAApp::app(), &GBAApp::resumeScreensaver);
	connect(m_controller.get(), &CoreController::paused, [this]() {
		emit paused(true);
	});
	connect(m_controller.get(), &CoreController::unpaused, [this]() {
		emit paused(false);
	});
	connect(m_controller.get(), &CoreController::unpaused, GBAApp::app(), &GBAApp::suspendScreensaver);
	connect(m_controller.get(), &CoreController::frameAvailable, this, &Window::recordFrame);
	connect(m_controller.get(), &CoreController::crashed, this, &Window::gameCrashed);
	connect(m_controller.get(), &CoreController::failed, this, &Window::gameFailed);
	connect(m_controller.get(), &CoreController::unimplementedBiosCall, this, &Window::unimplementedBiosCall);

#ifdef M_CORE_GBA
	if (m_controller->platform() == mPLATFORM_GBA) {
		QVariant mb = m_config->takeArgvOption(QString("mb"));
#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
		if (mb.canConvert(QMetaType::QString)) {
#else
		if (QMetaType::canConvert(mb.metaType(), QMetaType(QMetaType::QString))) {
#endif
			m_controller->replaceGame(mb.toString());
		}
	}
#endif

	if (!m_pendingPatch.isEmpty()) {
		m_controller->loadPatch(m_pendingPatch);
		m_pendingPatch = QString();
	}

#ifdef ENABLE_SCRIPTING
	if (m_scripting) {
		m_scripting->setController(m_controller);

		m_scripting->setVideoBackend(m_display->videoBackend());
	}
#endif

	attachDisplay();
	m_controller->loadConfig(m_config);
	m_config->updateOption("showOSD");
	m_config->updateOption("showFrameCounter");
	m_config->updateOption("showResetInfo");
	m_controller->start();

	if (!m_pendingState.isEmpty()) {
		m_controller->loadState(m_pendingState);
		m_pendingState = QString();
	}

	if (m_pendingPause) {
		m_controller->setPaused(true);
		m_pendingPause = false;
	}

#ifdef ENABLE_SCRIPTING
	if (!m_scripting) {
		QStringList scripts = m_config->getArgvOption("script").toStringList();
		if (!scripts.isEmpty()) {
			scriptingOpen();
			for (const auto& scriptPath : scripts) {
				m_scripting->loadFile(scriptPath);
			}
		}
	}
#endif
}

void Window::attachDisplay() {
	m_display->attach(m_controller);
	connect(m_display.get(), &QGBA::Display::drawingStarted, this, &Window::changeRenderer);
	if (m_config->getOption("lockFrameSize").toInt()) {
		m_display->setMaximumSize(m_savedSize);
	} else {
		m_display->setMaximumSize({});
	}
	m_display->startDrawing(m_controller);

#ifdef ENABLE_SCRIPTING
	if (m_scripting) {
		m_display->installEventFilter(m_scripting.get());
	}
#endif
}

void Window::updateMute() {
	if (!m_controller) {
		return;
	}

	bool mute = m_inactiveMute;

	if (!mute) {
		QString multiplayerAudio = m_config->getQtOption("multiplayerAudio").toString();
		if (multiplayerAudio == QLatin1String("p1")) {
			MultiplayerController* multiplayer = m_controller->multiplayerController();
			mute = multiplayer && multiplayer->attached() > 1 && m_playerId;
		} else if (multiplayerAudio == QLatin1String("active")) {
			mute = !m_multiActive;
		}
	}

	m_controller->overrideMute(mute);
}

void Window::setLogo() {
	m_screenWidget->setPixmap(m_logo);
	m_screenWidget->setDimensions(m_logo.width(), m_logo.height());
	centralWidget()->unsetCursor();
}

WindowBackground::WindowBackground(QWidget* parent)
	: QWidget(parent)
{
}

void WindowBackground::setPixmap(const QPixmap& pmap) {
	m_pixmap = pmap;
	update();
}

void WindowBackground::setSizeHint(const QSize& hint) {
	m_sizeHint = hint;
}

QSize WindowBackground::sizeHint() const {
	return m_sizeHint;
}

void WindowBackground::setDimensions(int width, int height) {
	m_aspectWidth = width;
	m_aspectHeight = height;
}

void WindowBackground::paintEvent(QPaintEvent* event) {
	QWidget::paintEvent(event);
	const QPixmap& logo = pixmap();
	QPainter painter(this);
	painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
	painter.fillRect(QRect(QPoint(), size()), Qt::black);
	QRect full(clampSize(QSize(m_aspectWidth, m_aspectHeight), size(), true, false));
	painter.drawPixmap(full, logo);
}
