/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include "Window.h"

#include "CheatsView.h"
#include "DebuggerConsole.h"
#include "DolphinConnector.h"
#include "FrameView.h"
#include "GDBWindow.h"
#include "GIFView.h"
#include "LogView.h"
#include "OverrideView.h"
#include "PopupManager.h"
#include "PrinterView.h"
#include "SensorView.h"
#include "VideoView.h"

namespace QGBA {

class WindowBackground : public QWidget {
Q_OBJECT

public:
	WindowBackground(QWidget* parent = 0);

	void setPixmap(const QPixmap& pixmap);
	void setSizeHint(const QSize& size);
	virtual QSize sizeHint() const override;
	void setDimensions(int width, int height);
	void setLockIntegerScaling(bool lock);
	void setLockAspectRatio(bool lock);

	const QPixmap& pixmap() const { return m_pixmap; }

protected:
	virtual void paintEvent(QPaintEvent*) override;

private:
	QPixmap m_pixmap;
	QSize m_sizeHint;
	int m_aspectWidth;
	int m_aspectHeight;
};

class WindowPopups {
public:
	PopupManager<LogView> logView;
	PopupManager<OverrideView> overrideView;
	PopupManager<SensorView> sensorView;
	PopupManager<DolphinConnector> dolphinView;
	PopupManager<FrameView> frameView;
	PopupManager<CheatsView> cheatsView;
#ifdef USE_FFMPEG
	PopupManager<VideoView> videoView;
	PopupManager<GIFView> gifView;
#endif
#ifdef ENABLE_GDB_STUB
	PopupManager<GDBWindow> gdbWindow;
#endif
#ifdef ENABLE_DEBUGGERS
	PopupManager<DebuggerConsole> console;
#endif
#ifdef M_CORE_GB
	PopupManager<PrinterView> printerView;
#endif
	QPointer<SettingsView> settingsView;
};

}
