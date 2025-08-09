/* Copyright (c) 2013-2025 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QWidget>

#include <mgba/core/core.h>

#include "FrameView.h"
#include "InputController.h"
#include "LogController.h"
#include "LogView.h"
#include "OverrideView.h"
#include "PopupManager.h"
#include "SensorView.h"

#ifdef M_CORE_GBA
#include "DolphinConnector.h"
#endif

#ifdef USE_FFMPEG
#include "GIFView.h"
#include "VideoView.h"
#endif

#ifdef ENABLE_SCRIPTING
#include "scripting/ScriptingController.h"
#endif

namespace QGBA {

class Window;

class WindowPrivate {
public:
	WindowPrivate(Window* p);

	InputController inputController;
	LogController log{0};

	PopupManager<LogView> logView;
	PopupManager<OverrideView> overrideView;
	PopupManager<SensorView> sensorView;
	PopupManager<FrameView> frameView;

#ifdef M_CORE_GBA
	PopupManager<DolphinConnector> dolphinView;
#endif

#ifdef USE_FFMPEG
	PopupManager<VideoView> videoView;
	PopupManager<GIFView> gifView;
#endif

#ifdef ENABLE_SCRIPTING
	std::unique_ptr<ScriptingController> scripting;
#endif
};

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

}
